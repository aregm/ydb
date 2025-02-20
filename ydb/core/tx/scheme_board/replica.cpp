#include "double_indexed.h"
#include "events.h"
#include "helpers.h"
#include "monitorable_actor.h"
#include "replica.h"

#include <contrib/libs/protobuf/src/google/protobuf/util/json_util.h>

#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/protos/services.pb.h>
#include <ydb/core/util/yverify_stream.h>

#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/log.h>
#include <library/cpp/actors/core/memory_track.h>

#include <util/generic/hash.h>
#include <util/generic/map.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/variant.h>
#include <util/string/builder.h>

namespace NKikimr {
namespace NSchemeBoard {

#define SBR_LOG_T(stream) SB_LOG_T(SCHEME_BOARD_REPLICA, stream)
#define SBR_LOG_D(stream) SB_LOG_D(SCHEME_BOARD_REPLICA, stream)
#define SBR_LOG_I(stream) SB_LOG_I(SCHEME_BOARD_REPLICA, stream)
#define SBR_LOG_N(stream) SB_LOG_N(SCHEME_BOARD_REPLICA, stream)
#define SBR_LOG_E(stream) SB_LOG_E(SCHEME_BOARD_REPLICA, stream)

class TReplica: public TMonitorableActor<TReplica> {
    using TDescribeSchemeResult = NKikimrScheme::TEvDescribeSchemeResult;
    using TCapabilities = NKikimrSchemeBoard::TEvSubscribe::TCapabilities;

    struct TEvPrivate {
        enum EEv {
            EvSendStrongNotifications = EventSpaceBegin(TKikimrEvents::ES_PRIVATE),

            EvEnd,
        };

        static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_PRIVATE)");

        struct TEvSendStrongNotifications: public TEventLocal<TEvSendStrongNotifications, EvSendStrongNotifications> {
            static constexpr ui32 BatchSize = 1000;
            const ui64 Owner;

            explicit TEvSendStrongNotifications(ui64 owner)
                : Owner(owner)
            {
            }
        };
    };

public:
    enum ESubscriptionType {
        SUBSCRIPTION_UNSPECIFIED, // for filtration
        SUBSCRIPTION_BY_PATH,
        SUBSCRIPTION_BY_PATH_ID,
    };

private:
    class TSubscriberInfo {
    public:
        explicit TSubscriberInfo(ESubscriptionType type, ui64 domainOwnerId, const TCapabilities& capabilities)
            : Type(type)
            , DomainOwnerId(domainOwnerId)
            , Capabilities(capabilities)
            , WaitForAck(false)
            , LastVersionSent(0)
            , NotifiedStrongly(true)
            , SyncRequestCookie(0)
            , SyncResponseCookie(0)
        {
        }

        ESubscriptionType GetType() const {
            return Type;
        }

        ui64 GetDomainOwnerId() const {
            return DomainOwnerId;
        }

        const TCapabilities& GetCapabilities() const {
            return Capabilities;
        }

        bool EnqueueVersion(ui64 version, bool strong) {
            if (!Capabilities.GetAckNotifications()) {
                NotifiedStrongly = strong;
                return true;
            }

            if (WaitForAck) {
                return false;
            }

            WaitForAck = true;
            LastVersionSent = version;
            NotifiedStrongly = strong;
            return true;
        }

        bool EnqueueVersion(const TSchemeBoardEvents::TEvNotifyBuilder* notify) {
            const auto& record = notify->Record;
            return EnqueueVersion(record.GetVersion(), record.GetStrong());
        }

        bool AckVersion(ui64 version) {
            if (LastVersionSent > version) {
                return false;
            }

            WaitForAck = false;
            return true;
        }

        bool IsWaitForAck() const {
            return WaitForAck;
        }

        void NeedStrongNotification() {
            NotifiedStrongly = false;
        }

        bool IsNotifiedStrongly() const {
            return NotifiedStrongly;
        }

        bool EnqueueSyncRequest(ui64 cookie) {
            if (cookie <= SyncRequestCookie) {
                return false;
            }

            SyncRequestCookie = cookie;
            return true;
        }

        TMaybe<ui64> ProcessSyncRequest() {
            if (SyncRequestCookie == SyncResponseCookie) {
                return Nothing();
            }

            SyncResponseCookie = SyncRequestCookie;
            return SyncResponseCookie;
        }

    private:
        const ESubscriptionType Type;
        const ui64 DomainOwnerId;
        const TCapabilities Capabilities;

        bool WaitForAck;
        ui64 LastVersionSent;
        bool NotifiedStrongly;

        ui64 SyncRequestCookie;
        ui64 SyncResponseCookie;
    };

public:
    class TDescription {
        static constexpr char MemoryLabelDescribeResult[] = "SchemeBoard/Replica/DescribeSchemeResult";

        void Notify() {
            if (!Subscribers) {
                return;
            }

            auto notify = BuildNotify();
            TVector<const TActorId*> subscribers(Reserve(Subscribers.size()));

            for (auto& [subscriber, info] : Subscribers) {
                if (!info.EnqueueVersion(notify.Get())) {
                    continue;
                }

                subscribers.push_back(&subscriber);
            }

            MultiSend(subscribers, Owner->SelfId(), std::move(notify));
        }

        void CalculateResultSize() {
            ResultSize = DescribeSchemeResult.ByteSizeLong();
        }

        size_t FullSize() const {
            size_t size = ResultSize;
            if (PreSerializedDescribeSchemeResult) {
                size += PreSerializedDescribeSchemeResult->size();
            }
            return size;
        }

        void TrackMemory() const {
            NActors::NMemory::TLabel<MemoryLabelDescribeResult>::Add(FullSize());
        }

        void UntrackMemory() const {
            NActors::NMemory::TLabel<MemoryLabelDescribeResult>::Sub(FullSize());
        }

        void Move(TDescription&& other) {
            UntrackMemory();
            other.UntrackMemory();

            Owner = other.Owner;
            Path = std::move(other.Path);
            PathId = std::move(other.PathId);
            DescribeSchemeResult = std::move(other.DescribeSchemeResult);
            PreSerializedDescribeSchemeResult = std::move(other.PreSerializedDescribeSchemeResult);
            ExplicitlyDeleted = other.ExplicitlyDeleted;
            Subscribers = std::move(other.Subscribers);

            ResultSize = other.ResultSize;
            other.ResultSize = 0;
            TrackNotify = other.TrackNotify;

            TrackMemory();
            other.TrackMemory();
        }

    public:
        explicit TDescription(TReplica* owner, const TString& path)
            : Owner(owner)
            , Path(path)
        {
            TrackMemory();
        }

        explicit TDescription(TReplica* owner, const TPathId& pathId)
            : Owner(owner)
            , PathId(pathId)
        {
            TrackMemory();
        }

        explicit TDescription(TReplica* owner, const TString& path, const TPathId& pathId)
            : Owner(owner)
            , Path(path)
            , PathId(pathId)
        {
            TrackMemory();
        }

        explicit TDescription(
                TReplica* owner,
                const TPathId& pathId,
                TDescribeSchemeResult&& describeSchemeResult)
            : Owner(owner)
            , PathId(pathId)
            , DescribeSchemeResult(std::move(describeSchemeResult))
        {
            CalculateResultSize();
            TrackMemory();
        }

        explicit TDescription(
                TReplica* owner,
                const TString& path,
                const TPathId& pathId,
                TDescribeSchemeResult&& describeSchemeResult)
            : Owner(owner)
            , Path(path)
            , PathId(pathId)
            , DescribeSchemeResult(std::move(describeSchemeResult))
        {
            CalculateResultSize();
            TrackMemory();
        }

        TDescription(TDescription&& other) {
            TrackMemory();
            Move(std::move(other));
        }

        TDescription& operator=(TDescription&& other) {
            Move(std::move(other));
            return *this;
        }

        TDescription(const TDescription& other) = delete;
        TDescription& operator=(const TDescription& other) = delete;

        ~TDescription()
        {
            UntrackMemory();
        }

        bool operator<(const TDescription& other) const {
            return GetVersion() < other.GetVersion();
        }

        bool operator>(const TDescription& other) const {
            return other < *this;
        }

        TDescription& Merge(TDescription&& other) noexcept {
            Y_VERIFY(Owner == other.Owner);

            if (!Path) {
                Path = other.Path;
            }

            Y_VERIFY_S(!other.Path || Path == other.Path, "Descriptions"
                << ": self# " << ToString()
                << ", other# " << other.ToString());

            if (!PathId) {
                PathId = other.PathId;
            }

            Y_VERIFY_S(!other.PathId || PathId == other.PathId, "Descriptions"
                << ": self# " << ToString()
                << ", other# " << other.ToString());

            SBR_LOG_T("Merge descriptions"
                << ": self# " << Owner->SelfId()
                << ", left path# " << Path
                << ", left pathId# " << PathId
                << ", left version# " << GetVersion()
                << ", rigth path# " << other.Path
                << ", rigth pathId# " << other.PathId
                << ", rigth version# " << other.GetVersion());

            UntrackMemory();
            other.UntrackMemory();
            TrackNotify = false;
            other.TrackNotify = false;

            if (*this > other) {
                other.DescribeSchemeResult.Swap(&DescribeSchemeResult);
                other.PreSerializedDescribeSchemeResult.Clear();
                other.ExplicitlyDeleted = ExplicitlyDeleted;
                other.Notify();
                DescribeSchemeResult.Swap(&other.DescribeSchemeResult);
                other.PreSerializedDescribeSchemeResult.Clear();
            } else if (*this < other) {
                DescribeSchemeResult = std::move(other.DescribeSchemeResult);
                PreSerializedDescribeSchemeResult.Clear();
                ExplicitlyDeleted = other.ExplicitlyDeleted;
                Notify();
            }

            CalculateResultSize();
            other.CalculateResultSize();

            TrackNotify = true;
            other.TrackNotify = true;
            TrackMemory();
            other.TrackMemory();

            Subscribers.insert(other.Subscribers.begin(), other.Subscribers.end());

            return *this;
        }

        const TDescribeSchemeResult& GetProto() const {
            return DescribeSchemeResult;
        }

        TString ToString() const {
            return TStringBuilder() << "{"
                << " Path# " << Path
                << " PathId# " << PathId
                << " DescribeSchemeResult# " << DescribeSchemeResult.ShortDebugString()
                << " ExplicitlyDeleted# " << (ExplicitlyDeleted ? "true" : "false")
            << " }";
        }

        const TString& GetPath() const {
            return Path;
        }

        const TPathId& GetPathId() const {
            return PathId;
        }

        bool IsExplicitlyDeleted() const {
            return ExplicitlyDeleted;
        }

        ui64 GetVersion() const {
            if (ExplicitlyDeleted) {
                return Max<ui64>();
            }

            return ::NKikimr::NSchemeBoard::GetPathVersion(DescribeSchemeResult);
        }

        TDomainId GetDomainId() const {
            return IsFilled() ? ::NKikimr::NSchemeBoard::GetDomainId(DescribeSchemeResult) : TDomainId();
        }

        TSet<ui64> GetAbandonedSchemeShardIds() const {
            return IsFilled() ? ::NKikimr::NSchemeBoard::GetAbandonedSchemeShardIds(DescribeSchemeResult) : TSet<ui64>();
        }

        bool IsFilled() const {
            return DescribeSchemeResult.ByteSizeLong();
        }

        void Clear() {
            ExplicitlyDeleted = true;
            UntrackMemory();
            TDescribeSchemeResult().Swap(&DescribeSchemeResult);
            PreSerializedDescribeSchemeResult.Clear();
            ResultSize = 0;
            TrackMemory();
            Notify();
        }

        THolder<TSchemeBoardEvents::TEvNotifyBuilder> BuildNotify(bool forceStrong = false) const {
            THolder<TSchemeBoardEvents::TEvNotifyBuilder> notify;

            const bool isDeletion = !IsFilled();

            if (!PathId) {
                Y_VERIFY(isDeletion);
                notify = MakeHolder<TSchemeBoardEvents::TEvNotifyBuilder>(Path, isDeletion);
            } else if (!Path) {
                Y_VERIFY(isDeletion);
                notify = MakeHolder<TSchemeBoardEvents::TEvNotifyBuilder>(PathId, isDeletion);
            } else {
                notify = MakeHolder<TSchemeBoardEvents::TEvNotifyBuilder>(Path, PathId, isDeletion);
            }

            if (!isDeletion) {
                if (!PreSerializedDescribeSchemeResult) {
                    TString serialized;
                    Y_PROTOBUF_SUPPRESS_NODISCARD DescribeSchemeResult.SerializeToString(&serialized);
                    if (TrackNotify) {
                        UntrackMemory();
                    }
                    PreSerializedDescribeSchemeResult = std::move(serialized);
                    if (TrackNotify) {
                        TrackMemory();
                    }
                }

                notify->SetDescribeSchemeResult(*PreSerializedDescribeSchemeResult);
            }

            notify->Record.SetVersion(GetVersion());
            if (IsFilled() || IsExplicitlyDeleted() || forceStrong) {
                notify->Record.SetStrong(true);
            }

            return notify;
        }

        void Subscribe(const TActorId& subscriber, const TString&, ui64 domainOwnerId, const TCapabilities& capabilities) {
            Subscribers.emplace(subscriber, TSubscriberInfo(SUBSCRIPTION_BY_PATH, domainOwnerId, capabilities));
        }

        void Subscribe(const TActorId& subscriber, const TPathId&, ui64 domainOwnerId, const TCapabilities& capabilities) {
            Subscribers.emplace(subscriber, TSubscriberInfo(SUBSCRIPTION_BY_PATH_ID, domainOwnerId, capabilities));
        }

        void Unsubscribe(const TActorId& subscriber) {
            Subscribers.erase(subscriber);
        }

        TSubscriberInfo& GetSubscriberInfo(const TActorId& subscriber) {
            auto it = Subscribers.find(subscriber);
            Y_VERIFY(it != Subscribers.end());
            return it->second;
        }

        THashMap<TActorId, TSubscriberInfo> GetSubscribers(const ESubscriptionType type = SUBSCRIPTION_UNSPECIFIED) const {
            THashMap<TActorId, TSubscriberInfo> result;

            for (const auto& [subscriber, info] : Subscribers) {
                if (type == SUBSCRIPTION_UNSPECIFIED || type == info.GetType()) {
                    result.emplace(subscriber, info);
                }
            }

            return result;
        }

    private:
        // used to notifications
        TReplica* Owner;

        // data
        TString Path;
        TPathId PathId;
        TDescribeSchemeResult DescribeSchemeResult;
        mutable TMaybe<TString> PreSerializedDescribeSchemeResult;
        bool ExplicitlyDeleted = false;

        // subscribers
        THashMap<TActorId, TSubscriberInfo> Subscribers;

        // memory tracking
        size_t ResultSize = 0;
        bool TrackNotify = true;

    }; // TDescription

    struct TMerger {
        TDescription& operator()(TDescription& dst, TDescription&& src) {
            return dst.Merge(std::move(src));
        }
    };

private:
    struct TPopulatorInfo {
        ui64 Generation = 0;
        ui64 PendingGeneration = 0;
        TActorId PopulatorActor;

        bool IsCommited() const {
            return Generation && Generation == PendingGeneration;
        }
    };

    bool IsPopulatorCommited(ui64 ownerId) const {
        auto it = Populators.find(ownerId);
        if (it != Populators.end() && it->second.IsCommited()) {
            return true;
        }

        return false;
    }

    template <typename TPath>
    TDescription& UpsertDescription(const TPath& path) {
        SBR_LOG_I("Upsert description"
            << ": self# " << SelfId()
            << ", path# " << path);

        return Descriptions.Upsert(path, TDescription(this, path));
    }

    TDescription& UpsertDescription(const TString& path, const TPathId& pathId) {
        SBR_LOG_I("Upsert description"
            << ": self# " << SelfId()
            << ", path# " << path
            << ", pathId# " << pathId);

        return Descriptions.Upsert(path, pathId, TDescription(this, path, pathId));
    }

    template <typename TPath>
    TDescription& UpsertDescription(const TPath path, TDescription&& description) {
        SBR_LOG_I("Upsert description"
            << ": self# " << SelfId()
            << ", path# " << path);

        return Descriptions.Upsert(path, std::move(description));
    }

    TDescription& UpsertDescription(
        const TString& path,
        const TPathId& pathId,
        TDescribeSchemeResult&& describeSchemeResult
    ) {
        SBR_LOG_I("Upsert description"
            << ": self# " << SelfId()
            << ", path# " << path
            << ", pathId# " << pathId);

        return Descriptions.Upsert(path, pathId, TDescription(this, path, pathId, std::move(describeSchemeResult)));
    }

    void SoftDeleteDescription(const TPathId& pathId, bool createIfNotExists = false) {
        TDescription* desc = Descriptions.FindPtr(pathId);

        if (!desc) {
            if (createIfNotExists) {
                desc = &UpsertDescription(pathId);
                desc->Clear(); // mark as deleted
            }

            return;
        }

        if (!desc->IsFilled()) {
            return;
        }

        auto path = desc->GetPath();

        SBR_LOG_I("Delete description"
            << ": self# " << SelfId()
            << ", path# " << path
            << ", pathId# " << pathId);

        if (TDescription* descByPath = Descriptions.FindPtr(path)) {
            if (descByPath != desc && descByPath->IsFilled()) {
                if (descByPath->GetPathId().OwnerId != pathId.OwnerId) {
                    auto curPathId = descByPath->GetPathId();
                    auto curDomainId = descByPath->GetDomainId();
                    auto domainId = desc->GetDomainId();

                    if (curDomainId == pathId) { //Deletion from GSS
                        SBR_LOG_N("Delete description by GSS"
                            << ": self# " << SelfId()
                            << ", path# " << path
                            << ", pathId# " << pathId
                            << ", domainId# " << domainId
                            << ", curPathId# " << curPathId
                            << ", curDomainId# " << curDomainId);

                        Descriptions.DeleteIndex(path);
                        UpsertDescription(path, pathId);
                        RelinkSubscribers(descByPath, path);

                        descByPath->Clear();
                    }
                }
            }
        }

        desc->Clear();
    }

    void RelinkSubscribers(TDescription* fromDesc, const TString& path) {
        for (const auto& [subscriber, info] : fromDesc->GetSubscribers(SUBSCRIPTION_BY_PATH)) {
            fromDesc->Unsubscribe(subscriber);
            Subscribers.erase(subscriber);
            SubscribeBy(subscriber, path, info.GetDomainOwnerId(), info.GetCapabilities(), false);
        }
    }

    void SoftDeleteDescriptions(const TPathId& begin, const TPathId& end) {
        const auto& pathIdIndex = Descriptions.GetSecondaryIndex();

        auto it = pathIdIndex.lower_bound(begin);
        if (it == pathIdIndex.end()) {
            return;
        }

        const auto endIt = pathIdIndex.upper_bound(end);
        while (it != endIt) {
            SoftDeleteDescription(it->first);
            ++it;
        }
    }

    // call it _after_ Subscribe() & _before_ Unsubscribe()
    bool IsSingleSubscriberOnNode(const TActorId& subscriber) const {
        const ui32 nodeId = subscriber.NodeId();
        auto it = Subscribers.lower_bound(TActorId(nodeId, 0, 0, 0));
        Y_VERIFY(it != Subscribers.end());

        return ++it == Subscribers.end() || it->first.NodeId() != nodeId;
    }

    template <typename TPath>
    void Subscribe(const TActorId& subscriber, const TPath& path, ui64 domainOwnerId, const TCapabilities& capabilities) {
        TDescription* desc = Descriptions.FindPtr(path);
        Y_VERIFY(desc);

        SBR_LOG_N("Subscribe"
            << ": self# " << SelfId()
            << ", subscriber# " << subscriber
            << ", path# " << path
            << ", domainOwnerId# " << domainOwnerId
            << ", capabilities# " << capabilities.ShortDebugString());

        desc->Subscribe(subscriber, path, domainOwnerId, capabilities);

        auto it = Subscribers.find(subscriber);
        Y_VERIFY_DEBUG(it == Subscribers.end() || std::holds_alternative<TPath>(it->second) && std::get<TPath>(it->second) == path);
        Subscribers.emplace(subscriber, path);
    }

    template <typename TPath>
    void Unsubscribe(const TActorId& subscriber, const TPath& path) {
        TDescription* desc = Descriptions.FindPtr(path);
        Y_VERIFY(desc);

        SBR_LOG_N("Unsubscribe"
            << ": self# " << SelfId()
            << ", subscriber# " << subscriber
            << ", path# " << path);

        desc->Unsubscribe(subscriber);
        Subscribers.erase(subscriber);
    }

    template <typename TPath>
    void SubscribeBy(const TActorId& subscriber, const TPath& path, ui64 domainOwnerId, const TCapabilities& capabilities,
            bool needNotify = true) {
        TDescription* desc = Descriptions.FindPtr(path);
        if (!desc) {
            desc = &UpsertDescription(path);
        }

        Subscribe(subscriber, path, domainOwnerId, capabilities);

        if (!needNotify) {
            return;
        }

        ui32 flags = 0;
        if (IsSingleSubscriberOnNode(subscriber)) {
            flags = IEventHandle::FlagSubscribeOnSession;
        }

        auto notify = desc->BuildNotify(IsPopulatorCommited(domainOwnerId));

        if (!notify->Record.GetStrong()) {
            auto& info = desc->GetSubscriberInfo(subscriber);
            info.NeedStrongNotification();

            WaitStrongNotifications[domainOwnerId].insert(subscriber);
        }

        Send(subscriber, std::move(notify), flags);
    }

    template <typename TPath>
    void UnsubscribeBy(const TActorId& subscriber, const TPath& path) {
        if (!Descriptions.FindPtr(path) || !Subscribers.contains(subscriber)) {
            return;
        }

        if (IsSingleSubscriberOnNode(subscriber)) {
            Send(MakeInterconnectProxyId(subscriber.NodeId()), new TEvents::TEvUnsubscribe());
        }

        Unsubscribe(subscriber, path);
    }

    template <typename TPath>
    ui64 GetVersion(const TPath& path) const {
        const TDescription* desc = Descriptions.FindPtr(path);
        return desc ? desc->GetVersion() : 0;
    }

    void AckUpdate(TSchemeBoardEvents::TEvUpdate::TPtr& ev) {
        const auto& record = ev->Get()->GetRecord();

        const ui64 owner = record.GetOwner();
        const ui64 generation = record.GetGeneration();

        Y_VERIFY(Populators.contains(owner));
        Y_VERIFY(Populators.at(owner).PendingGeneration == generation);

        if (!record.GetNeedAck()) {
            return;
        }

        TPathId ackPathId;

        if (record.HasDeletedLocalPathIds()) {
            ackPathId = TPathId(owner, record.GetDeletedLocalPathIds().GetEnd());
        }

        if (record.HasLocalPathId()) {
            ackPathId = ev->Get()->GetPathId();
        }

        if (record.HasMigratedLocalPathIds()) {
            ackPathId = TPathId(owner, record.GetMigratedLocalPathIds().GetEnd());
        }

        const ui64 version = GetVersion(ackPathId);
        Send(ev->Sender, new TSchemeBoardEvents::TEvUpdateAck(owner, generation, ackPathId, version), 0, ev->Cookie);
    }

    void Handle(TSchemeBoardEvents::TEvHandshakeRequest::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        const ui64 owner = record.GetOwner();
        const ui64 generation = record.GetGeneration();

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvHandshakeRequest"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", owner# " << owner
            << ", generation# " << generation);

        TPopulatorInfo& info = Populators[owner];
        if (generation < info.PendingGeneration) {
            SBR_LOG_E("Reject handshake from stale populator"
                << ": self# " << SelfId()
                << ", sender# " << ev->Sender
                << ", owner# " << owner
                << ", generation# " << generation
                << ", pending generation# " << info.PendingGeneration);
            return;
        }

        SBR_LOG_N("Successful handshake"
            << ": self# " << SelfId()
            << ", owner# " << owner
            << ", generation# " << generation);

        info.PendingGeneration = generation;
        info.PopulatorActor = ev->Sender;

        Send(ev->Sender, new TSchemeBoardEvents::TEvHandshakeResponse(owner, info.Generation), 0, ev->Cookie);
    }

    void Handle(TSchemeBoardEvents::TEvUpdate::TPtr& ev) {
        auto& record = *ev->Get()->MutableRecord();

        const ui64 owner = record.GetOwner();
        const ui64 generation = record.GetGeneration();

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvUpdate"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", cookie# " << ev->Cookie
            << ", owner# " << owner
            << ", generation# " << generation);
        SBR_LOG_T("Message:\n" << ev->Get()->ToString().substr(0, 10000));

        const auto populatorIt = Populators.find(owner);
        if (populatorIt == Populators.end()) {
            SBR_LOG_E("Reject update from unknown populator"
                << ": self# " << SelfId()
                << ", sender# " << ev->Sender
                << ", owner# " << owner
                << ", generation# " << generation);
            return;
        }
        if (generation != populatorIt->second.PendingGeneration) {
            SBR_LOG_E("Reject update from stale populator"
                << ": self# " << SelfId()
                << ", sender# " << ev->Sender
                << ", owner# " << owner
                << ", generation# " << generation
                << ", pending generation# " << populatorIt->second.PendingGeneration);
            return;
        }

        if (record.HasDeletedLocalPathIds()) {
            const TPathId begin(owner, record.GetDeletedLocalPathIds().GetBegin());
            const TPathId end(owner, record.GetDeletedLocalPathIds().GetEnd());
            SoftDeleteDescriptions(begin, end);
        }

        if (!record.HasLocalPathId()) {
            return AckUpdate(ev);
        }

        const TString& path = record.GetPath();
        const TPathId pathId = ev->Get()->GetPathId();

        SBR_LOG_N("Update description"
            << ": self# " << SelfId()
            << ", path# " << path
            << ", pathId# " << pathId
            << ", deletion# " << (record.GetIsDeletion() ? "true" : "false"));

        if (record.GetIsDeletion()) {
            SoftDeleteDescription(pathId, true);
            return AckUpdate(ev);
        }

        if (TDescription* desc = Descriptions.FindPtr(pathId)) {
            if (desc->IsExplicitlyDeleted()) {
                SBR_LOG_N("Path was explicitly deleted, ignoring"
                    << ": self# " << SelfId()
                    << ", path# " << path
                    << ", pathId# " << pathId);

                return AckUpdate(ev);
            }
        }

        TDescription* desc = Descriptions.FindPtr(path);
        if (!desc) {
            UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
            return AckUpdate(ev);
        }

        if (!desc->GetPathId()) {
            UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
            return AckUpdate(ev);
        }

        auto curPathId = desc->GetPathId();

        if (curPathId.OwnerId == pathId.OwnerId || !desc->IsFilled()) {
            if (curPathId > pathId) {
                return AckUpdate(ev);
            }

            if (curPathId < pathId) {
                SoftDeleteDescription(desc->GetPathId());
                Descriptions.DeleteIndex(path);
                RelinkSubscribers(desc, path);
            }

            UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
            return AckUpdate(ev);
        }

        Y_VERIFY_S(desc->IsFilled(), "desc :"
            << ": path# " << desc->GetPath()
            << ", pathId# " << desc->GetPathId()
            << ", domainId# " << desc->GetDomainId()
            << ", version# " << desc->GetVersion());

        auto curDomainId = desc->GetDomainId();
        auto domainId = GetDomainId(record.GetDescribeSchemeResult());

        if (curPathId == domainId) { //Update from TSS, GSS->TSS

            // it is only because we need to manage undo of upgrade subdomain, finally remove it
            auto abandonedSchemeShards = desc->GetAbandonedSchemeShardIds();
            if (abandonedSchemeShards.contains(pathId.OwnerId)) { //TSS is ignored, present GSS reverted it
                SBR_LOG_N("Replace GSS by TSS description is rejected, GSS implicitly knows that TSS has been reverted"
                    ", but still inject description only by pathId for safe"
                    << ": self# " << SelfId()
                    << ", path# " << path
                    << ", pathId# " << pathId
                    << ", domainId# " << domainId
                    << ", curPathId# " << curPathId
                    << ", curDomainId# " << curDomainId);
                UpsertDescription(pathId, TDescription(this, path, pathId, std::move(*record.MutableDescribeSchemeResult())));
                return AckUpdate(ev);
            }

            SBR_LOG_N("Replace GSS by TSS description"
                << ": self# " << SelfId()
                << ", path# " << path
                << ", pathId# " << pathId
                << ", domainId# " << domainId
                << ", curPathId# " << curPathId
                << ", curDomainId# " << curDomainId);
            //unlick GSS desc by path
            Descriptions.DeleteIndex(path);
            RelinkSubscribers(desc, path);
            UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
            return AckUpdate(ev);
        }

        if (curDomainId == pathId) { //Update from GSS, TSS->GSS

            // it is only because we need to manage undo of upgrade subdomain, finally remove it
            auto abandonedSchemeShards = GetAbandonedSchemeShardIds(record.GetDescribeSchemeResult());
            if (abandonedSchemeShards.contains(curPathId.OwnerId)) { //GSS reverts TSS
                SBR_LOG_N("Replace TSS by GSS description, TSS was implicitly reverted by GSS"
                    << ": self# " << SelfId()
                    << ", path# " << path
                    << ", pathId# " << pathId
                    << ", domainId# " << domainId
                    << ", curPathId# " << curPathId
                    << ", curDomainId# " << curDomainId);
                //unlick TSS desc by path
                Descriptions.DeleteIndex(path);
                RelinkSubscribers(desc, path);
                UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
                return AckUpdate(ev);
            }

            SBR_LOG_N("Inject description only by pathId, it is update from GSS"
                << ": self# " << SelfId()
                << ", path# " << path
                << ", pathId# " << pathId
                << ", domainId# " << domainId
                << ", curPathId# " << curPathId
                << ", curDomainId# " << curDomainId);
            UpsertDescription(pathId, TDescription(this, path, pathId, std::move(*record.MutableDescribeSchemeResult())));
            return AckUpdate(ev);
        }

        if (curDomainId == domainId) {
            if (curPathId > pathId) {
                SBR_LOG_N("Totally ignore description, path with obsolete pathId"
                          << ": self# " << SelfId()
                          << ", path# " << path
                          << ", pathId# " << pathId
                          << ", domainId# " << domainId
                          << ", curPathId# " << curPathId
                          << ", curDomainId# " << curDomainId);
                return AckUpdate(ev);
            }

            if (curPathId < pathId) {
                SBR_LOG_N("Update description by newest path form tenant schemeshard"
                          << ": self# " << SelfId()
                          << ", path# " << path
                          << ", pathId# " << pathId
                          << ", domainId# " << domainId
                          << ", curPathId# " << curPathId
                          << ", curDomainId# " << curDomainId);

                SoftDeleteDescription(desc->GetPathId());
                Descriptions.DeleteIndex(path);
                RelinkSubscribers(desc, path);
            }

            UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
            return AckUpdate(ev);
        } else if (curDomainId < domainId) {
            SBR_LOG_N("Update description by newest path with newer domainId"
                << ": self# " << SelfId()
                << ", path# " << path
                << ", pathId# " << pathId
                << ", domainId# " << domainId
                << ", curPathId# " << curPathId
                << ", curDomainId# " << curDomainId);
            Descriptions.DeleteIndex(path);
            RelinkSubscribers(desc, path);
            UpsertDescription(path, pathId, std::move(*record.MutableDescribeSchemeResult()));
            return AckUpdate(ev);
        } else {
            SBR_LOG_N("Totally ignore description, path with obsolete domainId"
                << ": self# " << SelfId()
                << ", path# " << path
                << ", pathId# " << pathId
                << ", domainId# " << domainId
                << ", curPathId# " << curPathId
                << ", curDomainId# " << curDomainId);
            return AckUpdate(ev);
        }

        Y_FAIL_S("Can't insert old description, no relation between obj"
            << ": self# " << SelfId()
            << ", path# " << path
            << ", pathId# " << pathId
            << ", domainId# " << domainId
            << ", curPathId# " << curPathId
            << ", curDomainId# " << curDomainId);
    }

    void Handle(TSchemeBoardEvents::TEvCommitRequest::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        const ui64 owner = record.GetOwner();
        const ui64 generation = record.GetGeneration();

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvCommitRequest"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", owner# " << owner
            << ", generation# " << generation);

        auto it = Populators.find(owner);
        if (it == Populators.end()) {
            SBR_LOG_E("Reject commit from unknown populator"
                << ": self# " << SelfId()
                << ", sender# " << ev->Sender
                << ", owner# " << owner
                << ", generation# " << generation);
            return;
        }

        TPopulatorInfo& info = it->second;
        if (generation != info.PendingGeneration) {
            SBR_LOG_E("Reject commit from stale populator"
                << ": self# " << SelfId()
                << ", sender# " << ev->Sender
                << ", owner# " << owner
                << ", generation# " << generation
                << ", pending generation# " << info.PendingGeneration);
            return;
        }

        SBR_LOG_N("Commit generation"
            << ": self# " << SelfId()
            << ", owner# " << owner
            << ", generation# " << generation);

        info.Generation = info.PendingGeneration;

        Send(ev->Sender, new TSchemeBoardEvents::TEvCommitResponse(owner, info.Generation), 0, ev->Cookie);
        Send(SelfId(), new TEvPrivate::TEvSendStrongNotifications(owner));
    }

    void Handle(TEvPrivate::TEvSendStrongNotifications::TPtr& ev) {
        const auto limit = ev->Get()->BatchSize;
        const auto owner = ev->Get()->Owner;

        SBR_LOG_D("Handle TEvPrivate::TEvSendStrongNotifications"
            << ": self# " << SelfId()
            << ", owner# " << owner);

        if (!IsPopulatorCommited(owner)) {
            SBR_LOG_N("Populator is not commited"
                << ": self# " << SelfId()
                << ", owner# " << owner);
            return;
        }

        auto itSubscribers = WaitStrongNotifications.find(owner);
        if (itSubscribers == WaitStrongNotifications.end()) {
            SBR_LOG_E("Invalid owner"
                << ": self# " << SelfId()
                << ", owner# " << owner);
            return;
        }

        auto& subscribers = itSubscribers->second;
        auto it = subscribers.begin();
        ui32 count = 0;

        while (count++ < limit && it != subscribers.end()) {
            const TActorId subscriber = *it;
            it = subscribers.erase(it);

            auto jt = Subscribers.find(subscriber);
            if (jt == Subscribers.end()) {
                continue;
            }

            TDescription* desc = nullptr;

            if (const TString* path = std::get_if<TString>(&jt->second)) {
                desc = Descriptions.FindPtr(*path);
            } else if (const TPathId* pathId = std::get_if<TPathId>(&jt->second)) {
                desc = Descriptions.FindPtr(*pathId);
            }

            Y_VERIFY(desc);
            auto& info = desc->GetSubscriberInfo(subscriber);

            Y_VERIFY(info.GetDomainOwnerId() == owner);
            if (info.IsNotifiedStrongly() || info.IsWaitForAck()) {
                continue;
            }

            auto notify = desc->BuildNotify(true);
            info.EnqueueVersion(notify.Get());
            Send(subscriber, std::move(notify));
        }

        if (subscribers) {
            Send(SelfId(), new TEvPrivate::TEvSendStrongNotifications(owner));
        } else {
            WaitStrongNotifications.erase(itSubscribers);
        }
    }

    void Handle(TSchemeBoardEvents::TEvSubscribe::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvSubscribe"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", record# " << record.ShortDebugString());

        const ui64 domainOwnerId = record.GetDomainOwnerId();
        const auto& capabilities = record.GetCapabilities();

        if (record.HasPath()) {
            SubscribeBy(ev->Sender, record.GetPath(), domainOwnerId, capabilities);
        } else {
            Y_VERIFY(record.HasPathOwnerId() && record.HasLocalPathId());
            SubscribeBy(ev->Sender, TPathId(record.GetPathOwnerId(), record.GetLocalPathId()), domainOwnerId, capabilities);
        }
    }

    void Handle(TSchemeBoardEvents::TEvUnsubscribe::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvUnsubscribe"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", record# " << record.ShortDebugString());

        if (record.HasPath()) {
            UnsubscribeBy(ev->Sender, record.GetPath());
        } else {
            Y_VERIFY(record.HasPathOwnerId() && record.HasLocalPathId());
            UnsubscribeBy(ev->Sender, TPathId(record.GetPathOwnerId(), record.GetLocalPathId()));
        }
    }

    void Handle(TSchemeBoardEvents::TEvNotifyAck::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvNotifyAck"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", record# " << record.ShortDebugString());

        auto it = Subscribers.find(ev->Sender);
        if (it == Subscribers.end()) {
            return;
        }

        TDescription* desc = nullptr;

        if (const TString* path = std::get_if<TString>(&it->second)) {
            desc = Descriptions.FindPtr(*path);
        } else if (const TPathId* pathId = std::get_if<TPathId>(&it->second)) {
            desc = Descriptions.FindPtr(*pathId);
        }

        Y_VERIFY(desc);
        auto& info = desc->GetSubscriberInfo(ev->Sender);

        const ui64 version = record.GetVersion();
        if (!info.AckVersion(version)) {
            return;
        }

        if (version < desc->GetVersion()) {
            auto notify = desc->BuildNotify(IsPopulatorCommited(info.GetDomainOwnerId()));
            info.EnqueueVersion(notify.Get());
            Send(ev->Sender, std::move(notify));
        }

        if (auto cookie = info.ProcessSyncRequest()) {
            Send(ev->Sender, new TSchemeBoardEvents::TEvSyncVersionResponse(desc->GetVersion()), 0, *cookie);
        }
    }

    void Handle(TSchemeBoardEvents::TEvSyncVersionRequest::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        SBR_LOG_D("Handle TSchemeBoardEvents::TEvSyncVersionRequest"
            << ": self# " << SelfId()
            << ", sender# " << ev->Sender
            << ", cookie# " << ev->Cookie
            << ", record# " << record.ShortDebugString());

        auto it = Subscribers.find(ev->Sender);
        if (it == Subscribers.end()) {
            // for backward compatability
            ui64 version = 0;

            if (record.HasPath()) {
                version = GetVersion(record.GetPath());
            } else {
                version = GetVersion(TPathId(record.GetPathOwnerId(), record.GetLocalPathId()));
            }

            Send(ev->Sender, new TSchemeBoardEvents::TEvSyncVersionResponse(version), 0, ev->Cookie);
            return;
        }

        TDescription* desc = nullptr;

        if (const TString* path = std::get_if<TString>(&it->second)) {
            desc = Descriptions.FindPtr(*path);
        } else if (const TPathId* pathId = std::get_if<TPathId>(&it->second)) {
            desc = Descriptions.FindPtr(*pathId);
        }

        Y_VERIFY(desc);
        auto& info = desc->GetSubscriberInfo(ev->Sender);

        if (!info.EnqueueSyncRequest(ev->Cookie) || info.IsWaitForAck()) {
            return;
        }

        auto cookie = info.ProcessSyncRequest();
        Y_VERIFY(cookie && *cookie == ev->Cookie);

        Send(ev->Sender, new TSchemeBoardEvents::TEvSyncVersionResponse(desc->GetVersion()), 0, *cookie);
    }

    void Handle(TSchemeBoardMonEvents::TEvInfoRequest::TPtr& ev) {
        const auto limit = ev->Get()->Record.GetLimitRepeatedFields();

        auto response = MakeHolder<TSchemeBoardMonEvents::TEvInfoResponse>(SelfId(), ActorActivityType());
        auto& record = *response->Record.MutableReplicaResponse();

        for (const auto& [owner, populator] : Populators) {
            auto& info = *record.AddPopulators();

            info.SetOwner(owner);
            info.SetGeneration(populator.Generation);
            info.SetPendingGeneration(populator.PendingGeneration);
            ActorIdToProto(populator.PopulatorActor, info.MutableActorId());

            if (record.PopulatorsSize() >= limit) {
                response->SetTruncated();
                break;
            }
        }

        record.MutableDescriptions()->SetTotalCount(Descriptions.Size());
        record.MutableDescriptions()->SetByPathCount(Descriptions.GetPrimaryIndex().size());
        record.MutableDescriptions()->SetByPathIdCount(Descriptions.GetSecondaryIndex().size());

        for (const auto& [subscriber, id] : Subscribers) {
            auto& info = *record.AddSubscribers();

            ActorIdToProto(subscriber, info.MutableActorId());
            if (const TString* path = std::get_if<TString>(&id)) {
                info.SetPath(*path);
            } else if (const TPathId* pathId = std::get_if<TPathId>(&id)) {
                info.MutablePathId()->SetOwnerId(pathId->OwnerId);
                info.MutablePathId()->SetLocalPathId(pathId->LocalPathId);
            }

            if (record.SubscribersSize() >= limit) {
                response->SetTruncated();
                break;
            }
        }

        Send(ev->Sender, std::move(response), 0, ev->Cookie);
    }

    void Handle(TSchemeBoardMonEvents::TEvDescribeRequest::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        TDescription* desc = nullptr;
        if (record.HasPath()) {
            desc = Descriptions.FindPtr(record.GetPath());
        } else if (record.HasPathId()) {
            desc = Descriptions.FindPtr(TPathId(record.GetPathId().GetOwnerId(), record.GetPathId().GetLocalPathId()));
        }

        TString json;
        if (desc) {
            using namespace google::protobuf::util;

            JsonPrintOptions opts;
            opts.preserve_proto_field_names = true;
            MessageToJsonString(desc->GetProto(), &json, opts);
        } else {
            json = "{}";
        }

        Send(ev->Sender, new TSchemeBoardMonEvents::TEvDescribeResponse(json), 0, ev->Cookie);
    }

    void Handle(TEvInterconnect::TEvNodeDisconnected::TPtr& ev) {
        const ui32 nodeId = ev->Get()->NodeId;

        SBR_LOG_D("Handle TEvInterconnect::TEvNodeDisconnected"
            << ": self# " << SelfId()
            << ", nodeId# " << nodeId);

        auto it = Subscribers.lower_bound(TActorId(nodeId, 0, 0, 0));
        while (it != Subscribers.end() && it->first.NodeId() == nodeId) {
            const TActorId subscriber = it->first;
            const auto id = it->second;
            ++it;

            if (const TString* path = std::get_if<TString>(&id)) {
                Unsubscribe(subscriber, *path);
            } else if (const TPathId* pathId = std::get_if<TPathId>(&id)) {
                Unsubscribe(subscriber, *pathId);
            }
        }

        Send(MakeInterconnectProxyId(nodeId), new TEvents::TEvUnsubscribe());
    }

    void PassAway() override {
        for (auto &xpair : Populators) {
            if (const TActorId populator = xpair.second.PopulatorActor) {
                Send(populator, new TEvStateStorage::TEvReplicaShutdown());
            }
        }

        for (auto &xpair : Subscribers) {
            Send(xpair.first, new TEvStateStorage::TEvReplicaShutdown());
        }

        TMonitorableActor::PassAway();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::SCHEME_BOARD_REPLICA_ACTOR;
    }

    void Bootstrap() {
        TMonitorableActor::Bootstrap();
        Become(&TThis::StateWork);
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TSchemeBoardEvents::TEvHandshakeRequest, Handle);
            hFunc(TSchemeBoardEvents::TEvUpdate, Handle);
            hFunc(TSchemeBoardEvents::TEvCommitRequest, Handle);
            hFunc(TEvPrivate::TEvSendStrongNotifications, Handle);
            hFunc(TSchemeBoardEvents::TEvSubscribe, Handle);
            hFunc(TSchemeBoardEvents::TEvUnsubscribe, Handle);
            hFunc(TSchemeBoardEvents::TEvNotifyAck, Handle);
            hFunc(TSchemeBoardEvents::TEvSyncVersionRequest, Handle);

            hFunc(TSchemeBoardMonEvents::TEvInfoRequest, Handle);
            hFunc(TSchemeBoardMonEvents::TEvDescribeRequest, Handle);

            hFunc(TEvInterconnect::TEvNodeDisconnected, Handle);
            cFunc(TEvents::TEvPoison::EventType, PassAway);
        }
    }

private:
    THashMap<ui64, TPopulatorInfo> Populators;
    TDoubleIndexedMap<TString, TPathId, TDescription, TMerger, THashMap, TMap> Descriptions;
    TMap<TActorId, std::variant<TString, TPathId>, TActorId::TOrderedCmp> Subscribers;
    THashMap<ui64, TSet<TActorId>> WaitStrongNotifications;

}; // TReplica

} // NSchemeBoard

IActor* CreateSchemeBoardReplica(const TIntrusivePtr<TStateStorageInfo>&, ui32) {
    return new NSchemeBoard::TReplica();
}

} // NKikimr
