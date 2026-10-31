/*
 * demo_ecu.c - stand-in two core ECU exercising what the BR1 reference project uses: RTE style extended
 * tasks driven by SetEvent alarms, a basic task with multiple activations, a schedule table, resources,
 * spinlocks, cross core events (XSignal), a start-up barrier, IOC, Cat2 ISRs, runnable instrumentation,
 * the virtual CAN driver with a tiny CanIf stand-in, and the virtual MCAL (Dio, Adc, Pwm, Icu, Gpt, Spi,
 * Fee, MemAcc, I2c, Uart, Lin) on the virtual I/O registry.
 *
 * CAN demo protocol (controller 0 = CT_CAN0, controller 1 = CT_CAN1, wired back to back through the
 * in-process link "loop:demo" unless --can<N>=... says otherwise):
 *   0x100  every 10 ms from CAN0: u32 counter, u8 buttons, u8 bus-off count, u8 rx count, u8 0
 *   0x201  command to CAN0 (any data): the ECU answers with 0x301 carrying the same data
 *   0x202  command: Mcu_PerformReset (the process restarts itself with --resume)
 *   CAN1 receives everything the loop carries (0x100 through a FULL mailbox, the rest BASIC)
 *
 * Peripheral demo (all values visible through the control server, keys in parentheses):
 *   BUTTON (dio.5) -> LED_GREEN (dio.1) follows it; AdcGroup_Sense (adc.0 = Vbat, adc.1 = Temp) every 10 ms,
 *   PwmChannel_Fan duty (pwm.0) = Temp * 8; IcuChannel_Speed period (icu.0.freq) and Pulses count (icu.2.freq);
 *   IcuChannel_Hall edges (icu.1.value) run Demo_HallEdgeNotification; GptChannel_Timeout fires once after 500 ms
 *   and sets LED_RED (dio.2); SPI sequence to the sensor device every 5 ms (echo slave by default); Fee block
 *   Odometer counts the seconds the ECU ran (persists in <nv>/fee.bin); MemAcc area 0 holds a boot counter;
 *   I2C reads the temperature sensor at 0x48 register 0 every 100 ms; UART channel 0 prints a status line
 *   every 100 ms; LIN channel 0 polls PID 0x10 (slave response) and channel 1 sends PID 0x11 every 20 ms.
 */
#include "Os.h"
#include "SilOs.h"
#include "SilTrace.h"
#include "sil_runtime.h"
#include "sil_port.h"
#include "Can_43_FLEXCAN.h"
#include "SilCan.h"
#include "CanIf_Cbk.h"
#include "Mcu.h"
#include "Port.h"
#include "Dio.h"
#include "Adc.h"
#include "Pwm.h"
#include "Icu.h"
#include "Gpt.h"
#include "Spi.h"
#include "Fee.h"
#include "MemAcc.h"
#include "CDD_I2c.h"
#include "CDD_Uart.h"
#include "Lin.h"

#include <stdio.h>
#include <string.h>

/* ---- runnable ids for tracing (would come from id_runnables.xml in the real project) ------ */
enum { R_1ms_Sense = 0, R_CanRx, R_SpiDone, R_10ms_Control, R_100ms_Diag, R_Bg_RamCheck, R_10ms_Core1, R_Event_Core1, R_Hi_Sched, R_Coop, R_CanTx,
       R_10ms_Io, R_100ms_Report, R_NvHandle, R_COUNT };
static const char *const runnableNames[R_COUNT] = {"R_1ms_Sense", "R_CanRx", "R_SpiDone", "R_10ms_Control", "R_100ms_Diag", "R_Bg_RamCheck",
                                                   "R_10ms_Core1", "R_Event_Core1", "R_Hi_Sched", "R_Coop", "R_CanTx", "R_10ms_Io", "R_100ms_Report",
                                                   "R_NvHandle"};
#define RUNNABLE(id, etNs, body)      \
    do {                              \
        SilOs_RunnableStart(id);      \
        body;                         \
        SilOs_ConsumeTime(etNs);      \
        SilOs_RunnableStop(id);       \
    } while (0)

/* ---- peripheral handles (the generated symbolic names) --------------------------------------- */
#define DEMO_LED_GREEN DioConf_DioChannel_LED_GREEN
#define DEMO_LED_RED DioConf_DioChannel_LED_RED
#define DEMO_BUTTON DioConf_DioChannel_BUTTON
#define DEMO_ADC_GROUP AdcConf_AdcGroup_AdcGroup_Sense
#define DEMO_PWM_FAN PwmConf_PwmChannel_PwmChannel_Fan
#define DEMO_ICU_SPEED IcuConf_IcuChannel_IcuChannel_Speed
#define DEMO_ICU_HALL IcuConf_IcuChannel_IcuChannel_Hall
#define DEMO_ICU_PULSES IcuConf_IcuChannel_IcuChannel_Pulses
#define DEMO_GPT_TIMEOUT GptConf_GptChannelConfiguration_GptChannel_Timeout
#define DEMO_SPI_SEQ SpiConf_SpiSequence_SpiSequence_Sensor
#define DEMO_SPI_CH SpiConf_SpiChannel_SpiChannel_Sensor
#define DEMO_FEE_ODOMETER FeeConf_FeeBlockConfiguration_FeeBlock_Odometer
#define DEMO_MEMACC_AREA 0u
#define DEMO_I2C_CH 0u
#define DEMO_I2C_TEMP_ADDR 0x48u
#define DEMO_UART_CH 0u

/* ---- peripheral models driven from the application -------------------------------------------- */
static void button_model(void *arg) {
    (void)arg;
    SilOs_RaiseIrq(Isr_Button);
    SilOs_TimerSchedule(333000000u, button_model, NULL);
}

/* ---- shared data ------------------------------------------------------------------------- */
static uint32 g_sharedCounter;      /* protected by Res_Shared on core 0 */
static uint32 g_crossCoreValue;     /* protected by Spin_Shared */
static uint32 g_spiDone, g_buttons, g_buttonPresses;
static volatile uint32_t g_core1Events; /* written on core 1, read on core 0 */
static Adc_ValueGroupType g_adcBuf[2], g_adcVbat, g_adcTemp;
static uint32 g_adcConversions;
static volatile uint32 g_hallEdges, g_gptFired;
static uint32 g_speedPeriod, g_pulses;
static uint8 g_spiTx[4], g_spiRx[4];
static uint32 g_spiEcho;
static uint32 g_i2cTemp, g_i2cReads;
static uint32 g_odometer, g_bootCount;
static uint8 g_feeBuf[8], g_memBuf[4];
static volatile uint32 g_nvEvents, g_nvErrors;
static uint8 g_nvState;            /* 0 idle, 1 reading odometer, 2 writing odometer, 3 reading boot, 4 writing boot */
static uint32 g_linRxOk, g_linTx;
static uint8 g_linRx[8], g_linTxData[2];

/* ---- CanIf stand-in: the callbacks of the virtual CAN driver (CanIf in a real project) ---- */
#define DEMO_ID_STATUS 0x100u
#define DEMO_ID_CMD 0x201u
#define DEMO_ID_RESET 0x202u
#define DEMO_ID_REPLY 0x301u
static uint32 g_canRx, g_canTxConf, g_canBusOff, g_canTxBusy, g_canCmd;
static volatile uint32 g_canRestart; /* bit per controller: recover from bus-off (what CanSM does) */
static volatile uint32 g_resetRequest;
static uint8 g_cmdData[64];
static uint8 g_cmdLen;
static volatile uint32 g_cmdPending;

void CanIf_RxIndication(const Can_HwType *Mailbox, const PduInfoType *PduInfoPtr) {
    uint32 id = Mailbox->CanId & 0x1FFFFFFFu;
    g_canRx++;
    if (Mailbox->ControllerId == CanConf_CanController_CT_CAN0 && id == DEMO_ID_CMD) {
        g_cmdLen = (uint8)(PduInfoPtr->SduLength > 64u ? 64u : PduInfoPtr->SduLength);
        memcpy(g_cmdData, PduInfoPtr->SduDataPtr, g_cmdLen);
        g_cmdPending = 1u;
    } else if (Mailbox->ControllerId == CanConf_CanController_CT_CAN0 && id == DEMO_ID_RESET) {
        g_resetRequest = 1u;
    }
    (void)SetEvent(Task_1ms_Core0, Ev_CanRx);
}
void CanIf_TxConfirmation(PduIdType CanTxPduId) {
    (void)CanTxPduId;
    g_canTxConf++;
}
void CanIf_ControllerBusOff(uint8 ControllerId) {
    g_canBusOff++;
    g_canRestart |= (1u << ControllerId);
}
void CanIf_ControllerModeIndication(uint8 ControllerId, Can_ControllerStateType ControllerMode) {
    (void)ControllerId;
    (void)ControllerMode;
}

static void can_send(Can_HwHandleType hth, uint32 id, const uint8 *data, uint8 len, PduIdType pdu) {
    Can_PduType p;
    p.id = id;
    p.length = len;
    p.sdu = (uint8 *)data;
    p.swPduHandle = pdu;
    if (Can_43_FLEXCAN_Write(hth, &p) != E_OK) g_canTxBusy++;
}

/* ---- notifications of the virtual MCAL (run in ISR context) ------------------------------- */
void Demo_AdcSenseNotification(void);
void Demo_HallEdgeNotification(void);
void Demo_GptTimeoutNotification(void);
void Demo_SpiSensorEndNotification(void);
void Demo_NvJobEndNotification(void);
void Demo_NvJobErrorNotification(void);

void Demo_AdcSenseNotification(void) { (void)SetEvent(Task_10ms_Core0, Ev_AdcDone); }
void Demo_HallEdgeNotification(void) { g_hallEdges++; }
void Demo_GptTimeoutNotification(void) {
    g_gptFired++;
    Dio_WriteChannel(DEMO_LED_RED, STD_HIGH);
}
void Demo_SpiSensorEndNotification(void) { (void)SetEvent(Task_1ms_Core0, Ev_SpiDone); }
void Demo_NvJobEndNotification(void) {
    g_nvEvents++;
    (void)SetEvent(Task_1ms_Core0, Ev_NvDone);
}
void Demo_NvJobErrorNotification(void) {
    g_nvErrors++;
    (void)SetEvent(Task_1ms_Core0, Ev_NvDone);
}

/* ---- hooks ------------------------------------------------------------------------------- */
void StartupHook(void) { sil_log("StartupHook on core %u", (unsigned)GetCoreID()); }
void ShutdownHook(StatusType Fatalerror) {
    sil_log("ShutdownHook(%u): canrx=%u cantx=%u busoff=%u spi=%u buttons=%u core1events=%u shared=%u odometer=%u boots=%u", (unsigned)Fatalerror,
            g_canRx, g_canTxConf, g_canBusOff, g_spiDone, g_buttons, (unsigned)sil_atomic_load32(&g_core1Events), g_sharedCounter, g_odometer,
            g_bootCount);
}
void ErrorHook(StatusType Error) {
    sil_log("ErrorHook: status %u service 0x%02X", (unsigned)Error, (unsigned)OSErrorGetServiceId());
}

/* ---- core entry (the project's main) ----------------------------------------------------- */
static void demo_core_entry(CoreIdType core) {
    Os_InitMemory();
    Os_Init();
    if (core == OS_CORE_ID_0) {
        StatusType st;
        uint32 i;
        for (i = 0; i < R_COUNT; ++i) SilTrace_SetName(SILTRACE_NAME_RUNNABLE, i, runnableNames[i]);
        StartCore(OS_CORE_ID_1, &st);
    }
    StartOS(OSDEFAULTAPPMODE);
}

/* ---- NV handling (what NvM does for the application) ------------------------------------- */
static void nv_start_read_odometer(void) {
    g_nvState = 1u;
    if (Fee_Read(DEMO_FEE_ODOMETER, 0u, g_feeBuf, 8u) != E_OK) g_nvState = 0u;
}

static void nv_handle_job_end(void) {
    MemIf_JobResultType r = Fee_GetJobResult();
    switch (g_nvState) {
    case 1u: /* odometer read */
        if (r == MEMIF_JOB_OK) {
            g_odometer = (uint32)g_feeBuf[0] | ((uint32)g_feeBuf[1] << 8) | ((uint32)g_feeBuf[2] << 16) | ((uint32)g_feeBuf[3] << 24);
        } else {
            g_odometer = 0u; /* first start: the block is invalid */
        }
        g_nvState = 0u;
        break;
    case 2u: /* odometer written */
        g_nvState = 0u;
        break;
    default:
        g_nvState = 0u;
        break;
    }
}

static void nv_write_odometer(void) {
    if (g_nvState != 0u || Fee_GetStatus() != MEMIF_IDLE) return;
    memset(g_feeBuf, 0, sizeof g_feeBuf);
    g_feeBuf[0] = (uint8)g_odometer;
    g_feeBuf[1] = (uint8)(g_odometer >> 8);
    g_feeBuf[2] = (uint8)(g_odometer >> 16);
    g_feeBuf[3] = (uint8)(g_odometer >> 24);
    g_nvState = 2u;
    if (Fee_Write(DEMO_FEE_ODOMETER, g_feeBuf) != E_OK) g_nvState = 0u;
}

/* boot counter in MemAcc area 0 (polled; MemAcc has no job end notification here) */
static uint8 g_memState; /* 0 idle, 1 reading, 2 writing, 3 done */
static void mem_poll(void) {
    if (g_memState == 1u && MemAcc_GetJobStatus(DEMO_MEMACC_AREA) == MEMACC_JOB_IDLE) {
        uint32 v = (uint32)g_memBuf[0] | ((uint32)g_memBuf[1] << 8) | ((uint32)g_memBuf[2] << 16) | ((uint32)g_memBuf[3] << 24);
        if (MemAcc_GetJobResult(DEMO_MEMACC_AREA) != MEMACC_MEM_OK || v == 0xFFFFFFFFu) v = 0u;
        g_bootCount = v + 1u;
        g_memBuf[0] = (uint8)g_bootCount;
        g_memBuf[1] = (uint8)(g_bootCount >> 8);
        g_memBuf[2] = (uint8)(g_bootCount >> 16);
        g_memBuf[3] = (uint8)(g_bootCount >> 24);
        g_memState = (MemAcc_Write(DEMO_MEMACC_AREA, 0u, g_memBuf, 4u) == E_OK) ? 2u : 3u;
    } else if (g_memState == 2u && MemAcc_GetJobStatus(DEMO_MEMACC_AREA) == MEMACC_JOB_IDLE) {
        g_memState = 3u;
    }
}

/* ---- tasks core 0 ------------------------------------------------------------------------ */
TASK(InitTask_Core0) {
    /* driver init as EcuM_AL_DriverInit* + CanSM would do it */
    Mcu_Init(NULL_PTR);
    (void)Mcu_InitClock(0u);
    Port_Init(NULL_PTR);
    Dio_WriteChannel(DEMO_LED_GREEN, STD_LOW);
    Dio_WriteChannel(DEMO_LED_RED, STD_LOW);
    Adc_Init(NULL_PTR);
    (void)Adc_SetupResultBuffer(DEMO_ADC_GROUP, g_adcBuf);
    Adc_EnableGroupNotification(DEMO_ADC_GROUP);
    Pwm_Init(NULL_PTR);
    Icu_Init(NULL_PTR);
    Icu_StartSignalMeasurement(DEMO_ICU_SPEED);
    Icu_EnableEdgeCount(DEMO_ICU_PULSES);
    Icu_EnableNotification(DEMO_ICU_HALL);
    Gpt_Init(NULL_PTR);
    Gpt_EnableNotification(DEMO_GPT_TIMEOUT);
    Gpt_StartTimer(DEMO_GPT_TIMEOUT, 500000u); /* 500 ms at 1 MHz */
    Spi_Init(NULL_PTR);
    MemAcc_Init(NULL_PTR);
    Fee_Init(NULL_PTR);
    I2c_Init(NULL_PTR);
    Uart_Init(NULL_PTR);
    Lin_InitMemory();
    Lin_Init(NULL_PTR);
    Can_43_FLEXCAN_Init(NULL_PTR);
    (void)Can_43_FLEXCAN_SetControllerMode(CanConf_CanController_CT_CAN0, CAN_CS_STARTED);
    (void)Can_43_FLEXCAN_SetControllerMode(CanConf_CanController_CT_CAN1, CAN_CS_STARTED);
    nv_start_read_odometer();
    g_memState = (MemAcc_Read(DEMO_MEMACC_AREA, 0u, g_memBuf, 4u) == E_OK) ? 1u : 3u;
    (void)ActivateTask(Task_1ms_Core0);  /* extended tasks are activated once, as Rte_Start does */
    (void)ActivateTask(Task_10ms_Core0);
    (void)SetRelAlarm(Al_1ms_Core0, 1, 1);
    (void)SetRelAlarm(Al_10ms_Core0, 10, 10);
    (void)StartScheduleTableRel(SchTbl_Core0, 1);
    (void)SilOs_TimerSchedule(100000000u, button_model, NULL);
    (void)Os_BarrierSynchronize(Barrier_Init);
    (void)TerminateTask();
}

TASK(Task_1ms_Core0) {
    static uint32 cycle;
    EventMaskType ev;
    for (;;) {
        (void)WaitEvent(Ev_1ms | Ev_CanRx | Ev_SpiDone | Ev_NvDone);
        (void)GetEvent(Task_1ms_Core0, &ev);
        (void)ClearEvent(ev & (Ev_1ms | Ev_CanRx | Ev_SpiDone | Ev_NvDone));
        if (ev & Ev_1ms) {
            RUNNABLE(R_1ms_Sense, 50000u, {
                cycle++;
                Fee_MainFunction();
                MemAcc_MainFunction();
                Spi_MainFunction_Handling();
                mem_poll();
                if ((cycle % 5u) == 0u) {
                    g_spiTx[0] = 0x80u;
                    g_spiTx[1] = (uint8)cycle;
                    g_spiTx[2] = (uint8)(cycle >> 8);
                    g_spiTx[3] = 0u;
                    (void)Spi_SetupEB(DEMO_SPI_CH, g_spiTx, g_spiRx, 4u);
                    (void)Spi_AsyncTransmit(DEMO_SPI_SEQ);
                }
            });
        }
        if (ev & Ev_CanRx) {
            RUNNABLE(R_CanRx, 20000u, {
                (void)IocWrite_Ioc_Speed_0(g_canRx * 3u);
                if (g_cmdPending) {
                    g_cmdPending = 0u;
                    g_canCmd++;
                    can_send(CanConf_CanHardwareObject_Can0_Tx, DEMO_ID_REPLY, g_cmdData, g_cmdLen, 1u);
                }
                if (g_resetRequest) {
                    g_resetRequest = 0u;
                    Mcu_PerformReset();
                }
            });
        }
        if (ev & Ev_SpiDone) {
            RUNNABLE(R_SpiDone, 10000u, {
                g_spiDone++;
                if (Spi_GetSequenceResult(DEMO_SPI_SEQ) == SPI_SEQ_OK && memcmp(g_spiRx, g_spiTx, 4u) == 0) g_spiEcho++;
            });
        }
        if (ev & Ev_NvDone) {
            RUNNABLE(R_NvHandle, 5000u, { nv_handle_job_end(); });
        }
    }
}

TASK(Task_10ms_Core0) {
    static uint32 n;
    static uint8 lastButton;
    static uint8 linPhase;
    EventMaskType ev;
    for (;;) {
        (void)WaitEvent(Ev_10ms | Ev_AdcDone);
        (void)GetEvent(Task_10ms_Core0, &ev);
        (void)ClearEvent(ev & (Ev_10ms | Ev_AdcDone));
        if (ev & Ev_AdcDone) {
            RUNNABLE(R_10ms_Io, 20000u, {
                if (Adc_ReadGroup(DEMO_ADC_GROUP, g_adcBuf) == E_OK) {
                    g_adcVbat = g_adcBuf[0];
                    g_adcTemp = g_adcBuf[1];
                    g_adcConversions++;
                    Pwm_SetDutyCycle(DEMO_PWM_FAN, (uint16)((g_adcTemp * 8u) > 0x8000u ? 0x8000u : (g_adcTemp * 8u)));
                }
            });
        }
        if (!(ev & Ev_10ms)) continue;
        RUNNABLE(R_10ms_Control, 300000u, {
            uint8 button;
            n++;
            (void)GetResource(Res_Shared);
            g_sharedCounter++;
            (void)ReleaseResource(Res_Shared);
            (void)GetSpinlock(Spin_Shared);
            g_crossCoreValue = n;
            (void)ReleaseSpinlock(Spin_Shared);
            if ((n % 10u) == 0u) (void)SetEvent(Task_Event_Core1, Ev_Trigger_Core1);
            button = Dio_ReadChannel(DEMO_BUTTON);
            if (button && !lastButton) g_buttonPresses++;
            lastButton = button;
            Dio_WriteChannel(DEMO_LED_GREEN, button);
            Adc_StartGroupConversion(DEMO_ADC_GROUP);
            /* LIN: alternate between polling PID 0x10 on channel 0 and sending PID 0x11 on channel 1 */
            if ((n % 2u) == 0u) {
                uint8 *sdu = NULL_PTR;
                Lin_PduType pdu;
                if (linPhase && Lin_GetStatus(0u, &sdu) == LIN_RX_OK && sdu != NULL_PTR) {
                    memcpy(g_linRx, sdu, 8u);
                    g_linRxOk++;
                }
                pdu.Pid = 0x10u;
                pdu.Cs = LIN_ENHANCED_CS;
                pdu.Drc = LIN_FRAMERESPONSE_RX;
                pdu.Dl = 8u;
                pdu.SduPtr = NULL_PTR;
                (void)Lin_SendFrame(0u, &pdu);
                g_linTxData[0] = (uint8)n;
                g_linTxData[1] = (uint8)g_buttonPresses;
                pdu.Pid = 0x11u;
                pdu.Drc = LIN_FRAMERESPONSE_TX;
                pdu.Dl = 2u;
                pdu.SduPtr = g_linTxData;
                (void)Lin_SendFrame(1u, &pdu);
                g_linTx++;
                linPhase = 1u;
            }
        });
        RUNNABLE(R_CanTx, 15000u, {
            uint8 d[8];
            uint32 restart = g_canRestart;
            uint8 c;
            for (c = 0; c < 2u; ++c) {
                if (restart & (1u << c)) {
                    g_canRestart &= ~(1u << c);
                    (void)Can_43_FLEXCAN_SetControllerMode(c, CAN_CS_STOPPED);
                    (void)Can_43_FLEXCAN_SetControllerMode(c, CAN_CS_STARTED);
                }
            }
            d[0] = (uint8)n;
            d[1] = (uint8)(n >> 8);
            d[2] = (uint8)(n >> 16);
            d[3] = (uint8)(n >> 24);
            d[4] = (uint8)g_buttons;
            d[5] = (uint8)g_canBusOff;
            d[6] = (uint8)g_canRx;
            d[7] = 0u;
            can_send(CanConf_CanHardwareObject_Can0_Tx, DEMO_ID_STATUS, d, 8u, 0u);
            Can_43_FLEXCAN_MainFunction_BusOff();
            Can_43_FLEXCAN_MainFunction_Mode();
        });
    }
}

TASK(Task_100ms_Core0) {
    static uint32 n;
    RUNNABLE(R_100ms_Diag, 1000000u, {
        n++;
        if ((n % 10u) == 0u) {
            sil_log("t=%.3f s: canrx=%u cantx=%u cmd=%u spi=%u/%u shared=%u core1events=%u", (double)SilOs_ClockNow() / 1e9, g_canRx,
                    g_canTxConf, g_canCmd, g_spiEcho, g_spiDone, g_sharedCounter, (unsigned)sil_atomic_load32(&g_core1Events));
            g_odometer++;
            nv_write_odometer();
        }
        (void)ActivateTask(Task_Bg_Core0);
        (void)ActivateTask(Task_Bg_Core0);
    });
    RUNNABLE(R_100ms_Report, 200000u, {
        char line[160];
        I2c_RequestType req;
        uint8 reg = 0u;
        uint8 temp[2];
        int len;
        temp[0] = 0u;
        temp[1] = 0u;
        /* I2C: temperature sensor at 0x48: write the register pointer, read two bytes */
        req.SlaveAddress = DEMO_I2C_TEMP_ADDR;
        req.HighSpeedMode = FALSE;
        req.ExpectNack = FALSE;
        req.RepeatedStart = TRUE;
        req.BitsSlaveAddress = I2C_7_BIT_ADDRESS;
        req.DataDirection = I2C_SEND_DATA;
        req.BufferSize = 1u;
        req.DataBuffer = &reg;
        if (I2c_SyncTransmit(DEMO_I2C_CH, &req) == E_OK) {
            req.DataDirection = I2C_RECEIVE_DATA;
            req.BufferSize = 2u;
            req.DataBuffer = temp;
            if (I2c_SyncTransmit(DEMO_I2C_CH, &req) == E_OK) {
                g_i2cTemp = ((uint32)temp[0] << 8) | temp[1];
                g_i2cReads++;
            }
        }
        g_speedPeriod = Icu_GetTimeElapsed(DEMO_ICU_SPEED);
        g_pulses = Icu_GetEdgeNumbers(DEMO_ICU_PULSES);
        len = snprintf(line, sizeof line, "STAT t=%.1f vbat=%u temp=%u fan=%u speed=%u pulses=%u hall=%u gpt=%u i2c=%u odo=%u boot=%u lin=%u/%u\n",
                       (double)SilOs_ClockNow() / 1e9, (unsigned)g_adcVbat, (unsigned)g_adcTemp, (unsigned)Pwm_GetChannelState(DEMO_PWM_FAN),
                       (unsigned)g_speedPeriod, (unsigned)g_pulses, (unsigned)g_hallEdges, (unsigned)g_gptFired, (unsigned)g_i2cTemp, g_odometer,
                       g_bootCount, g_linRxOk, g_linTx);
        if (len > 0) (void)Uart_SyncSend(DEMO_UART_CH, (const uint8 *)line, (uint32)len, 1000u);
    });
    (void)ChainTask(Task_Coop_Core0);
}

TASK(Task_Bg_Core0) {
    RUNNABLE(R_Bg_RamCheck, 100000u, {});
    (void)TerminateTask();
}

/* schedule table driven (every 25 ms) */
TASK(Task_Hi_Core0) {
    RUNNABLE(R_Hi_Sched, 40000u, {});
    (void)TerminateTask();
}

/* cooperative housekeeping, chained from the 100 ms task */
TASK(Task_Coop_Core0) {
    RUNNABLE(R_Coop, 500000u, {});
    (void)TerminateTask();
}

/* ---- tasks core 1 ------------------------------------------------------------------------ */
TASK(InitTask_Core1) {
    (void)ActivateTask(Task_10ms_Core1);
    (void)ActivateTask(Task_Event_Core1);
    (void)SetRelAlarm(Al_10ms_Core1, 10, 10);
    (void)Os_BarrierSynchronize(Barrier_Init);
    (void)TerminateTask();
}

TASK(Task_10ms_Core1) {
    uint32 speed = 0;
    uint16 sample = 0;
    for (;;) {
        (void)WaitEvent(Ev_10ms_Core1);
        (void)ClearEvent(Ev_10ms_Core1);
        RUNNABLE(R_10ms_Core1, 200000u, {
            uint32 v;
            (void)GetSpinlock(Spin_Shared);
            v = g_crossCoreValue;
            (void)ReleaseSpinlock(Spin_Shared);
            (void)IocRead_Ioc_Speed(&speed);
            sample = (uint16)(sample + 1u + (uint16)v);
            (void)IocSend_Ioc_Samples_0(sample);
        });
    }
}

TASK(Task_Event_Core1) {
    for (;;) {
        (void)WaitEvent(Ev_Trigger_Core1);
        (void)ClearEvent(Ev_Trigger_Core1);
        RUNNABLE(R_Event_Core1, 150000u, { (void)sil_atomic_add32(&g_core1Events, 1u); });
    }
}

/* ---- ISRs (core 0); the driver ISRs are the handlers reached through the OS wrappers ------- */
ISR(Isr_CanRx) { (void)SetEvent(Task_1ms_Core0, Ev_CanRx); }
ISR(Isr_SpiDone) { (void)SetEvent(Task_1ms_Core0, Ev_SpiDone); }
ISR(Isr_Button) { g_buttons++; }

/* ---- main -------------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    SilRt_Options opt;
    int r = SilRt_ParseArgs(&opt, argc, argv);
    if (r != 0) return r < 0 ? 2 : 0;
    if (opt.os.durationNs == 0u && !opt.control) opt.os.durationNs = 2000000000u; /* 2 s default for the demo */
    /* both controllers on one in-process link; --can0=udp etc. override this */
    (void)SilCan_SetBus(0u, "loop:demo");
    (void)SilCan_SetBus(1u, "loop:demo");
    r = SilRt_Run(&opt, demo_core_entry);
    sil_process_exit(r);
}
