// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/task/engine_clone_task.h"

#include <absl/strings/str_split.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <fmt/format.h>
#include <gen_cpp/AgentService_types.h>
#include <gen_cpp/BackendService.h>
#include <gen_cpp/HeartbeatService_types.h>
#include <gen_cpp/MasterService_types.h>
#include <gen_cpp/Status_types.h>
#include <gen_cpp/Types_constants.h>
#include <sys/stat.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <shared_mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/logging.h"
#include "http/http_client.h"
#include "http/utils.h"
#include "io/fs/file_system.h"
#include "io/fs/local_file_system.h"
#include "io/fs/path.h"
#include "olap/data_dir.h"
#include "olap/olap_common.h"
#include "olap/olap_define.h"
#include "olap/pb_helper.h"
#include "olap/rowset/rowset.h"
#include "olap/snapshot_manager.h"
#include "olap/storage_engine.h"
#include "olap/tablet.h"
#include "olap/tablet_manager.h"
#include "olap/tablet_meta.h"
#include "runtime/client_cache.h"
#include "runtime/memory/mem_tracker_limiter.h"
#include "runtime/thread_context.h"
#include "util/debug_points.h"
#include "util/defer_op.h"
#include "util/network_util.h"
#include "util/security.h"
#include "util/stopwatch.hpp"
#include "util/thrift_rpc_helper.h"
#include "util/trace.h"

using std::set;
using std::stringstream;

namespace doris {
using namespace ErrorCode;

namespace {
/// if binlog file exist, then check if binlog file md5sum equal
/// if equal, then skip link file
/// if not equal, then return error
/// return value: if binlog file not exist, then return to binlog file path
Result<std::string> check_dest_binlog_valid(const std::string& tablet_dir,
                                            const std::string& clone_dir,
                                            const std::string& clone_file, bool* skip_link_file) {
    std::string from, to;
    std::string new_clone_file = clone_file;
    if (clone_file.ends_with(".binlog")) {
        // change clone_file suffix from .binlog to .dat
        new_clone_file.replace(clone_file.size() - 7, 7, ".dat");
    } else if (clone_file.ends_with(".binlog-index")) {
        // change clone_file suffix from .binlog-index to .idx
        new_clone_file.replace(clone_file.size() - 13, 13, ".idx");
    }
    from = fmt::format("{}/{}", clone_dir, clone_file);
    to = fmt::format("{}/_binlog/{}", tablet_dir, new_clone_file);

    // check to to file exist
    bool exists = true;
    auto status = io::global_local_filesystem()->exists(to, &exists);
    if (!status.ok()) {
        return ResultError(std::move(status));
    }

    if (!exists) {
        return to;
    }

    LOG(WARNING) << "binlog file already exist. "
                 << "tablet_dir=" << tablet_dir << ", clone_file=" << from << ", to=" << to;

    std::string clone_file_md5sum;
    status = io::global_local_filesystem()->md5sum(from, &clone_file_md5sum);
    if (!status.ok()) {
        return ResultError(std::move(status));
    }
    std::string to_file_md5sum;
    status = io::global_local_filesystem()->md5sum(to, &to_file_md5sum);
    if (!status.ok()) {
        return ResultError(std::move(status));
    }

    if (clone_file_md5sum == to_file_md5sum) {
        // if md5sum equal, then skip link file
        *skip_link_file = true;
        return to;
    } else {
        auto err_msg = fmt::format(
                "binlog file already exist, but md5sum not equal. "
                "tablet_dir={}, clone_file={}",
                tablet_dir, clone_file);
        LOG(WARNING) << err_msg;
        return ResultError(Status::InternalError(std::move(err_msg)));
    }
}
} // namespace

#define RETURN_IF_ERROR_(status, stmt) \
    do {                               \
        status = (stmt);               \
        if (UNLIKELY(!status.ok())) {  \
            return status;             \
        }                              \
    } while (false)

EngineCloneTask::EngineCloneTask(StorageEngine& engine, const TCloneReq& clone_req,
                                 const ClusterInfo* cluster_info, int64_t signature,
                                 std::vector<TTabletInfo>* tablet_infos)
        : _engine(engine),
          _clone_req(clone_req),
          _tablet_infos(tablet_infos),
          _signature(signature),
          _cluster_info(cluster_info) {
    _mem_tracker = MemTrackerLimiter::create_shared(
            MemTrackerLimiter::Type::OTHER,
            "EngineCloneTask#tabletId=" + std::to_string(_clone_req.tablet_id));
}

Status EngineCloneTask::execute() {
    // register the tablet to avoid it is deleted by gc thread during clone process
    Status st = _do_clone();
    _engine.tablet_manager()->update_partitions_visible_version(
            {{_clone_req.partition_id, _clone_req.version}});
    return st;
}

Status EngineCloneTask::_do_clone() {
    DBUG_EXECUTE_IF("EngineCloneTask.wait_clone", {
        auto duration = std::chrono::milliseconds(dp->param("duration", 10 * 1000));
        std::this_thread::sleep_for(duration);
    });

    DBUG_EXECUTE_IF("EngineCloneTask.failed_clone", {
        LOG_WARNING("EngineCloneTask.failed_clone")
                .tag("tablet_id", _clone_req.tablet_id)
                .tag("replica_id", _clone_req.replica_id)
                .tag("version", _clone_req.version);
        return Status::InternalError(
                "in debug point, EngineCloneTask.failed_clone tablet={}, replica={}, version={}",
                _clone_req.tablet_id, _clone_req.replica_id, _clone_req.version);
    });
    Status status = Status::OK();
    std::string src_file_path;
    TBackend src_host;
    RETURN_IF_ERROR(
            _engine.tablet_manager()->register_transition_tablet(_clone_req.tablet_id, "clone"));
    Defer defer {[&]() {
        _engine.tablet_manager()->unregister_transition_tablet(_clone_req.tablet_id, "clone");
    }};

    // Check local tablet exist or not
    TabletSharedPtr tablet = _engine.tablet_manager()->get_tablet(_clone_req.tablet_id);

    // The status of a tablet is not ready, indicating that it is a residual tablet after a schema
    // change failure. Clone a new tablet from remote be to overwrite it. This situation basically only
    // occurs when the be_rebalancer_fuzzy_test configuration is enabled.
    if (tablet && tablet->tablet_state() == TABLET_NOTREADY) {
        LOG(WARNING) << "tablet state is not ready when clone, need to drop old tablet, tablet_id="
                     << tablet->tablet_id();
        RETURN_IF_ERROR(_engine.tablet_manager()->drop_tablet(tablet->tablet_id(),
                                                              tablet->replica_id(), false));
        tablet.reset();
    }
    _is_new_tablet = tablet == nullptr;
    // try to incremental clone
    Versions missed_versions;
    // try to repair a tablet with missing version
    if (tablet != nullptr) {
        std::shared_lock migration_rlock(tablet->get_migration_lock(), std::try_to_lock);
        if (!migration_rlock.owns_lock()) {
            return Status::Error<TRY_LOCK_FAILED>(
                    "EngineCloneTask::_do_clone meet try lock failed");
        }
        if (tablet->replica_id() < _clone_req.replica_id) {
            // `tablet` may be a dropped replica in FE, e.g:
            //   BE1 migrates replica of tablet_1 to BE2, but before BE1 drop this replica, another new replica of tablet_1 is migrated to BE1.
            // Clone can still continue in this case. But to keep `replica_id` consitent with FE, MUST reset `replica_id` with request `replica_id`.
            tablet->tablet_meta()->set_replica_id(_clone_req.replica_id);
        }

        // get download path
        auto local_data_path = fmt::format("{}/{}", tablet->tablet_path(), CLONE_PREFIX);
        bool allow_incremental_clone = false;

        int64_t specified_version = _clone_req.version;
        if (tablet->enable_unique_key_merge_on_write()) {
            int64_t min_pending_ver = _engine.get_pending_publish_min_version(tablet->tablet_id());
            if (min_pending_ver - 1 < specified_version) {
                LOG(INFO) << "use min pending publish version for clone, min_pending_ver: "
                          << min_pending_ver << " visible_version: " << _clone_req.version;
                specified_version = min_pending_ver - 1;
            }
        }

        missed_versions = tablet->get_missed_versions(specified_version);

        // if missed version size is 0, then it is useless to clone from remote be, it means local data is
        // completed. Or remote be will just return header not the rowset files. clone will failed.
        if (missed_versions.empty()) {
            LOG(INFO) << "missed version size = 0, skip clone and return success. tablet_id="
                      << _clone_req.tablet_id << " replica_id=" << _clone_req.replica_id;
            RETURN_IF_ERROR(_set_tablet_info());
            return Status::OK();
        }

        LOG(INFO) << "clone to existed tablet. missed_versions_size=" << missed_versions.size()
                  << ", allow_incremental_clone=" << allow_incremental_clone
                  << ", signature=" << _signature << ", tablet_id=" << _clone_req.tablet_id
                  << ", visible_version=" << _clone_req.version
                  << ", replica_id=" << _clone_req.replica_id;

        // try to download missing version from src backend.
        // if tablet on src backend does not contains missing version, it will download all versions,
        // and set allow_incremental_clone to false
        RETURN_IF_ERROR(_make_and_download_snapshots(*(tablet->data_dir()), local_data_path,
                                                     &src_host, &src_file_path, missed_versions,
                                                     &allow_incremental_clone));
        RETURN_IF_ERROR(_finish_clone(tablet.get(), local_data_path, specified_version,
                                      allow_incremental_clone));
    } else {
        LOG(INFO) << "clone tablet not exist, begin clone a new tablet from remote be. "
                  << "signature=" << _signature << ", tablet_id=" << _clone_req.tablet_id
                  << ", visible_version=" << _clone_req.version
                  << ", req replica=" << _clone_req.replica_id;
        // create a new tablet in this be
        // Get local disk from olap
        std::string local_shard_root_path;
        DataDir* store = nullptr;
        RETURN_IF_ERROR(_engine.obtain_shard_path(_clone_req.storage_medium,
                                                  _clone_req.dest_path_hash, &local_shard_root_path,
                                                  &store, _clone_req.partition_id));
        auto tablet_dir = fmt::format("{}/{}/{}", local_shard_root_path, _clone_req.tablet_id,
                                      _clone_req.schema_hash);

        Defer remove_useless_dir {[&] {
            if (status.ok()) {
                return;
            }
            LOG(INFO) << "clone failed. want to delete local dir: " << tablet_dir
                      << ". signature: " << _signature;
            WARN_IF_ERROR(io::global_local_filesystem()->delete_directory(tablet_dir),
                          "failed to delete useless clone dir ");
            WARN_IF_ERROR(DataDir::delete_tablet_parent_path_if_empty(tablet_dir),
                          "failed to delete parent dir");
        }};

        bool exists = true;
        Status exists_st = io::global_local_filesystem()->exists(tablet_dir, &exists);
        if (!exists_st) {
            LOG(WARNING) << "cant get path=" << tablet_dir << " state, st=" << exists_st;
            return exists_st;
        }
        if (exists) {
            LOG(WARNING) << "before clone dest path=" << tablet_dir << " exist, remove it first";
            RETURN_IF_ERROR(io::global_local_filesystem()->delete_directory(tablet_dir));
        }

        bool allow_incremental_clone = false;
        RETURN_IF_ERROR_(status,
                         _make_and_download_snapshots(*store, tablet_dir, &src_host, &src_file_path,
                                                      missed_versions, &allow_incremental_clone));

        LOG(INFO) << "clone copy done. src_host: " << src_host.host
                  << " src_file_path: " << src_file_path;
        auto tablet_manager = _engine.tablet_manager();
        RETURN_IF_ERROR_(status, tablet_manager->load_tablet_from_dir(store, _clone_req.tablet_id,
                                                                      _clone_req.schema_hash,
                                                                      tablet_dir, false));
        auto tablet = tablet_manager->get_tablet(_clone_req.tablet_id);
        if (!tablet) {
            status = Status::NotFound("tablet not found, tablet_id={}", _clone_req.tablet_id);
            return status;
        }
        // MUST reset `replica_id` to request `replica_id` to keep consistent with FE
        tablet->tablet_meta()->set_replica_id(_clone_req.replica_id);
        // clone success, delete .hdr file because tablet meta is stored in rocksdb
        std::string header_path =
                TabletMeta::construct_header_file_path(tablet_dir, _clone_req.tablet_id);
        RETURN_IF_ERROR(io::global_local_filesystem()->delete_file(header_path));
    }

    return _set_tablet_info();
}

Status EngineCloneTask::_set_tablet_info() {
    // Get clone tablet info
    TTabletInfo tablet_info;
    tablet_info.__set_tablet_id(_clone_req.tablet_id);
    tablet_info.__set_replica_id(_clone_req.replica_id);
    tablet_info.__set_schema_hash(_clone_req.schema_hash);
    RETURN_IF_ERROR(_engine.tablet_manager()->report_tablet_info(&tablet_info));
    if (_clone_req.__isset.version && tablet_info.version < _clone_req.version) {
        // if it is a new tablet and clone failed, then remove the tablet
        // if it is incremental clone, then must not drop the tablet
        if (_is_new_tablet) {
            // we need to check if this cloned table's version is what we expect.
            // if not, maybe this is a stale remaining table which is waiting for drop.
            // we drop it.
            LOG(WARNING) << "begin to drop the stale tablet. tablet_id:" << _clone_req.tablet_id
                         << ", replica_id:" << _clone_req.replica_id
                         << ", schema_hash:" << _clone_req.schema_hash
                         << ", signature:" << _signature << ", version:" << tablet_info.version
                         << ", expected_version: " << _clone_req.version;
            WARN_IF_ERROR(_engine.tablet_manager()->drop_tablet(_clone_req.tablet_id,
                                                                _clone_req.replica_id, false),
                          "drop stale cloned table failed");
        }
        return Status::InternalError("unexpected version. tablet version: {}, expected version: {}",
                                     tablet_info.version, _clone_req.version);
    }
    LOG(INFO) << "clone get tablet info success. tablet_id:" << _clone_req.tablet_id
              << ", schema_hash:" << _clone_req.schema_hash << ", signature:" << _signature
              << ", replica id:" << _clone_req.replica_id << ", version:" << tablet_info.version;
    _tablet_infos->push_back(tablet_info);
    return Status::OK();
}

/// This method will do following things:
/// 1. Make snapshots on source BE.
/// 2. Download all snapshots to CLONE dir.
/// 3. Convert rowset ids of downloaded snapshots(would also change the replica id).
/// 4. Release the snapshots on source BE.
Status EngineCloneTask::_make_and_download_snapshots(DataDir& data_dir,
                                                     const std::string& local_data_path,
                                                     TBackend* src_host, std::string* snapshot_path,
                                                     const std::vector<Version>& missed_versions,
                                                     bool* allow_incremental_clone) {
    Status status;

    const auto& token = _cluster_info->token;

    int timeout_s = 0;
    if (_clone_req.__isset.timeout_s) {
        timeout_s = _clone_req.timeout_s;
    }

    for (auto&& src : _clone_req.src_backends) {
        // Make snapshot in remote olap engine
        *src_host = src;
        // make snapshot
        status = _make_snapshot(src.host, src.be_port, _clone_req.tablet_id, _clone_req.schema_hash,
                                timeout_s, missed_versions, snapshot_path, allow_incremental_clone);
        if (!status.ok()) [[unlikely]] {
            LOG_WARNING("failed to make snapshot in remote BE")
                    .tag("host", src.host)
                    .tag("port", src.be_port)
                    .tag("tablet", _clone_req.tablet_id)
                    .tag("signature", _signature)
                    .tag("missed_versions", missed_versions)
                    .error(status);
            continue; // Try another BE
        }
        LOG_INFO("successfully make snapshot in remote BE")
                .tag("host", src.host)
                .tag("port", src.be_port)
                .tag("tablet", _clone_req.tablet_id)
                .tag("snapshot_path", *snapshot_path)
                .tag("signature", _signature)
                .tag("missed_versions", missed_versions);
        Defer defer {[host = src.host, port = src.be_port, &snapshot_path = *snapshot_path, this] {
            // TODO(plat1ko): Async release snapshot
            auto st = _release_snapshot(host, port, snapshot_path);
            if (!st.ok()) [[unlikely]] {
                LOG_WARNING("failed to release snapshot in remote BE")
                        .tag("host", host)
                        .tag("port", port)
                        .tag("snapshot_path", snapshot_path)
                        .error(st);
            }
        }};

        std::string remote_dir;
        {
            std::stringstream ss;
            if (snapshot_path->back() == '/') {
                ss << *snapshot_path << _clone_req.tablet_id << "/" << _clone_req.schema_hash
                   << "/";
            } else {
                ss << *snapshot_path << "/" << _clone_req.tablet_id << "/" << _clone_req.schema_hash
                   << "/";
            }
            remote_dir = ss.str();
        }

        std::string address = get_host_port(src.host, src.http_port);
        if (config::enable_batch_download && is_support_batch_download(address).ok()) {
            // download files via batch api.
            LOG_INFO("remote BE supports batch download, use batch file download")
                    .tag("address", address)
                    .tag("remote_dir", remote_dir);
            status = _batch_download_files(&data_dir, address, remote_dir, local_data_path);
            if (!status.ok()) [[unlikely]] {
                LOG_WARNING("failed to download snapshot from remote BE in batch")
                        .tag("address", address)
                        .tag("remote_dir", remote_dir)
                        .error(status);
                continue; // Try another BE
            }
        } else {
            if (config::enable_batch_download) {
                LOG_INFO("remote BE does not support batch download, use single file download")
                        .tag("address", address)
                        .tag("remote_dir", remote_dir);
            } else {
                LOG_INFO("batch download is disabled, use single file download")
                        .tag("address", address)
                        .tag("remote_dir", remote_dir);
            }

            std::string remote_url_prefix;
            {
                std::stringstream ss;
                ss << "http://" << address << HTTP_REQUEST_PREFIX << HTTP_REQUEST_TOKEN_PARAM
                   << token << HTTP_REQUEST_FILE_PARAM << remote_dir;
                remote_url_prefix = ss.str();
            }

            status = _download_files(&data_dir, remote_url_prefix, local_data_path);
            if (!status.ok()) [[unlikely]] {
                LOG_WARNING("failed to download snapshot from remote BE")
                        .tag("url", mask_token(remote_url_prefix))
                        .error(status);
                continue; // Try another BE
            }
        }

        // No need to try again with another BE
        _pending_rs_guards = DORIS_TRY(_engine.snapshot_mgr()->convert_rowset_ids(
                local_data_path, _clone_req.tablet_id, _clone_req.replica_id, _clone_req.table_id,
                _clone_req.partition_id, _clone_req.schema_hash));
        break;
    } // clone copy from one backend
    return status;
}

Status EngineCloneTask::_make_snapshot(const std::string& ip, int port, TTableId tablet_id,
                                       TSchemaHash schema_hash, int timeout_s,
                                       const std::vector<Version>& missed_versions,
                                       std::string* snapshot_path, bool* allow_incremental_clone) {
    TSnapshotRequest request;
    request.__set_tablet_id(tablet_id);
    request.__set_schema_hash(schema_hash);
    request.__set_preferred_snapshot_version(g_Types_constants.TPREFER_SNAPSHOT_REQ_VERSION);
    request.__set_version(_clone_req.version);
    request.__set_is_copy_binlog(true);
    // TODO: missing version composed of singleton delta.
    // if not, this place should be rewrote.
    // we make every TSnapshotRequest sent from be with __isset.missing_version = true
    // then if one be received one req with __isset.missing_version = false it means
    // this req is sent from FE(FE would never set this field)
    request.__isset.missing_version = true;
    for (auto& version : missed_versions) {
        request.missing_version.push_back(version.first);
    }
    if (timeout_s > 0) {
        request.__set_timeout(timeout_s);
    }

    TAgentResult result;
    RETURN_IF_ERROR(ThriftRpcHelper::rpc<BackendServiceClient>(
            ip, port, [&request, &result](BackendServiceConnection& client) {
                client->make_snapshot(result, request);
            }));
    if (result.status.status_code != TStatusCode::OK) {
        return Status::create(result.status);
    }

    if (!result.__isset.snapshot_path) {
        return Status::InternalError("success snapshot request without snapshot path");
    }
    *snapshot_path = result.snapshot_path;
    if (snapshot_path->at(snapshot_path->length() - 1) != '/') {
        snapshot_path->append("/");
    }

    if (result.__isset.allow_incremental_clone) {
        // During upgrading, some BE nodes still be installed an old previous old.
        // which incremental clone is not ready in those nodes.
        // should add a symbol to indicate it.
        *allow_incremental_clone = result.allow_incremental_clone;
    }
    return Status::OK();
}

Status EngineCloneTask::_release_snapshot(const std::string& ip, int port,
                                          const std::string& snapshot_path) {
    TAgentResult result;
    RETURN_IF_ERROR(ThriftRpcHelper::rpc<BackendServiceClient>(
            ip, port, [&snapshot_path, &result](BackendServiceConnection& client) {
                client->release_snapshot(result, snapshot_path);
            }));
    return Status::create(result.status);
}

Status EngineCloneTask::_download_files(DataDir* data_dir, const std::string& remote_url_prefix,
                                        const std::string& local_path) {
    // Check local path exist, if exist, remove it, then create the dir
    // local_file_full_path = tabletid/clone， for a specific tablet, there should be only one folder
    // if this folder exists, then should remove it
    // for example, BE clone from BE 1 to download file 1 with version (2,2), but clone from BE 1 failed
    // then it will try to clone from BE 2, but it will find the file 1 already exist, but file 1 with same
    // name may have different versions.
    RETURN_IF_ERROR(io::global_local_filesystem()->delete_directory(local_path));
    RETURN_IF_ERROR(io::global_local_filesystem()->create_directory(local_path));

    // Get remote dir file list
    std::string file_list_str;
    auto list_files_cb = [&remote_url_prefix, &file_list_str](HttpClient* client) {
        RETURN_IF_ERROR(client->init(remote_url_prefix));
        client->set_timeout_ms(LIST_REMOTE_FILE_TIMEOUT * 1000);
        return client->execute(&file_list_str);
    };
    RETURN_IF_ERROR(HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, list_files_cb));
    std::vector<std::string> file_name_list =
            absl::StrSplit(file_list_str, "\n", absl::SkipWhitespace());

    // If the header file is not exist, the table couldn't loaded by olap engine.
    // Avoid of data is not complete, we copy the header file at last.
    // The header file's name is end of .hdr.
    for (int i = 0; i + 1 < file_name_list.size(); ++i) {
        if (file_name_list[i].ends_with(".hdr")) {
            std::swap(file_name_list[i], file_name_list[file_name_list.size() - 1]);
            break;
        }
    }

    // Get copy from remote
    uint64_t total_file_size = 0;
    MonotonicStopWatch watch;
    watch.start();
    for (auto& file_name : file_name_list) {
        auto remote_file_url = remote_url_prefix + file_name;

        // get file length
        uint64_t file_size = 0;
        auto get_file_size_cb = [&remote_file_url, &file_size](HttpClient* client) {
            RETURN_IF_ERROR(client->init(remote_file_url));
            client->set_timeout_ms(GET_LENGTH_TIMEOUT * 1000);
            RETURN_IF_ERROR(client->head());
            RETURN_IF_ERROR(client->get_content_length(&file_size));
            return Status::OK();
        };
        RETURN_IF_ERROR(
                HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, get_file_size_cb));
        // check disk capacity
        if (data_dir->reach_capacity_limit(file_size)) {
            return Status::Error<EXCEEDED_LIMIT>(
                    "reach the capacity limit of path {}, file_size={}", data_dir->path(),
                    file_size);
        }

        total_file_size += file_size;
        uint64_t estimate_timeout = file_size / config::download_low_speed_limit_kbps / 1024;
        if (estimate_timeout < config::download_low_speed_time) {
            estimate_timeout = config::download_low_speed_time;
        }

        std::string local_file_path = local_path + "/" + file_name;

        LOG(INFO) << "clone begin to download file from: " << mask_token(remote_file_url)
                  << " to: " << local_file_path << ". size(B): " << file_size
                  << ", timeout(s): " << estimate_timeout;

        auto download_cb = [&remote_file_url, estimate_timeout, &local_file_path,
                            file_size](HttpClient* client) {
            RETURN_IF_ERROR(client->init(remote_file_url));
            client->set_timeout_ms(estimate_timeout * 1000);
            RETURN_IF_ERROR(client->download(local_file_path));

            std::error_code ec;
            // Check file length
            uint64_t local_file_size = std::filesystem::file_size(local_file_path, ec);
            if (ec) {
                LOG(WARNING) << "download file error" << ec.message();
                return Status::IOError("can't retrive file_size of {}, due to {}", local_file_path,
                                       ec.message());
            }
            if (local_file_size != file_size) {
                LOG(WARNING) << "download file length error"
                             << ", remote_path=" << mask_token(remote_file_url)
                             << ", file_size=" << file_size
                             << ", local_file_size=" << local_file_size;
                return Status::InternalError("downloaded file size is not equal");
            }
            return io::global_local_filesystem()->permission(local_file_path,
                                                             io::LocalFileSystem::PERMS_OWNER_RW);
        };
        RETURN_IF_ERROR(HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, download_cb));
    } // Clone files from remote backend

    uint64_t total_time_ms = watch.elapsed_time() / 1000 / 1000;
    total_time_ms = total_time_ms > 0 ? total_time_ms : 0;
    double copy_rate = 0.0;
    if (total_time_ms > 0) {
        copy_rate = total_file_size / ((double)total_time_ms) / 1000;
    }
    _copy_size = (int64_t)total_file_size;
    _copy_time_ms = (int64_t)total_time_ms;
    LOG(INFO) << "succeed to copy tablet " << _signature
              << ", total files: " << file_name_list.size()
              << ", total file size: " << total_file_size << " B, cost: " << total_time_ms << " ms"
              << ", rate: " << copy_rate << " MB/s";
    return Status::OK();
}

Status EngineCloneTask::_batch_download_files(DataDir* data_dir, const std::string& address,
                                              const std::string& remote_dir,
                                              const std::string& local_dir) {
    constexpr size_t BATCH_FILE_SIZE = 64 << 20; // 64MB
    constexpr size_t BATCH_FILE_NUM = 64;

    // Check local path exist, if exist, remove it, then create the dir
    // local_file_full_path = tabletid/clone， for a specific tablet, there should be only one folder
    // if this folder exists, then should remove it
    // for example, BE clone from BE 1 to download file 1 with version (2,2), but clone from BE 1 failed
    // then it will try to clone from BE 2, but it will find the file 1 already exist, but file 1 with same
    // name may have different versions.
    RETURN_IF_ERROR(io::global_local_filesystem()->delete_directory(local_dir));
    RETURN_IF_ERROR(io::global_local_filesystem()->create_directory(local_dir));

    const std::string& token = _cluster_info->token;
    std::vector<std::pair<std::string, size_t>> file_info_list;
    RETURN_IF_ERROR(list_remote_files_v2(address, token, remote_dir, &file_info_list));

    // If the header file is not exist, the table couldn't loaded by olap engine.
    // Avoid of data is not complete, we copy the header file at last.
    // The header file's name is end of .hdr.
    for (int i = 0; i + 1 < file_info_list.size(); ++i) {
        if (file_info_list[i].first.ends_with(".hdr")) {
            std::swap(file_info_list[i], file_info_list[file_info_list.size() - 1]);
            break;
        }
    }

    MonotonicStopWatch watch;
    watch.start();

    size_t total_file_size = 0;
    size_t total_files = file_info_list.size();
    std::vector<std::pair<std::string, size_t>> batch_files;
    for (size_t i = 0; i < total_files;) {
        size_t batch_file_size = 0;
        for (size_t j = i; j < total_files; j++) {
            // Split batchs by file number and file size,
            if (BATCH_FILE_NUM <= batch_files.size() || BATCH_FILE_SIZE <= batch_file_size ||
                // ... or separate the last .hdr file into a single batch.
                (j + 1 == total_files && !batch_files.empty())) {
                break;
            }
            batch_files.push_back(file_info_list[j]);
            batch_file_size += file_info_list[j].second;
        }

        // check disk capacity
        if (data_dir->reach_capacity_limit(batch_file_size)) {
            return Status::Error<EXCEEDED_LIMIT>(
                    "reach the capacity limit of path {}, file_size={}", data_dir->path(),
                    batch_file_size);
        }

        RETURN_IF_ERROR(download_files_v2(address, token, remote_dir, local_dir, batch_files));

        total_file_size += batch_file_size;
        i += batch_files.size();
        batch_files.clear();
    }

    uint64_t total_time_ms = watch.elapsed_time() / 1000 / 1000;
    total_time_ms = total_time_ms > 0 ? total_time_ms : 0;
    double copy_rate = 0.0;
    if (total_time_ms > 0) {
        copy_rate = total_file_size / ((double)total_time_ms) / 1000;
    }
    _copy_size = (int64_t)total_file_size;
    _copy_time_ms = (int64_t)total_time_ms;
    LOG(INFO) << "succeed to copy tablet " << _signature
              << ", total files: " << file_info_list.size()
              << ", total file size: " << total_file_size << " B, cost: " << total_time_ms << " ms"
              << ", rate: " << copy_rate << " MB/s";

    return Status::OK();
}

/// This method will only be called if tablet already exist in this BE when doing clone.
/// This method will do the following things:
/// 1. Link all files from CLONE dir to tablet dir if file does not exist in tablet dir
/// 2. Call _finish_xx_clone() to revise the tablet meta.
Status EngineCloneTask::_finish_clone(Tablet* tablet, const std::string& clone_dir, int64_t version,
                                      bool is_incremental_clone) {
    Defer remove_clone_dir {[&]() {
        std::error_code ec;
        std::filesystem::remove_all(clone_dir, ec);
        if (ec) {
            LOG(WARNING) << "failed to remove=" << clone_dir << " msg=" << ec.message();
        }
    }};

    // check clone dir existed
    bool exists = true;
    RETURN_IF_ERROR(io::global_local_filesystem()->exists(clone_dir, &exists));
    if (!exists) {
        return Status::InternalError("clone dir not existed. clone_dir={}", clone_dir);
    }

    // Load src header.
    // The tablet meta info is downloaded from source BE as .hdr file.
    // So we load it and generate cloned_tablet_meta.
    auto cloned_tablet_meta_file = fmt::format("{}/{}.hdr", clone_dir, tablet->tablet_id());
    auto cloned_tablet_meta = std::make_shared<TabletMeta>();
    RETURN_IF_ERROR(cloned_tablet_meta->create_from_file(cloned_tablet_meta_file));

    // remove the cloned meta file
    RETURN_IF_ERROR(io::global_local_filesystem()->delete_file(cloned_tablet_meta_file));

    // remove rowset binlog metas
    const auto& tablet_dir = tablet->tablet_path();
    auto binlog_metas_file = fmt::format("{}/rowset_binlog_metas.pb", clone_dir);
    bool binlog_metas_file_exists = false;
    auto file_exists_status =
            io::global_local_filesystem()->exists(binlog_metas_file, &binlog_metas_file_exists);
    if (!file_exists_status.ok()) {
        return file_exists_status;
    }
    bool contain_binlog = false;
    RowsetBinlogMetasPB rowset_binlog_metas_pb;
    if (binlog_metas_file_exists) {
        std::error_code ec;
        auto binlog_meta_filesize = std::filesystem::file_size(binlog_metas_file, ec);
        if (ec) {
            LOG(WARNING) << "get file size error" << ec.message();
            return Status::IOError("can't retrive file_size of {}, due to {}", binlog_metas_file,
                                   ec.message());
        }
        if (binlog_meta_filesize > 0) {
            contain_binlog = true;
            RETURN_IF_ERROR(read_pb(binlog_metas_file, &rowset_binlog_metas_pb));
        }
        RETURN_IF_ERROR(io::global_local_filesystem()->delete_file(binlog_metas_file));
    }
    if (contain_binlog) {
        auto binlog_dir = fmt::format("{}/_binlog", tablet_dir);
        RETURN_IF_ERROR(io::global_local_filesystem()->create_directory(binlog_dir));
    }

    // check all files in /clone and /tablet
    std::vector<io::FileInfo> clone_files;
    RETURN_IF_ERROR(io::global_local_filesystem()->list(clone_dir, true, &clone_files, &exists));
    std::unordered_set<std::string> clone_file_names;
    for (auto& file : clone_files) {
        clone_file_names.insert(file.file_name);
    }

    std::vector<io::FileInfo> local_files;
    RETURN_IF_ERROR(io::global_local_filesystem()->list(tablet_dir, true, &local_files, &exists));
    std::unordered_set<std::string> local_file_names;
    for (auto& file : local_files) {
        local_file_names.insert(file.file_name);
    }

    Status status;
    std::vector<std::string> linked_success_files;
    Defer remove_linked_files {[&]() { // clear linked files if errors happen
        if (!status.ok()) {
            std::vector<io::Path> paths;
            for (auto& file : linked_success_files) {
                paths.emplace_back(file);
            }
            static_cast<void>(io::global_local_filesystem()->batch_delete(paths));
        }
    }};
    /// Traverse all downloaded clone files in CLONE dir.
    /// If it does not exist in local tablet dir, link the file to local tablet dir
    /// And save all linked files in linked_success_files.
    for (const std::string& clone_file : clone_file_names) {
        if (local_file_names.find(clone_file) != local_file_names.end()) {
            VLOG_NOTICE << "find same file when clone, skip it. "
                        << "tablet=" << tablet->tablet_id() << ", clone_file=" << clone_file;
            continue;
        }

        /// if binlog exist in clone dir and md5sum equal, then skip link file
        bool skip_link_file = false;
        std::string to;
        if (clone_file.ends_with(".binlog") || clone_file.ends_with(".binlog-index")) {
            if (!contain_binlog) {
                LOG(WARNING) << "clone binlog file, but not contain binlog metas. "
                             << "tablet=" << tablet->tablet_id() << ", clone_file=" << clone_file;
                break;
            }

            if (auto&& result =
                        check_dest_binlog_valid(tablet_dir, clone_dir, clone_file, &skip_link_file);
                result) {
                to = std::move(result.value());
            } else {
                status = std::move(result.error());
                return status;
            }
        } else {
            to = fmt::format("{}/{}", tablet_dir, clone_file);
        }

        if (!skip_link_file) {
            auto from = fmt::format("{}/{}", clone_dir, clone_file);
            status = io::global_local_filesystem()->link_file(from, to);
            if (!status.ok()) {
                return status;
            }
            linked_success_files.emplace_back(std::move(to));
        }
    }
    if (contain_binlog) {
        status = tablet->ingest_binlog_metas(&rowset_binlog_metas_pb);
        if (!status.ok()) {
            return status;
        }
    }

    // clone and compaction operation should be performed sequentially
    std::lock_guard base_compaction_lock(tablet->get_base_compaction_lock());
    std::lock_guard cumulative_compaction_lock(tablet->get_cumulative_compaction_lock());
    std::lock_guard cold_compaction_lock(tablet->get_cold_compaction_lock());
    std::lock_guard build_inverted_index_lock(tablet->get_build_inverted_index_lock());
    std::lock_guard<std::mutex> push_lock(tablet->get_push_lock());
    std::lock_guard<std::mutex> rwlock(tablet->get_rowset_update_lock());
    std::lock_guard<std::shared_mutex> wrlock(tablet->get_header_lock());
    SCOPED_SIMPLE_TRACE_IF_TIMEOUT(TRACE_TABLET_LOCK_THRESHOLD);
    if (is_incremental_clone) {
        status = _finish_incremental_clone(tablet, cloned_tablet_meta, version);
    } else {
        status = _finish_full_clone(tablet, cloned_tablet_meta);
    }

    // if full clone success, need to update cumulative layer point
    if (!is_incremental_clone && status.ok()) {
        tablet->set_cumulative_layer_point(Tablet::K_INVALID_CUMULATIVE_POINT);
    }

    // clear clone dir
    return status;
}

/// This method will do:
/// 1. Get missing version from local tablet again and check if they exist in cloned tablet.
/// 2. Revise the local tablet meta to add all incremental cloned rowset's meta.
Status EngineCloneTask::_finish_incremental_clone(Tablet* tablet,
                                                  const TabletMetaSharedPtr& cloned_tablet_meta,
                                                  int64_t version) {
    LOG(INFO) << "begin to finish incremental clone. tablet=" << tablet->tablet_id()
              << ", visible_version=" << version
              << ", cloned_tablet_replica_id=" << cloned_tablet_meta->replica_id();

    /// Get missing versions again from local tablet.
    /// We got it before outside the lock, so it has to be got again.
    Versions missed_versions = tablet->get_missed_versions_unlocked(version);
    VLOG_NOTICE << "get missed versions again when finish incremental clone. "
                << "tablet=" << tablet->tablet_id() << ", clone version=" << version
                << ", missed_versions_size=" << missed_versions.size();

    // check missing versions exist in clone src
    std::vector<RowsetSharedPtr> rowsets_to_clone;
    for (Version version : missed_versions) {
        auto rs_meta = cloned_tablet_meta->acquire_rs_meta_by_version(version);
        if (rs_meta == nullptr) {
            return Status::InternalError("missed version {} is not found in cloned tablet meta",
                                         version.to_string());
        }
        RowsetSharedPtr rs;
        RETURN_IF_ERROR(tablet->create_rowset(rs_meta, &rs));
        rowsets_to_clone.push_back(std::move(rs));
    }

    /// clone_data to tablet
    /// For incremental clone, nothing will be deleted.
    /// So versions_to_delete is empty.
    return tablet->revise_tablet_meta(rowsets_to_clone, {}, true);
}

/// This method will do:
/// 1. Compare the version of local tablet and cloned tablet to decide which version to keep
/// 2. Revise the local tablet meta
Status EngineCloneTask::_finish_full_clone(Tablet* tablet,
                                           const TabletMetaSharedPtr& cloned_tablet_meta) {
    Version cloned_max_version = cloned_tablet_meta->max_version();
    LOG(INFO) << "begin to finish full clone. tablet=" << tablet->tablet_id()
              << ", cloned_max_version=" << cloned_max_version;

    // Compare the version of local tablet and cloned tablet.
    // For example:
    // clone version is 8
    //
    //      local tablet: [0-1] [2-5] [6-6] [7-7] [9-10]
    //      clone tablet: [0-1] [2-4] [5-6] [7-8]
    //
    // after compare, the version mark with "x" will be deleted
    //
    //      local tablet: [0-1]x [2-5]x [6-6]x [7-7]x [9-10]
    //      clone tablet: [0-1]  [2-4]  [5-6]  [7-8]

    std::vector<RowsetSharedPtr> to_delete;
    std::vector<RowsetSharedPtr> to_add;
    for (auto& [v, rs] : tablet->rowset_map()) {
        // if local version cross src latest, clone failed
        // if local version is : 0-0, 1-1, 2-10, 12-14, 15-15,16-16
        // cloned max version is 13-13, this clone is failed, because could not
        // fill local data by using cloned data.
        // It should not happen because if there is a hole, the following delta will not
        // do compaction.
        if (v.first <= cloned_max_version.second && v.second > cloned_max_version.second) {
            return Status::InternalError(
                    "version cross src latest. cloned_max_version={}, local_version={}",
                    cloned_max_version.second, v.to_string());
        }
        if (v.second <= cloned_max_version.second) {
            to_delete.push_back(rs);
        } else {
            // cooldowned rowsets MUST be continuous, so rowsets whose version > missed version MUST be local rowset
            DCHECK(rs->is_local());
        }
    }

    to_add.reserve(cloned_tablet_meta->all_rs_metas().size());
    for (auto& rs_meta : cloned_tablet_meta->all_rs_metas()) {
        RowsetSharedPtr rs;
        RETURN_IF_ERROR(tablet->create_rowset(rs_meta, &rs));
        to_add.push_back(std::move(rs));
    }
    {
        std::shared_lock cooldown_conf_rlock(tablet->get_cooldown_conf_lock());
        if (tablet->cooldown_conf_unlocked().cooldown_replica_id == tablet->replica_id()) {
            // If this replica is cooldown replica, MUST generate a new `cooldown_meta_id` to avoid use `cooldown_meta_id`
            // generated in old cooldown term which may lead to such situation:
            // Replica A is cooldown replica, cooldown_meta_id=2,
            // Replica B: cooldown_replica=A, cooldown_meta_id=1
            // Replica A: full clone Replica A, cooldown_meta_id=1, but remote cooldown_meta is still with cooldown_meta_id=2
            // After tablet report. FE finds all replicas' cooldowned data is consistent
            // Replica A: confirm_unused_remote_files, delete some cooldowned data of cooldown_meta_id=2
            // Replica B: follow_cooldown_data, cooldown_meta_id=2, data lost
            tablet->tablet_meta()->set_cooldown_meta_id(UniqueId::gen_uid());
        } else {
            tablet->tablet_meta()->set_cooldown_meta_id(cloned_tablet_meta->cooldown_meta_id());
        }
    }
    if (tablet->enable_unique_key_merge_on_write()) {
        tablet->tablet_meta()->delete_bitmap().merge(cloned_tablet_meta->delete_bitmap());
    }
    return tablet->revise_tablet_meta(to_add, to_delete, false);
    // TODO(plat1ko): write cooldown meta to remote if this replica is cooldown replica
}
} // namespace doris
