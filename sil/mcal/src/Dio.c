/* Dio.c / Port - virtual digital I/O on the SilIo registry. Channel values follow the NXP RTD scheme
 * (port index * 16 + pin) through the generated channel table; unknown channels are keyed as given. */
#include "Dio.h"
#include "Port.h"
#include "SilIo.h"
#include "SilMcal.h"

static const SilDio_ChannelCfg *chan_cfg(Dio_ChannelType ch) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numDioChannels; ++i)
        if (SilMcal_Cfg.dioChannels[i].channelId == ch) return &SilMcal_Cfg.dioChannels[i];
    return NULL_PTR;
}

/* channel key of (port, bit): the configured channel if there is one, else a private key */
static uint32 port_bit_key(Dio_PortType port, uint8 bit) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numDioChannels; ++i)
        if (SilMcal_Cfg.dioChannels[i].portId == port && SilMcal_Cfg.dioChannels[i].bit == bit) return SilMcal_Cfg.dioChannels[i].channelId;
    return 0x10000u + ((uint32)port << 4) + bit;
}

Dio_LevelType Dio_ReadChannel(Dio_ChannelType ChannelId) { return (SilIo_Get(SILIO_DIO, ChannelId, SILIO_F_VALUE) != 0.0) ? STD_HIGH : STD_LOW; }

void Dio_WriteChannel(Dio_ChannelType ChannelId, Dio_LevelType Level) { SilIo_Set(SILIO_DIO, ChannelId, SILIO_F_VALUE, Level ? 1.0 : 0.0); }

Dio_LevelType Dio_FlipChannel(Dio_ChannelType ChannelId) {
    Dio_LevelType l = Dio_ReadChannel(ChannelId) ? STD_LOW : STD_HIGH;
    Dio_WriteChannel(ChannelId, l);
    return l;
}

Dio_PortLevelType Dio_ReadPort(Dio_PortType PortId) {
    Dio_PortLevelType level = 0;
    uint8 bit;
    for (bit = 0; bit < 16u; ++bit)
        if (SilIo_Get(SILIO_DIO, port_bit_key(PortId, bit), SILIO_F_VALUE) != 0.0) level |= (Dio_PortLevelType)(1u << bit);
    return level;
}

void Dio_MaskedWritePort(Dio_PortType PortId, Dio_PortLevelType Level, Dio_PortLevelType Mask) {
    uint8 bit;
    for (bit = 0; bit < 16u; ++bit)
        if (Mask & (1u << bit)) SilIo_Set(SILIO_DIO, port_bit_key(PortId, bit), SILIO_F_VALUE, (Level & (1u << bit)) ? 1.0 : 0.0);
    SilIo_Set(SILIO_DIO_PORT, PortId, SILIO_F_VALUE, (double)Dio_ReadPort(PortId));
}

void Dio_WritePort(Dio_PortType PortId, Dio_PortLevelType Level) { Dio_MaskedWritePort(PortId, Level, 0xFFFFu); }

Dio_PortLevelType Dio_ReadChannelGroup(const Dio_ChannelGroupType *ChannelGroupIdPtr) {
    if (ChannelGroupIdPtr == NULL_PTR) return 0;
    return (Dio_PortLevelType)((Dio_ReadPort(ChannelGroupIdPtr->port) & ChannelGroupIdPtr->mask) >> ChannelGroupIdPtr->offset);
}

void Dio_WriteChannelGroup(const Dio_ChannelGroupType *ChannelGroupIdPtr, Dio_PortLevelType Level) {
    if (ChannelGroupIdPtr == NULL_PTR) return;
    Dio_MaskedWritePort(ChannelGroupIdPtr->port, (Dio_PortLevelType)(Level << ChannelGroupIdPtr->offset), ChannelGroupIdPtr->mask);
}

void Dio_GetVersionInfo(Std_VersionInfoType *VersionInfo) {
    if (VersionInfo == NULL_PTR) return;
    VersionInfo->vendorID = DIO_VENDOR_ID;
    VersionInfo->moduleID = DIO_MODULE_ID;
    VersionInfo->sw_major_version = 1u;
    VersionInfo->sw_minor_version = 0u;
    VersionInfo->sw_patch_version = 0u;
}

/* keep the table referenced even when no channel is configured */
const char *SilDio_ChannelName(Dio_ChannelType ChannelId);
const char *SilDio_ChannelName(Dio_ChannelType ChannelId) {
    const SilDio_ChannelCfg *c = chan_cfg(ChannelId);
    return c != NULL_PTR ? c->name : "";
}

/* ---- Port ------------------------------------------------------------------ */
void Port_Init(const Port_ConfigType *ConfigPtr) { (void)ConfigPtr; }
void Port_SetPinDirection(Port_PinType Pin, Port_PinDirectionType Direction) {
    (void)Pin;
    (void)Direction;
}
void Port_RefreshPortDirection(void) {}
void Port_SetPinMode(Port_PinType Pin, Port_PinModeType Mode) {
    (void)Pin;
    (void)Mode;
}
void Port_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = 43u;
    versioninfo->moduleID = 124u;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}
