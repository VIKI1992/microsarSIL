/*
 * sil_bus.h - virtual CAN bus backends.
 *
 * A bus delivers frames between the virtual CAN controllers of this process and external
 * participants. Backends (selected by a spec string):
 *   "null"                        frames are dropped, nothing is received
 *   "loop:<name>"                 in-process link: every frame sent on an endpoint of <name>
 *                                 is received by all other endpoints of <name>
 *   "udp:<group>:<port>"          UDP multicast (default 239.0.0.1:52000); python-can speaks
 *                                 the same wire format through python/microsarsil
 *
 * Wire format of a UDP datagram (little endian):
 *   0  "SILCAN1\0" magic (8)   8  u32 sender node id   12 u32 CAN id   16 u8 flags
 *   17 u8 len                  18 u16 reserved         20 u64 sender time (ns)   28 data[len]
 *   flags: bit0 extended id, bit1 CAN FD, bit2 RTR, bit3 BRS
 * Receivers ignore datagrams carrying their own node id (multicast loopback).
 */
#ifndef SIL_BUS_H
#define SIL_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SILBUS_MAX_DATA 64u
#define SILBUS_FLAG_EXT 0x01u
#define SILBUS_FLAG_FD 0x02u
#define SILBUS_FLAG_RTR 0x04u
#define SILBUS_FLAG_BRS 0x08u

typedef struct {
    uint32_t id;      /* CAN identifier (11 or 29 bit) */
    uint8_t flags;    /* SILBUS_FLAG_* */
    uint8_t len;      /* 0..64 */
    uint8_t data[SILBUS_MAX_DATA];
    uint64_t t_ns;    /* sender timestamp */
} SilBus_Frame;

typedef struct SilBus SilBus;
typedef void (*SilBus_RxFn)(void *user, const SilBus_Frame *frame);

/* Opens a backend. rx is called from a backend thread (udp) or from the sender's thread (loop). */
SilBus *SilBus_Open(const char *spec, SilBus_RxFn rx, void *user);
int SilBus_Send(SilBus *bus, const SilBus_Frame *frame);
void SilBus_Close(SilBus *bus);
const char *SilBus_Kind(const SilBus *bus);
uint32_t SilBus_NodeId(const SilBus *bus);
/* Encode/decode the UDP wire format (also used by tests). Returns the datagram length or 0. */
unsigned SilBus_Encode(const SilBus_Frame *frame, uint32_t nodeId, uint8_t *buf, unsigned bufLen);
int SilBus_Decode(const uint8_t *buf, unsigned len, SilBus_Frame *frame, uint32_t *nodeId);

#ifdef __cplusplus
}
#endif

#endif /* SIL_BUS_H */
