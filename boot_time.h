#pragma once

#include <stdint.h>

// The PIR edge timestamps (pir.c, to_ms_since_boot()) are 32-bit milliseconds since boot
// and wrap after 49.7 days. The Pi treats boot_ms going backwards in the pir/state stream
// as a Pico reboot, so every payload carries a 64-bit boot_ms that never wraps
// (PIR-MQTT-VMS-Pico.md §3.3, rule 1). The 32-bit value is widened here, at publish time,
// not in the ISR.
//
// now64 must come from the same clock (time_us_64() / 1000) and be taken after then32.
// Exact for any timestamp younger than 2^32 ms; the unsigned subtraction handles a wrap
// between the two.
static inline uint64_t boot_ms_widen(uint64_t now64, uint32_t then32)
{
    return now64 - (uint32_t)((uint32_t)now64 - then32);
}
