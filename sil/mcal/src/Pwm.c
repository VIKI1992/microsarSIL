/* Pwm.c - virtual PWM: duty (0..0x8000) and period of every channel live in the SilIo registry. */
#include "Pwm.h"
#include "SilIo.h"
#include "SilMcal.h"

static const SilPwm_ChannelCfg *cfg_of(Pwm_ChannelType ch) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numPwmChannels; ++i)
        if (SilMcal_Cfg.pwmChannels[i].channelId == ch) return &SilMcal_Cfg.pwmChannels[i];
    return NULL_PTR;
}

void Pwm_Init(const Pwm_ConfigType *ConfigPtr) {
    uint16 i;
    (void)ConfigPtr;
    for (i = 0; i < SilMcal_Cfg.numPwmChannels; ++i) {
        const SilPwm_ChannelCfg *c = &SilMcal_Cfg.pwmChannels[i];
        SilIo_Set(SILIO_PWM, c->channelId, SILIO_F_PERIOD, (double)c->periodDefault);
        SilIo_Set(SILIO_PWM, c->channelId, SILIO_F_VALUE, (double)c->dutyDefault);
        SilIo_Set(SILIO_PWM, c->channelId, SILIO_F_IDLE, 0.0);
    }
}

void Pwm_DeInit(void) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numPwmChannels; ++i) SilIo_Set(SILIO_PWM, SilMcal_Cfg.pwmChannels[i].channelId, SILIO_F_IDLE, 1.0);
}

void Pwm_SetDutyCycle(Pwm_ChannelType ChannelNumber, uint16 DutyCycle) {
    if (DutyCycle > PWM_DUTY_CYCLE_100_U16) DutyCycle = PWM_DUTY_CYCLE_100_U16;
    SilIo_Set(SILIO_PWM, ChannelNumber, SILIO_F_IDLE, 0.0);
    SilIo_Set(SILIO_PWM, ChannelNumber, SILIO_F_VALUE, (double)DutyCycle);
}

void Pwm_SetPeriodAndDuty(Pwm_ChannelType ChannelNumber, Pwm_PeriodType Period, uint16 DutyCycle) {
    SilIo_Set(SILIO_PWM, ChannelNumber, SILIO_F_PERIOD, (double)Period);
    Pwm_SetDutyCycle(ChannelNumber, DutyCycle);
}

void Pwm_SetOutputToIdle(Pwm_ChannelType ChannelNumber) { SilIo_Set(SILIO_PWM, ChannelNumber, SILIO_F_IDLE, 1.0); }

Pwm_OutputStateType Pwm_GetOutputState(Pwm_ChannelType ChannelNumber) {
    const SilPwm_ChannelCfg *c = cfg_of(ChannelNumber);
    uint8 polarityHigh = (c != NULL_PTR) ? c->polarityHigh : 1u;
    if (SilIo_Get(SILIO_PWM, ChannelNumber, SILIO_F_IDLE) != 0.0) return (c != NULL_PTR && c->idleHigh) ? PWM_HIGH : PWM_LOW;
    if (SilIo_Get(SILIO_PWM, ChannelNumber, SILIO_F_VALUE) <= 0.0) return polarityHigh ? PWM_LOW : PWM_HIGH;
    return polarityHigh ? PWM_HIGH : PWM_LOW;
}

void Pwm_DisableNotification(Pwm_ChannelType ChannelNumber) { (void)ChannelNumber; }
void Pwm_EnableNotification(Pwm_ChannelType ChannelNumber, Pwm_EdgeNotificationType Notification) {
    (void)ChannelNumber;
    (void)Notification; /* edge notifications are not modelled (PwmNotificationSupported is false in the reference project) */
}

void Pwm_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = PWM_VENDOR_ID;
    versioninfo->moduleID = PWM_MODULE_ID;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}

void Pwm_SetChannelDeadTime(Pwm_ChannelType ChannelNumber, uint16 DeadTimeTicks) {
    (void)ChannelNumber;
    (void)DeadTimeTicks;
}
void Pwm_SyncUpdate(uint8 ModuleId) { (void)ModuleId; }
uint16 Pwm_GetChannelState(Pwm_ChannelType ChannelNumber) { return (uint16)SilIo_Get(SILIO_PWM, ChannelNumber, SILIO_F_VALUE); }
