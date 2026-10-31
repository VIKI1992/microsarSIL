/* Platform.h - stand-in for the NXP RTD Platform driver (interrupt controller, MRU). */
#ifndef PLATFORM_H
#define PLATFORM_H

#include "Std_Types.h"

#define PLATFORM_VENDOR_ID_PLATFORM (43u)
#define PLATFORM_MODULE_ID_PLATFORM (255u)

typedef struct {
    uint32 reserved;
} Platform_ConfigType;

typedef void (*Platform_IrqHandlerType)(void);
typedef sint32 IRQn_Type;

void Platform_Init(const Platform_ConfigType *pConfig);
uint8 Platform_GetCoreID(void);
Std_ReturnType Platform_SetIrq(IRQn_Type eIrqNumber, boolean bEnable);
Std_ReturnType Platform_SetIrqPriority(IRQn_Type eIrqNumber, uint8 u8Priority);
Std_ReturnType Platform_GetIrqPriority(IRQn_Type eIrqNumber, uint8 *u8Priority);
Std_ReturnType Platform_InstallIrqHandler(IRQn_Type eIrqNumber, const Platform_IrqHandlerType pfNewHandler, Platform_IrqHandlerType *pfOldHandler);
Std_ReturnType Platform_MruEnableChannel(uint8 u8MruChannel);
Std_ReturnType Platform_MruDisableChannel(uint8 u8MruChannel);
Std_ReturnType Platform_MruResetChannel(uint8 u8MruChannel);
Std_ReturnType Platform_MruResetInstance(uint8 u8MruInstance);

#endif /* PLATFORM_H */
