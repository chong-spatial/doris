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

package org.apache.doris.nereids.rules.analysis;

import org.apache.doris.catalog.Type;
import org.apache.doris.nereids.exceptions.AnalysisException;
import org.apache.doris.nereids.properties.OrderKey;
import org.apache.doris.nereids.rules.Rule;
import org.apache.doris.nereids.rules.RuleType;
import org.apache.doris.nereids.trees.expressions.Alias;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.Match;
import org.apache.doris.nereids.trees.expressions.NamedExpression;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.expressions.SlotNotFromChildren;
import org.apache.doris.nereids.trees.expressions.SubqueryExpr;
import org.apache.doris.nereids.trees.expressions.VirtualSlotReference;
import org.apache.doris.nereids.trees.expressions.WindowExpression;
import org.apache.doris.nereids.trees.expressions.functions.ExpressionTrait;
import org.apache.doris.nereids.trees.expressions.functions.agg.AggregateFunction;
import org.apache.doris.nereids.trees.expressions.functions.generator.TableGeneratingFunction;
import org.apache.doris.nereids.trees.expressions.functions.scalar.GroupingScalarFunction;
import org.apache.doris.nereids.trees.expressions.functions.window.WindowFunction;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.algebra.Generate;
import org.apache.doris.nereids.trees.plans.logical.LogicalAggregate;
import org.apache.doris.nereids.trees.plans.logical.LogicalDeferMaterializeOlapScan;
import org.apache.doris.nereids.trees.plans.logical.LogicalFilter;
import org.apache.doris.nereids.trees.plans.logical.LogicalJoin;
import org.apache.doris.nereids.trees.plans.logical.LogicalOlapScan;
import org.apache.doris.nereids.trees.plans.logical.LogicalSort;
import org.apache.doris.nereids.trees.plans.logical.LogicalTopN;
import org.apache.doris.nereids.trees.plans.logical.LogicalWindow;

import com.google.common.collect.ImmutableSet;
import org.apache.commons.lang3.StringUtils;
import org.roaringbitmap.RoaringBitmap;

import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;

/**
 * some check need to do after analyze whole plan.
 */
public class CheckAfterRewrite extends OneAnalysisRuleFactory {

    @Override
    public Rule build() {
        return any().then(plan -> {
            checkAllSlotReferenceFromChildren(plan);
            checkUnexpectedExpression(plan);
            checkMetricTypeIsUsedCorrectly(plan);
            checkMatchIsUsedCorrectly(plan);
            return null;
        }).toRule(RuleType.CHECK_ANALYSIS);
    }

    private void checkUnexpectedExpression(Plan plan) {
        boolean isGenerate = plan instanceof Generate;
        boolean isAgg = plan instanceof LogicalAggregate;
        boolean isWindow = plan instanceof LogicalWindow;
        boolean notAggAndWindow = !isAgg && !isWindow;

        for (Expression expression : plan.getExpressions()) {
            expression.foreach(expr -> {
                if (expr instanceof SubqueryExpr) {
                    throw new AnalysisException("Subquery is not allowed in " + plan.getType());
                } else if (!isGenerate && expr instanceof TableGeneratingFunction) {
                    throw new AnalysisException("table generating function is not allowed in " + plan.getType());
                } else if (notAggAndWindow && expr instanceof AggregateFunction) {
                    throw new AnalysisException("aggregate function is not allowed in " + plan.getType());
                } else if (!isAgg && expr instanceof GroupingScalarFunction) {
                    throw new AnalysisException("grouping scalar function is not allowed in " + plan.getType());
                } else if (!isWindow && (expr instanceof WindowExpression || expr instanceof WindowFunction)) {
                    throw new AnalysisException("analytic function is not allowed in " + plan.getType());
                }
            });
        }
    }

    private void checkAllSlotReferenceFromChildren(Plan plan) {
        Set<Slot> inputSlots = plan.getInputSlots();
        RoaringBitmap childrenOutput = plan.getChildrenOutputExprIdBitSet();

        ImmutableSet.Builder<Slot> notFromChildrenBuilder = ImmutableSet.builderWithExpectedSize(inputSlots.size());
        for (Slot inputSlot : inputSlots) {
            if (!childrenOutput.contains(inputSlot.getExprId().asInt())) {
                notFromChildrenBuilder.add(inputSlot);
            }
        }
        Set<Slot> notFromChildren = notFromChildrenBuilder.build();
        if (notFromChildren.isEmpty()) {
            return;
        }
        notFromChildren = removeValidSlotsNotFromChildren(notFromChildren, childrenOutput);
        if (!notFromChildren.isEmpty()) {
            if (plan.arity() != 0 && plan.child(0) instanceof LogicalAggregate) {
                throw new AnalysisException(String.format("%s not in aggregate's output", notFromChildren
                        .stream().map(NamedExpression::getName).collect(Collectors.joining(", "))));
            } else {
                throw new AnalysisException(String.format(
                        "Input slot(s) not in child's output: %s in plan: %s\nchild output is: %s\nplan tree:\n%s",
                        StringUtils.join(notFromChildren.stream().map(ExpressionTrait::toString)
                                .collect(Collectors.toSet()), ", "),
                        plan,
                        plan.children().stream()
                                .flatMap(child -> child.getOutput().stream())
                                .collect(Collectors.toSet()),
                        plan.treeString()));
            }
        }
    }

    private Set<Slot> removeValidSlotsNotFromChildren(Set<Slot> slots, RoaringBitmap childrenOutput) {
        return slots.stream()
                .filter(expr -> {
                    if (expr instanceof VirtualSlotReference) {
                        List<Expression> realExpressions = ((VirtualSlotReference) expr).getRealExpressions();
                        if (realExpressions.isEmpty()) {
                            // valid
                            return false;
                        }
                        return realExpressions.stream()
                                .map(Expression::getInputSlots)
                                .flatMap(Set::stream)
                                .anyMatch(realUsedExpr -> !childrenOutput.contains(realUsedExpr.getExprId().asInt()));
                    } else {
                        return !(expr instanceof SlotNotFromChildren);
                    }
                })
                .collect(Collectors.toSet());
    }

    private void checkMetricTypeIsUsedCorrectly(Plan plan) {
        if (plan instanceof LogicalAggregate) {
            LogicalAggregate<?> agg = (LogicalAggregate<?>) plan;
            for (Expression groupBy : agg.getGroupByExpressions()) {
                if (groupBy.getDataType().isOnlyMetricType()
                        && !groupBy.getDataType().isArrayTypeNestedBaseType()) {
                    throw new AnalysisException(Type.OnlyMetricTypeErrorMsg);
                }
            }
        } else if (plan instanceof LogicalSort) {
            LogicalSort<?> sort = (LogicalSort<?>) plan;
            for (OrderKey orderKey : sort.getOrderKeys()) {
                if (orderKey.getExpr().getDataType().isOnlyMetricType()
                        && !orderKey.getExpr().getDataType().isArrayType()) {
                    throw new AnalysisException(Type.OnlyMetricTypeErrorMsg);
                }
            }
        } else if (plan instanceof LogicalTopN) {
            LogicalTopN<?> topN = (LogicalTopN<?>) plan;
            for (OrderKey orderKey : topN.getOrderKeys()) {
                if (orderKey.getExpr().getDataType().isOnlyMetricType()
                        && !orderKey.getExpr().getDataType().isArrayType()) {
                    throw new AnalysisException(Type.OnlyMetricTypeErrorMsg);
                }
            }
        } else if (plan instanceof LogicalWindow) {
            ((LogicalWindow<?>) plan).getWindowExpressions().forEach(a -> {
                if (!(a instanceof Alias && ((Alias) a).child() instanceof WindowExpression)) {
                    return;
                }
                WindowExpression windowExpression = (WindowExpression) ((Alias) a).child();
                if (windowExpression.getOrderKeys().stream().anyMatch((
                        orderKey -> orderKey.getDataType().isOnlyMetricType()
                                && !orderKey.getDataType().isArrayType()))) {
                    throw new AnalysisException(Type.OnlyMetricTypeErrorMsg);
                }
                if (windowExpression.getPartitionKeys().stream().anyMatch((
                        partitionKey -> partitionKey.getDataType().isOnlyMetricType()
                                && !partitionKey.getDataType().isArrayTypeNestedBaseType()))) {
                    throw new AnalysisException(Type.OnlyMetricTypeErrorMsg);
                }
            });
        } else if (plan instanceof LogicalJoin) {
            LogicalJoin<?, ?> join = (LogicalJoin<?, ?>) plan;
            for (Expression conjunct : join.getHashJoinConjuncts()) {
                if (conjunct.anyMatch(e -> ((Expression) e).getDataType().isVariantType())) {
                    throw new AnalysisException("variant type could not in join equal conditions: " + conjunct.toSql());
                }
            }
            for (Expression conjunct : join.getMarkJoinConjuncts()) {
                if (conjunct.anyMatch(e -> ((Expression) e).getDataType().isVariantType())) {
                    throw new AnalysisException("variant type could not in join equal conditions: " + conjunct.toSql());
                }
            }
        }
    }

    private void checkMatchIsUsedCorrectly(Plan plan) {
        for (Expression expression : plan.getExpressions()) {
            if (expression instanceof Match) {
                if (plan instanceof LogicalFilter && (plan.child(0) instanceof LogicalOlapScan
                        || plan.child(0) instanceof LogicalDeferMaterializeOlapScan)) {
                    return;
                } else {
                    throw new AnalysisException(String.format(
                            "Not support match in %s in plan: %s, only support in olapScan filter",
                            plan.child(0), plan));
                }
            }
        }
    }
}
