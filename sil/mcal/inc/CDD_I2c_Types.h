/* CDD_I2c_Types.h - types of the virtual I2C driver (NXP RTD CDD_I2c names). */
#ifndef CDD_I2C_TYPES_H
#define CDD_I2C_TYPES_H

#include "Std_Types.h"

typedef uint8 I2c_DataType;
typedef uint8 I2c_ChannelType;
typedef uint16 I2c_AddressType;

typedef enum { I2C_SEND_DATA = 0, I2C_RECEIVE_DATA } I2c_DataDirectionType;
typedef enum { I2C_7_BIT_ADDRESS = 0, I2C_10_BIT_ADDRESS } I2c_AddressBitsType;
typedef enum { I2C_CH_IDLE = 0, I2C_CH_SEND, I2C_CH_RECEIVE, I2C_CH_FINISHED, I2C_CH_ERROR_PRESENT } I2c_StatusType;
typedef enum { I2C_MASTER_EVENT_END_TRANSFER = 0, I2C_MASTER_EVENT_NACK, I2C_MASTER_EVENT_ARBITRATION_LOST, I2C_MASTER_EVENT_ERROR,
               I2C_SLAVE_EVENT_RX_REQ, I2C_SLAVE_EVENT_TX_REQ, I2C_SLAVE_EVENT_STOP, I2C_SLAVE_EVENT_ERROR } I2c_EventType;

typedef struct {
    I2c_AddressType SlaveAddress;
    boolean HighSpeedMode;
    boolean ExpectNack;
    boolean RepeatedStart;
    I2c_AddressBitsType BitsSlaveAddress;
    I2c_DataDirectionType DataDirection;
    uint32 BufferSize;
    I2c_DataType *DataBuffer;
} I2c_RequestType;

typedef struct {
    uint32 reserved;
} I2c_ConfigType;

#endif /* CDD_I2C_TYPES_H */
