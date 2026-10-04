// Host-side test of boot_ms_widen() (boot_time.h), including the 49.7-day wrap of the
// 32-bit to_ms_since_boot() counter (PIR-MQTT-VMS-Pico.md §4.3, Phase 1 check).
//
//   cc -std=c11 -Wall -Wextra -I.. boot_time_test.c -o /tmp/boot_time_test && /tmp/boot_time_test
#include "boot_time.h"

#include <inttypes.h>
#include <stdio.h>

static int failures;

static void check(const char *name, uint64_t event64, uint64_t now64)
{
    uint64_t got = boot_ms_widen(now64, (uint32_t)event64);
    if (got != event64) {
        printf("FAIL %-34s event=%" PRIu64 " now=%" PRIu64 " got=%" PRIu64 "\n",
               name, event64, now64, got);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    const uint64_t wrap = UINT64_C(1) << 32;   // 4294967296 ms = 49.7 days

    check("same instant",                    1000, 1000);
    check("event 5 s old, early in boot",    10000, 15000);
    check("event just before the wrap",      wrap - 5, wrap + 7);    // the case §4.3 names
    check("event exactly at the wrap",       wrap, wrap + 1);
    check("both after the wrap",             wrap + 100, wrap + 250);
    check("second wrap (99.4 days)",         2 * wrap - 1, 2 * wrap + 1);
    check("event 60 s old across the wrap",  wrap - 30000, wrap + 30000);
    check("oldest exact age (2^32 - 1 ms)",  1, wrap);

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
