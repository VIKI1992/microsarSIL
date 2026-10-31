/*
 * SilCan.h - configuration tables and SIL side API of the virtual CAN driver (Can_43_FLEXCAN API).
 *
 * tools/gen_can_cfg.py generates SilCan_Cfg.c from the project's Can ECUC: controllers with their
 * hardware channel, baud rates and processing modes, hardware objects (HOH) with filters, and the
 * FlexCAN style interrupt handler functions (CAN<n>_ORED_0_31_MB_IRQHandler, ...) that the OS wrappers call.
 */
#ifndef SILCAN_H
#define SILCAN_H

#include "Std_Types.h"
#include "Can_GeneralTypes.h"
#include "sil_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SILCAN_ID_STANDARD 0u
#define SILCAN_ID_EXTENDED 1u
#define SILCAN_ID_MIXED 2u

typedef struct {
    uint32 code;
    uint32 mask;
    uint8 idType;             /* SILCAN_ID_* */
} SilCan_FilterCfg;

typedef struct {
    const char *name;
    uint32 objectId;          /* CanObjectId: the Hth/Hrh handle CanIf uses */
    uint8 isTx;
    uint8 isFull;             /* CanHandleType FULL (one id) vs BASIC (filter/range) */
    uint8 idType;             /* SILCAN_ID_* */
    uint8 controller;         /* index into SilCan_Cfg.controllers */
    uint16 hwObjectCount;     /* mailboxes available (tx: in-flight limit) */
    uint16 numFilters;
    const SilCan_FilterCfg *filters;
    uint8 fd;
} SilCan_HohCfg;

typedef struct {
    const char *name;
    uint8 controllerId;       /* CanControllerId (API argument) */
    uint8 hwChannel;          /* FLEXCAN_<n> */
    uint32 baud;              /* bit/s, arbitration phase */
    uint32 fdBaud;            /* bit/s, data phase (0 = classic) */
    uint8 rxPolling;          /* CanRxProcessing POLLING */
    uint8 txPolling;
    uint8 busoffPolling;
    const char *isrRx;        /* OsIsr names to raise; resolved at Can_Init (may be unknown: then polling) */
    const char *isrTx;
    const char *isrBusOff;
} SilCan_ControllerCfg;

typedef struct {
    uint8 numControllers;
    const SilCan_ControllerCfg *controllers;
    uint16 numHohs;
    const SilCan_HohCfg *hohs;
} SilCan_Config;

extern const SilCan_Config SilCan_Cfg;

typedef enum { SILCAN_ISR_MB = 0, SILCAN_ISR_RXFIFO, SILCAN_ISR_BUSOFF, SILCAN_ISR_KINDS } SilCan_IsrKind;

/* ---- SIL side API ------------------------------------------------------- */
/* Prepare the driver's host objects. The runtime calls it once before the OS threads exist;
 * every other entry point calls it lazily. */
void SilCan_Setup(void);
/* Attach a bus backend to a controller (spec as in sil_bus.h). Call before or after Can_Init. */
int SilCan_SetBus(uint8 controllerIdx, const char *busSpec);
const char *SilCan_GetBusSpec(uint8 controllerIdx);
/* Override the ISR raised for RX / TX confirmation / bus-off (by OS ISR id). */
void SilCan_SetIsr(uint8 controllerIdx, SilCan_IsrKind kind, uint32 isrId);
/* Deliver a frame to a controller as if received from the bus. Thread safe. */
void SilCan_Receive(uint8 controllerIdx, const SilBus_Frame *frame);
/* Fault injection: the controller goes bus-off (CanIf_ControllerBusOff). */
void SilCan_InjectBusOff(uint8 controllerIdx);
/* Entry point of the generated interrupt handler functions. */
void SilCan_Isr(uint8 controllerIdx, SilCan_IsrKind kind);
/* Statistics */
typedef struct {
    uint32 txFrames;
    uint32 rxFrames;
    uint32 rxDropped;      /* no matching HOH or queue full */
    uint32 txBusy;         /* CAN_BUSY returned */
    uint32 busOff;
    uint8 state;           /* Can_ControllerStateType */
} SilCan_Stats;
void SilCan_GetStats(uint8 controllerIdx, SilCan_Stats *stats);
uint8 SilCan_NumControllers(void);
const char *SilCan_ControllerName(uint8 controllerIdx);
/* Observer of every TX/RX frame (control server logging). */
typedef void (*SilCan_SniffFn)(void *user, uint8 controllerIdx, int isTx, const SilBus_Frame *frame);
void SilCan_SetSniffer(SilCan_SniffFn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* SILCAN_H */
