/*
 * CanIf_Cbk.h - callbacks the CAN driver calls (AUTOSAR R4.x CanIf) for builds without a SIP.
 * A MICROSAR project provides its own CanIf_Cbk.h (Sip/Components/CanIf); the demo implements
 * these four functions in a tiny CanIf stand-in.
 */
#ifndef CANIF_CBK_H
#define CANIF_CBK_H

#include "Std_Types.h"
#include "ComStack_Types.h"
#include "Can_GeneralTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

void CanIf_RxIndication(const Can_HwType *Mailbox, const PduInfoType *PduInfoPtr);
void CanIf_TxConfirmation(PduIdType CanTxPduId);
void CanIf_ControllerBusOff(uint8 ControllerId);
void CanIf_ControllerModeIndication(uint8 ControllerId, Can_ControllerStateType ControllerMode);

#ifdef __cplusplus
}
#endif

#endif /* CANIF_CBK_H */
