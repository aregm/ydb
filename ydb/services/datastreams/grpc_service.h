#pragma once

#include <library/cpp/actors/core/actorsystem.h>
#include <library/cpp/grpc/server/grpc_server.h>
#include <ydb/public/api/grpc/draft/ydb_datastreams_v1.grpc.pb.h>

namespace NKikimr::NGRpcService {

    class TGRpcDataStreamsService : public NGrpc::TGrpcServiceBase<Ydb::DataStreams::V1::DataStreamsService>
    {
    public:
        TGRpcDataStreamsService(NActors::TActorSystem* system,
                        TIntrusivePtr<NMonitoring::TDynamicCounters> counters,
                        NActors::TActorId id);

        void InitService(grpc::ServerCompletionQueue* cq, NGrpc::TLoggerPtr logger) override;
        void SetGlobalLimiterHandle(NGrpc::TGlobalLimiter* limiter) override;

        bool IncRequest();
        void DecRequest();

    private:
        void SetupIncomingRequests(NGrpc::TLoggerPtr logger);
        void InitNewSchemeCache();

        NActors::TActorSystem* ActorSystem_;
        grpc::ServerCompletionQueue* CQ_ = nullptr;

        TIntrusivePtr<NMonitoring::TDynamicCounters> Counters_;
        NActors::TActorId GRpcRequestProxyId_;
        NActors::TActorId NewSchemeCache;
        NGrpc::TGlobalLimiter* Limiter_ = nullptr;
    };

}
