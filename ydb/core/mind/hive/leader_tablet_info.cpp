#include "hive_impl.h"

namespace NKikimr {
namespace NHive {

TString TLeaderTabletInfo::DEFAULT_STORAGE_POOL_NAME = "default";

TPathId TLeaderTabletInfo::GetTenant() const {
    // todo: must be explicit TenantPathId
    if (!ObjectDomain)
        return TPathId();

    return TPathId(ObjectDomain.GetSchemeShard(), ObjectDomain.GetPathId());
}

bool TLeaderTabletInfo::IsSomeoneAliveOnNode(TNodeId nodeId) const {
    if (CanBeAlive() && Node->Id == nodeId) {
        return true;
    }
    for (const TTabletInfo& follower : Followers) {
        if (follower.CanBeAlive() && follower.Node->Id == nodeId)
            return true;
    }
    return false;
}

ui32 TLeaderTabletInfo::GetFollowersAliveOnDataCenter(TDataCenterId dataCenterId) const {
    ui32 followers = 0;
    for (const TTabletInfo& follower : Followers) {
        if (follower.CanBeAlive() && follower.Node->Location.GetDataCenterId() == dataCenterId) {
            ++followers;
        }
    }
    return followers;
}

ui32 TLeaderTabletInfo::GetFollowersAliveOnDataCenterExcludingFollower(TDataCenterId dataCenterId, const TTabletInfo& excludingFollower) const {
    ui32 followers = 0;
    for (const TTabletInfo& follower : Followers) {
        if (follower == excludingFollower)
            continue;
        if (follower.CanBeAlive() && follower.Node->Location.GetDataCenterId() == dataCenterId) {
            ++followers;
        }
    }
    return followers;
}

bool TLeaderTabletInfo::IsFollowerPromotableOnNode(TNodeId nodeId) const {
    for (const TFollowerTabletInfo& follower : Followers) {
        if (follower.IsRunning() && follower.NodeId == nodeId && follower.FollowerGroup.AllowLeaderPromotion)
            return true;
    }
    return false;
}

TFollowerId TLeaderTabletInfo::GetFollowerPromotableOnNode(TNodeId nodeId) const {
    for (const TFollowerTabletInfo& follower : Followers) {
        if (follower.IsRunning() && follower.NodeId == nodeId && follower.FollowerGroup.AllowLeaderPromotion)
            return follower.Id;
    }
    return 0;
}

void TLeaderTabletInfo::AssignDomains(const TSubDomainKey& objectDomain, const TVector<TSubDomainKey>& allowedDomains) {
    if (!allowedDomains.empty()) {
        EffectiveAllowedDomains = allowedDomains;
        if (!objectDomain) {
            ObjectDomain = allowedDomains.front();
        } else {
            ObjectDomain = objectDomain;
        }
    } else if (objectDomain) {
        EffectiveAllowedDomains = { objectDomain };
        ObjectDomain = objectDomain;
    } else  {
        EffectiveAllowedDomains = { Hive.GetRootDomainKey() };
        ObjectDomain = { Hive.GetRootDomainKey() };
    }
}

bool TLeaderTabletInfo::InitiateAssignTabletGroups() {
    Hive.AssignTabletGroups(*this);
    return true;
}

bool TLeaderTabletInfo::InitiateBlockStorage(TSideEffects& sideEffects) {
    // attempt to kill tablet before blocking the storage group
    Kill(sideEffects);
    // blocks PREVIOUS entry of tablet history
    IActor* x = CreateTabletReqBlockBlobStorage(Hive.SelfId(), TabletStorageInfo.Get(), KnownGeneration, true);
    sideEffects.Register(x);
    return true;
}

bool TLeaderTabletInfo::InitiateBlockStorage(TSideEffects& sideEffects, ui32 generation) {
    // attempt to kill tablet before blocking the storage group
    Kill(sideEffects);
    // blocks LATEST entry of tablet history
    const TTabletChannelInfo* channel = TabletStorageInfo->ChannelInfo(0);
    if (IsDeleting() && channel == nullptr) {
        return false;
    }
    Y_VERIFY(channel != nullptr && !channel->History.empty());
    IActor* x = CreateTabletReqBlockBlobStorage(Hive.SelfId(), TabletStorageInfo.Get(), generation, false);
    sideEffects.Register(x);
    return true;
}

bool TLeaderTabletInfo::InitiateDeleteStorage(TSideEffects& sideEffects) {
    IActor* x = CreateTabletReqDelete(Hive.SelfId(), TabletStorageInfo);
    sideEffects.Register(x);
    return true;
}

TFollowerTabletInfo& TLeaderTabletInfo::AddFollower(TFollowerGroup& followerGroup, TFollowerId followerId) {
    Followers.emplace_back(*this, followerId, followerGroup);
    TFollowerTabletInfo& follower = Followers.back();
    if (followerId == 0) {
        follower.Id = GenerateFollowerId();
    } else {
        follower.Id = followerId;
    }
    Hive.UpdateCounterTabletsTotal(+1);
    return follower;
}

TFollowerGroupId TLeaderTabletInfo::GenerateFollowerGroupId() const {
    return GenerateId(FollowerGroups);
}

TFollowerGroup& TLeaderTabletInfo::AddFollowerGroup(TFollowerGroupId followerGroupId) {
    FollowerGroups.emplace_back();
    TFollowerGroup& followerGroup = FollowerGroups.back();
    if (followerGroupId == 0) {
        followerGroup.Id = GenerateFollowerGroupId();
    } else {
        followerGroup.Id = followerGroupId;
    }
    return followerGroup;
}

TActorId TLeaderTabletInfo::SetLockedToActor(const TActorId& actor, const TDuration& timeout) {
    TActorId previousOwner = LockedToActor;
    if (LockedToActor != actor) {
        if (LockedToActor.NodeId() != actor.NodeId()) {
            if (LockedToActor) {
                TNodeId oldNodeId = LockedToActor.NodeId();
                Y_VERIFY(oldNodeId != 0, "Unexpected oldNodeId == 0");
                Hive.GetNode(oldNodeId).LockedTablets.erase(this);
            }
            if (actor) {
                TNodeId newNodeId = actor.NodeId();
                Y_VERIFY(newNodeId != 0, "Unexpected newNodeId == 0");
                Hive.GetNode(newNodeId).LockedTablets.insert(this);
            }
        }
        LockedToActor = actor;
    }
    LockedReconnectTimeout = timeout;
    PendingUnlockSeqNo = 0;
    return previousOwner;
}

void TLeaderTabletInfo::AcquireAllocationUnits() {
    for (ui32 channel = 0; channel < TabletStorageInfo->Channels.size(); ++channel) {
        AcquireAllocationUnit(channel);
    }
}

void TLeaderTabletInfo::ReleaseAllocationUnits() {
    for (ui32 channel = 0; channel < TabletStorageInfo->Channels.size(); ++channel) {
        ReleaseAllocationUnit(channel);
    }
}

bool TLeaderTabletInfo::AcquireAllocationUnit(ui32 channelId) {
    if (channelId < TabletStorageInfo->Channels.size()) {
        const TTabletChannelInfo& channel = TabletStorageInfo->Channels[channelId];
        if (!channel.History.empty()) {
            TStoragePoolInfo& storagePool = Hive.GetStoragePool(GetChannelStoragePoolName(channel));
            return storagePool.AcquireAllocationUnit(this, channel.Channel, channel.History.back().GroupID);
        }
    }
    return false;
}

bool TLeaderTabletInfo::ReleaseAllocationUnit(ui32 channelId) {
    if (channelId < TabletStorageInfo->Channels.size()) {
        const TTabletChannelInfo& channel = TabletStorageInfo->Channels[channelId];
        if (!channel.History.empty()) {
            TStoragePoolInfo& storagePool = Hive.GetStoragePool(GetChannelStoragePoolName(channel));
            return storagePool.ReleaseAllocationUnit(this, channel.Channel, channel.History.back().GroupID);
        }
    }
    return false;
}

const NKikimrBlobStorage::TEvControllerSelectGroupsResult::TGroupParameters* TLeaderTabletInfo::FindFreeAllocationUnit(ui32 channelId) {
    TStoragePoolInfo* storagePool = Hive.FindStoragePool(GetChannelStoragePoolName(channelId));
    if (storagePool != nullptr) {
        THolder<NKikimrBlobStorage::TEvControllerSelectGroups::TGroupParameters> params = Hive.BuildGroupParametersForChannel(*this, channelId);
        const TStorageGroupInfo* currentGroup = nullptr;

        // searching for last change of this channel
        if (TabletStorageInfo && TabletStorageInfo->Channels.size() > channelId && !TabletStorageInfo->Channels[channelId].History.empty()) {
            TTabletChannelInfo::THistoryEntry& lastHistory = TabletStorageInfo->Channels[channelId].History.back();
            auto itGroup = storagePool->Groups.find(lastHistory.GroupID);
            if (itGroup != storagePool->Groups.end()) {
                currentGroup = &itGroup->second;
            }
        }

        switch (ChannelProfileReassignReason) {
            default:
            case NKikimrHive::TEvReassignTablet::HIVE_REASSIGN_REASON_NO: {
                return storagePool->FindFreeAllocationUnit([params = *params, currentGroup](const TStorageGroupInfo& newGroup) -> bool {
                    if (newGroup.IsMatchesParameters(params)) {
                        if (currentGroup) {
                            return newGroup.Id != currentGroup->Id;
                        }
                        return true;
                    }
                    return false;
                });
                break;
            }
            case NKikimrHive::TEvReassignTablet::HIVE_REASSIGN_REASON_SPACE: {
                NKikimrConfig::THiveConfig::EHiveStorageBalanceStrategy balanceStrategy = Hive.CurrentConfig.GetStorageBalanceStrategy();
                Hive.CurrentConfig.SetStorageBalanceStrategy(NKikimrConfig::THiveConfig::HIVE_STORAGE_BALANCE_STRATEGY_SIZE);
                auto result = storagePool->FindFreeAllocationUnit([params = *params, currentGroup](const TStorageGroupInfo& newGroup) -> bool {
                    if (newGroup.IsMatchesParameters(params)) {
                        if (currentGroup) {
                            return newGroup.Id != currentGroup->Id
                                    && newGroup.GroupParameters.GetAvailableSize() > currentGroup->GroupParameters.GetAvailableSize();
                        }
                        return true;
                    }
                    return false;
                });
                Hive.CurrentConfig.SetStorageBalanceStrategy(balanceStrategy);
                return result;
                break;
            }
        }
    }
    return nullptr;
}

TString TLeaderTabletInfo::GetChannelStoragePoolName(const TTabletChannelInfo& channel) {
    return channel.StoragePool.empty() ? DEFAULT_STORAGE_POOL_NAME : channel.StoragePool;
}

TString TLeaderTabletInfo::GetChannelStoragePoolName(const TChannelProfiles::TProfile::TChannel& channel) {
    return channel.PoolKind.empty() ? DEFAULT_STORAGE_POOL_NAME : channel.PoolKind;
}

TString TLeaderTabletInfo::GetChannelStoragePoolName(ui32 channelId) {
    if (BoundChannels.size() > channelId) {
        return BoundChannels[channelId].GetStoragePoolName();
    }
    if (TabletStorageInfo && TabletStorageInfo->Channels.size() > channelId) {
        return GetChannelStoragePoolName(TabletStorageInfo->Channels[channelId]);
    }
    return DEFAULT_STORAGE_POOL_NAME;
}

TStoragePoolInfo& TLeaderTabletInfo::GetStoragePool(ui32 channelId) {
    TStoragePoolInfo& storagePool = Hive.GetStoragePool(GetChannelStoragePoolName(channelId));
    return storagePool;
}

void TLeaderTabletInfo::ActualizeTabletStatistics(TInstant now) {
    TTabletInfo::ActualizeTabletStatistics(now);
    for (TTabletInfo& follower : Followers) {
        follower.ActualizeTabletStatistics(now);
    }
}

}
}
