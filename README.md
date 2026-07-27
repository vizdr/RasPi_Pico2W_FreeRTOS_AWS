# blink_freertos

FreeRTOS on a Raspberry Pi Pico 2 W (RP2350 + CYW43439 WiFi), starting from a blinking LED and
ending with a working telemetry pipeline: onboard temperature + DHT11 humidity/ambient-temperature
readings, published over mutual-TLS MQTT to AWS IoT Core, routed by an IoT Rule into an SQS queue.

Built with the [Raspberry Pi Pico VS Code extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico),
Pico SDK 2.3.0, and the FreeRTOS-Kernel `RP2350_ARM_NTZ` SMP port.

## What's here

- **WiFi station connection** with automatic reconnect (`wifi_task`)
- **SNTP time sync**, required before any TLS certificate validity check can succeed (`time_task`)
- **Mutual-TLS MQTT client** to AWS IoT Core — device certificate + private key, server identity
  verified against the Amazon Root CA (`aws_iot_task`)
- **Sensor telemetry**:
  - RP2350's internal die temperature sensor (`temp_task`)
  - DHT11 humidity + ambient temperature over a PIO-based single-wire driver (`humiture_task`,
    `dht.c/h/.pio`) — no bit-banging, no interrupt-masking, the PIO state machine does the
    microsecond-level timing in hardware while the task just sleeps
  - I2C MPU6050 accelerometer example, not currently wired to real hardware (`sensor_task`)
- **AWS IoT Rule → SQS** — telemetry is routed from the device's MQTT topic into an SQS queue for
  downstream consumption, independent of the MQTT test client

## Hardware

| | |
|---|---|
| Board | Raspberry Pi Pico 2 W (RP2350, Cortex-M33, CYW43439 WiFi/BT) |
| DHT11 | Makeblock Me Humiture Sensor (a DHT11 clone) — DATA on GPIO15, 3V3, GND. See [README-DHT11.md](README-DHT11.md) for wiring, voltage-level details, and why this uses PIO instead of bit-banging. |
| MPU6050 | I2C0, SDA=GPIO4, SCL=GPIO5 — driver present, not physically wired |
| Debug | Raspberry Pi Debug Probe (CMSIS-DAP + UART bridge) |

## Building and flashing

This project is set up for the Pico VS Code extension, which manages the SDK/toolchain/FreeRTOS-Kernel
under `~/.pico-sdk` and provides the CMake kit. From VS Code:

1. **Compile Project** (build task) — runs `ninja -C build`.
2. **Run Project** / **Flash** (build tasks in `.vscode/tasks.json`) — flashes via `picotool` or
   OpenOCD + Debug Probe.
3. **Pico Debug (Cortex-Debug)** (Run and Debug panel, F5) — OpenOCD + GDB via the Debug Probe.

Serial output is on UART (via the Debug Probe's UART bridge), not USB CDC — USB stdio is disabled
(see the comment in `CMakeLists.txt` for why). Connect at 115200 8N1:
```bash
picocom -b 115200 /dev/ttyACM<N>   # find the right port with: ls /dev/ttyACM*
```

Command-line equivalent, if not using the extension:
```bash
cmake -S . -B build
cmake --build build
```

## Configuration required before first build

Two things are gitignored and must be created locally — the project won't build/run without them:

**1. WiFi credentials**
```bash
cp wifi_credentials.h.example wifi_credentials.h
# then edit wifi_credentials.h with your actual SSID/password
```

**2. AWS IoT Core credentials** (only needed if you want the `aws_iot_task`/telemetry path — the
rest of the project builds and runs without AWS at all)
- Create a Thing, device certificate, and policy in AWS IoT Core.
- Drop the device certificate, private key, and Amazon Root CA as PEM files into `certs/`.
- Update the endpoint/client ID/topic constants at the top of `aws_iot_task.c`.
- Generate the embedded credential arrays:
  ```bash
  python3 generate_aws_credentials.py
  ```
  This reads `certs/*.pem` and writes `aws_credentials.c` (gitignored — it embeds the private key).

Full step-by-step AWS setup, including the IoT policy pitfalls and how to test with the AWS CLI,
is in [AWS-RasPi_PicoW2.md](AWS-RasPi_PicoW2.md).

## Project layout

| File | Role |
|---|---|
| `main.c` | Task creation, FreeRTOS hooks (`vApplicationMallocFailedHook` etc.) |
| `CMakeLists.txt` | Build configuration, library links, compile definitions |
| `FreeRTOSConfig.h` | FreeRTOS-Kernel configuration for the RP2350 SMP port |
| `lwipopts.h` | lwIP configuration (SNTP, MQTT, TLS-over-altcp tuning) |
| `mbedtls_config.h` | mbedtls configuration (TLS 1.2, ciphersuites, buffer sizing) |
| `wifi_task.c/h` | WiFi station connect + auto-reconnect; `wifi_wait_connected()` |
| `time_task.c/h` | SNTP time sync; `time_wait_synced()`; drives `settimeofday()` |
| `aws_iot_task.c/h` | DNS resolve, mutual-TLS MQTT connect, periodic publish, auto-reconnect |
| `aws_credentials.h` / `aws_credentials.c`* | Embedded cert/key/root-CA byte arrays (`.c` generated, gitignored) |
| `generate_aws_credentials.py` | Regenerates `aws_credentials.c` from `certs/*.pem` |
| `wifi_credentials.h`* / `.h.example` | WiFi SSID/password (`.h` gitignored) |
| `certs/`* | Raw PEM files from AWS IoT Core (gitignored) |
| `led.c/h` | LED as WiFi connection indicator |
| `temp_task.c/h` | Internal RP2350 temperature sensor; feeds telemetry |
| `humiture_task.c/h` | FreeRTOS task wrapping the DHT11 driver; feeds telemetry |
| `dht.c/h`, `dht.pio` | PIO-based DHT11/DHT22 single-wire driver |
| `sensor_task.c/h` | I2C MPU6050 example (independent of the AWS IoT path) |

\* gitignored — see [Configuration](#configuration-required-before-first-build) above.

## Documentation

- **[Manual_Setup_FreeRTOS_RasPi_Pico2w.md](Manual_Setup_FreeRTOS_RasPi_Pico2w.md)** — getting a
  bare FreeRTOS project actually building and running on the Pico 2 W: every bug hit standing this
  up from a manually-assembled project, plus how to read fault registers and the vector table
  directly via a debug probe.
- **[AWS-RasPi_PicoW2.md](AWS-RasPi_PicoW2.md)** — the full AWS IoT Core integration: plan,
  phase-by-phase implementation, every bug and its root cause (including a genuinely nasty mbedtls
  buffer-sizing bug), the IoT Rule → SQS pipeline, a `jq`/AWS CLI primer, and a reusable
  GDB/cortex-debug manual for debugging live network/TLS code.
- **[README-DHT11.md](README-DHT11.md)** — DHT11 wiring, voltage-level reasoning, and why the
  driver uses PIO instead of bit-banging under FreeRTOS with WiFi/TLS running concurrently.

## Security notes

- `certs/`, `wifi_credentials.h`, and `aws_credentials.c` are all gitignored. Never commit them —
  `aws_credentials.c` embeds your device's private key.
- The current mbedtls config trades away TLS hostname verification against AWS IoT's server
  certificate (see AWS-RasPi_PicoW2.md §5.6 for why, and the tradeoff involved) — the certificate
  chain is still fully verified against the Amazon Root CA, but the CN/SAN isn't checked against
  the endpoint hostname. Revisit before using this as a template for anything more
  security-sensitive.

## License

No license file yet — all rights reserved by default until one is added.
