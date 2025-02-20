#include "tablet_impl.h"
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>
#include <util/generic/set.h>

namespace NKikimr {

constexpr ui32 MAX_ATTEMPTS = 3;

class TTabletReqBlockBlobStorageGroup : public TActorBootstrapped<TTabletReqBlockBlobStorageGroup> {
public:
    TActorId Owner;
    ui64 TabletId;
    ui32 GroupId;
    ui32 Generation;
    ui32 ErrorCount;

    void ReplyAndDie(NKikimrProto::EReplyStatus status, const TString &reason = { }) {
        Send(Owner, new TEvTabletBase::TEvBlockBlobStorageResult(status, TabletId, reason));
        PassAway();
    }

    void SendRequest() {
        const TActorId proxy = MakeBlobStorageProxyID(GroupId);
        THolder<TEvBlobStorage::TEvBlock> event(new TEvBlobStorage::TEvBlock(TabletId, Generation, TInstant::Max()));
        event->IsMonitored = false;
        SendToBSProxy(TlsActivationContext->AsActorContext(), proxy, event.Release());
    }

    void Handle(TEvents::TEvUndelivered::TPtr&) {
        return ReplyAndDie(NKikimrProto::ERROR, "BlobStorage proxy unavailable");
    }

    void Handle(TEvBlobStorage::TEvBlockResult::TPtr &ev) {
        const TEvBlobStorage::TEvBlockResult *msg = ev->Get();

        switch (msg->Status) {
        case NKikimrProto::OK:
            return ReplyAndDie(NKikimrProto::OK);
        case NKikimrProto::BLOCKED:
        case NKikimrProto::RACE:
        case NKikimrProto::NO_GROUP:
            // The request will never succeed
            return ReplyAndDie(msg->Status, msg->ErrorReason);
        default:
            ++ErrorCount;
            if (ErrorCount >= MAX_ATTEMPTS) {
                return ReplyAndDie(NKikimrProto::ERROR, msg->ErrorReason);
            }
            return SendRequest();
        }
    }

    STFUNC(StateWait) {
        Y_UNUSED(ctx);
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvBlobStorage::TEvBlockResult, Handle);
            cFunc(TEvents::TEvPoisonPill::EventType, PassAway);
            hFunc(TEvents::TEvUndelivered, Handle);
        }
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TABLET_REQ_BLOCK_BS;
    }

    void Bootstrap() {
        SendRequest();
        Become(&TThis::StateWait);
    }

    TTabletReqBlockBlobStorageGroup(ui64 tabletId, ui32 groupId, ui32 gen)
        : TabletId(tabletId)
        , GroupId(groupId)
        , Generation(gen)
        , ErrorCount(0)
    {}
};

class TTabletReqBlockBlobStorage : public TActorBootstrapped<TTabletReqBlockBlobStorage> {
    TActorId Owner;
    ui64 TabletId;
    ui32 Generation;
    ui32 Replied = 0;
    TVector<THolder<TTabletReqBlockBlobStorageGroup>> Requests;
    TVector<TActorId> ReqActors;

    void PassAway() override {
        for (auto &x : ReqActors)
            if (x)
                Send(x, new TEvents::TEvPoisonPill());

        TActor::PassAway();
    }

    void ReplyAndDie(NKikimrProto::EReplyStatus status, const TString &reason = { }) {
        Send(Owner, new TEvTabletBase::TEvBlockBlobStorageResult(status, TabletId, reason));
        PassAway();
    }

    void Handle(TEvTabletBase::TEvBlockBlobStorageResult::TPtr &ev) {
        auto *msg = ev->Get();
        auto it = Find(ReqActors, ev->Sender);
        Y_VERIFY(it != ReqActors.end(), "must not get response from unknown actor");
        *it = TActorId();

        switch (msg->Status) {
        case NKikimrProto::OK:
            if (++Replied == ReqActors.size())
                return ReplyAndDie(NKikimrProto::OK);
            break;
        default:
            return ReplyAndDie(msg->Status, msg->ErrorReason);
        }
    }
public:
    TTabletReqBlockBlobStorage(TActorId owner, TTabletStorageInfo* info, ui32 generation, bool blockPrevEntry)
        : Owner(owner)
        , TabletId(info->TabletID)
        , Generation(generation)
    {
        std::unordered_set<ui32> blocked;
        Requests.reserve(blockPrevEntry ? info->Channels.size() * 2 : info->Channels.size());
        for (auto& channel : info->Channels) {
            if (channel.History.empty()) {
                continue;
            }
            auto itEntry = channel.History.rbegin();
            while (itEntry != channel.History.rend() && itEntry->FromGeneration > generation) {
                ++itEntry;
            }
            if (itEntry == channel.History.rend()) {
                continue;
            }
            if (blocked.insert(itEntry->GroupID).second) {
                Requests.emplace_back(new TTabletReqBlockBlobStorageGroup(TabletId, itEntry->GroupID, Generation));
            }

            if (blockPrevEntry) {
                ++itEntry;
                if (itEntry == channel.History.rend()) {
                    continue;
                }
                if (blocked.insert(itEntry->GroupID).second) {
                    Requests.emplace_back(new TTabletReqBlockBlobStorageGroup(TabletId, itEntry->GroupID, Generation));
                }
            }
        }
    }

    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TABLET_REQ_BLOCK_BS;
    }

    void Bootstrap() {
        ReqActors.reserve(Requests.size());
        for (auto& req : Requests) {
            req->Owner = SelfId();
            ReqActors.push_back(RegisterWithSameMailbox(req.Release()));
        }
        Become(&TThis::StateWait);
    }

    STFUNC(StateWait) {
        Y_UNUSED(ctx);
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvTabletBase::TEvBlockBlobStorageResult, Handle);
            cFunc(TEvents::TEvPoisonPill::EventType, PassAway);
        }
    }
};

IActor* CreateTabletReqBlockBlobStorage(const TActorId& owner, TTabletStorageInfo* info, ui32 generation, bool blockPrevEntry) {
    return new TTabletReqBlockBlobStorage(owner, info, generation, blockPrevEntry);
}

}
