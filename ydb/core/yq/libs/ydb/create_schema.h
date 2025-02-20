#pragma once
#include <ydb/core/yq/libs/ydb/ydb.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/retry/retry_policy.h>

namespace NYq {

using TYdbSdkRetryPolicy = IRetryPolicy<const NYdb::TStatus&>;

// Actor that creates table.
// Send TEvSchemaCreated to parent (if any).
NActors::IActor* MakeCreateTableActor(
    NActors::TActorId parent,
    ui64 logComponent,
    TYdbConnectionPtr connection,
    const TString& tablePath,
    const NYdb::NTable::TTableDescription& tableDesc,
    TYdbSdkRetryPolicy::TPtr,
    ui64 cookie = 0);

// Actor that creates directory.
// Send TEvSchemaCreated to parent (if any).
NActors::IActor* MakeCreateDirectoryActor(
    NActors::TActorId parent,
    ui64 logComponent,
    TYdbConnectionPtr connection,
    const TString& directoryPath,
    TYdbSdkRetryPolicy::TPtr retryPolicy,
    ui64 cookie = 0);

} // namespace NYq
