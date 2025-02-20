#include "log.h"
#include "cfg.h"
#include "executor.h"
#include "params.h"
#include "purge.h"
#include "serviceid.h"

#include <ydb/core/ymq/base/counters.h>
#include <ydb/core/ymq/base/debug_info.h>
#include <ydb/core/ymq/base/query_id.h>

using NKikimr::NClient::TValue;

namespace NKikimr::NSQS {

TPurgeActor::TPurgeActor(const TQueuePath& queuePath, TIntrusivePtr<TQueueCounters> counters, const TActorId& queueLeader, bool isFifo)
    : QueuePath_(queuePath)
    , RequestId_(CreateGuidAsString())
    , Counters_(std::move(counters))
    , QueueLeader_(queueLeader)
    , IsFifo_(isFifo)
{
    DebugInfo->QueuePurgeActors.emplace(TStringBuilder() << TLogQueueName(QueuePath_), this);
}

TPurgeActor::~TPurgeActor() {
    DebugInfo->QueuePurgeActors.EraseKeyValue(TStringBuilder() << TLogQueueName(QueuePath_), this);
}

void TPurgeActor::Bootstrap() {
    RLOG_SQS_INFO("Create purge actor for queue " << TString(QueuePath_));
    Become(&TThis::StateFunc);
}

void TPurgeActor::MakeGetRetentionOffsetRequest(const ui64 shardId, TShard* shard) {
    shard->KeysTruncated = false;
    const TInstant boundary = shard->TargetBoundary;
    auto onExecuted = [this, shardId, shard, boundary] (const TSqsEvents::TEvExecuted::TRecord& ev) {
        const ui32 status = ev.GetStatus();
        if (status == TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecComplete) {
            const TValue val(TValue::Create(ev.GetExecutionEngineEvaluatedResponse()));
            const TValue& messages = val["messages"];
            shard->KeysTruncated = val["truncated"];
            if (messages.Size() > 0) {
                const ui64 from = messages[0]["Offset"];
                const ui64 to = messages[messages.Size() - 1]["Offset"];
                MakeStage1Request(shardId, shard, std::make_pair(from, to));
            } else {
                RLOG_SQS_DEBUG("No messages to cleanup");
                shard->PreviousSuccessfullyProcessedLastMessage.SentTimestamp = boundary;
                shard->Purging = false;
                shard->BoundaryPurged = shard->TargetBoundary;
            }
        } else {
            RLOG_SQS_WARN("Failed to execute cleanup request on queue [" << QueuePath_ << "] shard [" << shardId << "] get retention offset: " << ev);
            shard->Purging = false;
            shard->TargetBoundary = shard->BoundaryPurged;
        }
    };

    TExecutorBuilder(SelfId(), RequestId_)
        .User(QueuePath_.UserName)
        .Queue(QueuePath_.QueueName)
        .Shard(shardId)
        .QueueLeader(QueueLeader_)
        .QueryId(GET_RETENTION_OFFSET_ID)
        .Counters(Counters_)
        .RetryOnTimeout()
        .OnExecuted(onExecuted)
        .Params()
            .Uint64("OFFSET_FROM", shard->PreviousSuccessfullyProcessedLastMessage.Offset)
            .Uint64("TIME_FROM", shard->PreviousSuccessfullyProcessedLastMessage.SentTimestamp.MilliSeconds())
            .Uint64("TIME_TO", boundary.MilliSeconds())
            .Uint64("BATCH_SIZE", Cfg().GetCleanupBatchSize())
        .ParentBuilder().Start();
}

void TPurgeActor::MakeStage1Request(const ui64 shardId, TShard* shard, const std::pair<ui64, ui64>& offsets) {
    auto onExecuted = [this, shardId, shard] (const TSqsEvents::TEvExecuted::TRecord& ev) {
        const ui32 status = ev.GetStatus();
        if (status == TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecComplete) {
            const TValue val(TValue::Create(ev.GetExecutionEngineEvaluatedResponse()));
            const TValue& messages = val["messages"];
            TMaybe<TValue> inflyMessages;
            if (!IsFifo_) {
                inflyMessages.ConstructInPlace(val["inflyMessages"]);
            }
            const bool truncated = val["truncated"];
            shard->KeysTruncated = shard->KeysTruncated || truncated;
            if (messages.Size() > 0 || !IsFifo_ && inflyMessages->Size() > 0) {
                const ui64 cleanupVersion = val["cleanupVersion"];
                MakeStage2Request(cleanupVersion, messages, inflyMessages, shardId, shard);
            } else {
                RLOG_SQS_DEBUG("No messages to cleanup");
                shard->Purging = false;
                shard->BoundaryPurged = shard->TargetBoundary;
            }
        } else {
            RLOG_SQS_WARN("Failed to execute cleanup request on queue [" << QueuePath_ << "] shard [" << shardId << "] stage 1: " << ev);
            shard->Purging = false;
            shard->TargetBoundary = shard->BoundaryPurged;
        }
    };

    TExecutorBuilder(SelfId(), RequestId_)
        .User(QueuePath_.UserName)
        .Queue(QueuePath_.QueueName)
        .Shard(shardId)
        .QueueLeader(QueueLeader_)
        .QueryId(PURGE_QUEUE_ID)
        .Counters(Counters_)
        .RetryOnTimeout()
        .OnExecuted(onExecuted)
        .Params()
            .Uint64("OFFSET_FROM", offsets.first)
            .Uint64("OFFSET_TO", offsets.second)
            .Uint64("NOW", Now().MilliSeconds())
            .Uint64("SHARD", shardId)
            .Uint64("BATCH_SIZE", Cfg().GetCleanupBatchSize())
        .ParentBuilder().Start();
}

static void FillMessagesParam(NClient::TWriteValue& messagesParam, const NClient::TValue& messages, ui64& lastOffset, TInstant& lastSentTimestamp, TSqsEvents::TEvInflyIsPurgingNotification* notification = nullptr) {
    if (notification) {
        notification->Offsets.reserve(messages.Size());
    }
    for (size_t i = 0; i < messages.Size(); ++i) {
        const TValue& message = messages[i];
        auto messageParam = messagesParam.AddListItem();
        const ui64 offset = message["Offset"];
        const ui64 sentTimestamp = message["SentTimestamp"];
        if (notification) {
            notification->Offsets.push_back(offset);
        }
        messageParam["Offset"] = offset;
        messageParam["RandomId"] = ui64(message["RandomId"]);
        messageParam["SentTimestamp"] = sentTimestamp;
        lastOffset = Max(lastOffset, offset);
        lastSentTimestamp = Max(TInstant::MilliSeconds(sentTimestamp), lastSentTimestamp);
    }
}

void TPurgeActor::MakeStage2Request(ui64 cleanupVersion, const TValue& messages, const TMaybe<TValue>& inflyMessages, const ui64 shardId, TShard* shard) {
    auto onExecuted = [this, shardId, shard] (const TSqsEvents::TEvExecuted::TRecord& ev) {
        const ui32 status = ev.GetStatus();
        if (status == TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecComplete) {
            const TValue val(TValue::Create(ev.GetExecutionEngineEvaluatedResponse()));
            const ui64 messagesDeleted = val["messagesDeleted"];
            ADD_COUNTER_COUPLE(Counters_, MessagesPurged, purged_count_per_second, messagesDeleted);
            RLOG_SQS_DEBUG("Purged " << messagesDeleted << " messages from queue [" << QueuePath_ << "]");
            const bool versionIsSame = val["versionIsSame"];
            if (versionIsSame) {
                shard->PreviousSuccessfullyProcessedLastMessage = shard->CurrentLastMessage;
            }
            if (!IsFifo_) {
                const i64 newMessagesCount = val["newMessagesCount"];
                Y_VERIFY(newMessagesCount >= 0);
                auto notification = MakeHolder<TSqsEvents::TEvQueuePurgedNotification>();
                notification->Shard = shardId;
                notification->NewMessagesCount = static_cast<ui64>(newMessagesCount);
                Send(QueueLeader_, std::move(notification));
            }

            shard->BoundaryPurged = shard->CurrentLastMessage.SentTimestamp;
            if (shard->KeysTruncated) {
                MakeGetRetentionOffsetRequest(shardId, shard);
            } else {
                shard->Purging = false;
            }
        } else {
            RLOG_SQS_WARN("Failed to execute cleanup request on queue [" << QueuePath_ << "] shard [" << shardId << "] stage 2: " << ev);
            shard->Purging = false;
            shard->TargetBoundary = shard->BoundaryPurged;
        }
    };

    TExecutorBuilder builder(SelfId(), RequestId_);
    builder
        .User(QueuePath_.UserName)
        .Queue(QueuePath_.QueueName)
        .Shard(shardId)
        .QueueLeader(QueueLeader_)
        .QueryId(PURGE_QUEUE_STAGE2_ID)
        .Counters(Counters_)
        .RetryOnTimeout()
        .OnExecuted(onExecuted);

    NClient::TWriteValue params = builder.ParamsValue();
    params["CLEANUP_VERSION"] = cleanupVersion;
    params["SHARD"] = shardId;
    params["NOW"] = TActivationContext::Now().MilliSeconds();

    auto messagesParam = params["MESSAGES"];
    FillMessagesParam(messagesParam, messages, shard->CurrentLastMessage.Offset, shard->CurrentLastMessage.SentTimestamp);
    if (inflyMessages) {
        THolder<TSqsEvents::TEvInflyIsPurgingNotification> notification(new TSqsEvents::TEvInflyIsPurgingNotification());
        notification->Shard = shardId;
        FillMessagesParam(messagesParam, *inflyMessages, shard->CurrentLastMessage.Offset, shard->CurrentLastMessage.SentTimestamp, notification.Get());
        if (!notification->Offsets.empty()) {
            Send(QueueLeader_, std::move(notification));
        }
    }

    builder.Start();
}

void TPurgeActor::HandlePurgeQueue(TSqsEvents::TEvPurgeQueue::TPtr& ev) {
    auto& shard = Shards_[ev->Get()->Shard];

    const char* skipReason = "";
    if (ev->Get()->Boundary > shard.TargetBoundary) {
        shard.TargetBoundary = ev->Get()->Boundary;

        if (!shard.Purging) {
            shard.Purging = true;
            MakeGetRetentionOffsetRequest(ev->Get()->Shard, &shard);
        } else {
            skipReason = ". Skipping (already purging)";
        }
    } else {
        skipReason = ". Skipping (old boundary)";
    }

    RLOG_SQS_INFO("Purge queue request [" << QueuePath_ << "/" << ev->Get()->Shard << "] to " << ev->Get()->Boundary.MilliSeconds() << " (" << ev->Get()->Boundary << ")" << skipReason);
}

void TPurgeActor::HandleExecuted(TSqsEvents::TEvExecuted::TPtr& ev) {
    ev->Get()->Call();
}

void TPurgeActor::HandlePoisonPill(TEvPoisonPill::TPtr&) {
    PassAway();
}

} // namespace NKikimr::NSQS
