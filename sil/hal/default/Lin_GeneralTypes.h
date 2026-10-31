/* Lin_GeneralTypes.h - AUTOSAR LIN general types for builds without a SIP (demo). */
#ifndef LIN_GENERAL_TYPES_H
#define LIN_GENERAL_TYPES_H

#include "Std_Types.h"

typedef uint8 Lin_FramePidType;
typedef enum { LIN_ENHANCED_CS = 0, LIN_CLASSIC_CS } Lin_FrameCsModelType;
typedef enum { LIN_FRAMERESPONSE_TX = 0, LIN_FRAMERESPONSE_RX, LIN_FRAMERESPONSE_IGNORE } Lin_FrameResponseType;
#define LIN_MASTER_RESPONSE LIN_FRAMERESPONSE_TX
#define LIN_SLAVE_RESPONSE LIN_FRAMERESPONSE_RX
#define LIN_SLAVE_TO_SLAVE LIN_FRAMERESPONSE_IGNORE
typedef uint8 Lin_FrameDlType;

typedef struct {
    Lin_FramePidType Pid;
    Lin_FrameCsModelType Cs;
    Lin_FrameResponseType Drc;
    Lin_FrameDlType Dl;
    uint8 *SduPtr;
} Lin_PduType;

typedef enum {
    LIN_NOT_OK = 0, LIN_TX_OK, LIN_TX_BUSY, LIN_TX_HEADER_ERROR, LIN_TX_ERROR, LIN_RX_OK, LIN_RX_BUSY, LIN_RX_ERROR,
    LIN_RX_NO_RESPONSE, LIN_OPERATIONAL, LIN_CH_SLEEP
} Lin_StatusType;

typedef enum { LIN_ERR_HEADER = 0, LIN_ERR_RESP_STOPBIT, LIN_ERR_RESP_CHKSUM, LIN_ERR_RESP_DATABIT, LIN_ERR_NO_RESP, LIN_ERR_INC_RESP } Lin_SlaveErrorType;

#endif /* LIN_GENERAL_TYPES_H */
