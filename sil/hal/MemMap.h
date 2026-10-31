/*
 * MemMap.h - no-op memory mapping for the host build.
 *
 * AUTOSAR modules include this file with a START/STOP_SEC_* macro defined to
 * place code and data into linker sections. On the host there is nothing to
 * map; the file only undefines every section selector so that the next
 * inclusion starts clean (the original files error on unknown selectors).
 */
#ifndef SIL_MEMMAP_NOOP_INCLUDED
#define SIL_MEMMAP_NOOP_INCLUDED 1
#endif

#define MEMMAP_ERROR

#ifdef SIL_MEMMAP_UNDEF_ALL
/* Nothing: left for tooling that greps the selectors. */
#endif

/* Any macro of the form <MODULE>_START_SEC_<X> / <MODULE>_STOP_SEC_<X> is accepted.
 * We cannot enumerate them, so this file relies on the fact that the
 * including module never checks that MEMMAP_ERROR was undefined. Vector and
 * NXP modules only emit "#error" when their *specific* selector was not
 * handled, which only happens inside the original MemMap.h. */
#undef MEMMAP_ERROR
