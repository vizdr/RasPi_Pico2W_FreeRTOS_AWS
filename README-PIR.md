# Makeblock Me PIR Motion Sensor v1.1 on Raspberry Pi Pico 2 W + FreeRTOS

## 1. What the sensor gives you

One digital output: **high while motion is detected**, low otherwise. After the
last detected movement it stays high for the hold time set by the module's
potentiometer. The Makeblock Arduino library (`MePIRMotionSensor::isHumanDetected()`)
just reads that line. As with the humiture sensor, there is no protocol to port,
only a GPIO to read.

The firmware maps the two edges to two events:

| Edge | Event | Hook |
| --- | --- | --- |
| rising | motion start | `pir_on_motion_start(time_ms)` |
| falling | motion stop | `pir_on_motion_stop(time_ms, duration_ms)` |

The trigger mode is set by the BISS0001's **A** input, wired to RJ25 **S1**
(Makeblock: `SetPirMotionMode()` → `dWrite1()`; 1 = retriggerable). The firmware
drives S1 high at start-up to select **retriggerable** mode. The output then
stays high for as long as movement continues, so one start/stop pair means one
period of activity. In single-trigger mode the output drops after the hold
time even while someone is still moving, and you get several short start/stop
pairs instead. `pir_task_set_retrigger(false)` switches to single-trigger mode
at runtime.

## 2. Wiring: soldered to the pads next to the RJ25 jack

| Module pad | Connect to Pico 2 W |
| --- | --- |
| VCC | VBUS (pin 40) = 5 V when USB powered, or VSYS (pin 39) |
| GND | GND (pin 18, between GPIO13 and GPIO14) |
| OUT (the pad you measured, RJ25 S2) | **GPIO14 (pin 19)** |
| MODE (RJ25 S1) | **GPIO13 (pin 17)** |

GPIO13 and GPIO14 are plain digital pins: not ADC inputs, not used by UART0
stdio (GPIO0/1), I2C0 (GPIO4/5) or the DHT11 (GPIO15). To use other pins,
change `PIR_GPIO` / `PIR_MODE_GPIO` in `pir_task.c` (or pass `-DPIR_GPIO=n`),
and keep away from the ADC-capable GPIO26–29. If S1 is not wired, set
`PIR_MODE_GPIO` to `PIR_NO_PIN`; the module's own default mode then applies.

GPIO13 is a 3.3 V push-pull output. The BISS0001 runs from roughly the same
~3.8 V that appears on its output, so 3.3 V should read as a logic high on A.
Confirm on the bench (§6, step 4).

## 3. The 3.8 V output level

The output is measured at up to **3.8 V**, which is above the 3.3 V rail. On
GPIO14 that is within specification.

The Pico 2 W uses the RP2350A. In its pin list (datasheet Table 1427) GPIO0–25
are type **Digital IO (FT)**, i.e. fault-tolerant. The limits for FT pins:

| Condition (RP2350 datasheet §14.9) | Limit on an FT pin | 3.8 V is |
| --- | --- | --- |
| Input high (VIH), IOVDD = 3.3 V | 2.0 V … 5.5 V | a valid logic 1 |
| Absolute maximum, IOVDD = 3.3 V (Table 1433) | 5.5 V | 1.7 V under |
| Absolute maximum, IOVDD = 0 V (Pico unpowered) | 3.63 V | 0.17 V **over** |

- **Normal operation needs no level shifter.** The 5.5 V limit is an operating
  input range, not just a damage threshold.
- **Unpowered is the only case to avoid.** It matters only if the PIR output
  can be high while the Pico's 3V3 is off: for example, a separate PIR supply,
  or 3V3_EN pulled low with VBUS still present. When both run from the same
  VBUS they come up together, and PIR outputs are low right after power-on. If
  your setup can hit that case, add a 1 kΩ series resistor in the OUT line to
  limit the current.
- **Not on GPIO26–29.** On the RP2350A these are "Digital IO / Analogue"
  (standard) pins, limited to IOVDD + 0.5 V = 3.8 V, exactly the measured
  level with no margin. Above IOVDD they leak through their ESD diodes (§12.4).
- **No internal pulls.** The sensor drives the line actively both high and low,
  so the firmware calls `gpio_disable_pulls()`. An internal pull-up would not
  be harmful here: at most ~16 µA flows from 3.8 V through ≥ 32 kΩ into the
  rail.
- **No high-value resistor divider.** Erratum RP2350-E9 affects A2-stepping
  chips (fixed in A3): an input sitting between V_IL (0.8 V) and
  V_IH (2.0 V) can source ~120 µA and hold itself near 2.2 V unless
  driven through ≤ 8.2 kΩ. The PIR's push-pull output passes through that
  region only on its edges, so it is unaffected when wired directly.

Source: [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
(Tables 1426, 1427, 1433; §14.9 GPIO electrical characteristics; RP2350-E9).

## 4. Firmware design

The code is split the same way as the DHT11 (`dht.c` driver, `humiture_task.c`
service):

| Layer | Files | Owns |
| --- | --- | --- |
| Driver | `pir.c/h` | Pin setup, trigger mode (S1), `pir_read()`, the GPIO ISR. No FreeRTOS. |
| Service | `pir_task.c/h` | Queue, task, warm-up, start/stop state, logging, hooks, `pir_get_status()` |

```
             pir.c (driver)                          pir_task.c (service)
PIR OUT ──► GPIO14 edge IRQ ──► pir_gpio_isr() ──cb──► pir_on_edge_isr() ──queue──► pir task ──► hooks + status
(S2)        (rise + fall)       ack, level, time_ms    xQueueSendFromISR             start/stop,  pir_on_motion_*()
                                                                                    count, log   pir_get_status()

MODE    ◄── GPIO13 output ◄──── pir_set_retrigger() ◄──── pir_task_set_retrigger()
(S1)
```

- **Driver ISR** (`pir_gpio_isr`): the ISR is registered with
  `gpio_add_raw_irq_handler()`, not `gpio_set_irq_enabled_with_callback()`.
  `IO_IRQ_BANK0` is shared with the CYW43 driver, which has its own raw handler
  for `WL_HOST_WAKE` (GPIO24). The ISR checks and acknowledges only its own pin,
  samples the *current level* and a timestamp, and passes them to the callback
  given to `pir_irq_enable()`. SDK raw handlers take no context argument, so the
  driver supports one sensor (a second `pir_init()` returns `false`).
- **Edge callback** (`pir_on_edge_isr`, in `pir_task.c`): still interrupt
  context. It posts the event to a queue with `xQueueSendFromISR()` and does
  no printing and nothing that blocks. The IRQ runs at the SDK default priority
  (0x80), which is below `configMAX_SYSCALL_INTERRUPT_PRIORITY` (16), so the
  FromISR API is legal there.
- **Task** (`pir`, priority idle+2): the task turns the level into a transition
  only when the level differs from the current state. As a result:
  - a glitch shorter than the IRQ latency (rise and fall both latched) is
    dropped;
  - a lost edge is recovered by a pin re-read every `PIR_RESYNC_MS` (1 s).
- **Warm-up**: PIR elements give false triggers for a while after power-up.
  Edge interrupts are enabled only after `PIR_WARMUP_MS` (30 s, defined in
  `pir.h` as a sensor property). `pir_irq_enable()` acknowledges and discards
  the edges latched during that time. `pir_get_status()` returns
  `false` until then.

## 5. Using the events

Override the weak hooks anywhere in the project. They run in the `pir` task, so
you can call normal FreeRTOS APIs, but keep them short and non-blocking:

```c
#include "pir_task.h"

void pir_on_motion_start(uint32_t time_ms)
{
    xEventGroupSetBits(app_events, APP_MOTION_BIT);
}

void pir_on_motion_stop(uint32_t time_ms, uint32_t duration_ms)
{
    xEventGroupClearBits(app_events, APP_MOTION_BIT);
}
```

`time_ms` is the edge time sampled in the ISR (ms since boot, 32-bit), not the time the
hook runs. Only one strong override of each hook can exist in the program; in this
project it is in `lan_mqtt_task.c`, which forwards the events to the Pi's MQTT broker.

To read the state instead, call `pir_get_status(&st)`. It returns
`st.motion`, `st.motion_count` and `st.last_change_ms`.

## 6. Bring-up checklist

1. On boot, the serial console shows `[pir] warming up for 30 s` and then
   `[pir] ready`.
2. Wave a hand: `[pir] motion start (#1)`. Stay still for longer than the hold
   time: `[pir] motion stop after N ms`.
3. Nothing ever triggers: check with a meter that OUT goes high on motion at the
   pad. Then check that it reaches pin 19, and not pin 20 (GPIO15, the DHT11).
4. `motion stop` arrives only seconds after `start` although movement continues:
   the module is in single-trigger mode. Check that S1 is wired to pin 17 and
   reads ~3.3 V; if it does and the behaviour persists, 3.3 V may be too low a
   logic high for the module's A input. Otherwise the hold-time pot is at
   minimum.
5. `event queue overflowed`: the input is toggling far faster than any PIR
   can. Look for a floating or loose wire, or a long unshielded run beside the
   WiFi antenna.
