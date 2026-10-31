/*
 * SilIo.h - virtual I/O registry shared by the virtual MCAL drivers, the control server and the models.
 *
 * Every pin, analog input, PWM output and capture input is an entry (kind, id) with a few numeric
 * fields. Drivers read and write it; test scripts set inputs and read outputs through the control
 * server ("io_set", "io_get", "io_list"); subscribers (ICU edge detection) are told about changes.
 *
 * Keys understood by SilIo_Parse: "<kind>.<id>[.<field>]" with kind dio | port | adc | pwm | icu and
 * field value (default) | period | freq | duty | tickhz | idle, or a generated symbolic name such as
 * "DioConf_DioChannel_LED_GREEN" (optionally followed by ".<field>").
 */
#ifndef SILIO_H
#define SILIO_H

#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SILIO_DIO = 0, SILIO_DIO_PORT, SILIO_ADC, SILIO_PWM, SILIO_ICU, SILIO_KINDS } SilIo_Kind;

#define SILIO_F_VALUE 0u   /* dio level 0/1, adc raw counts, pwm duty 0..0x8000, icu static level */
#define SILIO_F_PERIOD 1u  /* pwm period (ticks) */
#define SILIO_F_FREQ 2u    /* icu signal generator frequency (Hz, 0 = static level) */
#define SILIO_F_DUTY 3u    /* icu signal generator duty 0..1 (default 0.5) */
#define SILIO_F_TICKHZ 4u  /* icu timer tick frequency (default 1 MHz) */
#define SILIO_F_IDLE 5u    /* pwm output set to idle (1) */
#define SILIO_FIELDS 6u

typedef struct {
    const char *symbol;   /* e.g. DioConf_DioChannel_LED_GREEN */
    uint8 kind;           /* SilIo_Kind */
    uint32 id;
} SilIo_NameCfg;

double SilIo_Get(SilIo_Kind kind, uint32 id, uint8 field);
/* Thread safe; change subscribers run in the caller's thread after the value was stored. */
void SilIo_Set(SilIo_Kind kind, uint32 id, uint8 field, double value);
int SilIo_IsSet(SilIo_Kind kind, uint32 id, uint8 field);

typedef void (*SilIo_ChangeFn)(void *user, SilIo_Kind kind, uint32 id, uint8 field, double oldValue, double newValue);
void SilIo_Subscribe(SilIo_Kind kind, SilIo_ChangeFn fn, void *user);

/* "dio.17", "pwm.3.period", "DioConf_DioChannel_LED.value" -> (kind, id, field). Returns 0 on success. */
int SilIo_Parse(const char *key, SilIo_Kind *kind, uint32 *id, uint8 *field);
const char *SilIo_KindName(SilIo_Kind kind);
const char *SilIo_FieldName(uint8 field);
/* symbolic name of an entry if the generated name table has one, else NULL */
const char *SilIo_Symbol(SilIo_Kind kind, uint32 id);

typedef void (*SilIo_VisitFn)(void *user, SilIo_Kind kind, uint32 id, uint8 field, double value);
/* All fields that have been set, in no particular order. */
void SilIo_Visit(SilIo_VisitFn fn, void *user);

/* Generated name table (SilMcal_Cfg.c); empty when no MCAL ECUC was given. */
extern const SilIo_NameCfg SilIo_Names[];
extern const uint16 SilIo_NumNames;

#ifdef __cplusplus
}
#endif

#endif /* SILIO_H */
