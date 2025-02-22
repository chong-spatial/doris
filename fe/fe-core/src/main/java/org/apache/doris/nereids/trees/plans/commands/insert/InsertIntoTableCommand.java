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

package org.apache.doris.nereids.trees.plans.commands.insert;

import org.apache.doris.analysis.RedirectStatus;
import org.apache.doris.analysis.StmtType;
import org.apache.doris.catalog.Column;
import org.apache.doris.catalog.Env;
import org.apache.doris.catalog.OlapTable;
import org.apache.doris.catalog.TableIf;
import org.apache.doris.common.ErrorCode;
import org.apache.doris.common.ErrorReport;
import org.apache.doris.common.profile.ProfileManager.ProfileType;
import org.apache.doris.common.util.DebugUtil;
import org.apache.doris.datasource.hive.HMSExternalTable;
import org.apache.doris.datasource.iceberg.IcebergExternalTable;
import org.apache.doris.datasource.jdbc.JdbcExternalTable;
import org.apache.doris.load.loadv2.LoadStatistic;
import org.apache.doris.mysql.privilege.PrivPredicate;
import org.apache.doris.nereids.NereidsPlanner;
import org.apache.doris.nereids.analyzer.UnboundTableSink;
import org.apache.doris.nereids.exceptions.AnalysisException;
import org.apache.doris.nereids.glue.LogicalPlanAdapter;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.Explainable;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.PlanType;
import org.apache.doris.nereids.trees.plans.commands.Command;
import org.apache.doris.nereids.trees.plans.commands.ForwardWithSync;
import org.apache.doris.nereids.trees.plans.logical.LogicalPlan;
import org.apache.doris.nereids.trees.plans.physical.PhysicalEmptyRelation;
import org.apache.doris.nereids.trees.plans.physical.PhysicalHiveTableSink;
import org.apache.doris.nereids.trees.plans.physical.PhysicalIcebergTableSink;
import org.apache.doris.nereids.trees.plans.physical.PhysicalJdbcTableSink;
import org.apache.doris.nereids.trees.plans.physical.PhysicalOlapTableSink;
import org.apache.doris.nereids.trees.plans.physical.PhysicalOneRowRelation;
import org.apache.doris.nereids.trees.plans.physical.PhysicalSink;
import org.apache.doris.nereids.trees.plans.physical.PhysicalUnion;
import org.apache.doris.nereids.trees.plans.visitor.PlanVisitor;
import org.apache.doris.nereids.util.RelationUtil;
import org.apache.doris.planner.DataSink;
import org.apache.doris.qe.ConnectContext;
import org.apache.doris.qe.ConnectContext.ConnectType;
import org.apache.doris.qe.StmtExecutor;

import com.google.common.base.Preconditions;
import com.google.common.base.Throwables;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.List;
import java.util.Objects;
import java.util.Optional;

/**
 * insert into select command implementation
 * insert into select command support the grammar: explain? insert into table columns? partitions? hints? query
 * InsertIntoTableCommand is a command to represent insert the answer of a query into a table.
 * class structure's:
 * InsertIntoTableCommand(Query())
 * ExplainCommand(Query())
 */
public class InsertIntoTableCommand extends Command implements ForwardWithSync, Explainable {

    public static final Logger LOG = LogManager.getLogger(InsertIntoTableCommand.class);

    private LogicalPlan originalLogicalQuery;
    private LogicalPlan logicalQuery;
    private Optional<String> labelName;
    /**
     * When source it's from job scheduler,it will be set.
     */
    private long jobId;
    private final Optional<InsertCommandContext> insertCtx;
    private final Optional<LogicalPlan> cte;

    /**
     * constructor
     */
    public InsertIntoTableCommand(LogicalPlan logicalQuery, Optional<String> labelName,
                                  Optional<InsertCommandContext> insertCtx, Optional<LogicalPlan> cte) {
        super(PlanType.INSERT_INTO_TABLE_COMMAND);
        this.originalLogicalQuery = Objects.requireNonNull(logicalQuery, "logicalQuery should not be null");
        this.logicalQuery = originalLogicalQuery;
        this.labelName = Objects.requireNonNull(labelName, "labelName should not be null");
        this.insertCtx = insertCtx;
        this.cte = cte;
    }

    public LogicalPlan getLogicalQuery() {
        return logicalQuery;
    }

    public Optional<String> getLabelName() {
        return labelName;
    }

    public void setLabelName(Optional<String> labelName) {
        this.labelName = labelName;
    }

    public void setJobId(long jobId) {
        this.jobId = jobId;
    }

    @Override
    public void run(ConnectContext ctx, StmtExecutor executor) throws Exception {
        runInternal(ctx, executor);
    }

    public void runWithUpdateInfo(ConnectContext ctx, StmtExecutor executor,
                                  LoadStatistic loadStatistic) throws Exception {
        // TODO: add coordinator statistic
        runInternal(ctx, executor);
    }

    public AbstractInsertExecutor initPlan(ConnectContext ctx, StmtExecutor executor) throws Exception {
        return initPlan(ctx, executor, true);
    }

    /**
     * This function is used to generate the plan for Nereids.
     * There are some load functions that only need to the plan, such as stream_load.
     * Therefore, this section will be presented separately.
     * @param needBeginTransaction whether to start a transaction.
     *       For external uses such as creating a job, only basic analysis is needed without starting a transaction,
     *       in which case this can be set to false.
     */
    public AbstractInsertExecutor initPlan(ConnectContext ctx, StmtExecutor executor,
                                           boolean needBeginTransaction) throws Exception {
        List<String> qualifiedTargetTableName = InsertUtils.getTargetTableQualified(logicalQuery, ctx);
        AbstractInsertExecutor insertExecutor;
        int retryTimes = 0;
        while (++retryTimes < Math.max(ctx.getSessionVariable().dmlPlanRetryTimes, 3)) {
            TableIf targetTableIf = RelationUtil.getTable(qualifiedTargetTableName, ctx.getEnv());
            // check auth
            if (!Env.getCurrentEnv().getAccessManager()
                    .checkTblPriv(ConnectContext.get(), targetTableIf.getDatabase().getCatalog().getName(),
                            targetTableIf.getDatabase().getFullName(), targetTableIf.getName(),
                            PrivPredicate.LOAD)) {
                ErrorReport.reportAnalysisException(ErrorCode.ERR_TABLEACCESS_DENIED_ERROR, "LOAD",
                        ConnectContext.get().getQualifiedUser(), ConnectContext.get().getRemoteIP(),
                        targetTableIf.getDatabase().getFullName() + "." + targetTableIf.getName());
            }
            BuildInsertExecutorResult buildResult;
            try {
                buildResult = initPlanOnce(ctx, executor, targetTableIf);
            } catch (Throwable e) {
                Throwables.throwIfInstanceOf(e, RuntimeException.class);
                throw new IllegalStateException(e.getMessage(), e);
            }
            insertExecutor = buildResult.executor;
            if (!needBeginTransaction) {
                return insertExecutor;
            }
            // lock after plan and check does table's schema changed to ensure we lock table order by id.
            TableIf newestTargetTableIf = RelationUtil.getTable(qualifiedTargetTableName, ctx.getEnv());
            newestTargetTableIf.readLock();
            try {
                if (targetTableIf.getId() != newestTargetTableIf.getId()) {
                    LOG.warn("insert plan failed {} times. query id is {}. table id changed from {} to {}",
                            retryTimes, DebugUtil.printId(ctx.queryId()),
                            targetTableIf.getId(), newestTargetTableIf.getId());
                    continue;
                }
                if (!targetTableIf.getFullSchema().equals(newestTargetTableIf.getFullSchema())) {
                    LOG.warn("insert plan failed {} times. query id is {}. table schema changed from {} to {}",
                            retryTimes, DebugUtil.printId(ctx.queryId()),
                            targetTableIf.getFullSchema(), newestTargetTableIf.getFullSchema());
                    continue;
                }
                if (!insertExecutor.isEmptyInsert()) {
                    insertExecutor.beginTransaction();
                    insertExecutor.finalizeSink(
                            buildResult.planner.getFragments().get(0), buildResult.dataSink,
                            buildResult.physicalSink
                    );
                }
                newestTargetTableIf.readUnlock();
            } catch (Throwable e) {
                newestTargetTableIf.readUnlock();
                // the abortTxn in onFail need to acquire table write lock
                if (insertExecutor != null) {
                    insertExecutor.onFail(e);
                }
                Throwables.throwIfInstanceOf(e, RuntimeException.class);
                throw new IllegalStateException(e.getMessage(), e);
            }
            executor.setProfileType(ProfileType.LOAD);
            // We exposed @StmtExecutor#cancel as a unified entry point for statement interruption,
            // so we need to set this here
            insertExecutor.getCoordinator().setTxnId(insertExecutor.getTxnId());
            executor.setCoord(insertExecutor.getCoordinator());
            // for prepare and execute, avoiding normalization for every execute command
            this.originalLogicalQuery = this.logicalQuery;
            return insertExecutor;
        }
        LOG.warn("insert plan failed {} times. query id is {}.", retryTimes, DebugUtil.printId(ctx.queryId()));
        throw new AnalysisException("Insert plan failed. Could not get target table lock.");
    }

    private BuildInsertExecutorResult initPlanOnce(ConnectContext ctx,
            StmtExecutor executor, TableIf targetTableIf) throws Throwable {
        AbstractInsertExecutor insertExecutor;
        // 1. process inline table (default values, empty values)
        this.logicalQuery = (LogicalPlan) InsertUtils.normalizePlan(logicalQuery, targetTableIf, insertCtx);
        if (cte.isPresent()) {
            this.logicalQuery = ((LogicalPlan) cte.get().withChildren(logicalQuery));
        }
        OlapGroupCommitInsertExecutor.analyzeGroupCommit(ctx, targetTableIf, this.logicalQuery, this.insertCtx);
        LogicalPlanAdapter logicalPlanAdapter = new LogicalPlanAdapter(logicalQuery, ctx.getStatementContext());
        NereidsPlanner planner = new NereidsPlanner(ctx.getStatementContext());
        planner.plan(logicalPlanAdapter, ctx.getSessionVariable().toThrift());
        executor.setPlanner(planner);
        executor.checkBlockRules();
        if (ctx.getConnectType() == ConnectType.MYSQL && ctx.getMysqlChannel() != null) {
            ctx.getMysqlChannel().reset();
        }
        Optional<PhysicalSink<?>> plan = (planner.getPhysicalPlan()
                .<PhysicalSink<?>>collect(PhysicalSink.class::isInstance)).stream()
                .findAny();
        Preconditions.checkArgument(plan.isPresent(), "insert into command must contain target table");
        PhysicalSink physicalSink = plan.get();
        DataSink sink = planner.getFragments().get(0).getSink();
        // Transaction insert should reuse the label in the transaction.
        String label = this.labelName.orElse(
                ctx.isTxnModel() ? null : String.format("label_%x_%x", ctx.queryId().hi, ctx.queryId().lo));

        if (physicalSink instanceof PhysicalOlapTableSink) {
            boolean emptyInsert = childIsEmptyRelation(physicalSink);
            OlapTable olapTable = (OlapTable) targetTableIf;
            // the insertCtx contains some variables to adjust SinkNode
            if (ctx.isTxnModel()) {
                insertExecutor = new OlapTxnInsertExecutor(ctx, olapTable, label, planner, insertCtx, emptyInsert);
            } else if (ctx.isGroupCommit()) {
                insertExecutor = new OlapGroupCommitInsertExecutor(ctx, olapTable, label, planner, insertCtx,
                        emptyInsert);
            } else {
                insertExecutor = new OlapInsertExecutor(ctx, olapTable, label, planner, insertCtx, emptyInsert);
            }

            boolean isEnableMemtableOnSinkNode =
                    olapTable.getTableProperty().getUseSchemaLightChange()
                            ? insertExecutor.getCoordinator().getQueryOptions().isEnableMemtableOnSinkNode()
                            : false;
            insertExecutor.getCoordinator().getQueryOptions()
                    .setEnableMemtableOnSinkNode(isEnableMemtableOnSinkNode);
        } else if (physicalSink instanceof PhysicalHiveTableSink) {
            boolean emptyInsert = childIsEmptyRelation(physicalSink);
            HMSExternalTable hiveExternalTable = (HMSExternalTable) targetTableIf;
            insertExecutor = new HiveInsertExecutor(ctx, hiveExternalTable, label, planner,
                    Optional.of(insertCtx.orElse((new HiveInsertCommandContext()))), emptyInsert);
            // set hive query options
        } else if (physicalSink instanceof PhysicalIcebergTableSink) {
            boolean emptyInsert = childIsEmptyRelation(physicalSink);
            IcebergExternalTable icebergExternalTable = (IcebergExternalTable) targetTableIf;
            insertExecutor = new IcebergInsertExecutor(ctx, icebergExternalTable, label, planner,
                    Optional.of(insertCtx.orElse((new BaseExternalTableInsertCommandContext()))), emptyInsert);
        } else if (physicalSink instanceof PhysicalJdbcTableSink) {
            boolean emptyInsert = childIsEmptyRelation(physicalSink);
            List<Column> cols = ((PhysicalJdbcTableSink<?>) physicalSink).getCols();
            List<Slot> slots = ((PhysicalJdbcTableSink<?>) physicalSink).getOutput();
            if (physicalSink.children().size() == 1) {
                if (physicalSink.child(0) instanceof PhysicalOneRowRelation
                        || physicalSink.child(0) instanceof PhysicalUnion) {
                    for (int i = 0; i < cols.size(); i++) {
                        if (!(cols.get(i).isAllowNull()) && slots.get(i).nullable()) {
                            throw new AnalysisException("Column `" + cols.get(i).getName()
                                    + "` is not nullable, but the inserted value is nullable.");
                        }
                    }
                }
            }
            JdbcExternalTable jdbcExternalTable = (JdbcExternalTable) targetTableIf;
            insertExecutor = new JdbcInsertExecutor(ctx, jdbcExternalTable, label, planner,
                    Optional.of(insertCtx.orElse((new JdbcInsertCommandContext()))), emptyInsert);
        } else {
            // TODO: support other table types
            throw new AnalysisException("insert into command only support [olap, hive, iceberg, jdbc] table");
        }
        return new BuildInsertExecutorResult(planner, insertExecutor, sink, physicalSink);
    }

    private void runInternal(ConnectContext ctx, StmtExecutor executor) throws Exception {
        AbstractInsertExecutor insertExecutor = initPlan(ctx, executor);
        // if the insert stmt data source is empty, directly return, no need to be executed.
        if (insertExecutor.isEmptyInsert()) {
            return;
        }
        insertExecutor.executeSingleInsert(executor, jobId);
    }

    public boolean isExternalTableSink() {
        return !(logicalQuery instanceof UnboundTableSink);
    }

    @Override
    public Plan getExplainPlan(ConnectContext ctx) {
        return InsertUtils.getPlanForExplain(ctx, this.logicalQuery);
    }

    @Override
    public <R, C> R accept(PlanVisitor<R, C> visitor, C context) {
        return visitor.visitInsertIntoTableCommand(this, context);
    }

    private boolean childIsEmptyRelation(PhysicalSink sink) {
        if (sink.children() != null && sink.children().size() == 1
                && sink.child(0) instanceof PhysicalEmptyRelation) {
            return true;
        }
        return false;
    }

    @Override
    public StmtType stmtType() {
        return StmtType.INSERT;
    }

    @Override
    public RedirectStatus toRedirectStatus() {
        if (ConnectContext.get().isGroupCommit()) {
            return RedirectStatus.NO_FORWARD;
        } else {
            return RedirectStatus.FORWARD_WITH_SYNC;
        }
    }

    private static class BuildInsertExecutorResult {
        private final NereidsPlanner planner;
        private final AbstractInsertExecutor executor;
        private final DataSink dataSink;
        private final PhysicalSink<?> physicalSink;

        public BuildInsertExecutorResult(NereidsPlanner planner, AbstractInsertExecutor executor, DataSink dataSink,
                PhysicalSink<?> physicalSink) {
            this.planner = planner;
            this.executor = executor;
            this.dataSink = dataSink;
            this.physicalSink = physicalSink;
        }
    }
}
