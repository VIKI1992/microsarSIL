/*
 * Compiler_Cfg.h - default (empty) memory class and pointer class definitions for the demo.
 *
 * A real MICROSAR project ships its own Compiler_Cfg.h chain (BR1: BSw/StartUpCode/inc), which
 * ends in the generated Os_Compiler_Cfg.h. Do not put sil/hal/default on the include path of a
 * real project; sil/hal (Compiler.h, Platform_Types.h, Std_Types.h, MemMap.h) is enough.
 */
#ifndef COMPILER_CFG_H
#define COMPILER_CFG_H

#define AUTOSAR_COMSTACKDATA

#define OS_CODE
#define OS_CONST
#define OS_VAR_NOINIT
#define OS_VAR_ZERO_INIT
#define OS_VAR_INIT
#define OS_APPL_DATA
#define OS_APPL_CODE
#define OS_STARTUPHOOK_CODE
#define OS_SHUTDOWNHOOK_CODE
#define OS_ERRORHOOK_CODE
#define OS_PROTECTIONHOOK_CODE
#define OS_PRETASKHOOK_CODE
#define OS_POSTTASKHOOK_CODE
#define OS_PANICHOOK_CODE

#define RTE_CODE
#define RTE_CONST
#define RTE_VAR_NOINIT
#define RTE_VAR_ZERO_INIT
#define RTE_VAR_INIT
#define RTE_APPL_DATA
#define RTE_APPL_CODE
#define RTE_APPL_VAR

#define ECUM_CODE
#define ECUM_CONST
#define ECUM_APPL_DATA
#define BSWM_CODE
#define BSWM_CONST
#define DET_CODE
#define DET_CONST
#define DET_APPL_DATA

#endif /* COMPILER_CFG_H */
