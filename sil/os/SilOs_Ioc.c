/* SilOs_Ioc.c - IOC channel helpers; every operation is atomic under the OS lock. */
#include "SilOs_Ioc.h"
#include "SilOs_Internal.h"

#include <string.h>

void SilOs_IocInit(SilOs_IocChannel *ch, const void *initValue) {
    silos_lock();
    ch->head = 0u;
    ch->count = 0u;
    ch->lostData = 0u;
    if (initValue != NULL && ch->numSlots >= 1u) {
        memcpy(ch->buffer, initValue, ch->elementSize);
        ch->count = 1u;
    }
    ch->initialized = 1u;
    silos_unlock();
}

Std_ReturnType SilOs_IocWriteUnqueued(SilOs_IocChannel *ch, const void *data) {
    silos_lock();
    memcpy(ch->buffer, data, ch->elementSize);
    ch->count = 1u;
    silos_unlock();
    return IOC_E_OK;
}

Std_ReturnType SilOs_IocReadUnqueued(SilOs_IocChannel *ch, void *data) {
    silos_lock();
    memcpy(data, ch->buffer, ch->elementSize);
    silos_unlock();
    return IOC_E_OK;
}

Std_ReturnType SilOs_IocSendQueued(SilOs_IocChannel *ch, const void *data) {
    Std_ReturnType r = IOC_E_OK;
    silos_lock();
    if (ch->count >= ch->numSlots) {
        ch->lostData = 1u;
        r = IOC_E_LIMIT;
    } else {
        uint32 idx = (ch->head + ch->count) % ch->numSlots;
        memcpy((uint8 *)ch->buffer + idx * ch->elementSize, data, ch->elementSize);
        ch->count++;
    }
    silos_unlock();
    return r;
}

Std_ReturnType SilOs_IocReceiveQueued(SilOs_IocChannel *ch, void *data) {
    Std_ReturnType r = IOC_E_OK;
    silos_lock();
    if (ch->count == 0u) {
        r = IOC_E_NO_DATA;
    } else {
        memcpy(data, (uint8 *)ch->buffer + ch->head * ch->elementSize, ch->elementSize);
        ch->head = (ch->head + 1u) % ch->numSlots;
        ch->count--;
        if (ch->lostData) {
            ch->lostData = 0u;
            r = IOC_E_LOST_DATA;
        }
    }
    silos_unlock();
    return r;
}

Std_ReturnType SilOs_IocEmptyQueue(SilOs_IocChannel *ch) {
    silos_lock();
    ch->head = 0u;
    ch->count = 0u;
    ch->lostData = 0u;
    silos_unlock();
    return IOC_E_OK;
}
