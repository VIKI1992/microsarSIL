/*
 * SilOs_Ioc.h - helpers behind the generated IOC API (IocWrite_<ch>/IocRead_<ch>/IocSend/IocReceive).
 */
#ifndef SILOS_IOC_H
#define SILOS_IOC_H

#include "Os_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *buffer;        /* numSlots * elementSize bytes */
    uint32 elementSize;
    uint32 numSlots;     /* 1 for unqueued channels */
    uint32 head;         /* queued: read index */
    uint32 count;        /* queued: filled slots; unqueued: 1 once written */
    uint8 lostData;
    uint8 initialized;
} SilOs_IocChannel;

Std_ReturnType SilOs_IocWriteUnqueued(SilOs_IocChannel *ch, const void *data);
Std_ReturnType SilOs_IocReadUnqueued(SilOs_IocChannel *ch, void *data);
Std_ReturnType SilOs_IocSendQueued(SilOs_IocChannel *ch, const void *data);
Std_ReturnType SilOs_IocReceiveQueued(SilOs_IocChannel *ch, void *data);
Std_ReturnType SilOs_IocEmptyQueue(SilOs_IocChannel *ch);
void SilOs_IocInit(SilOs_IocChannel *ch, const void *initValue);

#ifdef __cplusplus
}
#endif

#endif /* SILOS_IOC_H */
