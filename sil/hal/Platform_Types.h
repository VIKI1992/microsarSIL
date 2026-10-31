/*
 * Platform_Types.h - AUTOSAR platform types for the SIL host build.
 *
 * Replaces the MCAL generated Platform_Types.h. The CPU_TYPE reflects the
 * host; sizes of the fixed-width types are identical to the target so that
 * serialized data (CAN payloads, NvM blocks) keep their layout.
 */
#ifndef PLATFORM_TYPES_H
#define PLATFORM_TYPES_H

#include <stdint.h>

#define PLATFORM_VENDOR_ID (0xFFFFu)
#define PLATFORM_MODULE_ID (199u)
#define PLATFORM_AR_RELEASE_MAJOR_VERSION (4u)
#define PLATFORM_AR_RELEASE_MINOR_VERSION (4u)
#define PLATFORM_AR_RELEASE_REVISION_VERSION (0u)
#define PLATFORM_SW_MAJOR_VERSION (1u)
#define PLATFORM_SW_MINOR_VERSION (0u)
#define PLATFORM_SW_PATCH_VERSION (0u)

#define CPU_TYPE_8 (8)
#define CPU_TYPE_16 (16)
#define CPU_TYPE_32 (32)
#define CPU_TYPE_64 (64)

#define MSB_FIRST (0)
#define LSB_FIRST (1)
#define HIGH_BYTE_FIRST (0)
#define LOW_BYTE_FIRST (1)

#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_X64) || defined(_M_ARM64)
#define CPU_TYPE (CPU_TYPE_64)
#else
#define CPU_TYPE (CPU_TYPE_32)
#endif

#define CPU_BIT_ORDER (LSB_FIRST)
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define CPU_BYTE_ORDER (HIGH_BYTE_FIRST)
#else
#define CPU_BYTE_ORDER (LOW_BYTE_FIRST)
#endif

#ifndef TRUE
#define TRUE (1u)
#endif
#ifndef FALSE
#define FALSE (0u)
#endif

typedef unsigned char boolean;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t sint8;
typedef int16_t sint16;
typedef int32_t sint32;
typedef int64_t sint64;
typedef uint_fast8_t uint8_least;
typedef uint_fast16_t uint16_least;
typedef uint_fast32_t uint32_least;
typedef int_fast8_t sint8_least;
typedef int_fast16_t sint16_least;
typedef int_fast32_t sint32_least;
typedef float float32;
typedef double float64;
typedef void *VoidPtr;
typedef const void *ConstVoidPtr;

#endif /* PLATFORM_TYPES_H */
