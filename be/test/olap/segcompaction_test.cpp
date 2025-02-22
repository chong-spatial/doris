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

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "gen_cpp/AgentService_types.h"
#include "gen_cpp/olap_file.pb.h"
#include "io/fs/local_file_system.h"
#include "olap/data_dir.h"
#include "olap/row_cursor.h"
#include "olap/rowset/beta_rowset_reader.h"
#include "olap/rowset/beta_rowset_writer.h"
#include "olap/rowset/rowset_factory.h"
#include "olap/rowset/rowset_reader_context.h"
#include "olap/rowset/rowset_writer.h"
#include "olap/rowset/rowset_writer_context.h"
#include "olap/rowset/segment_v2/segment_writer.h"
#include "olap/storage_engine.h"
#include "olap/tablet_meta.h"
#include "olap/tablet_schema.h"
#include "olap/utils.h"
#include "runtime/exec_env.h"
#include "runtime/memory/mem_tracker.h"
#include "util/slice.h"

namespace doris {
using namespace ErrorCode;

static const uint32_t MAX_PATH_LEN = 1024;
static StorageEngine* l_engine = nullptr;
static const std::string lTestDir = "./data_test/data/segcompaction_test";
constexpr static std::string_view tmp_dir = "./data_test/tmp";

class SegCompactionTest : public testing::Test {
public:
    SegCompactionTest() = default;

    void SetUp() {
        config::enable_segcompaction = true;
        config::tablet_map_shard_size = 1;
        config::txn_map_shard_size = 1;
        config::txn_shard_size = 1;
        config::inverted_index_fd_number_limit_percent = 0;

        char buffer[MAX_PATH_LEN];
        EXPECT_NE(getcwd(buffer, MAX_PATH_LEN), nullptr);
        config::storage_root_path = std::string(buffer) + "/data_test";

        auto st = io::global_local_filesystem()->delete_directory(config::storage_root_path);
        ASSERT_TRUE(st.ok()) << st;
        st = io::global_local_filesystem()->create_directory(config::storage_root_path);
        ASSERT_TRUE(st.ok()) << st;

        std::vector<StorePath> paths;
        paths.emplace_back(config::storage_root_path, -1);

        // tmp dir
        EXPECT_TRUE(io::global_local_filesystem()->delete_directory(tmp_dir).ok());
        EXPECT_TRUE(io::global_local_filesystem()->create_directory(tmp_dir).ok());
        paths.emplace_back(std::string(tmp_dir), 1024000000);
        auto tmp_file_dirs = std::make_unique<segment_v2::TmpFileDirs>(paths);
        EXPECT_TRUE(tmp_file_dirs->init().ok());
        ExecEnv::GetInstance()->set_tmp_file_dir(std::move(tmp_file_dirs));

        // use memory limit
        int64_t inverted_index_cache_limit = 0;
        _inverted_index_searcher_cache = std::unique_ptr<segment_v2::InvertedIndexSearcherCache>(
                InvertedIndexSearcherCache::create_global_instance(inverted_index_cache_limit,
                                                                   256));

        ExecEnv::GetInstance()->set_inverted_index_searcher_cache(
                _inverted_index_searcher_cache.get());

        doris::EngineOptions options;
        options.store_paths = paths;

        auto engine = std::make_unique<StorageEngine>(options);
        Status s = engine->open();
        EXPECT_TRUE(s.ok()) << s.to_string();

        l_engine = engine.get();
        ExecEnv::GetInstance()->set_storage_engine(std::move(engine));

        s = ThreadPoolBuilder("SegCompactionTaskThreadPool")
                    .set_min_threads(config::segcompaction_num_threads)
                    .set_max_threads(config::segcompaction_num_threads)
                    .build(&l_engine->_seg_compaction_thread_pool);
        EXPECT_TRUE(s.ok()) << s.to_string();

        _data_dir = std::make_unique<DataDir>(*l_engine, lTestDir);
        static_cast<void>(_data_dir->update_capacity());

        EXPECT_TRUE(io::global_local_filesystem()->create_directory(lTestDir).ok());
    }

    void TearDown() {
        config::enable_segcompaction = false;
        ExecEnv* exec_env = doris::ExecEnv::GetInstance();
        l_engine = nullptr;
        exec_env->set_storage_engine(nullptr);
        exec_env->set_inverted_index_searcher_cache(nullptr);
    }

protected:
    OlapReaderStatistics _stats;

    bool check_dir(std::vector<std::string>& vec) {
        std::vector<std::string> result;
        for (const auto& entry : std::filesystem::directory_iterator(lTestDir)) {
            result.push_back(std::filesystem::path(entry.path()).filename());
        }

        LOG(INFO) << "expected ls:" << std::endl;
        for (auto& i : vec) {
            LOG(INFO) << i;
        }
        LOG(INFO) << "acutal ls:" << std::endl;
        for (auto& i : result) {
            LOG(INFO) << i;
        }

        if (result.size() != vec.size()) {
            return false;
        } else {
            for (auto& i : vec) {
                if (std::find(result.begin(), result.end(), i) == result.end()) {
                    return false;
                }
            }
        }
        return true;
    }

    // (k1 int, k2 varchar(20), k3 int) keys (k1, k2)
    void create_tablet_schema(TabletSchemaSPtr tablet_schema, KeysType keystype,
                              int num_value_col = 1) {
        TabletSchemaPB tablet_schema_pb;
        tablet_schema_pb.set_keys_type(keystype);
        tablet_schema_pb.set_num_short_key_columns(2);
        tablet_schema_pb.set_num_rows_per_row_block(1024);
        tablet_schema_pb.set_compress_kind(COMPRESS_NONE);
        tablet_schema_pb.set_next_column_unique_id(4);
        tablet_schema_pb.set_inverted_index_storage_format(InvertedIndexStorageFormatPB::V2);

        ColumnPB* column_1 = tablet_schema_pb.add_column();
        column_1->set_unique_id(1);
        column_1->set_name("k1");
        column_1->set_type("INT");
        column_1->set_is_key(true);
        column_1->set_length(4);
        column_1->set_index_length(4);
        column_1->set_is_nullable(true);
        column_1->set_is_bf_column(false);
        auto tablet_index_1 = tablet_schema_pb.add_index();
        tablet_index_1->set_index_id(1);
        tablet_index_1->set_index_name("column_1");
        tablet_index_1->set_index_type(IndexType::INVERTED);
        tablet_index_1->add_col_unique_id(1);

        ColumnPB* column_2 = tablet_schema_pb.add_column();
        column_2->set_unique_id(2);
        column_2->set_name("k2");
        column_2->set_type(
                "INT"); // TODO change to varchar(20) when dict encoding for string is supported
        column_2->set_length(4);
        column_2->set_index_length(4);
        column_2->set_is_nullable(true);
        column_2->set_is_key(true);
        column_2->set_is_nullable(true);
        column_2->set_is_bf_column(false);
        auto tablet_index_2 = tablet_schema_pb.add_index();
        tablet_index_2->set_index_id(2);
        tablet_index_2->set_index_name("column_2");
        tablet_index_2->set_index_type(IndexType::INVERTED);
        tablet_index_2->add_col_unique_id(2);

        for (int i = 1; i <= num_value_col; i++) {
            ColumnPB* v_column = tablet_schema_pb.add_column();
            v_column->set_unique_id(2 + i);
            v_column->set_name(fmt::format("v{}", i));
            v_column->set_type("INT");
            v_column->set_length(4);
            v_column->set_is_key(false);
            v_column->set_is_nullable(false);
            v_column->set_is_bf_column(false);
            v_column->set_default_value(std::to_string(i * 10));
            v_column->set_aggregation("SUM");
        }

        tablet_schema->init_from_pb(tablet_schema_pb);
    }

    void construct_column(ColumnPB* column_pb, TabletIndexPB* tablet_index, int64_t index_id,
                          const std::string& index_name, int32_t col_unique_id,
                          const std::string& column_type, const std::string& column_name,
                          bool parser = false) {
        column_pb->set_unique_id(col_unique_id);
        column_pb->set_name(column_name);
        column_pb->set_type(column_type);
        column_pb->set_is_key(false);
        column_pb->set_is_nullable(true);
        tablet_index->set_index_id(index_id);
        tablet_index->set_index_name(index_name);
        tablet_index->set_index_type(IndexType::INVERTED);
        tablet_index->add_col_unique_id(col_unique_id);
        if (parser) {
            auto* properties = tablet_index->mutable_properties();
            (*properties)[INVERTED_INDEX_PARSER_KEY] = INVERTED_INDEX_PARSER_UNICODE;
        }
    }
    // use different id to avoid conflict
    void create_rowset_writer_context(int64_t id, TabletSchemaSPtr tablet_schema,
                                      RowsetWriterContext* rowset_writer_context) {
        RowsetId rowset_id;
        rowset_id.init(id);
        // rowset_writer_context->data_dir = _data_dir.get();
        rowset_writer_context->rowset_id = rowset_id;
        rowset_writer_context->tablet_id = 12345;
        rowset_writer_context->tablet_schema_hash = 1111;
        rowset_writer_context->partition_id = 10;
        rowset_writer_context->rowset_type = BETA_ROWSET;
        rowset_writer_context->tablet_path = lTestDir;
        rowset_writer_context->rowset_state = VISIBLE;
        rowset_writer_context->tablet_schema = tablet_schema;
        rowset_writer_context->version.first = 10;
        rowset_writer_context->version.second = 10;

#if 0
        RuntimeProfile profile("CreateTablet");
        TCreateTabletReq req;
        req.table_id =
        req.tablet_id =
        req.tablet_scheme =
        req.partition_id =
        l_engine->create_tablet(req, &profile);
        rowset_writer_context->tablet = l_engine->tablet_manager()->get_tablet(TTabletId tablet_id);
#endif
        TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
        tablet_meta->_tablet_id = 1;
        static_cast<void>(tablet_meta->set_partition_id(10000));
        tablet_meta->_schema = tablet_schema;
        auto tablet = std::make_shared<Tablet>(*l_engine, tablet_meta, _data_dir.get(), "test_str");
        // tablet->key
        rowset_writer_context->tablet = tablet;
    }

    void create_and_init_rowset_reader(Rowset* rowset, RowsetReaderContext& context,
                                       RowsetReaderSharedPtr* result) {
        auto s = rowset->create_reader(result);
        EXPECT_EQ(Status::OK(), s);
        EXPECT_TRUE(*result != nullptr);

        s = (*result)->init(&context);
        EXPECT_EQ(Status::OK(), s);
    }

private:
    std::unique_ptr<DataDir> _data_dir;
    std::unique_ptr<InvertedIndexSearcherCache> _inverted_index_searcher_cache;
};

TEST_F(SegCompactionTest, SegCompactionThenRead) {
    config::enable_segcompaction = true;
    Status s;
    TabletSchemaSPtr tablet_schema = std::make_shared<TabletSchema>();
    create_tablet_schema(tablet_schema, DUP_KEYS);

    RowsetSharedPtr rowset;
    const int num_segments = 15;
    const uint32_t rows_per_segment = 4096;
    config::segcompaction_candidate_max_rows = 6000; // set threshold above
                                                     // rows_per_segment
    config::segcompaction_batch_size = 10;
    std::vector<uint32_t> segment_num_rows;
    { // write `num_segments * rows_per_segment` rows to rowset
        RowsetWriterContext writer_context;
        create_rowset_writer_context(10047, tablet_schema, &writer_context);

        auto res = RowsetFactory::create_rowset_writer(*l_engine, writer_context, false);
        EXPECT_TRUE(res.has_value()) << res.error();
        auto rowset_writer = std::move(res).value();
        EXPECT_EQ(Status::OK(), s);

        // for segment "i", row "rid"
        // k1 := rid*10 + i
        // k2 := k1 * 10
        // k3 := rid
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
            sleep(1);
        }

        EXPECT_EQ(Status::OK(), rowset_writer->build(rowset));
        std::vector<std::string> ls;
        ls.push_back("10047_0.dat");
        ls.push_back("10047_1.dat");
        ls.push_back("10047_2.dat");
        ls.push_back("10047_3.dat");
        ls.push_back("10047_4.dat");
        ls.push_back("10047_5.dat");
        ls.push_back("10047_6.dat");
        ls.push_back("10047_0.idx");
        ls.push_back("10047_1.idx");
        ls.push_back("10047_2.idx");
        ls.push_back("10047_3.idx");
        ls.push_back("10047_4.idx");
        ls.push_back("10047_5.idx");
        ls.push_back("10047_6.idx");
        EXPECT_TRUE(check_dir(ls));
    }

    { // read
        RowsetReaderContext reader_context;
        reader_context.tablet_schema = tablet_schema;
        // use this type to avoid cache from other ut
        reader_context.reader_type = ReaderType::READER_CUMULATIVE_COMPACTION;
        reader_context.need_ordered_result = true;
        std::vector<uint32_t> return_columns = {0, 1, 2};
        reader_context.return_columns = &return_columns;
        reader_context.stats = &_stats;

        // without predicates
        {
            RowsetReaderSharedPtr rowset_reader;
            create_and_init_rowset_reader(rowset.get(), reader_context, &rowset_reader);

            uint32_t num_rows_read = 0;
            bool eof = false;
            while (!eof) {
                std::shared_ptr<vectorized::Block> output_block =
                        std::make_shared<vectorized::Block>(
                                tablet_schema->create_block(return_columns));
                s = rowset_reader->next_block(output_block.get());
                if (s != Status::OK()) {
                    eof = true;
                }
                EXPECT_GT(output_block->rows(), 0);
                EXPECT_EQ(return_columns.size(), output_block->columns());
                for (int i = 0; i < output_block->rows(); ++i) {
                    vectorized::ColumnPtr col0 = output_block->get_by_position(0).column;
                    vectorized::ColumnPtr col1 = output_block->get_by_position(1).column;
                    vectorized::ColumnPtr col2 = output_block->get_by_position(2).column;
                    auto field1 = (*col0)[i];
                    auto field2 = (*col1)[i];
                    auto field3 = (*col2)[i];
                    uint32_t k1 = *reinterpret_cast<uint32_t*>((char*)(&field1));
                    uint32_t k2 = *reinterpret_cast<uint32_t*>((char*)(&field2));
                    uint32_t v3 = *reinterpret_cast<uint32_t*>((char*)(&field3));
                    EXPECT_EQ(100 * v3 + k2, k1);
                    num_rows_read++;
                }
                output_block->clear();
            }
            EXPECT_EQ(Status::Error<END_OF_FILE>(""), s);
            EXPECT_EQ(rowset->rowset_meta()->num_rows(), num_rows_read);
            EXPECT_EQ(num_rows_read, num_segments * rows_per_segment);
            EXPECT_TRUE(rowset_reader->get_segment_num_rows(&segment_num_rows).ok());
            size_t total_num_rows = 0;
            for (const auto& i : segment_num_rows) {
                total_num_rows += i;
            }
            EXPECT_EQ(total_num_rows, num_rows_read);
        }
    }
}

TEST_F(SegCompactionTest, SegCompactionInterleaveWithBig_ooooOOoOooooooooO) {
    config::enable_segcompaction = true;
    Status s;
    TabletSchemaSPtr tablet_schema = std::make_shared<TabletSchema>();
    create_tablet_schema(tablet_schema, DUP_KEYS);

    RowsetSharedPtr rowset;
    config::segcompaction_candidate_max_rows = 6000; // set threshold above
                                                     // rows_per_segment
    std::vector<uint32_t> segment_num_rows;
    { // write `num_segments * rows_per_segment` rows to rowset
        RowsetWriterContext writer_context;
        create_rowset_writer_context(10048, tablet_schema, &writer_context);

        auto res = RowsetFactory::create_rowset_writer(*l_engine, writer_context, false);
        EXPECT_TRUE(res.has_value()) << res.error();
        auto rowset_writer = std::move(res).value();
        EXPECT_EQ(Status::OK(), s);

        // for segment "i", row "rid"
        // k1 := rid*10 + i
        // k2 := k1 * 10
        // k3 := 4096 * i + rid
        int num_segments = 4;
        uint32_t rows_per_segment = 4096;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 2;
        rows_per_segment = 6400;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 1;
        rows_per_segment = 4096;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 1;
        rows_per_segment = 6400;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 8;
        rows_per_segment = 4096;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
            sleep(1);
        }
        num_segments = 1;
        rows_per_segment = 6400;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
            sleep(1);
        }

        EXPECT_EQ(Status::OK(), rowset_writer->build(rowset));
        std::vector<std::string> ls;
        // ooooOOoOooooooooO
        ls.push_back("10048_0.dat"); // oooo
        ls.push_back("10048_1.dat"); // O
        ls.push_back("10048_2.dat"); // O
        ls.push_back("10048_3.dat"); // o
        ls.push_back("10048_4.dat"); // O
        ls.push_back("10048_5.dat"); // oooooooo
        ls.push_back("10048_6.dat"); // O
        ls.push_back("10048_0.idx"); // oooo
        ls.push_back("10048_1.idx"); // O
        ls.push_back("10048_2.idx"); // O
        ls.push_back("10048_3.idx"); // o
        ls.push_back("10048_4.idx"); // O
        ls.push_back("10048_5.idx"); // oooooooo
        ls.push_back("10048_6.idx"); // O
        EXPECT_TRUE(check_dir(ls));
    }
}

TEST_F(SegCompactionTest, SegCompactionInterleaveWithBig_OoOoO) {
    config::enable_segcompaction = true;
    Status s;
    TabletSchemaSPtr tablet_schema = std::make_shared<TabletSchema>();
    create_tablet_schema(tablet_schema, DUP_KEYS);

    RowsetSharedPtr rowset;
    config::segcompaction_candidate_max_rows = 6000; // set threshold above
    config::segcompaction_batch_size = 5;
    std::vector<uint32_t> segment_num_rows;
    { // write `num_segments * rows_per_segment` rows to rowset
        RowsetWriterContext writer_context;
        create_rowset_writer_context(10049, tablet_schema, &writer_context);

        auto res = RowsetFactory::create_rowset_writer(*l_engine, writer_context, false);
        EXPECT_TRUE(res.has_value()) << res.error();
        auto rowset_writer = std::move(res).value();
        EXPECT_EQ(Status::OK(), s);

        // for segment "i", row "rid"
        // k1 := rid*10 + i
        // k2 := k1 * 10
        // k3 := 4096 * i + rid
        int num_segments = 1;
        uint32_t rows_per_segment = 6400;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 1;
        rows_per_segment = 4096;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 1;
        rows_per_segment = 6400;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 1;
        rows_per_segment = 4096;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
        }
        num_segments = 1;
        rows_per_segment = 6400;
        for (int i = 0; i < num_segments; ++i) {
            vectorized::Block block = tablet_schema->create_block();
            auto columns = block.mutate_columns();
            for (int rid = 0; rid < rows_per_segment; ++rid) {
                uint32_t k1 = rid * 100 + i;
                uint32_t k2 = i;
                uint32_t k3 = rid;
                columns[0]->insert_data((const char*)&k1, sizeof(k1));
                columns[1]->insert_data((const char*)&k2, sizeof(k2));
                columns[2]->insert_data((const char*)&k3, sizeof(k3));
            }
            s = rowset_writer->add_block(&block);
            EXPECT_TRUE(s.ok());
            s = rowset_writer->flush();
            EXPECT_EQ(Status::OK(), s);
            sleep(1);
        }

        EXPECT_EQ(Status::OK(), rowset_writer->build(rowset));
        std::vector<std::string> ls;
        ls.push_back("10049_0.dat"); // O
        ls.push_back("10049_1.dat"); // o
        ls.push_back("10049_2.dat"); // O
        ls.push_back("10049_3.dat"); // o
        ls.push_back("10049_4.dat"); // O
        ls.push_back("10049_0.idx"); // O
        ls.push_back("10049_1.idx"); // o
        ls.push_back("10049_2.idx"); // O
        ls.push_back("10049_3.idx"); // o
        ls.push_back("10049_4.idx"); // O
        EXPECT_TRUE(check_dir(ls));
    }
}

TEST_F(SegCompactionTest, SegCompactionThenReadUniqueTableSmall) {
    config::enable_segcompaction = true;
    Status s;
    TabletSchemaSPtr tablet_schema = std::make_shared<TabletSchema>();
    create_tablet_schema(tablet_schema, UNIQUE_KEYS);

    RowsetSharedPtr rowset;
    config::segcompaction_candidate_max_rows = 6000; // set threshold above
                                                     // rows_per_segment
    config::segcompaction_batch_size = 3;
    std::vector<uint32_t> segment_num_rows;
    { // write `num_segments * rows_per_segment` rows to rowset
        RowsetWriterContext writer_context;
        create_rowset_writer_context(10051, tablet_schema, &writer_context);

        auto res = RowsetFactory::create_rowset_writer(*l_engine, writer_context, false);
        EXPECT_TRUE(res.has_value()) << res.error();
        auto rowset_writer = std::move(res).value();
        EXPECT_EQ(Status::OK(), s);

        uint32_t k1 = 0;
        uint32_t k2 = 0;
        uint32_t k3 = 0;

        vectorized::Block block = tablet_schema->create_block();
        auto columns = block.mutate_columns();
        // segment#0
        k1 = k2 = 1;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 4;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 6;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);
        // segment#1
        k1 = k2 = 2;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 4;
        k3 = 2;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 6;
        k3 = 2;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#2
        k1 = k2 = 3;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 6;
        k3 = 3;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 9;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#3
        k1 = k2 = 4;
        k3 = 3;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 9;
        k3 = 2;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 12;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#4
        k1 = k2 = 25;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#5
        k1 = k2 = 26;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        EXPECT_EQ(Status::OK(), rowset_writer->build(rowset));
        std::vector<std::string> ls;
        ls.push_back("10051_0.dat");
        ls.push_back("10051_1.dat");
        ls.push_back("10051_2.dat");
        ls.push_back("10051_3.dat");
        ls.push_back("10051_0.idx");
        ls.push_back("10051_1.idx");
        ls.push_back("10051_2.idx");
        ls.push_back("10051_3.idx");
        EXPECT_TRUE(check_dir(ls));
    }

    { // read
        RowsetReaderContext reader_context;
        reader_context.tablet_schema = tablet_schema;
        // use this type to avoid cache from other ut
        reader_context.reader_type = ReaderType::READER_CUMULATIVE_COMPACTION;
        reader_context.need_ordered_result = true;
        std::vector<uint32_t> return_columns = {0, 1, 2};
        reader_context.return_columns = &return_columns;
        reader_context.stats = &_stats;
        reader_context.is_unique = true;

        // without predicates
        {
            RowsetReaderSharedPtr rowset_reader;
            create_and_init_rowset_reader(rowset.get(), reader_context, &rowset_reader);

            uint32_t num_rows_read = 0;
            bool eof = false;
            while (!eof) {
                std::shared_ptr<vectorized::Block> output_block =
                        std::make_shared<vectorized::Block>(
                                tablet_schema->create_block(return_columns));
                s = rowset_reader->next_block(output_block.get());
                if (s != Status::OK()) {
                    eof = true;
                }
                EXPECT_GT(output_block->rows(), 0);
                EXPECT_EQ(return_columns.size(), output_block->columns());
                for (int i = 0; i < output_block->rows(); ++i) {
                    vectorized::ColumnPtr col0 = output_block->get_by_position(0).column;
                    vectorized::ColumnPtr col1 = output_block->get_by_position(1).column;
                    vectorized::ColumnPtr col2 = output_block->get_by_position(2).column;
                    auto field1 = (*col0)[i];
                    auto field2 = (*col1)[i];
                    auto field3 = (*col2)[i];
                    uint32_t k1 = *reinterpret_cast<uint32_t*>((char*)(&field1));
                    uint32_t k2 = *reinterpret_cast<uint32_t*>((char*)(&field2));
                    uint32_t v3 = *reinterpret_cast<uint32_t*>((char*)(&field3));
                    std::cout << "k1 k2 k3: " << k1 << " " << k2 << " " << v3 << std::endl;
                    num_rows_read++;
                }
                output_block->clear();
            }
            EXPECT_EQ(Status::Error<END_OF_FILE>(""), s);
            // duplicated keys between segments are counted duplicately
            // so actual read by rowset reader is less or equal to it
            EXPECT_GE(rowset->rowset_meta()->num_rows(), num_rows_read);
            EXPECT_TRUE(rowset_reader->get_segment_num_rows(&segment_num_rows).ok());
            size_t total_num_rows = 0;
            for (const auto& i : segment_num_rows) {
                total_num_rows += i;
            }
            EXPECT_GE(total_num_rows, num_rows_read);
        }
    }
}

TEST_F(SegCompactionTest, CreateSegCompactionWriter) {
    config::enable_segcompaction = true;
    Status s;
    TabletSchemaSPtr tablet_schema = std::make_shared<TabletSchema>();
    TabletSchemaPB schema_pb;
    schema_pb.set_keys_type(KeysType::DUP_KEYS);
    schema_pb.set_inverted_index_storage_format(InvertedIndexStorageFormatPB::V2);

    construct_column(schema_pb.add_column(), schema_pb.add_index(), 10000, "key_index", 0, "INT",
                     "key");
    construct_column(schema_pb.add_column(), schema_pb.add_index(), 10001, "v1_index", 1, "STRING",
                     "v1");
    construct_column(schema_pb.add_column(), schema_pb.add_index(), 10002, "v2_index", 2, "STRING",
                     "v2", true);
    construct_column(schema_pb.add_column(), schema_pb.add_index(), 10003, "v3_index", 3, "INT",
                     "v3");

    tablet_schema.reset(new TabletSchema);
    tablet_schema->init_from_pb(schema_pb);
    RowsetSharedPtr rowset;
    config::segcompaction_candidate_max_rows = 6000; // set threshold above
    // rows_per_segment
    config::segcompaction_batch_size = 3;
    std::vector<uint32_t> segment_num_rows;
    {
        RowsetWriterContext writer_context;
        create_rowset_writer_context(10052, tablet_schema, &writer_context);

        auto res = RowsetFactory::create_rowset_writer(*l_engine, writer_context, false);
        EXPECT_TRUE(res.has_value()) << res.error();
        auto rowset_writer = std::move(res).value();
        EXPECT_EQ(Status::OK(), s);
        auto beta_rowset_writer = dynamic_cast<BetaRowsetWriter*>(rowset_writer.get());
        EXPECT_TRUE(beta_rowset_writer != nullptr);
        std::unique_ptr<segment_v2::SegmentWriter> writer = nullptr;
        auto status = beta_rowset_writer->create_segment_writer_for_segcompaction(&writer, 0, 1);
        EXPECT_TRUE(beta_rowset_writer != nullptr);
        EXPECT_TRUE(status == Status::OK());
        int64_t inverted_index_file_size = 0;
        status = writer->close_inverted_index(&inverted_index_file_size);
        EXPECT_TRUE(status == Status::OK());
        std::cout << inverted_index_file_size << std::endl;
    }
}

TEST_F(SegCompactionTest, SegCompactionThenReadAggTableSmall) {
    config::enable_segcompaction = true;
    Status s;
    TabletSchemaSPtr tablet_schema = std::make_shared<TabletSchema>();
    create_tablet_schema(tablet_schema, AGG_KEYS);

    RowsetSharedPtr rowset;
    config::segcompaction_candidate_max_rows = 6000; // set threshold above
                                                     // rows_per_segment
    config::segcompaction_batch_size = 3;
    std::vector<uint32_t> segment_num_rows;
    { // write `num_segments * rows_per_segment` rows to rowset
        RowsetWriterContext writer_context;
        create_rowset_writer_context(10052, tablet_schema, &writer_context);

        auto res = RowsetFactory::create_rowset_writer(*l_engine, writer_context, false);
        EXPECT_TRUE(res.has_value()) << res.error();
        auto rowset_writer = std::move(res).value();
        EXPECT_EQ(Status::OK(), s);

        uint32_t k1 = 0;
        uint32_t k2 = 0;
        uint32_t k3 = 0;

        vectorized::Block block = tablet_schema->create_block();
        auto columns = block.mutate_columns();

        // segment#0
        k1 = k2 = 1;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 4;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 6;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);
        // segment#1
        k1 = k2 = 2;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 4;
        k3 = 2;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 6;
        k3 = 2;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#2
        k1 = k2 = 3;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 6;
        k3 = 3;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 9;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#3
        k1 = k2 = 4;
        k3 = 3;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 9;
        k3 = 2;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        k1 = k2 = 12;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#4
        k1 = k2 = 25;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        // segment#5
        k1 = k2 = 26;
        k3 = 1;
        columns[0]->insert_data((const char*)&k1, sizeof(k1));
        columns[1]->insert_data((const char*)&k2, sizeof(k2));
        columns[2]->insert_data((const char*)&k3, sizeof(k3));

        s = rowset_writer->add_block(&block);
        EXPECT_TRUE(s.ok());
        s = rowset_writer->flush();
        EXPECT_EQ(Status::OK(), s);
        sleep(1);

        EXPECT_EQ(Status::OK(), rowset_writer->build(rowset));
        std::vector<std::string> ls;
        ls.push_back("10052_0.dat");
        ls.push_back("10052_1.dat");
        ls.push_back("10052_2.dat");
        ls.push_back("10052_3.dat");
        ls.push_back("10052_0.idx");
        ls.push_back("10052_1.idx");
        ls.push_back("10052_2.idx");
        ls.push_back("10052_3.idx");
        EXPECT_TRUE(check_dir(ls));
    }

    { // read
        RowsetReaderContext reader_context;
        reader_context.tablet_schema = tablet_schema;
        // use this type to avoid cache from other ut
        reader_context.reader_type = ReaderType::READER_CUMULATIVE_COMPACTION;
        reader_context.need_ordered_result = true;
        std::vector<uint32_t> return_columns = {0, 1, 2};
        reader_context.return_columns = &return_columns;
        reader_context.stats = &_stats;
        // reader_context.is_unique = true;

        // without predicates
        {
            RowsetReaderSharedPtr rowset_reader;
            create_and_init_rowset_reader(rowset.get(), reader_context, &rowset_reader);

            uint32_t num_rows_read = 0;
            bool eof = false;
            while (!eof) {
                std::shared_ptr<vectorized::Block> output_block =
                        std::make_shared<vectorized::Block>(
                                tablet_schema->create_block(return_columns));
                s = rowset_reader->next_block(output_block.get());
                if (s != Status::OK()) {
                    eof = true;
                }
                EXPECT_GT(output_block->rows(), 0);
                EXPECT_EQ(return_columns.size(), output_block->columns());
                for (int i = 0; i < output_block->rows(); ++i) {
                    vectorized::ColumnPtr col0 = output_block->get_by_position(0).column;
                    vectorized::ColumnPtr col1 = output_block->get_by_position(1).column;
                    vectorized::ColumnPtr col2 = output_block->get_by_position(2).column;
                    auto field1 = (*col0)[i];
                    auto field2 = (*col1)[i];
                    auto field3 = (*col2)[i];
                    uint32_t k1 = *reinterpret_cast<uint32_t*>((char*)(&field1));
                    uint32_t k2 = *reinterpret_cast<uint32_t*>((char*)(&field2));
                    uint32_t v3 = *reinterpret_cast<uint32_t*>((char*)(&field3));
                    // dup keys may exist between segments, but not in single segment
                    std::cout << "k1 k2 k3: " << k1 << " " << k2 << " " << v3 << std::endl;
                    num_rows_read++;
                }
                output_block->clear();
            }
            EXPECT_EQ(Status::Error<END_OF_FILE>(""), s);
            // duplicated keys between segments are counted duplicately
            // so actual read by rowset reader is less or equal to it
            EXPECT_GE(rowset->rowset_meta()->num_rows(), num_rows_read);
            EXPECT_TRUE(rowset_reader->get_segment_num_rows(&segment_num_rows).ok());
            size_t total_num_rows = 0;
            for (const auto& i : segment_num_rows) {
                total_num_rows += i;
            }
            EXPECT_GE(total_num_rows, num_rows_read);
        }
    }
}

} // namespace doris

// @brief Test Stub
