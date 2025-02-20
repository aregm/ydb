#pragma once

#include "schema.h"

#include <ydb/core/protos/sys_view.pb.h>
#include <ydb/core/protos/counters_sysview_processor.pb.h>

#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/sys_view/common/common.h>
#include <ydb/core/sys_view/common/events.h>
#include <ydb/core/sys_view/common/db_counters.h>
#include <ydb/core/sys_view/service/query_interval.h>
#include <ydb/core/tablet_flat/tablet_flat_executed.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/tx.h>

#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/memory_track.h>

namespace NKikimr {
namespace NSysView {

static constexpr char MemoryLabelResults[] = "SysViewProcessor/Results";

class TSysViewProcessor : public TActor<TSysViewProcessor>, public NTabletFlatExecutor::TTabletExecutedFlat {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::SYSTEM_VIEW_PROCESSOR;
    }

    TSysViewProcessor(const TActorId& tablet, TTabletStorageInfo* info, EProcessorMode processorMode);

private:
    using Schema = TProcessorSchema;
    using TTxBase = NTabletFlatExecutor::TTransactionBase<TSysViewProcessor>;

    struct TTxInitSchema;
    struct TTxInit;
    struct TTxConfigure;
    struct TTxCollect;
    struct TTxAggregate;
    struct TTxIntervalSummary;
    struct TTxIntervalMetrics;

    struct TEvPrivate {
        enum EEv {
            EvCollect = EventSpaceBegin(TEvents::ES_PRIVATE),
            EvAggregate,
            EvSendRequests,
            EvProcess,
            EvApplyCounters,
            EvSendNavigate,
            EvEnd
        };

        struct TEvCollect : public TEventLocal<TEvCollect, EvCollect> {};

        struct TEvAggregate : public TEventLocal<TEvAggregate, EvAggregate> {};

        struct TEvSendRequests : public TEventLocal<TEvSendRequests, EvSendRequests> {};

        struct TEvProcess : public TEventLocal<TEvProcess, EvProcess> {};

        struct TEvApplyCounters : public TEventLocal<TEvApplyCounters, EvApplyCounters> {};

        struct TEvSendNavigate : public TEventLocal<TEvSendNavigate, EvSendNavigate> {};
    };

    struct TTopQuery {
        TQueryHash Hash;
        ui64 Value;
        TNodeId NodeId;
        THolder<NKikimrSysView::TQueryStats> Stats;
    };
    using TTop = std::vector<TTopQuery>;

    using TResultKey = std::pair<ui64, ui32>;

    template <typename TEntry>
    using TResultMap = std::map<TResultKey, TEntry, std::less<TResultKey>,
        NActors::NMemory::TAlloc<std::pair<const TResultKey, TEntry>, MemoryLabelResults>>;
    using TResultStatsMap = TResultMap<NKikimrSysView::TQueryStats>;

private:
    static bool TopQueryCompare(const TTopQuery& l, const TTopQuery& r) {
        return l.Value == r.Value ? l.Hash > r.Hash : l.Value > r.Value;
    }

    void OnDetach(const TActorContext& ctx) override;
    void OnTabletDead(TEvTablet::TEvTabletDead::TPtr& ev, const TActorContext& ctx) override;
    void OnActivateExecutor(const TActorContext& ctx) override;
    void DefaultSignalTabletActive(const TActorContext& ctx) override;
    bool OnRenderAppHtmlPage(NMon::TEvRemoteHttpInfo::TPtr ev, const TActorContext &ctx) override;

    NTabletFlatExecutor::ITransaction* CreateTxInitSchema();
    NTabletFlatExecutor::ITransaction* CreateTxInit();

    void Handle(TEvSysView::TEvConfigureProcessor::TPtr& ev);
    void Handle(TEvPrivate::TEvCollect::TPtr& ev);
    void Handle(TEvPrivate::TEvAggregate::TPtr& ev);
    void Handle(TEvPrivate::TEvSendRequests::TPtr& ev);
    void Handle(TEvPrivate::TEvProcess::TPtr& ev);
    void Handle(TEvSysView::TEvIntervalQuerySummary::TPtr& ev);
    void Handle(TEvSysView::TEvGetIntervalMetricsResponse::TPtr& ev);
    void Handle(TEvSysView::TEvGetQueryMetricsRequest::TPtr& ev);

    void Handle(TEvSysView::TEvSendDbCountersRequest::TPtr& ev);
    void Handle(TEvPrivate::TEvApplyCounters::TPtr& ev);
    void Handle(TEvPrivate::TEvSendNavigate::TPtr& ev);
    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev);
    void Handle(TEvTxProxySchemeCache::TEvWatchNotifyUpdated::TPtr& ev);
    void Handle(TEvTxProxySchemeCache::TEvWatchNotifyDeleted::TPtr& ev);

    void Handle(TEvents::TEvPoisonPill::TPtr& ev);
    void Handle(TEvents::TEvUndelivered::TPtr& ev);
    void Handle(TEvInterconnect::TEvNodeDisconnected::TPtr& ev);

    void PersistSysParam(NIceDb::TNiceDb& db, ui64 id, const TString& value);
    void PersistDatabase(NIceDb::TNiceDb& db);
    void PersistStage(NIceDb::TNiceDb& db);
    void PersistIntervalEnd(NIceDb::TNiceDb& db);

    template <typename TSchema>
    void PersistTopResults(NIceDb::TNiceDb& db,
        TTop& top, TResultStatsMap& results, TInstant intervalEnd);

    void PersistResults(NIceDb::TNiceDb& db);

    void ScheduleAggregate();
    void ScheduleCollect();
    void ScheduleSendRequests();
    void ScheduleApplyCounters();
    void ScheduleSendNavigate();

    template <typename TSchema, typename TEntry>
    void CutHistory(NIceDb::TNiceDb& db, TResultMap<TEntry>& results,
        TDuration historySize);

    static TInstant EndOfHourInterval(TInstant intervalEnd);

    void ClearIntervalSummaries(NIceDb::TNiceDb& db);

    void Reset(NIceDb::TNiceDb& db, const TActorContext& ctx);

    void SendRequests();
    void IgnoreFailure(TNodeId nodeId);

    template <typename TResponse>
    void ReplyOverloaded(TEvSysView::TEvGetQueryMetricsRequest::TPtr& ev);

    template <typename TEntry, typename TResponse>
    void Reply(TEvSysView::TEvGetQueryMetricsRequest::TPtr& ev);

    TIntrusivePtr<IDbCounters> CreateCountersForService(NKikimrSysView::EDbCountersService service);
    void AttachExternalCounters();
    void AttachInternalCounters();
    void DetachExternalCounters();
    void DetachInternalCounters();
    void SendNavigate();

    STFUNC(StateInit) {
        switch(ev->GetTypeRewrite()) {
            hFunc(TEvSysView::TEvConfigureProcessor, Handle);
            hFunc(TEvents::TEvPoisonPill, Handle);
            IgnoreFunc(TEvSysView::TEvIntervalQuerySummary);
            IgnoreFunc(TEvSysView::TEvGetIntervalMetricsResponse);
            IgnoreFunc(TEvSysView::TEvGetQueryMetricsRequest);
            IgnoreFunc(TEvSysView::TEvSendDbCountersRequest);
            default:
                if (!HandleDefaultEvents(ev, ctx)) {
                    LOG_CRIT(ctx, NKikimrServices::SYSTEM_VIEWS,
                        "TSysViewProcessor StateInit unexpected event 0x%08" PRIx32, ev->GetTypeRewrite());
                }
        }
    }

    STFUNC(StateOffline) {
        switch(ev->GetTypeRewrite()) {
            hFunc(TEvents::TEvPoisonPill, Handle);
            hFunc(TEvSysView::TEvConfigureProcessor, Handle);
            IgnoreFunc(TEvSysView::TEvIntervalQuerySummary);
            IgnoreFunc(TEvSysView::TEvGetIntervalMetricsResponse);
            IgnoreFunc(TEvSysView::TEvGetQueryMetricsRequest);
            IgnoreFunc(TEvSysView::TEvSendDbCountersRequest);
            default:
                if (!HandleDefaultEvents(ev, ctx)) {
                    LOG_CRIT(ctx, NKikimrServices::SYSTEM_VIEWS,
                        "TSysViewProcessor StateOffline unexpected event 0x%08" PRIx32, ev->GetTypeRewrite());
                }
        }
    }

    STFUNC(StateWork) {
        switch(ev->GetTypeRewrite()) {
            hFunc(TEvSysView::TEvConfigureProcessor, Handle);
            hFunc(TEvPrivate::TEvCollect, Handle);
            hFunc(TEvPrivate::TEvAggregate, Handle);
            hFunc(TEvPrivate::TEvSendRequests, Handle);
            hFunc(TEvPrivate::TEvProcess, Handle);
            hFunc(TEvSysView::TEvIntervalQuerySummary, Handle);
            hFunc(TEvSysView::TEvGetIntervalMetricsResponse, Handle);
            hFunc(TEvSysView::TEvGetQueryMetricsRequest, Handle);
            hFunc(TEvSysView::TEvSendDbCountersRequest, Handle);
            hFunc(TEvPrivate::TEvApplyCounters, Handle);
            hFunc(TEvPrivate::TEvSendNavigate, Handle);
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            hFunc(TEvTxProxySchemeCache::TEvWatchNotifyUpdated, Handle);
            hFunc(TEvTxProxySchemeCache::TEvWatchNotifyDeleted, Handle);
            hFunc(TEvents::TEvPoisonPill, Handle);
            hFunc(TEvents::TEvUndelivered, Handle);
            hFunc(TEvInterconnect::TEvNodeDisconnected, Handle);
            IgnoreFunc(TEvInterconnect::TEvNodeConnected);
            IgnoreFunc(TEvTabletPipe::TEvServerConnected);
            IgnoreFunc(TEvTabletPipe::TEvServerDisconnected);
            default:
                if (!HandleDefaultEvents(ev, ctx)) {
                    LOG_CRIT(ctx, NKikimrServices::SYSTEM_VIEWS,
                        "TSysViewProcessor StateWork unexpected event 0x%08" PRIx32, ev->GetTypeRewrite());
                }
        }
    }

    STFUNC(StateBroken) {
        HandleDefaultEvents(ev, ctx);
    }

private:
    // limit on number of distinct queries when gathering summaries
    static constexpr size_t DistinctQueriesLimit = 1024;
    // limit on number of queries to aggregate metrics
    static constexpr size_t TopCountLimit = 256;
    // limit on number of concurrent metrics requests from services
    static constexpr size_t MaxInFlightRequests = 16;
    // limit on scan batch size
    static constexpr size_t BatchSizeLimit = 4 << 20;
    // interval of db counters processing
    static constexpr TDuration ProcessCountersInterval = TDuration::Seconds(5);
    // interval of sending next navigate request
    static constexpr TDuration SendNavigateInterval = TDuration::Seconds(5);

    const TDuration TotalInterval;
    const TDuration CollectInterval;

    TString Database;

    TInstant IntervalEnd;

    enum EStage {
        COLLECT,
        AGGREGATE
    };
    EStage CurrentStage = COLLECT;

    // IntervalSummaries
    struct TQueryToNodes {
        ui64 Cpu = 0;
        std::vector<std::pair<TNodeId, ui64>> Nodes; // nodeId, cpu
    };
    std::unordered_map<TQueryHash, TQueryToNodes> Queries;
    std::multimap<ui64, TQueryHash> ByCpu;
    std::unordered_set<TNodeId> SummaryNodes;

    // IntervalMetrics
    struct TQueryToMetrics {
        NKikimrSysView::TQueryMetrics Metrics;
        TString Text;
    };
    std::unordered_map<TQueryHash, TQueryToMetrics> QueryMetrics;

    // NodesToRequest
    using THashVector = std::vector<TQueryHash>;
    struct TNodeToQueries {
        TNodeId NodeId = 0;
        THashVector Hashes;
        THashVector TextsToGet;
        THashVector ByDuration;
        THashVector ByReadBytes;
        THashVector ByCpuTime;
        THashVector ByRequestUnits;
    };
    std::vector<TNodeToQueries> NodesToRequest;
    std::unordered_map<TNodeId, TNodeToQueries> NodesInFlight;

    // IntervalTops
    TTop ByDurationMinute;
    TTop ByReadBytesMinute;
    TTop ByCpuTimeMinute;
    TTop ByRequestUnitsMinute;
    TTop ByDurationHour;
    TTop ByReadBytesHour;
    TTop ByCpuTimeHour;
    TTop ByRequestUnitsHour;

    // Metrics...
    using TResultMetricsMap = TResultMap<TQueryToMetrics>;

    TResultMetricsMap MetricsOneMinute;
    TResultMetricsMap MetricsOneHour;

    // TopBy...
    TResultStatsMap TopByDurationOneMinute;
    TResultStatsMap TopByDurationOneHour;
    TResultStatsMap TopByReadBytesOneMinute;
    TResultStatsMap TopByReadBytesOneHour;
    TResultStatsMap TopByCpuTimeOneMinute;
    TResultStatsMap TopByCpuTimeOneHour;
    TResultStatsMap TopByRequestUnitsOneMinute;
    TResultStatsMap TopByRequestUnitsOneHour;

    // limited queue of user requests
    static constexpr size_t PendingRequestsLimit = 5;
    std::queue<TEvSysView::TEvGetQueryMetricsRequest::TPtr> PendingRequests;
    bool ProcessInFly = false;

    // db counters
    TString CloudId;
    TString FolderId;
    TString DatabaseId;

    NMonitoring::TDynamicCounterPtr ExternalGroup;
    NMonitoring::TDynamicCounterPtr InternalGroup;

    using TDbCountersServiceMap = std::unordered_map<NKikimrSysView::EDbCountersService,
        NKikimr::NSysView::TDbServiceCounters>;

    struct TNodeCountersState {
        TDbCountersServiceMap Simple; // only simple counters
        ui64 Generation = 0;
        size_t FreshCount = 0;
    };
    std::unordered_map<TNodeId, TNodeCountersState> NodeCountersStates;
    TDbCountersServiceMap AggregatedCountersState;

    std::unordered_map<NKikimrSysView::EDbCountersService, TIntrusivePtr<IDbCounters>> Counters;
};

} // NSysView
} // NKikimr

