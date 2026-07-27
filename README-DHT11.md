# Makeblock Me Humiture Sensor on Raspberry Pi Pico 2 W + FreeRTOS

## 1. What the Makeblock library actually does

`MeHumitureSensor.cpp` is not a Makeblock protocol. The header credits its
origin: it is a fork of the old virtuabotix/Rob Tillaart `dht11.cpp` Arduino
library. `MeHumiture::update()` does exactly the DHT11 single-wire sequence:

| Phase | What the driver does |
|---|---|
| settle | drive line high, `delay(250)` |
| start | drive low `delay(20)` (ms) |
| release | high 40 µs, then switch to `INPUT_PULLUP` |
| ACK | wait for the sensor's ~80 µs low + ~80 µs high |
| 40 bits | for each bit: wait out the 50 µs low, time the following high pulse; `> 40 µs` → `1`, else `0` |
| verify | `data[4] == (data[0]+data[1]+data[2]+data[3]) & 0xFF` |

Bytes are `RH-int, RH-dec, T-int, T-dec, checksum`, MSB first. The Makeblock
class only uses `data[0]` and `data[2]`, i.e. integer °C and integer %RH.

**So: there is nothing Makeblock-specific to port.** You need a DHT11 driver and
the right wiring out of the RJ25 connector. Everything else in the Arduino
library (`MePort`, `dRead2()`, `dWrite2()`) is just Makeblock's port-to-pin map.

## 2. Wiring — soldered to the module's three pads

This build solders directly to the three through-holes next to the RJ25 jack.
Makeblock documents these as the module's port pins for Dupont-wire use, so this
is the intended breakout, not a modification. They carry **GND, VCC and DATA**,
which are the RJ25 GND, VCC and S2 nets — S2 being the line `dWrite2()` /
`dRead2()` drive in the Makeblock library.

### Identify the pads from the DHT11, not from the RJ25

Do not assume the order. The likely order mirrors the connector
(GND, VCC, DATA), but probing the RJ25 contacts is fiddly and the sensor itself
is the better reference.

DHT11 pin order, grille facing you and pins downward, left to right:
**VDD, DATA, NC, GND**.

With the module unpowered, on continuity:

| Pad shows continuity to | Pad is | Connect to Pico 2 W |
|---|---|---|
| DHT11 pin 1 | VCC | 3V3 OUT (pin 36) — see §3 |
| DHT11 pin 4 | GND | any GND (pin 38) |
| DHT11 pin 2 | DATA | GPIO15 |

Then check on resistance: DATA to VCC should read the on-board pull-up, typically
4.7–10 kΩ. If it reads open the module has no pull-up and you must add one.

### Pull-up

**Measure before adding anything** — any resistor you fit is in *parallel* with
the module's.

At 10 kΩ with 20 cm of hookup wire (~20 pF) the time constant is ~0.2 µs, which
is irrelevant against the sensor's 20–40 µs response window. Past roughly a metre
you are at ~1 µs and starting to eat margin; a 10 kΩ from DATA to 3V3 at the Pico
end brings the effective value to 5 kΩ. Run the ground wire alongside the data
wire — twisted, or as a ribbon pair — so the return path is defined.

### Mechanical and thermal

- Give the leads strain relief (hot glue or heat-shrink over the joint). These
  pads lift off a thin PCB easily.
- Keep the iron away from the sensor body. The DHT11's humidity element is a
  polymer film that dislikes prolonged heat and flux vapour. If humidity reads
  high immediately after soldering, leave the module at normal room conditions
  for a few hours — it recovers.
- Don't solvent-wash the board.

## 3. Supply voltage — one measurement decides everything

The on-board pull-up sits between DATA and the module's VCC net, and those pads
*are* that net. **Whatever you feed the VCC pad becomes the logic level of the
data line.** Feed it 3V3 and the whole link is 3.3 V referenced: no level shifter,
no series resistor, pad straight to GPIO15.

That matters because **RP2350 GPIOs are not 5 V tolerant** (abs max ≈
VDDIO + 0.3 V). Powering the module from 5 V would put 5 V on the data line.
Soldered access removes both the temptation and the ambiguity.

### The check

Power the VCC pad from 3V3 and **measure at DHT11 pin 1, not at the pad.**
Makeblock fits reverse-polarity protection on these modules because their *red*
ports carry 6–12 V, and a series MOSFET or diode can cost you a few hundred
millivolts.

| Reading at DHT11 pin 1 | Action |
|---|---|
| ≥ 3.1 V | Done. Wire straight through; ignore level shifting entirely. |
| < 3.0 V | Below the DHT11 floor — see below. |

A sensor sitting under 3.0 V produces the nastiest failure in the table in §6:
the frame is well-formed and the checksum passes, but every value reads zero.

### If the measurement comes up short

- **Power the pad from VBUS (5 V) and put a BSS138-style bidirectional shifter on
  DATA** (10 kΩ to 3V3 low side, 10 kΩ to 5 V high side). Not a resistor divider —
  the line is bidirectional and a divider fights the pull-ups.
- **Or solder VCC directly to DHT11 pin 1**, bypassing the module's protection.
  Keep GND and DATA on the pads so the on-board pull-up stays referenced to the
  same rail.

### Firmware impact

None. The PIO program, driver and task are unaffected. `gpio_pull_up(pin)` in
`dht_program_init()` stays — at ~56 kΩ in parallel with the module's 10 kΩ it is a
rounding error, and it keeps the line defined if a wire ever comes off.

## 4. Why PIO instead of bit-banging

The Arduino approach measures 26 µs vs 70 µs pulses in a polling loop. Under
FreeRTOS on RP2350 that means either:

* accepting that any interrupt — CYW43 SPI, lwIP timer, systick, mbedTLS — lands
  mid-frame and corrupts the bit, or
* wrapping ~5 ms of the frame in `taskENTER_CRITICAL()`, which destroys your
  interrupt latency and, in the SMP port (`configNUMBER_OF_CORES=2`), only masks
  interrupts on the calling core anyway.

Neither is acceptable in a device that is simultaneously running a TLS MQTT
session. The PIO state machine does the timing in hardware; the task sleeps
through the frame with `vTaskDelay()` and then drains five words from the RX
FIFO. CPU cost is effectively zero and there is no jitter coupling at all.

Resource cost: one state machine, 17 instructions, one GPIO. Note that
**pio0 is typically claimed by the CYW43 driver on Pico W hardware**, so this
code uses `pio1`.

### How the PIO program works

Clocked at exactly 1 instruction per microsecond (`clkdiv = clk_sys / 1 MHz`):

1. `set pindirs,1` + `set pins,0`, then a nested `jmp x--/jmp y--` loop burning
   20 × 30 × 31 µs ≈ 18.6 ms — the start pulse.
2. `set pindirs,0` releases the line; three `wait` instructions consume the
   sensor's ACK low/high and land on the falling edge that starts bit 0.
3. Per bit: `wait 1 pin 0` catches the rising edge, `nop [31]` + `nop [9]`
   burns ~43 µs, then `in pins,1` samples. A `0` bit (27 µs) has already fallen;
   a `1` bit (70 µs) is still high. ~15 µs of margin on both sides.
4. `sm_config_set_in_shift(..., false, true, 8)` autopushes one byte at a time,
   MSB first. `PIO_FIFO_JOIN_RX` gives an 8-deep RX FIFO — required, because a
   frame pushes 5 words and the default depth is 4.
5. After 40 bits the program parks in a self-loop. `dht_read()` re-arms the SM
   for each sample, so a dead or unplugged sensor stalls harmlessly on a `wait`
   instead of leaking a half-frame into the next read.

## 5. Build integration

```cmake
# CMakeLists.txt
pico_generate_pio_header(medimate ${CMAKE_CURRENT_LIST_DIR}/src/dht.pio)

target_sources(medimate PRIVATE
    src/dht.c
    src/humiture_task.c
)

target_link_libraries(medimate PRIVATE
    pico_stdlib
    hardware_pio
    hardware_clocks
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
```

Then, after the other MediMate tasks are created and before `vTaskStartScheduler()`:

```c
#include "humiture_task.h"   /* or just declare the two prototypes */

bool humiture_task_start(UBaseType_t priority);
bool humiture_get_latest(dht_reading_t *out, uint32_t *age_ms);

humiture_task_start(tskIDLE_PRIORITY + 1);   /* low priority; it only sleeps */
```

To forward samples to AWS IoT, override the weak hook — the coreMQTT-Agent is
thread-safe, but keep this function short and non-blocking:

```c
void humiture_on_sample(const dht_reading_t *r)
{
    char json[96];
    snprintf(json, sizeof json,
             "{\"state\":{\"reported\":{\"tempC\":%d.%d,\"rh\":%d.%d}}}",
             r->temperature_x10 / 10, r->temperature_x10 % 10,
             r->humidity_x10    / 10, r->humidity_x10    % 10);
    shadow_update_enqueue(json);   /* your agent wrapper */
}
```

`humiture_get_latest()` is the read path for your httpd SSI handlers.

## 6. Bring-up checklist

1. Power the module, wait 2 s, take a reading. `DHT_ERR_TIMEOUT` = the sensor
   never pulled the line low → wiring, power or wrong RJ25 pin (S1 vs S2).
2. `DHT_ERR_CHECKSUM` on some but not all reads → pull-up too weak, cable too
   long, or VCC sagging. Try 4.7 kΩ and shorten the cable.
3. Consistently 0 °C / 0 % with a valid checksum → you are reading a genuine
   frame but the sensor is unhappy with its supply. Measure VCC at the module.
4. Scope tip: the ACK pulse (80 µs low, 80 µs high) tells you immediately
   whether the sensor is alive, independent of any decoding bug.

## 7. Fit for MediMate — a design note worth weighing

DHT11 accuracy is roughly **±2 °C and ±5 % RH**, with 1 °C / 1 % resolution and
a 0–50 °C, 20–90 % RH range. For a connected pill dispenser, typical storage
requirements are "below 25 °C" or "15–25 °C" and "protect from moisture", often
with RH limits around 60 %. A ±5 % RH, ±2 °C sensor cannot credibly evidence
compliance with those limits — the measurement uncertainty is a large fraction
of the tolerance band, and DHT11 humidity drift over a year is significant.

The DHT11 is fine as a development stand-in and for coarse "is it obviously too
hot/humid" alarms. If temperature/humidity logging is going to be a claimed
feature rather than a demo, an **SHT40/SHT45 (±0.2 °C, ±1.8 % RH) or BME280** is
a better part: I²C, no PIO, no single-wire timing, ~€3, and it frees the state
machine. Same task structure, you would just swap the driver behind
`humiture_get_latest()` — which is why the task keeps the sensor private.

## 8. If you want the bit-banged version anyway

For a quick test on bare metal without FreeRTOS, the direct port of the
Makeblock logic is:

```c
static inline uint32_t wait_level(uint pin, bool level, uint32_t timeout_us) {
    uint32_t t0 = time_us_32();
    while (gpio_get(pin) != level) {
        if (time_us_32() - t0 > timeout_us) return 0;
    }
    return time_us_32() - t0;
}

bool dht_read_bitbang(uint pin, uint8_t out[5]) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    busy_wait_ms(20);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    uint32_t irq = save_and_disable_interrupts();   /* <-- the problem */
    bool ok = wait_level(pin, 0, 100) && wait_level(pin, 1, 100)
                                      && wait_level(pin, 0, 100);
    for (int i = 0; ok && i < 40; i++) {
        ok = wait_level(pin, 1, 100);
        uint32_t t0 = time_us_32();
        ok = ok && wait_level(pin, 0, 150);
        out[i / 8] = (uint8_t)((out[i / 8] << 1) |
                               ((time_us_32() - t0 > 45) ? 1 : 0));
    }
    restore_interrupts(irq);

    return ok && (uint8_t)(out[0]+out[1]+out[2]+out[3]) == out[4];
}
```

That `save_and_disable_interrupts()` covers ~5 ms. With Wi-Fi and TLS running it
is not something you want in MediMate — which is the whole argument for the PIO
version above.
