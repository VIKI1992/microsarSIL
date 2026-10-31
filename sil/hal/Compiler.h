/*
 * Compiler.h - AUTOSAR compiler abstraction for the SIL host build (GCC, Clang, MSVC).
 */
#ifndef COMPILER_H
#define COMPILER_H

#include "Compiler_Cfg.h"

#define COMPILER_VENDOR_ID (0xFFFFu)
#define COMPILER_AR_RELEASE_MAJOR_VERSION (4u)
#define COMPILER_AR_RELEASE_MINOR_VERSION (4u)

#define AUTOMATIC
#define TYPEDEF
#define STATIC static

#ifndef NULL_PTR
#define NULL_PTR ((void *)0)
#endif

#if defined(_MSC_VER)
#define INLINE __inline
#define LOCAL_INLINE static __inline
#else
#define INLINE __inline__
#define LOCAL_INLINE static __inline__
#endif

#define FUNC(rettype, memclass) rettype
#define FUNC_P2CONST(rettype, ptrclass, memclass) const rettype *
#define FUNC_P2VAR(rettype, ptrclass, memclass) rettype *
#define P2VAR(ptrtype, memclass, ptrclass) ptrtype *
#define P2CONST(ptrtype, memclass, ptrclass) const ptrtype *
#define CONSTP2VAR(ptrtype, memclass, ptrclass) ptrtype *const
#define CONSTP2CONST(ptrtype, memclass, ptrclass) const ptrtype *const
#define P2FUNC(rettype, ptrclass, fctname) rettype (*fctname)
#define CONSTP2FUNC(rettype, ptrclass, fctname) rettype (*const fctname)
#define CONST(type, memclass) const type
#define VAR(vartype, memclass) vartype

/* Memory classes that vendor code may use as plain tokens. */
#define REGSPACE
#define MSR_REGSPACE REGSPACE

#endif /* COMPILER_H */
