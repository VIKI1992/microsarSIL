/* Pit_Ip.h - stand-in for the NXP PIT IP layer; the lifetime timer is the virtual clock at 100 MHz. */
#ifndef PIT_IP_H
#define PIT_IP_H

#include "Std_Types.h"

#define PIT_IP_LIFETIME_HZ (100000000u)

void Pit_Ip_SetLifetimeTimer(uint8 instance);
uint64 Pit_Ip_GetLifetimeTimer(uint8 instance);
uint32 Pit_Ip_GetCurrentTimer(uint8 instance, uint8 channel);

#endif /* PIT_IP_H */
