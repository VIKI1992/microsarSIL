/* OsIf.h - stand-in for the NXP RTD OS interface layer (timers and core id on top of SilOs). */
#ifndef OSIF_H
#define OSIF_H

#include "Std_Types.h"

typedef enum { OSIF_COUNTER_DUMMY = 0, OSIF_COUNTER_SYSTEM, OSIF_COUNTER_CUSTOM } OsIf_CounterType;

void OsIf_Init(const void *Config);
uint32 OsIf_GetCounter(OsIf_CounterType SelectedCounter);
uint32 OsIf_GetElapsed(uint32 *CurrentRef, OsIf_CounterType SelectedCounter);
void OsIf_SetTimerFrequency(uint32 Freq, OsIf_CounterType SelectedCounter);
uint32 OsIf_MicrosToTicks(uint32 Micros, OsIf_CounterType SelectedCounter);
uint8 OsIf_GetCoreID(void);
uint32 OsIf_GetUserId(void);

void OsIf_SuspendAllInterrupts(void);
void OsIf_ResumeAllInterrupts(void);

#endif /* OSIF_H */
