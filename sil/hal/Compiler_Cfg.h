/*
 * Compiler_Cfg.h - default (empty) memory class and pointer class definitions.
 *
 * A real MICROSAR project ships its own Compiler_Cfg.h chain; when integrating a SIP
 * put the project's include directory *after* sil/hal so that this file is replaced
 * only if the project does not provide one, or pass -DSIL_USE_PROJECT_COMPILER_CFG
 * and add the project directory before sil/hal.
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
