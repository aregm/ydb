#pragma once
#include "defs.h"

namespace NKikimr {

void SendVDiskResponse(const TActorContext &ctx, const TActorId &recipient, IEventBase *ev, const IActor& actor,
        ui64 cookie);

void SendVDiskResponse(const TActorContext &ctx, const TActorId &recipient, IEventBase *ev, const IActor& actor,
        ui64 cookie, ui32 channel);

}//NKikimr
