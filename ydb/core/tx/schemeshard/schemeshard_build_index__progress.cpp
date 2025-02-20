#include "schemeshard_build_index.h"
#include "schemeshard_impl.h"
#include "schemeshard_build_index_helpers.h"
#include "schemeshard_build_index_tx_base.h"

#include <ydb/core/tx/datashard/range_ops.h>

#include <ydb/public/api/protos/ydb_issue_message.pb.h>
#include <ydb/public/api/protos/ydb_status_codes.pb.h>

#include <ydb/library/yql/public/issue/yql_issue_message.h>


namespace NKikimr {
namespace NSchemeShard {

NKikimrSchemeOp::TIndexBuildConfig GetInitiateIndexBuildMessage(TSchemeShard* ss, const TIndexBuildInfo::TPtr buildInfo) {
    NKikimrSchemeOp::TIndexBuildConfig message;

    message.SetTable(TPath::Init(buildInfo->TablePathId, ss).PathString());
    auto& index = *message.MutableIndex();
    index.SetName(buildInfo->IndexName);
    for (const auto& x: buildInfo->IndexColumns) {
        *index.AddKeyColumnNames() = x;
    }
    for (const auto& x: buildInfo->DataColumns) {
        *index.AddDataColumnNames() = x;
    }
    index.SetType(buildInfo->IndexType);

    return message;
}

THolder<TEvSchemeShard::TEvModifySchemeTransaction> LockPropose(
    TSchemeShard* ss, const TIndexBuildInfo::TPtr buildInfo)
{
    auto propose = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>(ui64(buildInfo->LockTxId), ss->TabletID());
    propose->Record.SetFailOnExist(false);

    NKikimrSchemeOp::TModifyScheme& modifyScheme = *propose->Record.AddTransaction();
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateLockForIndexBuild);
    modifyScheme.SetInternal(true);

    TPath path = TPath::Init(buildInfo->TablePathId, ss);
    modifyScheme.SetWorkingDir(path.Parent().PathString());

    auto& lockConfig = *modifyScheme.MutableLockConfig();
    lockConfig.SetName(path.LeafName());

    *modifyScheme.MutableInitiateIndexBuild() = GetInitiateIndexBuildMessage(ss, buildInfo);

    return propose;
}

THolder<TEvSchemeShard::TEvModifySchemeTransaction> InitiatePropose(
    TSchemeShard* ss, const TIndexBuildInfo::TPtr buildInfo)
{
    auto propose = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>(ui64(buildInfo->InitiateTxId), ss->TabletID());
    propose->Record.SetFailOnExist(true);

    NKikimrSchemeOp::TModifyScheme& modifyScheme = *propose->Record.AddTransaction();
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateIndexBuild);
    modifyScheme.SetInternal(true);

    modifyScheme.SetWorkingDir(TPath::Init(buildInfo->DomainPathId, ss).PathString());

    modifyScheme.MutableLockGuard()->SetOwnerTxId(ui64(buildInfo->LockTxId));

    *modifyScheme.MutableInitiateIndexBuild() = GetInitiateIndexBuildMessage(ss, buildInfo);

    return propose;
}

THolder<TEvSchemeShard::TEvModifySchemeTransaction> ApplyPropose(
    TSchemeShard* ss, const TIndexBuildInfo::TPtr buildInfo)
{
    auto propose = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>(ui64(buildInfo->ApplyTxId), ss->TabletID());
    propose->Record.SetFailOnExist(true);

    NKikimrSchemeOp::TModifyScheme& modifyScheme = *propose->Record.AddTransaction();
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpApplyIndexBuild);
    modifyScheme.SetInternal(true);

    modifyScheme.SetWorkingDir(TPath::Init(buildInfo->DomainPathId, ss).PathString());

    modifyScheme.MutableLockGuard()->SetOwnerTxId(ui64(buildInfo->LockTxId));

    auto& indexBuild = *modifyScheme.MutableApplyIndexBuild();
    indexBuild.SetTablePath(TPath::Init(buildInfo->TablePathId, ss).PathString());
    indexBuild.SetIndexName(buildInfo->IndexName);
    indexBuild.SetSnaphotTxId(ui64(buildInfo->InitiateTxId));
    indexBuild.SetBuildIndexId(ui64(buildInfo->Id));

    return propose;
}

THolder<TEvSchemeShard::TEvModifySchemeTransaction> UnlockPropose(
    TSchemeShard* ss, const TIndexBuildInfo::TPtr buildInfo)
{
    auto propose = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>(ui64(buildInfo->UnlockTxId), ss->TabletID());
    propose->Record.SetFailOnExist(true);

    NKikimrSchemeOp::TModifyScheme& modifyScheme = *propose->Record.AddTransaction();
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpDropLock);
    modifyScheme.SetInternal(true);

    modifyScheme.MutableLockGuard()->SetOwnerTxId(ui64(buildInfo->LockTxId));

    TPath path = TPath::Init(buildInfo->TablePathId, ss);
    modifyScheme.SetWorkingDir(path.Parent().PathString());

    auto& lockConfig = *modifyScheme.MutableLockConfig();
    lockConfig.SetName(path.LeafName());

    return propose;
}

THolder<TEvSchemeShard::TEvModifySchemeTransaction> CancelPropose(
    TSchemeShard* ss, const TIndexBuildInfo::TPtr buildInfo)
{
    auto propose = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>(ui64(buildInfo->ApplyTxId), ss->TabletID());
    propose->Record.SetFailOnExist(true);

    NKikimrSchemeOp::TModifyScheme& modifyScheme = *propose->Record.AddTransaction();
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCancelIndexBuild);
    modifyScheme.SetInternal(true);

    modifyScheme.SetWorkingDir(TPath::Init(buildInfo->DomainPathId, ss).PathString());

    modifyScheme.MutableLockGuard()->SetOwnerTxId(ui64(buildInfo->LockTxId));

    auto& indexBuild = *modifyScheme.MutableCancelIndexBuild();
    indexBuild.SetTablePath(TPath::Init(buildInfo->TablePathId, ss).PathString());
    indexBuild.SetIndexName(buildInfo->IndexName);
    indexBuild.SetSnaphotTxId(ui64(buildInfo->InitiateTxId));
    indexBuild.SetBuildIndexId(ui64(buildInfo->Id));

    return propose;
}

using namespace NTabletFlatExecutor;

struct TSchemeShard::TIndexBuilder::TTxProgress: public TSchemeShard::TIndexBuilder::TTxBase  {
private:
    TIndexBuildId BuildId;

    TDeque<std::tuple<TTabletId, ui64, THolder<IEventBase>>> ToTabletSend;

public:
    explicit TTxProgress(TSelf* self, TIndexBuildId id)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , BuildId(id)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_PROGRESS_INDEX_BUILD;
    }

    bool DoExecute(TTransactionContext& txc, const TActorContext& ctx) override {
        Y_VERIFY(Self->IndexBuilds.contains(BuildId));
        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(BuildId);

        LOG_I("TTxBuildProgress: Resume"
              << ": id# " << BuildId);
        LOG_D("TTxBuildProgress: Resume"
              << ": " << *buildInfo);

        switch (buildInfo->State) {
        case TIndexBuildInfo::EState::Invalid:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Locking:
            if (buildInfo->LockTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->LockTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), LockPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->LockTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->LockTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::GatheringStatistics);
                Progress(BuildId);
            }

            break;
        case TIndexBuildInfo::EState::GatheringStatistics:
            ChangeState(BuildId, TIndexBuildInfo::EState::Initiating);
            Progress(BuildId);
            break;
        case TIndexBuildInfo::EState::Initiating:
            if (buildInfo->InitiateTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->InitiateTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), InitiatePropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->InitiateTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->InitiateTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Filling);
                Progress(BuildId);
            }

            break;
        case TIndexBuildInfo::EState::Filling:
            if (buildInfo->IsCancellationRequested()) {
                buildInfo->DoneShards.clear();
                buildInfo->InProgressShards.clear();

                Self->IndexBuildPipes.CloseAll(BuildId, ctx);

                ChangeState(BuildId, TIndexBuildInfo::EState::Cancellation_Applying);
                Progress(BuildId);

                // make final bill
                Bill(buildInfo);

                break;
            }

            if (buildInfo->Shards.empty()) {
                NIceDb::TNiceDb db(txc.DB);
                InitiateShards(db, buildInfo);
            }

            if (buildInfo->ToUploadShards.empty()
                && buildInfo->DoneShards.empty()
                && buildInfo->InProgressShards.empty())
            {
                for (const auto& item: buildInfo->Shards) {
                    const TIndexBuildInfo::TShardStatus& shardStatus = item.second;
                    switch (shardStatus.Status) {
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::INVALID:
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::ACCEPTED:
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::INPROGRESS:
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::ABORTED:
                        buildInfo->ToUploadShards.push_back(item.first);
                        break;
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::DONE:
                        buildInfo->DoneShards.insert(item.first);
                        break;
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::BUILD_ERROR:
                    case NKikimrTxDataShard::TEvBuildIndexProgressResponse::BAD_REQUEST:
                        Y_FAIL("Unreachable");
                        break;
                    }
                }
            }

            if (!buildInfo->SnapshotTxId || !buildInfo->SnapshotStep) {
                Y_VERIFY(Self->TablesWithSnaphots.contains(buildInfo->TablePathId));
                Y_VERIFY(Self->TablesWithSnaphots.at(buildInfo->TablePathId) == buildInfo->InitiateTxId);

                buildInfo->SnapshotTxId = buildInfo->InitiateTxId;
                Y_VERIFY(buildInfo->SnapshotTxId);
                buildInfo->SnapshotStep = Self->SnapshotsStepIds.at(buildInfo->SnapshotTxId);
                Y_VERIFY(buildInfo->SnapshotStep);
            }

            if (buildInfo->ImplTablePath.Empty()) {
                TPath implTable = TPath::Init(buildInfo->TablePathId, Self).Dive(buildInfo->IndexName).Dive("indexImplTable");
                buildInfo->ImplTablePath = implTable.PathString();

                TTableInfo::TPtr imptTableInfo = Self->Tables.at(implTable.Base()->PathId);
                buildInfo->ImplTableColumns = NTableIndex::ExtractInfo(imptTableInfo);
            }

            while (!buildInfo->ToUploadShards.empty()
                   && buildInfo->InProgressShards.size() < buildInfo->Limits.MaxShards)
            {
                TShardIdx shardIdx = buildInfo->ToUploadShards.front();
                buildInfo->ToUploadShards.pop_front();
                buildInfo->InProgressShards.insert(shardIdx);

                auto ev = MakeHolder<TEvDataShard::TEvBuildIndexCreateRequest>();
                ev->Record.SetBuildIndexId(ui64(BuildId));

                TTabletId shardId = Self->ShardInfos.at(shardIdx).TabletID;
                ev->Record.SetTabletId(ui64(shardId));

                ev->Record.SetOwnerId(buildInfo->TablePathId.OwnerId);
                ev->Record.SetPathId(buildInfo->TablePathId.LocalPathId);

                ev->Record.SetTargetName(buildInfo->ImplTablePath);

                THashSet<TString> columns = buildInfo->ImplTableColumns.Columns;
                for (const auto& x: buildInfo->ImplTableColumns.Keys) {
                    *ev->Record.AddIndexColumns() = x;
                    columns.erase(x);
                }
                for (const auto& x: columns) {
                    *ev->Record.AddDataColumns() = x;
                }

                TIndexBuildInfo::TShardStatus& shardStatus = buildInfo->Shards.at(shardIdx);
                if (shardStatus.LastKeyAck) {
                    TSerializedTableRange range = TSerializedTableRange(shardStatus.LastKeyAck, "", false, false);
                    range.Serialize(*ev->Record.MutableKeyRange());
                } else {
                    shardStatus.Range.Serialize(*ev->Record.MutableKeyRange());
                }

                ev->Record.SetMaxBatchRows(buildInfo->Limits.MaxBatchRows);
                ev->Record.SetMaxBatchBytes(buildInfo->Limits.MaxBatchBytes);
                ev->Record.SetMaxRetries(buildInfo->Limits.MaxRetries);

                ev->Record.SetSnapshotTxId(ui64(buildInfo->SnapshotTxId));
                ev->Record.SetSnapshotStep(ui64(buildInfo->SnapshotStep));

                ev->Record.SetSeqNoGeneration(Self->Generation());
                ev->Record.SetSeqNoRound(++shardStatus.SeqNoRound);

                LOG_D("TTxBuildProgress: TEvBuildIndexCreateRequest"
                      << ": " << ev->Record.ShortDebugString());

                ToTabletSend.emplace_back(shardId, ui64(BuildId), std::move(ev));
            }

            if (buildInfo->InProgressShards.empty() && buildInfo->ToUploadShards.empty()
                && buildInfo->DoneShards.size() == buildInfo->Shards.size()) {
                // all done
                Y_VERIFY(0 == Self->IndexBuildPipes.CloseAll(BuildId, ctx));

                ChangeState(BuildId, TIndexBuildInfo::EState::Applying);
                Progress(BuildId);

                // make final bill
                Bill(buildInfo);
            } else {
                AskToScheduleBilling(buildInfo);
            }

            break;
        case TIndexBuildInfo::EState::Applying:
            if (buildInfo->ApplyTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->ApplyTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), ApplyPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->ApplyTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->ApplyTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Unlocking);
                Progress(BuildId);
            }
            break;
        case TIndexBuildInfo::EState::Unlocking:
            if (buildInfo->UnlockTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->UnlockTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), UnlockPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->UnlockTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->UnlockTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Done);
                Progress(BuildId);
            }
            break;
        case TIndexBuildInfo::EState::Done:
            SendNotificationsIfFinished(buildInfo);
            // stay calm keep status/issues
            break;

        case TIndexBuildInfo::EState::Cancellation_Applying:
            if (buildInfo->ApplyTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->ApplyTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), CancelPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->ApplyTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->ApplyTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Cancellation_Unlocking);
                Progress(BuildId);
            }
            break;
        case TIndexBuildInfo::EState::Cancellation_Unlocking:
            if (buildInfo->UnlockTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->UnlockTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), UnlockPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->UnlockTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->UnlockTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Cancelled);
                Progress(BuildId);
            }
            break;
        case TIndexBuildInfo::EState::Cancelled:
            SendNotificationsIfFinished(buildInfo);
            // stay calm keep status/issues
            break;

        case TIndexBuildInfo::EState::Rejection_Applying:
            if (buildInfo->ApplyTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->ApplyTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), CancelPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->ApplyTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->ApplyTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Rejection_Unlocking);
                Progress(BuildId);
            }
            break;
        case TIndexBuildInfo::EState::Rejection_Unlocking:
            if (buildInfo->UnlockTxId == InvalidTxId) {
                Send(Self->TxAllocatorClient, MakeHolder<TEvTxAllocatorClient::TEvAllocate>(), 0, ui64(BuildId));
            } else if (buildInfo->UnlockTxStatus == NKikimrScheme::StatusSuccess) {
                Send(Self->SelfId(), UnlockPropose(Self, buildInfo), 0, ui64(BuildId));
            } else if (!buildInfo->UnlockTxDone) {
                Send(Self->SelfId(), MakeHolder<TEvSchemeShard::TEvNotifyTxCompletion>(ui64(buildInfo->UnlockTxId)));
            } else {
                ChangeState(BuildId, TIndexBuildInfo::EState::Rejected);
                Progress(BuildId);
            }
            break;
        case TIndexBuildInfo::EState::Rejected:
            SendNotificationsIfFinished(buildInfo);
            // stay calm keep status/issues
            break;
        }

        return true;
    }

    void InitiateShards(NIceDb::TNiceDb& db, TIndexBuildInfo::TPtr& buildInfo) {
        TTableInfo::TPtr table = Self->Tables.at(buildInfo->TablePathId);
        for (const auto& x: table->GetPartitions()) {
            Y_VERIFY(Self->ShardInfos.contains(x.ShardIdx));

            buildInfo->Shards.emplace(x.ShardIdx, TIndexBuildInfo::TShardStatus());
            Self->PersistBuildIndexUploadInitiate(db, buildInfo, x.ShardIdx);
        }
    }

    void DoComplete(const TActorContext& ctx) override {
        for (auto& x: ToTabletSend) {
            Self->IndexBuildPipes.Create(BuildId, std::get<0>(x), std::move(std::get<2>(x)), ctx);
        }
        ToTabletSend.clear();
    }
};

ITransaction* TSchemeShard::CreateTxProgress(TIndexBuildId id) {
    return new TIndexBuilder::TTxProgress(this, id);
}

struct TSchemeShard::TIndexBuilder::TTxBilling: public TSchemeShard::TIndexBuilder::TTxBase  {
private:
    TIndexBuildId BuildIndexId;
    TInstant ScheduledAt;

public:
    explicit TTxBilling(TSelf* self, TEvPrivate::TEvIndexBuildingMakeABill::TPtr& ev)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , BuildIndexId(ev->Get()->BuildId)
        , ScheduledAt(ev->Get()->SendAt)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_MAKEBILL_INDEX_BUILD;
    }

    bool DoExecute(TTransactionContext& , const TActorContext& ctx) override {
        LOG_I("TTxReply : TTxBilling"
              << ", buildIndexId# " << BuildIndexId);

        if (!Self->IndexBuilds.contains(BuildIndexId)) {
            return true;
        }

        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(BuildIndexId);

        if (!GotScheduledBilling(buildInfo)) {
            return true;
        }

        Bill(buildInfo, ScheduledAt, ctx.Now());

        AskToScheduleBilling(buildInfo);

        return true;
    }

    void DoComplete(const TActorContext&) override {
    }
};

struct TSchemeShard::TIndexBuilder::TTxReply: public TSchemeShard::TIndexBuilder::TTxBase  {
private:
    TEvTxAllocatorClient::TEvAllocateResult::TPtr AllocateResult;
    TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr ModifyResult;
    TEvSchemeShard::TEvNotifyTxCompletionResult::TPtr Notification;
    TEvDataShard::TEvBuildIndexProgressResponse::TPtr ShardProgress;
    struct {
        TIndexBuildId BuildIndexId;
        TTabletId TabletId;
        explicit operator bool() const { return BuildIndexId && TabletId; }
    } PipeRetry;

public:
    explicit TTxReply(TSelf* self, TEvTxAllocatorClient::TEvAllocateResult::TPtr& allocateResult)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , AllocateResult(allocateResult)
    {
    }

    explicit TTxReply(TSelf* self, TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr& modifyResult)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , ModifyResult(modifyResult)
    {
    }

    explicit TTxReply(TSelf* self, TEvSchemeShard::TEvNotifyTxCompletionResult::TPtr& notification)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , Notification(notification)
    {
    }

    explicit TTxReply(TSelf* self, TEvDataShard::TEvBuildIndexProgressResponse::TPtr& shardProgress)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , ShardProgress(shardProgress)
    {
    }

    explicit TTxReply(TSelf* self, TIndexBuildId buildId, TTabletId tabletId)
        : TSchemeShard::TIndexBuilder::TTxBase(self)
        , PipeRetry({buildId, tabletId})
    {
    }


    TTxType GetTxType() const override {
        return TXTYPE_PROGRESS_INDEX_BUILD;
    }

    bool DoExecute(TTransactionContext& txc, const TActorContext& ctx) override {
        if (AllocateResult) {
            return OnAllocation(txc, ctx);
        } else if (ModifyResult) {
            return OnModifyResult(txc, ctx);
        } else if (Notification) {
            return OnNotification(txc, ctx);
        } else if (ShardProgress) {
            return OnProgress(txc, ctx);
        } else if (PipeRetry) {
            return OnPipeRetry(txc, ctx);
        }

        return true;
    }

    bool OnPipeRetry(TTransactionContext&, const TActorContext& ctx) {
        const auto& buildId = PipeRetry.BuildIndexId;
        const auto& tabletId = PipeRetry.TabletId;
        const auto& shardIdx = Self->GetShardIdx(tabletId);

        LOG_I("TTxReply : PipeRetry"
              << ", buildIndexId# " << buildId
              << ", tabletId# " << tabletId
              << ", shardIdx# " << shardIdx);

        if (!Self->IndexBuilds.contains(buildId)) {
            return true;
        }

        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(buildId);

        if (!buildInfo->Shards.contains(shardIdx)) {
            return true;
        }

        LOG_D("TTxReply : PipeRetry"
              << ", TIndexBuildInfo: " << *buildInfo);

        switch (buildInfo->State) {
        case TIndexBuildInfo::EState::Invalid:
        case TIndexBuildInfo::EState::Locking:
        case TIndexBuildInfo::EState::GatheringStatistics:
        case TIndexBuildInfo::EState::Initiating:
            Y_FAIL("Unreachable");
        case TIndexBuildInfo::EState::Filling:
        {
            // reschedule shard
            if (buildInfo->InProgressShards.contains(shardIdx)) {
                buildInfo->ToUploadShards.push_front(shardIdx);
                buildInfo->InProgressShards.erase(shardIdx);

                Self->IndexBuildPipes.Close(buildId, tabletId, ctx);

                // generate new message with actual LastKeyAck to continue scan
                Progress(buildId);
            }
            break;
        }
        case TIndexBuildInfo::EState::Applying:
        case TIndexBuildInfo::EState::Unlocking:
        case TIndexBuildInfo::EState::Done:
            Y_FAIL("Unreachable");
        case TIndexBuildInfo::EState::Cancellation_Applying:
        case TIndexBuildInfo::EState::Cancellation_Unlocking:
        case TIndexBuildInfo::EState::Cancelled:
        case TIndexBuildInfo::EState::Rejection_Applying:
        case TIndexBuildInfo::EState::Rejection_Unlocking:
        case TIndexBuildInfo::EState::Rejected:
            LOG_D("TTxReply : PipeRetry"
                  << " superflous event"
                  << ", buildIndexId# " << buildId
                  << ", tabletId# " << tabletId
                  << ", shardIdx# " << shardIdx);
            break;
        }

        return true;
    }

    bool OnProgress(TTransactionContext& txc, const TActorContext& ctx) {
        const NKikimrTxDataShard::TEvBuildIndexProgressResponse& record = ShardProgress->Get()->Record;

        LOG_I("TTxReply : TEvBuildIndexProgressResponse"
              << ", buildIndexId# " << record.GetBuildIndexId());

        const auto buildId = TIndexBuildId(record.GetBuildIndexId());
        if (!Self->IndexBuilds.contains(buildId)) {
            return true;
        }

        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(buildId);
        LOG_D("TTxReply : TEvBuildIndexProgressResponse"
              << ", TIndexBuildInfo: " << *buildInfo
              << ", record: " << record.ShortDebugString());

        TTabletId shardId = TTabletId(record.GetTabletId());
        if (!Self->TabletIdToShardIdx.contains(shardId)) {
            return true;
        }

        TShardIdx shardIdx = Self->TabletIdToShardIdx.at(shardId);
        if (!buildInfo->Shards.contains(shardIdx)) {
            return true;
        }

        switch (buildInfo->State) {
        case TIndexBuildInfo::EState::Invalid:
        case TIndexBuildInfo::EState::Locking:
        case TIndexBuildInfo::EState::GatheringStatistics:
        case TIndexBuildInfo::EState::Initiating:
            Y_FAIL("Unreachable");
        case TIndexBuildInfo::EState::Filling:
        {
            TIndexBuildInfo::TShardStatus& shardStatus = buildInfo->Shards.at(shardIdx);

            auto actualSeqNo = std::pair<ui64, ui64>(Self->Generation(), shardStatus.SeqNoRound);
            auto recordSeqNo = std::pair<ui64, ui64>(record.GetRequestSeqNoGeneration(), record.GetRequestSeqNoRound());

            if (actualSeqNo != recordSeqNo) {
                LOG_D("TTxReply : TEvBuildIndexProgressResponse"
                      << " ignore progress message by seqNo"
                      << ", TIndexBuildInfo: " << *buildInfo
                      << ", actual seqNo for the shard " << shardId << " (" << shardIdx << ") is: "  << Self->Generation() << ":" <<  shardStatus.SeqNoRound
                      << ", record: " << record.ShortDebugString());
                Y_VERIFY(actualSeqNo > recordSeqNo);
                return true;
            }

            if (record.HasLastKeyAck()) {
                {
                    //check that all LastKeyAcks are monotonously increase
                    TTableInfo::TPtr tableInfo = Self->Tables.at(buildInfo->TablePathId);
                    TVector<ui16> keyTypes;
                    for (ui32 keyPos: tableInfo->KeyColumnIds) {
                        keyTypes.push_back(tableInfo->Columns.at(keyPos).PType);
                    }

                    TSerializedCellVec last;
                    last.Parse(shardStatus.LastKeyAck);

                    TSerializedCellVec update;
                    update.Parse(record.GetLastKeyAck());

                    int cmp = CompareBorders<true, true>(last.GetCells(),
                                                         update.GetCells(),
                                                         true,
                                                         true,
                                                         keyTypes);
                    Y_VERIFY_S(cmp < 0,
                               "check that all LastKeyAcks are monotonously increase"
                                   << ", last: " << DebugPrintPoint(keyTypes, last.GetCells(), *AppData()->TypeRegistry)
                                   << ", update: " <<  DebugPrintPoint(keyTypes, update.GetCells(), *AppData()->TypeRegistry));
                }

                shardStatus.LastKeyAck = record.GetLastKeyAck();
            }

            if (record.HasRowsDelta() || record.HasBytesDelta()) {
                TBillingStats delta(record.GetRowsDelta(), record.GetBytesDelta());
                shardStatus.Processed += delta;
                buildInfo->Processed += delta;
            }

            shardStatus.Status = record.GetStatus();
            shardStatus.UploadStatus = record.GetUploadStatus();
            NYql::TIssues issues;
            NYql::IssuesFromMessage(record.GetIssues(), issues);
            shardStatus.DebugMessage = issues.ToString();

            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUploadProgress(db, buildInfo, shardIdx);

            switch (shardStatus.Status) {
            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::INVALID:
                Y_FAIL("Unreachable");

            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::ACCEPTED:
            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::INPROGRESS:
                // no oop, wait resolution. Progress key are persisted
                break;

            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::DONE:
                if (buildInfo->InProgressShards.contains(shardIdx)) {
                    buildInfo->InProgressShards.erase(shardIdx);
                    buildInfo->DoneShards.emplace(shardIdx);

                    Self->IndexBuildPipes.Close(buildId, shardId, ctx);

                    Progress(buildId);
                }
                break;

            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::ABORTED:
                // datashard gracefully rebooted, reschedule shard
                if (buildInfo->InProgressShards.contains(shardIdx)) {
                    buildInfo->ToUploadShards.push_front(shardIdx);
                    buildInfo->InProgressShards.erase(shardIdx);

                    Self->IndexBuildPipes.Close(buildId, shardId, ctx);

                    Progress(buildId);
                }
                break;

            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::BUILD_ERROR:
                buildInfo->Issue += TStringBuilder()
                    << "One of the shards report BUILD_ERROR at Filling stage, procces has to be canceled"
                    << ", shardId: " << shardId
                    << ", shardIdx: " << shardIdx;
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejection_Applying);

                Progress(buildId);
                break;
            case  NKikimrTxDataShard::TEvBuildIndexProgressResponse::BAD_REQUEST:
                buildInfo->Issue += TStringBuilder()
                    << "One of the shards report BAD_REQUEST at Filling stage, procces has to be canceled"
                    << ", shardId: " << shardId
                    << ", shardIdx: " << shardIdx;
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejection_Applying);

                Progress(buildId);
                break;
            }

            break;
        }
        case TIndexBuildInfo::EState::Applying:
        case TIndexBuildInfo::EState::Unlocking:
        case TIndexBuildInfo::EState::Done:
            Y_FAIL("Unreachable");
        case TIndexBuildInfo::EState::Cancellation_Applying:
        case TIndexBuildInfo::EState::Cancellation_Unlocking:
        case TIndexBuildInfo::EState::Cancelled:
        case TIndexBuildInfo::EState::Rejection_Applying:
        case TIndexBuildInfo::EState::Rejection_Unlocking:
        case TIndexBuildInfo::EState::Rejected:
            LOG_D("TTxReply : TEvBuildIndexProgressResponse"
                  << " superflous message " << record.ShortDebugString());
            break;
        }

        return true;
    }

    bool OnNotification(TTransactionContext& txc, const TActorContext&) {
        const auto& record = Notification->Get()->Record;

        const auto txId = TTxId(record.GetTxId());
        if (!Self->TxIdToIndexBuilds.contains(txId)) {
            LOG_I("TTxReply : TEvNotifyTxCompletionResult superflous message"
                  << ", txId: " << record.GetTxId()
                  << ", buildInfoId not found");
            return true;
        }

        const auto buildId = Self->TxIdToIndexBuilds.at(txId);
        Y_VERIFY(Self->IndexBuilds.contains(buildId));

        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(buildId);
        LOG_I("TTxReply : TEvNotifyTxCompletionResult"
              << ", txId# " << record.GetTxId()
              << ", buildInfoId: " << buildInfo->Id);
        LOG_D("TTxReply : TEvNotifyTxCompletionResult"
              << ", txId# " << record.GetTxId()
              << ", buildInfo: " << *buildInfo);

        switch (buildInfo->State) {
        case TIndexBuildInfo::EState::Invalid:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Locking:
        {
            Y_VERIFY(txId == buildInfo->LockTxId);

            buildInfo->LockTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexLockTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::GatheringStatistics:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Initiating:
        {
            Y_VERIFY(txId == buildInfo->InitiateTxId);

            buildInfo->InitiateTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexInitiateTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Filling:
            Y_FAIL("Unreachable");
        case TIndexBuildInfo::EState::Applying:
        {
            Y_VERIFY(txId == buildInfo->ApplyTxId);

            buildInfo->ApplyTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexApplyTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Unlocking:
        {
            Y_VERIFY(txId == buildInfo->UnlockTxId);

            buildInfo->UnlockTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUnlockTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Done:
            Y_FAIL("Unreachable");
        case TIndexBuildInfo::EState::Cancellation_Applying:
        {
            Y_VERIFY(txId == buildInfo->ApplyTxId);

            buildInfo->ApplyTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexApplyTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Cancellation_Unlocking:
        {
            Y_VERIFY(txId == buildInfo->UnlockTxId);

            buildInfo->UnlockTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUnlockTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Cancelled:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Rejection_Applying:
        {
            Y_VERIFY(txId == buildInfo->ApplyTxId);

            buildInfo->ApplyTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexApplyTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Rejection_Unlocking:
        {
            Y_VERIFY(txId == buildInfo->UnlockTxId);

            buildInfo->UnlockTxDone = true;
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUnlockTxDone(db, buildInfo);

            break;
        }
        case TIndexBuildInfo::EState::Rejected:
            Y_FAIL("Unreachable");
        }

        Progress(buildId);

        return true;
    }

    void ReplyOnCreation(const TIndexBuildInfo::TPtr buildInfo,
                         const Ydb::StatusIds::StatusCode status = Ydb::StatusIds::SUCCESS)
    {
        auto responseEv = MakeHolder<TEvIndexBuilder::TEvCreateResponse>(ui64(buildInfo->Id));

        Fill(*responseEv->Record.MutableIndexBuild(), buildInfo);

        auto& response = responseEv->Record;
        response.SetStatus(status);

        if (buildInfo->Issue) {
            AddIssue(response.MutableIssues(), buildInfo->Issue);
        }

        LOG_N("TIndexBuilder::TTxReply: ReplyOnCreation"
              << ", BuildIndexId: " << buildInfo->Id
              << ", status: " << Ydb::StatusIds::StatusCode_Name(status)
              << ", error: " << buildInfo->Issue
              << ", replyTo: " << buildInfo->CreateSender.ToString());
        LOG_D("Message:\n" << responseEv->Record.ShortDebugString());

        Send(buildInfo->CreateSender, std::move(responseEv), 0, buildInfo->SenderCookie);
    }

    bool OnModifyResult(TTransactionContext& txc, const TActorContext&) {
        const auto& record = ModifyResult->Get()->Record;

        const auto txId = TTxId(record.GetTxId());
        if (!Self->TxIdToIndexBuilds.contains(txId)) {
            LOG_I("TTxReply : TEvModifySchemeTransactionResult superflous message"
                  << ", cookie: " << ModifyResult->Cookie
                  << ", txId: " << record.GetTxId()
                  << ", status: " << NKikimrScheme::EStatus_Name(record.GetStatus())
                  << ", BuildIndexId not found");
            return true;
        }

        const auto buildId = Self->TxIdToIndexBuilds.at(txId);
        Y_VERIFY(Self->IndexBuilds.contains(buildId));

        LOG_I("TTxReply : TEvModifySchemeTransactionResult"
              << ", BuildIndexId: " << buildId
              << ", cookie: " << ModifyResult->Cookie
              << ", txId: " << record.GetTxId()
              << ", status: " << NKikimrScheme::EStatus_Name(record.GetStatus()));

        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(buildId);
        LOG_D("TTxReply : TEvModifySchemeTransactionResult"
              << ", buildInfo: " << *buildInfo
              << ", record: " << record.ShortDebugString());

        switch (buildInfo->State) {
        case TIndexBuildInfo::EState::Invalid:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Locking:
        {
            Y_VERIFY(txId == buildInfo->LockTxId);

            buildInfo->LockTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexLockTxStatus(db, buildInfo);

            auto statusCode = TranslateStatusCode(record.GetStatus());

            if (statusCode != Ydb::StatusIds::SUCCESS) {
                buildInfo->Issue += TStringBuilder()
                    << "At locking state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->LockTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);

                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexForget(db, buildInfo);

                EraseBuildInfo(buildInfo);
            }

            ReplyOnCreation(buildInfo, statusCode);

            break;
        }
        case TIndexBuildInfo::EState::GatheringStatistics:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Initiating:
        {
            Y_VERIFY(txId == buildInfo->InitiateTxId);

            buildInfo->InitiateTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexInitiateTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                Y_FAIL("NEED MORE TESTING");
               // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At initiating state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejection_Unlocking);
            }

            break;
        }
        case TIndexBuildInfo::EState::Filling:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Applying:
        {
            Y_VERIFY(txId == buildInfo->ApplyTxId);

            buildInfo->ApplyTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexApplyTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                Y_FAIL("NEED MORE TESTING");
                // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At applying state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejection_Unlocking);
            }

            break;
        }
        case TIndexBuildInfo::EState::Unlocking:
        {
            Y_VERIFY(txId == buildInfo->UnlockTxId);

            buildInfo->UnlockTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUnlockTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At unlocking state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejection_Unlocking);
            }

            break;
        }
        case TIndexBuildInfo::EState::Done:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Cancellation_Applying:
        {
            Y_VERIFY(txId == buildInfo->ApplyTxId);

            buildInfo->ApplyTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexApplyTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                Y_FAIL("NEED MORE TESTING");
                // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At cancelation applying state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Cancellation_Unlocking);
            }

            break;
        }
        case TIndexBuildInfo::EState::Cancellation_Unlocking:
        {
            Y_VERIFY(txId == buildInfo->UnlockTxId);

            buildInfo->UnlockTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUnlockTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At cancelation unlocking state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Cancelled);
            }

            break;
        }
        case TIndexBuildInfo::EState::Cancelled:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Rejection_Applying:
        {
            Y_VERIFY(txId == buildInfo->ApplyTxId);

            buildInfo->ApplyTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexApplyTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                Y_FAIL("NEED MORE TESTING");
                // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At rejection_applying state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejection_Unlocking);
            }

            break;
        }
        case TIndexBuildInfo::EState::Rejection_Unlocking:
        {
            Y_VERIFY(txId == buildInfo->UnlockTxId);

            buildInfo->UnlockTxStatus = record.GetStatus();
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistBuildIndexUnlockTxStatus(db, buildInfo);

            if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
                // no op
            } else if (record.GetStatus() == NKikimrScheme::StatusAlreadyExists) {
                // no op
            } else {
                buildInfo->Issue += TStringBuilder()
                    << "At rejection_unlockiing state got unsuccess propose result"
                    << ", status: " << NKikimrScheme::EStatus_Name(buildInfo->InitiateTxStatus)
                    << ", reason: " << record.GetReason();
                Self->PersistBuildIndexIssue(db, buildInfo);
                ChangeState(buildInfo->Id, TIndexBuildInfo::EState::Rejected);
            }

            break;
        }
        case TIndexBuildInfo::EState::Rejected:
            Y_FAIL("Unreachable");
        }

        Progress(buildId);

        return true;
    }

    bool OnAllocation(TTransactionContext& txc, const TActorContext&) {
        TIndexBuildId buildId = TIndexBuildId(AllocateResult->Cookie);
        const auto txId = TTxId(AllocateResult->Get()->TxIds.front());

        LOG_I("TTxReply : TEvAllocateResult"
              << ", BuildIndexId: " << buildId
              << ", txId# " << txId);

        Y_VERIFY(Self->IndexBuilds.contains(buildId));
        TIndexBuildInfo::TPtr buildInfo = Self->IndexBuilds.at(buildId);

        LOG_D("TTxReply : TEvAllocateResult"
              << ", buildInfo: " << *buildInfo);

        switch (buildInfo->State) {
        case TIndexBuildInfo::EState::Invalid:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Locking:
            if (!buildInfo->LockTxId) {
                buildInfo->LockTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexLockTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::GatheringStatistics:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Initiating:
            if (!buildInfo->InitiateTxId) {
                buildInfo->InitiateTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexInitiateTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::Filling:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Applying:
            if (!buildInfo->ApplyTxId) {
                buildInfo->ApplyTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexApplyTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::Unlocking:
            if (!buildInfo->UnlockTxId) {
                buildInfo->UnlockTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexUnlockTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::Done:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Cancellation_Applying:
            if (!buildInfo->ApplyTxId) {
                buildInfo->ApplyTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexApplyTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::Cancellation_Unlocking:
            if (!buildInfo->UnlockTxId) {
                buildInfo->UnlockTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexUnlockTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::Cancelled:
            Y_FAIL("Unreachable");

        case TIndexBuildInfo::EState::Rejection_Applying:
            if (!buildInfo->ApplyTxId) {
                buildInfo->ApplyTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexApplyTxId(db, buildInfo);
            }
            break;
        case TIndexBuildInfo::EState::Rejection_Unlocking:
            if (!buildInfo->UnlockTxId) {
                buildInfo->UnlockTxId = txId;
                NIceDb::TNiceDb db(txc.DB);
                Self->PersistBuildIndexUnlockTxId(db, buildInfo);
            }
            break;

        case TIndexBuildInfo::EState::Rejected:
            Y_FAIL("Unreachable");
        }

        Progress(buildId);

        return true;
    }

    void DoComplete(const TActorContext&) override {
    }
};


ITransaction* TSchemeShard::CreateTxReply(TEvTxAllocatorClient::TEvAllocateResult::TPtr& allocateResult) {
    return new TIndexBuilder::TTxReply(this, allocateResult);
}

ITransaction* TSchemeShard::CreateTxReply(TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr& modifyResult) {
    return new TIndexBuilder::TTxReply(this, modifyResult);
}

ITransaction* TSchemeShard::CreateTxReply(TEvSchemeShard::TEvNotifyTxCompletionResult::TPtr& notification) {
    return new TIndexBuilder::TTxReply(this, notification);
}

ITransaction* TSchemeShard::CreateTxReply(TEvDataShard::TEvBuildIndexProgressResponse::TPtr& progress) {
    return new TIndexBuilder::TTxReply(this, progress);
}

ITransaction* TSchemeShard::CreatePipeRetry(TIndexBuildId indexBuildId, TTabletId tabletId) {
    return new TIndexBuilder::TTxReply(this, indexBuildId, tabletId);
}

ITransaction* TSchemeShard::CreateTxBilling(TEvPrivate::TEvIndexBuildingMakeABill::TPtr& ev) {
    return new TIndexBuilder::TTxBilling(this, ev);
}


} // NSchemeShard
} // NKikimr
