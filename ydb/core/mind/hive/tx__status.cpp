#include "hive_impl.h"
#include "hive_log.h"

namespace NKikimr {
namespace NHive {

class TTxStatus : public TTransactionBase<THive> {
    TActorId Local;
    NKikimrLocal::TEvStatus Record;

public:
    TTxStatus(const TActorId& local, NKikimrLocal::TEvStatus record, THive* hive)
        : TBase(hive)
        , Local(local)
        , Record(std::move(record))
    {}

    TTxType GetTxType() const override { return NHive::TXTYPE_STATUS; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        TNodeId nodeId = Local.NodeId();
        BLOG_D("THive::TTxStatus(" << nodeId << ")::Execute");
        TEvLocal::TEvStatus::EStatus status = (TEvLocal::TEvStatus::EStatus)Record.GetStatus();
        TNodeInfo& node = Self->GetNode(nodeId);
        if (status == TEvLocal::TEvStatus::StatusOk && node.BecomeConnected()) {
            node.Local = Local;
            node.UpdateResourceMaximum(Record.GetResourceMaximum());
            if (Record.HasStartTime()) {
                node.StartTime = TInstant::MicroSeconds(Record.GetStartTime());
            }
            if (node.LocationAcquired) {
                NIceDb::TNiceDb db(txc.DB);
                NActorsInterconnect::TNodeLocation location;
                node.Location.Serialize(&location, false);
                db.Table<Schema::Node>().Key(nodeId).Update<Schema::Node::Location>(location);
                Self->UpdateRegisteredDataCenters(node.Location.GetDataCenterId());
            }
            Self->ProcessWaitQueue(); // new node connected
            if (node.Drain && Self->BalancerNodes.count(nodeId) == 0) {
                BLOG_D("THive::TTxStatus(" << nodeId << ")::Complete - continuing node drain");
                Self->StartHiveDrain(nodeId, {.Persist = true, .KeepDown = node.Down});
            }
        } else {
            BLOG_W("THive::TTxStatus(status=" << static_cast<int>(status)
                   << " node=" << TNodeInfo::EVolatileStateName(node.GetVolatileState()) << ") - killing node " << node.Id);
            Self->KillNode(node.Id, Local);
        }
        return true;
    }

    void Complete(const TActorContext&) override {
        TNodeId nodeId = Local.NodeId();
        BLOG_D("THive::TTxStatus(" << nodeId << ")::Complete");
    }
};

ITransaction* THive::CreateStatus(const TActorId& local, NKikimrLocal::TEvStatus rec) {
    return new TTxStatus(local, std::move(rec), this);
}

} // NHive
} // NKikimr
