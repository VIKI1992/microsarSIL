/*
 * ComStack_Types.h - AUTOSAR communication stack types for builds without a SIP (demo).
 * A MICROSAR project provides its own (Sip/Components/_Common with the generated ComStack_Cfg.h).
 */
#ifndef COMSTACK_TYPES_H
#define COMSTACK_TYPES_H

#include "Std_Types.h"

typedef uint16 PduIdType;
typedef uint32 PduLengthType;

typedef struct {
    uint8 *SduDataPtr;
    uint8 *MetaDataPtr;
    PduLengthType SduLength;
} PduInfoType;

typedef enum { TP_DATACONF = 0, TP_DATARETRY, TP_CONFPENDING } TpDataStateType;

typedef struct {
    TpDataStateType TpDataState;
    PduLengthType TxTpDataCnt;
} RetryInfoType;

typedef uint8 NetworkHandleType;
typedef uint8 IcomConfigIdType;
typedef enum { ICOM_SWITCH_E_OK = 0, ICOM_SWITCH_E_FAILED } IcomSwitch_ErrorType;

typedef uint8 BufReq_ReturnType;
#define BUFREQ_OK ((BufReq_ReturnType)0u)
#define BUFREQ_E_NOT_OK ((BufReq_ReturnType)1u)
#define BUFREQ_E_BUSY ((BufReq_ReturnType)2u)
#define BUFREQ_E_OVFL ((BufReq_ReturnType)3u)

#endif /* COMSTACK_TYPES_H */
