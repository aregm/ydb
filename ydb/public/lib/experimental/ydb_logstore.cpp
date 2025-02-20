#include "ydb_logstore.h"

#define INCLUDE_YDB_INTERNAL_H
#include <ydb/public/sdk/cpp/client/impl/ydb_internal/make_request/make.h>
#include <ydb/public/sdk/cpp/client/impl/ydb_internal/table_helpers/helpers.h>
#undef INCLUDE_YDB_INTERNAL_H

#include <ydb/public/api/grpc/draft/ydb_logstore_v1.grpc.pb.h>
#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>
#include <ydb/public/sdk/cpp/client/ydb_common_client/impl/client.h>

#include <ydb/library/yql/public/issue/yql_issue.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>

namespace NYdb {
namespace NLogStore {

TMaybe<TTtlSettings> TtlSettingsFromProto(const Ydb::Table::TtlSettings& proto) {
    switch (proto.mode_case()) {
    case Ydb::Table::TtlSettings::kDateTypeColumn:
        return TTtlSettings(
            proto.date_type_column(),
            proto.run_interval_seconds()
        );

    case Ydb::Table::TtlSettings::kValueSinceUnixEpoch:
        return TTtlSettings(
            proto.value_since_unix_epoch(),
            proto.run_interval_seconds()
        );

    default:
        break;
    }
    return {};
}

TType MakeColumnType(EPrimitiveType primitiveType) {
    return TTypeBuilder().BeginOptional().Primitive(primitiveType).EndOptional().Build();
}

TSchema::TSchema(const Ydb::LogStore::Schema& schema)
    : Columns()
    , PrimaryKeyColumns(schema.primary_key().begin(), schema.primary_key().end())
{
    Columns.reserve(schema.columns().size());
    for (const auto& col : schema.columns()) {
        TColumn c(col.name(), TType(col.type()));
        Columns.emplace_back(std::move(c));
    }
}

void TSchema::SerializeTo(Ydb::LogStore::Schema& schema) const {
    for (const auto& c : Columns) {
        auto& col = *schema.add_columns();
        col.set_name(c.Name);
        col.mutable_type()->CopyFrom(TProtoAccessor::GetProto(c.Type));
    }
    for (const auto& pkc : PrimaryKeyColumns) {
        schema.add_primary_key(pkc);
    }
}

TLogStoreDescription::TLogStoreDescription(ui32 columnShardCount, const THashMap<TString, TSchema>& schemaPresets)
    : ColumnShardCount(columnShardCount)
    , SchemaPresets(schemaPresets)
{}

TLogStoreDescription::TLogStoreDescription(Ydb::LogStore::DescribeLogStoreResult&& desc, const TDescribeLogStoreSettings& describeSettings)
    : ColumnShardCount(desc.column_shard_count())
    , SchemaPresets()
    , Owner(desc.self().owner())
{
    Y_UNUSED(describeSettings);
    for (const auto& sp : desc.schema_presets()) {
        SchemaPresets[sp.name()] = TSchema(sp.schema());
    }
    PermissionToSchemeEntry(desc.self().permissions(), &Permissions);
    PermissionToSchemeEntry(desc.self().effective_permissions(), &EffectivePermissions);
}

void TLogStoreDescription::SerializeTo(Ydb::LogStore::CreateLogStoreRequest& request) const {
    for (const auto& sp : SchemaPresets) {
        auto& pb = *request.add_schema_presets();
        pb.set_name(sp.first);
        sp.second.SerializeTo(*pb.mutable_schema());
    }
    request.set_column_shard_count(ColumnShardCount);
}

TDescribeLogStoreResult::TDescribeLogStoreResult(TStatus&& status, Ydb::LogStore::DescribeLogStoreResult&& desc,
    const TDescribeLogStoreSettings& describeSettings)
    : TStatus(std::move(status))
    , LogStoreDescription_(std::move(desc), describeSettings)
{}


TLogTableDescription::TLogTableDescription(const TString& schemaPresetName, const TVector<TString>& shardingColumns,
    ui32 columnShardCount, const TMaybe<TTtlSettings>& ttlSettings)
    : SchemaPresetName(schemaPresetName)
    , ShardingColumns(shardingColumns)
    , ColumnShardCount(columnShardCount)
    , TtlSettings(ttlSettings)
{}

TLogTableDescription::TLogTableDescription(const TSchema& schema, const TVector<TString>& shardingColumns,
    ui32 columnShardCount, const TMaybe<TTtlSettings>& ttlSettings)
    : Schema(schema)
    , ShardingColumns(shardingColumns)
    , ColumnShardCount(columnShardCount)
    , TtlSettings(ttlSettings)
{}

TLogTableDescription::TLogTableDescription(Ydb::LogStore::DescribeLogTableResult&& desc, const TDescribeLogTableSettings& describeSettings)
    : Schema(desc.schema())
    , ShardingColumns(desc.sharding_columns().begin(), desc.sharding_columns().end())
    , ColumnShardCount(desc.column_shard_count())
    , TtlSettings(TtlSettingsFromProto(desc.ttl_settings()))
    , Owner(desc.self().owner())
{
    Y_UNUSED(describeSettings);
    PermissionToSchemeEntry(desc.self().permissions(), &Permissions);
    PermissionToSchemeEntry(desc.self().effective_permissions(), &EffectivePermissions);
}

void TLogTableDescription::SerializeTo(Ydb::LogStore::CreateLogTableRequest& request) const {
    if (!Schema.GetColumns().empty()) {
        Schema.SerializeTo(*request.mutable_schema());
    }
    request.set_schema_preset_name(SchemaPresetName);
    request.set_column_shard_count(ColumnShardCount);
    for (const auto& sc : ShardingColumns) {
        request.add_sharding_columns(sc);
    }
    if (TtlSettings) {
        TtlSettings->SerializeTo(*request.mutable_ttl_settings());
    }
}

TDescribeLogTableResult::TDescribeLogTableResult(TStatus&& status, Ydb::LogStore::DescribeLogTableResult&& desc,
    const TDescribeLogTableSettings& describeSettings)
    : TStatus(std::move(status))
    , LogTableDescription_(std::move(desc), describeSettings)
{}

class TLogStoreClient::TImpl: public TClientImplCommon<TLogStoreClient::TImpl> {
public:
    TImpl(std::shared_ptr<TGRpcConnectionsImpl>&& connections, const TCommonClientSettings& settings)
        : TClientImplCommon(std::move(connections), settings)
    {}

    TAsyncStatus CreateLogStore(const TString& path, TLogStoreDescription&& storeDesc,
        const TCreateLogStoreSettings& settings)
    {
        auto request = MakeOperationRequest<Ydb::LogStore::CreateLogStoreRequest>(settings);
        storeDesc.SerializeTo(request);
        request.set_path(path);
        return RunSimple<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::CreateLogStoreRequest,
            Ydb::LogStore::CreateLogStoreResponse>(
            std::move(request),
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncCreateLogStore,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);
    }

    TAsyncDescribeLogStoreResult DescribeLogStore(const TString& path, const TDescribeLogStoreSettings& settings) {
        auto request = MakeOperationRequest<Ydb::LogStore::DescribeLogStoreRequest>(settings);
        request.set_path(path);

        auto promise = NThreading::NewPromise<TDescribeLogStoreResult>();

        auto extractor = [promise, settings]
            (google::protobuf::Any* any, TPlainStatus status) mutable {
                Ydb::LogStore::DescribeLogStoreResult result;
                if (any) {
                    any->UnpackTo(&result);
                }
                TDescribeLogStoreResult describeLogStoreResult(TStatus(std::move(status)),
                    std::move(result), settings);
                promise.SetValue(std::move(describeLogStoreResult));
            };

        Connections_->RunDeferred<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::DescribeLogStoreRequest,
            Ydb::LogStore::DescribeLogStoreResponse>(
            std::move(request),
            extractor,
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncDescribeLogStore,
            DbDriverState_,
            INITIAL_DEFERRED_CALL_DELAY,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);

        return promise.GetFuture();
    }

    TAsyncStatus DropLogStore(const TString& path, const TDropLogStoreSettings& settings) {
        auto request = MakeOperationRequest<Ydb::LogStore::DropLogStoreRequest>(settings);
        request.set_path(path);
        return RunSimple<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::DropLogStoreRequest,
            Ydb::LogStore::DropLogStoreResponse>(
            std::move(request),
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncDropLogStore,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);
    }

    TAsyncStatus CreateLogTable(const TString& path, TLogTableDescription&& tableDesc,
        const TCreateLogTableSettings& settings)
    {
        auto request = MakeOperationRequest<Ydb::LogStore::CreateLogTableRequest>(settings);
        tableDesc.SerializeTo(request);
        request.set_path(path);
        return RunSimple<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::CreateLogTableRequest,
            Ydb::LogStore::CreateLogTableResponse>(
            std::move(request),
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncCreateLogTable,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);
    }

    TAsyncDescribeLogTableResult DescribeLogTable(const TString& path, const TDescribeLogTableSettings& settings) {
        auto request = MakeOperationRequest<Ydb::LogStore::DescribeLogTableRequest>(settings);
        request.set_path(path);

        auto promise = NThreading::NewPromise<TDescribeLogTableResult>();

        auto extractor = [promise, settings]
            (google::protobuf::Any* any, TPlainStatus status) mutable {
                Ydb::LogStore::DescribeLogTableResult result;
                if (any) {
                    any->UnpackTo(&result);
                }
                TDescribeLogTableResult describeLogTableResult(TStatus(std::move(status)),
                    std::move(result), settings);
                promise.SetValue(std::move(describeLogTableResult));
            };

        Connections_->RunDeferred<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::DescribeLogTableRequest,
            Ydb::LogStore::DescribeLogTableResponse>(
            std::move(request),
            extractor,
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncDescribeLogTable,
            DbDriverState_,
            INITIAL_DEFERRED_CALL_DELAY,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);

        return promise.GetFuture();
    }

    TAsyncStatus DropLogTable(const TString& path, const TDropLogTableSettings& settings) {
        auto request = MakeOperationRequest<Ydb::LogStore::DropLogTableRequest>(settings);
        request.set_path(path);
        return RunSimple<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::DropLogTableRequest,
            Ydb::LogStore::DropLogTableResponse>(
            std::move(request),
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncDropLogTable,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);
    }

    TAsyncStatus AlterLogTable(const TString& path, const TAlterLogTableSettings& settings) {
        auto request = MakeOperationRequest<Ydb::LogStore::AlterLogTableRequest>(settings);
        request.set_path(path);
        if (const auto& ttl = settings.GetAlterTtlSettings()) {
            switch (ttl->GetAction()) {
            case TAlterTtlSettings::EAction::Set:
                ttl->GetTtlSettings().SerializeTo(*request.mutable_set_ttl_settings());
                break;
            case TAlterTtlSettings::EAction::Drop:
                request.mutable_drop_ttl_settings();
                break;
            }
        }
        return RunSimple<
            Ydb::LogStore::V1::LogStoreService,
            Ydb::LogStore::AlterLogTableRequest,
            Ydb::LogStore::AlterLogTableResponse>(
            std::move(request),
            &Ydb::LogStore::V1::LogStoreService::Stub::AsyncAlterLogTable,
            TRpcRequestSettings::Make(settings),
            settings.ClientTimeout_);
    }
};

TLogStoreClient::TLogStoreClient(const TDriver& driver, const TCommonClientSettings& settings)
    : Impl_(new TImpl(CreateInternalInterface(driver), settings))
{}

TAsyncStatus TLogStoreClient::CreateLogStore(const TString& path, TLogStoreDescription&& storeDesc,
        const TCreateLogStoreSettings& settings)
{
    return Impl_->CreateLogStore(path, std::move(storeDesc), settings);
}

TAsyncDescribeLogStoreResult TLogStoreClient::DescribeLogStore(const TString& path, const TDescribeLogStoreSettings& settings)
{
    return Impl_->DescribeLogStore(path, settings);
}

TAsyncStatus TLogStoreClient::DropLogStore(const TString& path, const TDropLogStoreSettings& settings)
{
    return Impl_->DropLogStore(path, settings);
}

TAsyncStatus TLogStoreClient::CreateLogTable(const TString& path, TLogTableDescription&& storeDesc,
        const TCreateLogTableSettings& settings)
{
    return Impl_->CreateLogTable(path, std::move(storeDesc), settings);
}

TAsyncDescribeLogTableResult TLogStoreClient::DescribeLogTable(const TString& path, const TDescribeLogTableSettings& settings)
{
    return Impl_->DescribeLogTable(path, settings);
}

TAsyncStatus TLogStoreClient::DropLogTable(const TString& path, const TDropLogTableSettings& settings)
{
    return Impl_->DropLogTable(path, settings);
}

TAsyncStatus TLogStoreClient::AlterLogTable(const TString& path, const TAlterLogTableSettings& settings)
{
    return Impl_->AlterLogTable(path, settings);
}

TAlterLogTableSettings& TAlterLogTableSettings::AlterTtlSettings(const TMaybe<TAlterTtlSettings>& value) {
    AlterTtlSettings_ = value;
    return *this;
}

const TMaybe<TAlterTtlSettings>& TAlterLogTableSettings::GetAlterTtlSettings() const {
    return AlterTtlSettings_;
}

}}
