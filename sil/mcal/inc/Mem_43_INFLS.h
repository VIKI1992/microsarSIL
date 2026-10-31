/* Mem_43_INFLS.h - stand-in for the NXP internal flash memory driver (Fee emulation does not use it). */
#ifndef MEM_43_INFLS_H
#define MEM_43_INFLS_H

#include "Std_Types.h"

typedef struct {
    uint32 reserved;
} Mem_43_INFLS_ConfigType;

extern const Mem_43_INFLS_ConfigType Mem_43_INFLS_Config;

void Mem_43_INFLS_Init(const Mem_43_INFLS_ConfigType *ConfigPtr);
void Mem_43_INFLS_DeInit(void);
void Mem_43_INFLS_MainFunction(void);
void Mem_43_INFLS_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr);

#endif /* MEM_43_INFLS_H */
