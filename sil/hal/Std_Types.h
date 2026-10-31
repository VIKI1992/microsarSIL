/*
 * Std_Types.h - AUTOSAR standard types for the SIL host build.
 */
#ifndef STD_TYPES_H
#define STD_TYPES_H

#include "Platform_Types.h"
#include "Compiler.h"

#define STD_VENDOR_ID (0xFFFFu)
#define STD_AR_RELEASE_MAJOR_VERSION (4u)
#define STD_AR_RELEASE_MINOR_VERSION (4u)

#define STD_HIGH (0x01u)
#define STD_LOW (0x00u)
#define STD_ACTIVE (0x01u)
#define STD_IDLE (0x00u)
#define STD_ON (0x01u)
#define STD_OFF (0x00u)

typedef uint8 Std_ReturnType;

#ifndef STATUSTYPEDEFINED
#define STATUSTYPEDEFINED
#define E_OK (0x00u)
typedef unsigned char StatusType; /* OSEK compliance */
#endif
#define E_NOT_OK (0x01u)

typedef struct {
    uint16 vendorID;
    uint16 moduleID;
    uint8 sw_major_version;
    uint8 sw_minor_version;
    uint8 sw_patch_version;
} Std_VersionInfoType;

#define E_SAFETY_INVALID_REP (0x01u)
#define E_SAFETY_INVALID_SEQ (0x02u)
#define E_SAFETY_INVALID_CRC (0x03u)

#endif /* STD_TYPES_H */
