/* Lin.h - virtual LIN master driver (AUTOSAR Lin API as used by MICROSAR LinIf). A MICROSAR project's own
 * Lin.h is replaced by this one; Lin_GeneralTypes.h comes from the project (or sil/hal/default). */
#ifndef LIN_H
#define LIN_H

#include "Std_Types.h"
#include "Lin_GeneralTypes.h"
#if defined(__has_include)
#if __has_include("Lin_Cfg.h")
#include "Lin_Cfg.h"
#endif
#endif

#define LIN_VENDOR_ID (30u)
#define LIN_MODULE_ID (82u)

typedef struct {
    uint32 reserved;
} Lin_ConfigType;

extern const Lin_ConfigType Lin_Config;

void Lin_InitMemory(void);
void Lin_Init(const Lin_ConfigType *Config);
void Lin_GetVersionInfo(Std_VersionInfoType *versioninfo);
Std_ReturnType Lin_CheckWakeup(uint8 Channel);
Std_ReturnType Lin_SendFrame(uint8 Channel, const Lin_PduType *PduInfoPtr);
Std_ReturnType Lin_GoToSleep(uint8 Channel);
Std_ReturnType Lin_GoToSleepInternal(uint8 Channel);
Std_ReturnType Lin_Wakeup(uint8 Channel);
Std_ReturnType Lin_WakeupInternal(uint8 Channel);
Lin_StatusType Lin_GetStatus(uint8 Channel, uint8 **Lin_SduPtr);
/* ISR body of channel Channel; the project's ISR(LinIsr_n) bodies (or the generated ones) call it. */
void Lin_Interrupt(uint8 Channel);

#endif /* LIN_H */
