/* CDD_Serial.c - virtual I2C (CDD_I2c) and UART (CDD_Uart) of the NXP RTD.
 *
 * I2C: every (channel, slave address) is a register-file device (SilMcal_RegWrite/Read): a write
 * transfer stores data[1..] at register data[0]; a read transfer returns registers from the last
 * written pointer. Test scripts preload registers through the control server.
 * UART: bytes the ECU sends are captured (SilUart_TakeTx) and echoed to the log line by line;
 * SilUart_FeedRx supplies bytes for pending receives. */
#include "CDD_I2c.h"
#include "CDD_Uart.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#include <string.h>

/* ---- I2C ------------------------------------------------------------------- */
#define I2C_MAX_CH 8u
#define SILMCAL_BUS_I2C 1u

typedef struct {
    volatile uint32 status;    /* I2c_StatusType */
    uint8 regPointer[128];     /* last register pointer per 7-bit address */
    uint32 timer;
    I2c_RequestType pending;
} I2cChan;

static I2cChan g_i2c[I2C_MAX_CH];
static uint8 g_i2cInit;

static uint32 i2c_bits_ns(uint8 channel, uint32 bytes) {
    uint32 baud = (channel < SilMcal_Cfg.numI2cChannels && SilMcal_Cfg.i2cChannels[channel].baud) ? SilMcal_Cfg.i2cChannels[channel].baud : 100000u;
    return (uint32)(((uint64)(bytes + 1u) * 9u * 1000000000u) / baud);
}

static void i2c_execute(uint8 channel, const I2c_RequestType *req) {
    I2cChan *c = &g_i2c[channel];
    uint16 addr = req->SlaveAddress;
    uint8 *ptr = &c->regPointer[addr & 0x7Fu];
    SilMcal_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = SILMCAL_EV_I2C;
    ev.channel = channel;
    ev.id = addr;
    if (req->DataBuffer == NULL_PTR || req->BufferSize == 0u) return;
    if (req->DataDirection == I2C_SEND_DATA) {
        *ptr = req->DataBuffer[0];
        if (req->BufferSize > 1u) SilMcal_RegWrite(SILMCAL_BUS_I2C, addr, *ptr, req->DataBuffer + 1, req->BufferSize - 1u);
        ev.dir = 0u;
        ev.len = (uint8)(req->BufferSize > 64u ? 64u : req->BufferSize);
        memcpy(ev.tx, req->DataBuffer, ev.len);
        if (req->BufferSize > 1u) *ptr = (uint8)(*ptr + (req->BufferSize - 1u));
    } else {
        SilMcal_RegRead(SILMCAL_BUS_I2C, addr, *ptr, req->DataBuffer, req->BufferSize);
        *ptr = (uint8)(*ptr + req->BufferSize);
        ev.dir = 1u;
        ev.len = (uint8)(req->BufferSize > 64u ? 64u : req->BufferSize);
        memcpy(ev.rx, req->DataBuffer, ev.len);
    }
    SilMcal_Capture(&ev);
}

static void i2c_async_done(void *arg) {
    I2cChan *c = (I2cChan *)arg;
    uint8 channel = (uint8)(c - g_i2c);
    i2c_execute(channel, &c->pending);
    c->timer = 0u;
    c->status = I2C_CH_FINISHED;
    if (channel < SilMcal_Cfg.numI2cChannels) (void)SilMcal_RaiseIsr(SilMcal_Cfg.i2cChannels[channel].isrHandler);
}

void I2c_Init(const I2c_ConfigType *Config) {
    uint8 i;
    (void)Config;
    for (i = 0; i < I2C_MAX_CH; ++i) g_i2c[i].status = I2C_CH_IDLE;
    g_i2cInit = 1u;
}

void I2c_DeInit(void) { g_i2cInit = 0u; }

Std_ReturnType I2c_SyncTransmit(uint8 Channel, const I2c_RequestType *Request) {
    if (!g_i2cInit || Channel >= I2C_MAX_CH || Request == NULL_PTR) return E_NOT_OK;
    if (g_i2c[Channel].status == I2C_CH_SEND || g_i2c[Channel].status == I2C_CH_RECEIVE) return E_NOT_OK;
    g_i2c[Channel].status = (Request->DataDirection == I2C_SEND_DATA) ? I2C_CH_SEND : I2C_CH_RECEIVE;
    SilOs_ConsumeTime(i2c_bits_ns(Channel, Request->BufferSize));
    i2c_execute(Channel, Request);
    g_i2c[Channel].status = I2C_CH_FINISHED;
    return E_OK;
}

Std_ReturnType I2c_AsyncTransmit(uint8 Channel, const I2c_RequestType *Request) {
    I2cChan *c;
    if (!g_i2cInit || Channel >= I2C_MAX_CH || Request == NULL_PTR) return E_NOT_OK;
    c = &g_i2c[Channel];
    if (c->status == I2C_CH_SEND || c->status == I2C_CH_RECEIVE) return E_NOT_OK;
    c->pending = *Request;
    c->status = (Request->DataDirection == I2C_SEND_DATA) ? I2C_CH_SEND : I2C_CH_RECEIVE;
    c->timer = SilOs_TimerSchedule(i2c_bits_ns(Channel, Request->BufferSize), i2c_async_done, c);
    if (c->timer == 0u) i2c_async_done(c);
    return E_OK;
}

I2c_StatusType I2c_GetStatus(uint8 Channel) { return (Channel < I2C_MAX_CH) ? (I2c_StatusType)g_i2c[Channel].status : I2C_CH_ERROR_PRESENT; }

Std_ReturnType I2c_StartListening(uint8 Channel) {
    (void)Channel;
    return E_OK;
}
Std_ReturnType I2c_PrepareSlaveBuffer(uint8 Channel, uint8 NumberOfBytes, I2c_DataType *DataBuffer) {
    (void)Channel;
    (void)NumberOfBytes;
    (void)DataBuffer;
    return E_OK;
}
void I2c_GetVersionInfo(Std_VersionInfoType *VersionInfo) {
    if (VersionInfo == NULL_PTR) return;
    VersionInfo->vendorID = I2C_VENDOR_ID;
    VersionInfo->moduleID = I2C_MODULE_ID;
    VersionInfo->sw_major_version = 1u;
    VersionInfo->sw_minor_version = 0u;
    VersionInfo->sw_patch_version = 0u;
}

/* ---- UART ------------------------------------------------------------------ */
#define UART_MAX_CH 8u
#define UART_RING 8192u

typedef struct {
    uint8 txRing[UART_RING];
    uint32 txHead, txCount;
    uint8 rxRing[UART_RING];
    uint32 rxHead, rxCount;
    char line[256];
    uint32 lineLen;
    volatile uint32 txStatus, rxStatus; /* Uart_StatusType */
    uint32 txDone, rxDone;
    uint8 *rxBuf;
    uint32 rxWanted;
    uint32 txTimer;
    uint32 baud;
} UartChan;

static UartChan g_uart[UART_MAX_CH];
static sil_mutex_t *g_uartLock;
static uint8 g_uartInit;

static void uart_ensure(void) {
    uint8 i;
    if (g_uartLock != NULL_PTR) return;
    g_uartLock = sil_mutex_create();
    for (i = 0; i < UART_MAX_CH; ++i) {
        g_uart[i].baud = (i < SilMcal_Cfg.numUartChannels && SilMcal_Cfg.uartChannels[i].baud) ? SilMcal_Cfg.uartChannels[i].baud : 115200u;
        g_uart[i].txStatus = UART_STATUS_NO_ERROR;
        g_uart[i].rxStatus = UART_STATUS_NO_ERROR;
    }
}

static void uart_log_bytes(UartChan *c, uint8 channel, const uint8 *d, uint32 n) {
    uint32 i;
    for (i = 0; i < n; ++i) {
        char ch = (char)d[i];
        if (ch == '\n' || c->lineLen >= sizeof(c->line) - 1u) {
            c->line[c->lineLen] = '\0';
            if (c->lineLen > 0u || ch == '\n') sil_log("uart%u: %s", (unsigned)channel, c->line);
            c->lineLen = 0u;
            if (ch != '\n' && ch != '\r') c->line[c->lineLen++] = ch;
        } else if (ch != '\r') {
            c->line[c->lineLen++] = ch;
        }
    }
}

static void uart_push_tx(uint8 channel, const uint8 *d, uint32 n) {
    UartChan *c = &g_uart[channel];
    uint32 i;
    SilMcal_Event ev;
    sil_mutex_lock(g_uartLock);
    for (i = 0; i < n; ++i) {
        if (c->txCount < UART_RING) {
            c->txRing[(c->txHead + c->txCount) % UART_RING] = d[i];
            c->txCount++;
        }
    }
    uart_log_bytes(c, channel, d, n);
    sil_mutex_unlock(g_uartLock);
    memset(&ev, 0, sizeof ev);
    ev.kind = SILMCAL_EV_UART;
    ev.channel = channel;
    ev.dir = 0u;
    ev.len = (uint8)(n > 64u ? 64u : n);
    memcpy(ev.tx, d, ev.len);
    SilMcal_Capture(&ev);
}

uint32 SilUart_TakeTx(uint8 channel, uint8 *buf, uint32 max) {
    UartChan *c;
    uint32 n = 0;
    uart_ensure();
    if (channel >= UART_MAX_CH) return 0;
    c = &g_uart[channel];
    sil_mutex_lock(g_uartLock);
    while (c->txCount > 0u && n < max) {
        buf[n++] = c->txRing[c->txHead];
        c->txHead = (c->txHead + 1u) % UART_RING;
        c->txCount--;
    }
    sil_mutex_unlock(g_uartLock);
    return n;
}

/* lock held: move ring bytes into a pending receive */
static void uart_serve_rx(UartChan *c) {
    while (c->rxBuf != NULL_PTR && c->rxDone < c->rxWanted && c->rxCount > 0u) {
        c->rxBuf[c->rxDone++] = c->rxRing[c->rxHead];
        c->rxHead = (c->rxHead + 1u) % UART_RING;
        c->rxCount--;
    }
    if (c->rxBuf != NULL_PTR && c->rxDone >= c->rxWanted) {
        c->rxBuf = NULL_PTR;
        c->rxStatus = UART_STATUS_NO_ERROR;
    }
}

void SilUart_FeedRx(uint8 channel, const uint8 *data, uint32 len) {
    UartChan *c;
    uint32 i;
    int complete;
    uart_ensure();
    if (channel >= UART_MAX_CH) return;
    c = &g_uart[channel];
    sil_mutex_lock(g_uartLock);
    for (i = 0; i < len; ++i) {
        if (c->rxCount < UART_RING) {
            c->rxRing[(c->rxHead + c->rxCount) % UART_RING] = data[i];
            c->rxCount++;
        }
    }
    uart_serve_rx(c);
    complete = (c->rxBuf == NULL_PTR && c->rxWanted > 0u && c->rxStatus == UART_STATUS_NO_ERROR);
    sil_mutex_unlock(g_uartLock);
    if (complete && channel < SilMcal_Cfg.numUartChannels) (void)SilMcal_RaiseIsr(SilMcal_Cfg.uartChannels[channel].isrHandler);
}

static void uart_tx_done(void *arg) {
    UartChan *c = (UartChan *)arg;
    uint8 channel = (uint8)(c - g_uart);
    c->txTimer = 0u;
    c->txStatus = UART_STATUS_NO_ERROR;
    if (channel < SilMcal_Cfg.numUartChannels) (void)SilMcal_RaiseIsr(SilMcal_Cfg.uartChannels[channel].isrHandler);
}

static uint64 uart_ns(const UartChan *c, uint32 bytes) { return ((uint64)bytes * 10u * 1000000000u) / (c->baud ? c->baud : 115200u); }

void Uart_Init(const Uart_ConfigType *Config) {
    (void)Config;
    uart_ensure();
    g_uartInit = 1u;
}
void Uart_DeInit(void) { g_uartInit = 0u; }

Std_ReturnType Uart_SetBaudrate(uint8 Channel, Uart_BaudrateType Baudrate) {
    uart_ensure();
    if (Channel >= UART_MAX_CH) return E_NOT_OK;
    g_uart[Channel].baud = (uint32)Baudrate;
    return E_OK;
}

Std_ReturnType Uart_GetBaudrate(uint8 Channel, uint32 *Baudrate) {
    uart_ensure();
    if (Channel >= UART_MAX_CH || Baudrate == NULL_PTR) return E_NOT_OK;
    *Baudrate = g_uart[Channel].baud;
    return E_OK;
}

Std_ReturnType Uart_Abort(uint8 Channel, Uart_DataDirectionType TransmissionType) {
    UartChan *c;
    uart_ensure();
    if (Channel >= UART_MAX_CH) return E_NOT_OK;
    c = &g_uart[Channel];
    sil_mutex_lock(g_uartLock);
    if (TransmissionType == UART_SEND) {
        if (c->txTimer) SilOs_TimerCancel(c->txTimer);
        c->txTimer = 0u;
        c->txStatus = UART_STATUS_ABORTED;
    } else {
        c->rxBuf = NULL_PTR;
        c->rxStatus = UART_STATUS_ABORTED;
    }
    sil_mutex_unlock(g_uartLock);
    return E_OK;
}

Std_ReturnType Uart_SyncSend(uint8 Channel, const uint8 *Buffer, uint32 BufferSize, uint32 Timeout) {
    (void)Timeout;
    uart_ensure();
    if (!g_uartInit || Channel >= UART_MAX_CH || Buffer == NULL_PTR) return E_NOT_OK;
    uart_push_tx(Channel, Buffer, BufferSize);
    g_uart[Channel].txDone = BufferSize;
    SilOs_ConsumeTime(uart_ns(&g_uart[Channel], BufferSize));
    return E_OK;
}

Std_ReturnType Uart_SyncReceive(uint8 Channel, uint8 *Buffer, uint32 BufferSize, uint32 Timeout) {
    UartChan *c;
    uint32 got;
    (void)Timeout;
    uart_ensure();
    if (!g_uartInit || Channel >= UART_MAX_CH || Buffer == NULL_PTR) return E_NOT_OK;
    c = &g_uart[Channel];
    sil_mutex_lock(g_uartLock);
    for (got = 0; got < BufferSize && c->rxCount > 0u; ++got) {
        Buffer[got] = c->rxRing[c->rxHead];
        c->rxHead = (c->rxHead + 1u) % UART_RING;
        c->rxCount--;
    }
    c->rxDone = got;
    sil_mutex_unlock(g_uartLock);
    return (got == BufferSize) ? E_OK : E_NOT_OK;
}

Std_ReturnType Uart_AsyncSend(uint8 Channel, const uint8 *Buffer, uint32 BufferSize) {
    UartChan *c;
    uart_ensure();
    if (!g_uartInit || Channel >= UART_MAX_CH || Buffer == NULL_PTR) return E_NOT_OK;
    c = &g_uart[Channel];
    if (c->txStatus == UART_STATUS_OPERATION_ONGOING) return E_NOT_OK;
    uart_push_tx(Channel, Buffer, BufferSize);
    c->txDone = BufferSize;
    c->txStatus = UART_STATUS_OPERATION_ONGOING;
    c->txTimer = SilOs_TimerSchedule(uart_ns(c, BufferSize), uart_tx_done, c);
    if (c->txTimer == 0u) uart_tx_done(c);
    return E_OK;
}

Std_ReturnType Uart_AsyncReceive(uint8 Channel, uint8 *Buffer, uint32 BufferSize) {
    UartChan *c;
    uart_ensure();
    if (!g_uartInit || Channel >= UART_MAX_CH || Buffer == NULL_PTR || BufferSize == 0u) return E_NOT_OK;
    c = &g_uart[Channel];
    sil_mutex_lock(g_uartLock);
    c->rxBuf = Buffer;
    c->rxWanted = BufferSize;
    c->rxDone = 0u;
    c->rxStatus = UART_STATUS_OPERATION_ONGOING;
    uart_serve_rx(c);
    sil_mutex_unlock(g_uartLock);
    return E_OK;
}

Uart_StatusType Uart_GetStatus(uint8 Channel, uint32 *BytesTransfered, Uart_DataDirectionType TransmissionType) {
    UartChan *c;
    uart_ensure();
    if (Channel >= UART_MAX_CH) return UART_STATUS_FRAMING_ERROR;
    c = &g_uart[Channel];
    if (BytesTransfered != NULL_PTR) *BytesTransfered = (TransmissionType == UART_SEND) ? c->txDone : c->rxDone;
    return (Uart_StatusType)((TransmissionType == UART_SEND) ? c->txStatus : c->rxStatus);
}

void Uart_GetVersionInfo(Std_VersionInfoType *VersionInfo) {
    if (VersionInfo == NULL_PTR) return;
    VersionInfo->vendorID = UART_VENDOR_ID;
    VersionInfo->moduleID = UART_MODULE_ID;
    VersionInfo->sw_major_version = 1u;
    VersionInfo->sw_minor_version = 0u;
    VersionInfo->sw_patch_version = 0u;
}
