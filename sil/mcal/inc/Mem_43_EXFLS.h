/* Mem_43_EXFLS.h - stand-in for the NXP external flash memory driver (Fee emulation does not use it). */
#ifndef MEM_43_EXFLS_H
#define MEM_43_EXFLS_H

#include "Std_Types.h"

typedef struct {
    uint32 reserved;
} Mem_43_EXFLS_ConfigType;

extern const Mem_43_EXFLS_ConfigType Mem_43_EXFLS_Config;

void Mem_43_EXFLS_Init(const Mem_43_EXFLS_ConfigType *ConfigPtr);
void Mem_43_EXFLS_DeInit(void);
void Mem_43_EXFLS_MainFunction(void);
void Mem_43_EXFLS_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr);

#endif /* MEM_43_EXFLS_H */
