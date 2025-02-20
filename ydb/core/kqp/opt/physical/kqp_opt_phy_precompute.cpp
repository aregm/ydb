#include "kqp_opt_phy_rules.h"
#include "kqp_opt_phy_impl.h"

// Set of rules to avoid additional query phase (physical tx) for results computations on
// already precomputed inputs.
// This is important for simple OLTP queries where single additional phase can significantly increase
// total CPU usage of the query.
// Following rules propagate precompute over specific simple callables.
// We have to do it in additional rules because currently there is no support for DqCnValue inputs
// in stages, so we cannot express this as general callable-to-stage pushdown.

namespace NKikimr::NKqp::NOpt {

using namespace NYql;
using namespace NYql::NDq;
using namespace NYql::NNodes;

TExprBase KqpPropagatePrecomuteScalarRowset(TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage)
{
    if (!node.Maybe<TCoAsList>()) {
        return node;
    }

    auto asList = node.Cast<TCoAsList>();

    TMaybeNode<TDqPhyPrecompute> precompute;
    bool visitResult = true;
    for (const auto& listItem : asList) {
        if (!listItem.Maybe<TCoAsStruct>()) {
            return node;
        }

        if (!visitResult) {
            break;
        }

        auto asStruct = listItem.Cast<TCoAsStruct>();

        VisitExpr(asStruct.Ptr(), [&](const TExprNode::TPtr& exprPtr) {
            TExprBase expr{exprPtr};

            if (expr.Maybe<TDqConnection>()) {
                visitResult = false;
                return false;
            }

            if (auto maybePrecompute = expr.Maybe<TDqPhyPrecompute>()) {
                if (precompute && precompute.Cast().Raw() != maybePrecompute.Cast().Raw()) {
                    visitResult = false;
                    return false;
                }

                if (!maybePrecompute.Cast().Connection().Maybe<TDqCnValue>()) {
                    visitResult = false;
                    return false;
                }

                precompute = maybePrecompute;
                return false;
            }

            if (auto maybeCallable = expr.Maybe<TCallable>()) {
                auto consumers = GetConsumers(maybeCallable.Cast(), parentsMap);
                if (consumers.size() > 1) {
                    visitResult = false;
                    return false;
                }
            }

            return true;
        });
    }

    if (!visitResult || !precompute) {
        return node;
    }

    if (!IsSingleConsumerConnection(precompute.Cast().Connection(), parentsMap, allowStageMultiUsage)) {
        return node;
    }

    auto processLambda = Build<TCoLambda>(ctx, node.Pos())
        .Args({"stream"})
        .Body<TCoMap>()
            .Input("stream")
            .Lambda()
                .Args({"stream_item"})
                .Body<TExprApplier>()
                    .Apply(asList)
                    .With(precompute.Cast(), "stream_item")
                    .Build()
                .Build()
            .Build()
        .Done();

    auto result = DqPushLambdaToStageUnionAll(precompute.Cast().Connection(), processLambda, {}, ctx, optCtx);
    if (!result) {
        return node;
    }

    return Build<TDqPhyPrecompute>(ctx, node.Pos())
        .Connection(result.Cast())
        .Done();
}

TExprBase KqpPropagatePrecomuteTake(TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage)
{
    if (!node.Maybe<TCoTake>().Input().Maybe<TDqPhyPrecompute>()) {
        return node;
    }

    auto take = node.Cast<TCoTake>();
    auto precompute = take.Input().Cast<TDqPhyPrecompute>();

    if (!IsSingleConsumerConnection(precompute.Connection(), parentsMap, allowStageMultiUsage)) {
        return node;
    }

    if (!CanPushDqExpr(take.Count(), precompute.Connection())) {
        return node;
    }

    auto takeLambda = Build<TCoLambda>(ctx, node.Pos())
        .Args({"list_stream"})
        .Body<TCoMap>()
            .Input("list_stream")
            .Lambda()
                .Args({"list"})
                .Body<TCoTake>()
                    .Input("list")
                    .Count(take.Count())
                    .Build()
                .Build()
            .Build()
        .Done();

    auto result = DqPushLambdaToStageUnionAll(precompute.Connection(), takeLambda, {}, ctx, optCtx);
    if (!result) {
        return node;
    }

    return Build<TDqPhyPrecompute>(ctx, node.Pos())
        .Connection(result.Cast())
        .Done();
}

TExprBase KqpPropagatePrecomuteFlatmap(TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage)
{
    if (!node.Maybe<TCoFlatMap>().Input().Maybe<TDqPhyPrecompute>()) {
        return node;
    }

    auto flatmap = node.Cast<TCoFlatMap>();
    auto precompute = flatmap.Input().Cast<TDqPhyPrecompute>();

    if (!IsSingleConsumerConnection(precompute.Connection(), parentsMap, allowStageMultiUsage)) {
        return node;
    }

    if (!CanPushDqExpr(flatmap.Lambda(), precompute.Connection())) {
        return node;
    }

    auto flatmapLambda = Build<TCoLambda>(ctx, node.Pos())
        .Args({"list_stream"})
        .Body<TCoMap>()
            .Input("list_stream")
            .Lambda()
                .Args({"list"})
                .Body<TCoFlatMap>()
                    .Input("list")
                    .Lambda(flatmap.Lambda())
                    .Build()
                .Build()
            .Build()
        .Done();

    auto result = DqPushLambdaToStageUnionAll(precompute.Connection(), flatmapLambda, {}, ctx, optCtx);
    if (!result) {
        return node;
    }

    return Build<TDqPhyPrecompute>(ctx, node.Pos())
        .Connection(result.Cast())
        .Done();
}

} // namespace NKikimr::NKqp::NOpt

