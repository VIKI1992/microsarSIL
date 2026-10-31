/* Can_GeneralTypes.h - AUTOSAR CAN general types (layout as used by MICROSAR CanIf with a 3rd party driver). */
#ifndef CAN_GENERAL_TYPES
#define CAN_GENERAL_TYPES
#define CAN_GENERAL_TYPES_H
#define CAN_DRVGENERALTYPES_3RDPARTYCANDRV_H

#include "Std_Types.h"
#include "ComStack_Types.h"

typedef uint8 *Can_SduPtrType;
typedef uint8 *Can_DataPtrType;
typedef uint32 Can_IdType;
typedef uint16 Can_HwHandleType;
typedef sint16 Can_SignedHwHandleType;

typedef struct Can_PduTypeTag {
    Can_IdType id;
    uint8 length;
    Can_SduPtrType sdu;
    PduIdType swPduHandle;
} Can_PduType;
typedef const Can_PduType *Can_PduInfoPtrType;
typedef Can_PduType *Can_PduInfoVarPtrType;

typedef struct Can_HwTypeTag {
    Can_IdType CanId;
    Can_HwHandleType Hoh;
    uint8 ControllerId;
} Can_HwType;

#define CAN_BUSY 2u

typedef enum { CAN_CS_UNINIT = 0, CAN_CS_STARTED = 1, CAN_CS_STOPPED = 2, CAN_CS_SLEEP = 3 } Can_ControllerStateType;
typedef enum { CAN_ERRORSTATE_ACTIVE = 0, CAN_ERRORSTATE_PASSIVE, CAN_ERRORSTATE_BUSOFF } Can_ErrorStateType;
typedef Can_ErrorStateType *Can_ErrorStatePtrType;
typedef enum Can_ErrorTypeTag {
    CAN_ERROR_TYPE_NOT_AVAILABLE = 0x0, CAN_ERROR_BIT_MONITORING1 = 0x1, CAN_ERROR_BIT_MONITORING0 = 0x2, CAN_ERROR_BIT = 0x3,
    CAN_ERROR_CHECK_ACK_FAILED = 0x4, CAN_ERROR_ACK_DELIMITER = 0x5, CAN_ERROR_ARBITRATION_LOST = 0x6, CAN_ERROR_OVERLOAD = 0x7,
    CAN_ERROR_CHECK_FORM_FAILED = 0x8, CAN_ERROR_CHECK_STUFFING_FAILED = 0x9, CAN_ERROR_CHECK_CRC_FAILED = 0xA, CAN_ERROR_BUS_LOCK = 0xB
} Can_ErrorType;

/* legacy return type still used by integration code */
typedef enum { CAN_OK = 0, CAN_NOT_OK = 1 } Can_ReturnType;
typedef enum { CAN_T_START = 0, CAN_T_STOP, CAN_T_SLEEP, CAN_T_WAKEUP } Can_StateTransitionType;

/* transceiver types (order as in the Vector Can_GeneralTypes.h) */
typedef enum CanTrcv_TrcvModeTypeTag { CANTRCV_TRCVMODE_NORMAL = 0u, CANTRCV_TRCVMODE_SLEEP, CANTRCV_TRCVMODE_STANDBY } CanTrcv_TrcvModeType;
typedef enum CanTrcv_TrcvWakeupModeTypeTag { CANTRCV_WUMODE_ENABLE = 0u, CANTRCV_WUMODE_CLEAR, CANTRCV_WUMODE_DISABLE } CanTrcv_TrcvWakeupModeType;
typedef enum CanTrcv_TrcvWakeupReasonTypeTag {
    CANTRCV_WU_ERROR = 0u, CANTRCV_WU_BY_BUS, CANTRCV_WU_BY_PIN, CANTRCV_WU_INTERNALLY, CANTRCV_WU_NOT_SUPPORTED, CANTRCV_WU_POWER_ON,
    CANTRCV_WU_RESET, CANTRCV_WU_BY_SYSERR
} CanTrcv_TrcvWakeupReasonType;
typedef enum { CANTRCV_PN_ACTIVATION_OFF = 0, CANTRCV_PN_ACTIVATION_ON } CanTrcv_PNActivationType;
typedef enum { CANTRCV_FLAG_SET = 0, CANTRCV_FLAG_CLEARED } CanTrcv_TrcvFlagStateType;

#endif /* CAN_GENERAL_TYPES */
