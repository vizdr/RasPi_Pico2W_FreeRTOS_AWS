# Connecting a Raspberry Pi Pico 2 W to AWS IoT Core (FreeRTOS)

This document covers the full integration of the `blink_freertos` project with AWS IoT Core:
the plan, what was actually built, every bug hit along the way and how it was diagnosed and
fixed, and a reusable manual for the debugging techniques used throughout. It picks up directly
from `Manual_Setup_FreeRTOS_RasPi_Pico2w.md` (getting FreeRTOS itself running) and assumes that
groundwork is already in place.

Board: `pico2_w` (RP2350 + CYW43439 WiFi). SDK 2.3.0. FreeRTOS-Kernel (RP2350_ARM_NTZ SMP port).

---

## 1. The plan

Six phases, in dependency order:

1. **AWS IoT Core setup** — Thing, device certificate, private key, Amazon Root CA, policy,
   account endpoint.
2. **Firmware architecture** — swap the LED-only CYW43 arch for the full networking stack,
   add `lwipopts.h`/`mbedtls_config.h`, link lwIP/mbedtls libraries.
3. **WiFi station connection** — connect and stay connected, with automatic reconnect.
4. **SNTP time sync** — required before any TLS handshake, since certificate validity checking
   needs a real clock.
5. **Mutual-TLS MQTT client** — connect to AWS IoT Core, authenticate with the device
   certificate, verify AWS's identity against the Amazon Root CA, publish telemetry.
6. **Verification** — watch messages arrive in AWS IoT Core's MQTT test client.

---

## 2. Phase 1 — AWS IoT Core setup

Done entirely in the AWS Console, no firmware involved:

- Created a Thing (`pico2w-VZ-210726-freertos`) via IoT Core's "Connect a device" wizard.
- Downloaded and activated the device certificate + private key.
- Downloaded `AmazonRootCA1.pem` directly from `https://www.amazontrust.com/repository/AmazonRootCA1.pem`
  (this file is **not** per-Thing — it's a single global file, identical for every AWS
  customer, since it's just Amazon's public root CA certificate).
- Found the account's `iot:Data-ATS` endpoint (`a3dp4umq4qv6ul-ats.iot.eu-central-1.amazonaws.com`)
  via IoT Core → Domain configurations (the Settings page didn't show it directly in this
  console version).
- Verified the certificate/key are a genuinely matching pair by comparing their RSA modulus via
  `openssl x509 ... -modulus` / `openssl rsa ... -modulus` — a mismatch here would have been a
  silent, confusing failure much later.

All of this lives in `certs/` (gitignored — see `.gitignore`), except `AmazonRootCA1.pem`'s
source URL and the general approach, which are safe to document.

### The policy trap

The AWS wizard auto-generates a policy scoped to its own connectivity-test tooling — client IDs
like `sdk-java`/`basicPubSub`/`sdk-nodejs-*` and topics under `sdk/test/*`. This is **not**
usable by the actual device and had to be edited twice during Phase 5 (see §5.6) before the
device's real client ID and topic were both actually authorized. Lesson: a policy update needs
to cover **both** `iot:Connect` (client ID) and `iot:Publish`/`iot:Subscribe` (topics) —
updating only one silently leaves the connection rejected.

---

## 3. Phase 2 — Firmware architecture

### Networking library swap

`pico_cyw43_arch_none` (LED-GPIO-only, what Phase 3 of the FreeRTOS setup used) was replaced
with **`pico_cyw43_arch_lwip_sys_freertos`** — the full lwIP stack integrated as its own
FreeRTOS task (`NO_SYS=0`). Added to `CMakeLists.txt`: `pico_lwip_mqtt`, `pico_lwip_mbedtls`,
`pico_mbedtls`, `pico_lwip_sntp`.

### `lwipopts.h` and `mbedtls_config.h`

Neither ships a usable default — every lwIP/mbedtls-based project must supply both. Sourced
from the official **`picow_freertos_http_client_sys`** pico-examples project (closest real
match: TLS under full FreeRTOS integration), fetched directly from GitHub and merged from its
multi-file "common + wrapper" structure into single self-contained files, since this project
doesn't share config across multiple example targets the way pico-examples does.

Mechanically important detail: `pico_mbedtls_crypto`/`x509`/`tls` are `INTERFACE` CMake
libraries (same pattern as `FreeRTOS-Kernel-Heap4`) — their `.c` files compile as part of
whichever executable links them, inheriting *that executable's* include directories. So placing
`lwipopts.h`/`mbedtls_config.h` in the project root (already on the include path via
`target_include_directories`) was sufficient — no `PICO_MBEDTLS_CONFIG_FILE` CMake variable
needed.

Compile definitions added to `CMakeLists.txt`:
- `ALTCP_MBEDTLS_AUTHMODE=MBEDTLS_SSL_VERIFY_REQUIRED` — actually enforces server certificate
  verification (without it, any server would be silently accepted, defeating the point of
  shipping the Root CA at all).
- `CYW43_TASK_STACK_SIZE=2048` — the reference example needed this same bump for the same
  library combination (TLS work is stack-hungry).

### Missing FreeRTOS macro

Build failed until `INCLUDE_xSemaphoreGetMutexHolder` was added to `FreeRTOSConfig.h` —
`pico_async_context_freertos` (which the new arch variant depends on) calls
`xSemaphoreGetMutexHolder()` directly. This had shown up earlier as a seemingly-inert "omitted
from our config" difference against the upstream reference; turned out not to be inert once
networking entered the picture.

---

## 4. Phase 3 — WiFi station connection

`wifi_task.c`: `cyw43_arch_enable_sta_mode()` → `cyw43_arch_wifi_connect_timeout_ms()` with
retry/backoff, then a monitoring loop that reconnects automatically if the link drops. Exposes
`wifi_wait_connected()` via a FreeRTOS event group so later tasks (time sync, MQTT) can block on
connectivity rather than polling.

Two real bugs found here, both significant:

### 4.1 `cyw43_arch_init()` must run *after* the scheduler starts

Originally called from `main()` before `vTaskStartScheduler()` (this had worked fine with the
simpler `pico_cyw43_arch_none` variant). With the full `lwip_sys_freertos` variant, this caused
either a Debug-build assertion (`xSemaphoreGetMutexHolder(self->lock_mutex) ==
xTaskGetCurrentTaskHandle()` in `async_context_freertos.c`) or a silent Release-build deadlock.

**Mechanism**: `cyw43_arch_init()` spins up its own FreeRTOS worker task and has that task hold
an internal lock during setup. `async_context_freertos_lock_check()` compares the lock's actual
holder against `xTaskGetCurrentTaskHandle()` called from `main()`'s own context — but FreeRTOS's
`pxCurrentTCB` isn't set until the *first* task is created via `xTaskCreate()`, and `main()`
called `cyw43_arch_init()` *before* any `xTaskCreate()` call. The comparison could never match.

**Fix**: moved the `led_init()`/`cyw43_arch_init()` call into `wifi_task` itself (see the comment
block at the top of `wifi_task()` in `wifi_task.c`).

### 4.2 Wrong link-status function caused permanent reconnect-flapping

The reconnect-monitoring loop checked `cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) ==
CYW43_LINK_UP`. This function's own documented value table never includes `CYW43_LINK_UP` —
that's specifically "has an IP," a TCP/IP-level concept reported only by the *different*
function `cyw43_tcpip_link_status()` (which is what `cyw43_arch_wifi_connect_timeout_ms()`
itself polls internally). The loop condition failed immediately on its first check, every
cycle, regardless of actual connection health — a tight, endless connect→disconnect→reconnect
loop that looked like a router/RF problem but was pure software.

**Fix**: switched to `cyw43_tcpip_link_status()`.

(A `cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM)` power-management change was also tried as a
hypothesis for the flapping and left in — harmless, but it turned out not to be the actual
cause.)

---

## 5. Phase 4 & 5 — Time sync and TLS/MQTT

### 5.1 SNTP → real wall-clock time

`time_task.c` starts lwIP's SNTP client (`pool.ntp.org`) once WiFi is connected, wrapped in
`cyw43_arch_lwip_begin()`/`end()` (required: `sntp_init()` is a raw lwIP API call, confirmed via
the `LWIP_ASSERT_CORE_LOCKED()` calls inside lwIP's own `sntp.c`, not a socket call which
wouldn't need it).

Two lwipopts.h settings were required beyond the defaults:
- `SNTP_SERVER_DNS 1` — defaults to `0`; without it, only raw IPs work as server names, not
  `"pool.ntp.org"`.
- `SNTP_SET_SYSTEM_TIME(sec)` — defaults to a complete no-op (`LWIP_UNUSED_ARG(sec)`). Wired to
  `time_task_sntp_set_system_time()`, which calls the standard `settimeofday()`. The Pico SDK's
  own weak `_gettimeofday()` implementation (`pico_clib_interface/newlib_interface.c`) is
  *already* built to honor this (it stores an offset between "microseconds since boot" and the
  epoch time it's given) — so once `settimeofday()` is called once, `time()` (and therefore
  `mbedtls`'s X.509 validity checks) starts returning correct wall-clock time with zero further
  plumbing needed on the C-library side.

This worked cleanly on the first real test — no debugging needed.

### 5.2 Embedding credentials

`generate_aws_credentials.py` (committed) reads the three PEM files from `certs/` and generates
`aws_credentials.c` (gitignored — it embeds the private key) as C byte arrays, with the
null-terminator PEM buffers require included in the reported length (a real mbedtls requirement:
DER buffers must *not* have one, PEM buffers must). `aws_credentials.h` (declarations only, no
secrets, committed) is the stable interface `aws_iot_task.c` includes.

### 5.3 The MQTT/TLS client

`aws_iot_task.c`: resolves the endpoint via raw lwIP DNS (`dns_gethostbyname`, blocking wrapper
built on a FreeRTOS event group), builds a mutual-TLS config via
`altcp_tls_create_config_client_2wayauth()` (device cert + key, verified server-side against the
Root CA), connects MQTT on port 8883, publishes `{"temperature_c": ...}` periodically, and
reconnects automatically on failure — same event-group-gated pattern as `wifi_task`.

### 5.4 `MEMP_SYS_TIMEOUT is empty` panic

First real-hardware test crashed immediately on the MQTT/TLS connection attempt:
```
sys_timeout: timeout != NULL, pool MEMP_SYS_TIMEOUT is empty
```
lwIP auto-sizes its "simultaneous timeout" pool via `LWIP_NUM_SYS_TIMEOUT_INTERNAL`, a formula
that only counts *core* modules (TCP/ARP/DHCP/DNS). It has no way to know about SNTP, MQTT
keepalive, or TLS handshake/retransmit timers — all "apps" that register their own
`sys_timeout()` independently. With WiFi+DHCP+SNTP already running and the MQTT/TLS connection
adding its own timer on top, the default pool was exhausted.

**Fix**: `MEMP_NUM_SYS_TIMEOUT 16` in `lwipopts.h` (generous headroom over the auto-calculated
default).

### 5.5 The `-28928` saga (`MBEDTLS_ERR_SSL_BAD_INPUT_DATA`)

This was the longest chase of the project. Same symptom, immediate handshake failure, for
several rounds:

1. **First hypothesis (wrong): missing ciphersuite.** `mbedtls_ssl_ciphersuite_from_id()`
   returning `NULL` for a server-chosen ciphersuite ID felt plausible (grep found this exact
   `BAD_INPUT_DATA` return site in `ssl_tls12_client.c`'s ServerHello parsing). Added
   `MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED` (genuinely was missing — our cert is RSA, and only
   `ECDHE_ECDSA` and plain `RSA` key exchange were enabled, not the standard modern
   `ECDHE_RSA` family). **Didn't fix it** — but was still a real, worthwhile fix on its own
   merits, just not *the* bug.
2. Enabled `MBEDTLS_DEBUG_C` + `ALTCP_MBEDTLS_LIB_DEBUG` for verbose tracing — produced zero
   extra output, because the actual failing checks are silent (`return
   MBEDTLS_ERR_SSL_BAD_INPUT_DATA;` with no accompanying `MBEDTLS_SSL_DEBUG_MSG` call nearby).
3. **Debugger, precisely targeted** (see §6 for the technique): a conditional breakpoint with
   auto-continue (`break ssl_tls.c:4676 if ret != 0` / `commands` / `finish` / `continue`)
   caught the exact failing state without manually stepping through dozens of benign
   `MBEDTLS_ERR_SSL_WANT_READ` returns (expected — TLS messages arrive fragmented across
   multiple TCP segments). Found: `ssl->state == 3` (`MBEDTLS_SSL_SERVER_CERTIFICATE`) at the
   moment of the real `-28928` return — the failure is specifically while parsing/handling
   AWS's server certificate chain, confirmed to happen *before* even reaching the chain-parsing
   or verification sub-functions (breakpoints on both `ssl_parse_certificate_chain` and
   `mbedtls_ssl_verify_certificate` never fired).
4. **Root cause**: an earlier "fix" (§5.6 below wasn't the cause — this was from *this* project's
   own Phase 2 tuning) had shrunk `MBEDTLS_SSL_IN_CONTENT_LEN` from mbedtls's 16384-byte default
   down to 4096, specifically to silence a `altcp_tls: TCP_WND is smaller than the RX decrypion
   buffer` warning. AWS IoT's server certificate chain (leaf + Amazon intermediate CA) plus
   handshake framing overhead very plausibly exceeds 4096 bytes combined — and since no
   `max_fragment_length` extension was negotiated to tell AWS's server about our reduced buffer,
   it could legitimately send more than we could hold.
   **Fix**: `MBEDTLS_SSL_IN_CONTENT_LEN 8192` — large enough for the chain, still within
   `TCP_WND` (11680), avoiding the original warning too.

This alone got the handshake **past** `SERVER_CERTIFICATE` entirely — confirmed by the error
code changing to something new on the next test.

### 5.6 `MBEDTLS_ERR_SSL_CERTIFICATE_VERIFICATION_WITHOUT_HOSTNAME`

New, much more specific error after the buffer fix. Modern mbedtls refuses to verify a
certificate under `MBEDTLS_SSL_VERIFY_REQUIRED` without an expected hostname having been set —
a deliberate safeguard against checking "signed by a trusted CA" while forgetting to check "is
this actually the server I meant to talk to."

Investigated whether this could be fixed properly (calling `mbedtls_ssl_set_hostname()`) and
found a genuine gap in lwIP's bundled `mqtt.c`: `mqtt_client_connect()` always creates its own
TLS connection internally (`altcp_tls_new()`) with no hook for setting SNI/hostname first, and
`mqtt_client_t`/the underlying `altcp_pcb` are fully opaque to callers — there is no supported
way to reach in and set it through the public API.

**Decision (deliberately surfaced, not silently picked)**: accepted the tradeoff via
`MBEDTLS_SSL_CLI_ALLOW_WEAK_CERTIFICATE_VERIFICATION_WITHOUT_HOSTNAME`. The full certificate
chain is still verified up to `AmazonRootCA1` (a random attacker can't just present a
self-signed cert), but the certificate's CN/SAN is not checked against the endpoint hostname.
Residual risk: an attacker would need both a DNS-hijack/rogue-AP *and* another valid
Amazon-signed IoT certificate from a different AWS account. See the comment above this define in
`mbedtls_config.h` for the full reasoning if revisiting this later (the "proper" fix would mean
patching lwIP's `mqtt.c` locally, or writing a lower-level MQTT-over-TLS client that doesn't go
through it).

### 5.7 TinyUSB assert during early enumeration (unrelated tangent)

Separately, USB-CDC stdio (added purely as a debugging convenience) started hitting a TinyUSB
control-transfer assert (`usbd_control_xfer_cb:157`) during early enumeration, after which every
task's `printf()` appeared to hang (most likely blocked on the wedged USB side of the combined
USB+UART stdio output). Since the project never needed USB-CDC functionally — UART via the
Debug Probe bridge had been the reliable channel throughout — it was simply disabled
(`pico_enable_stdio_usb(blink_freertos 0)`) rather than debugged further.

### 5.8 The final blocker: AWS policy, `iot:Connect` specifically

Once the handshake genuinely succeeded, the connection still got closed immediately after
(`connection was closed gracefully`, `MQTT connection status=256` /
`MQTT_CONNECT_DISCONNECTED`) — the classic signature of AWS IoT Core accepting the TLS
connection but rejecting the MQTT session at the authorization level. The attached policy had
been updated to allow `iot:Publish` on the device's telemetry topic, but the separate
`iot:Connect` statement still only listed the original SDK-test client IDs
(`sdk-java`/`basicPubSub`/`sdk-nodejs-*`), never the device's actual client ID
(`pico2w-VZ-210726-freertos`). Adding the device's client ARN to that statement's `Resource`
array was the final fix — confirmed working immediately after: `MQTT connected`, then
`published {"temperature_c":...}` on a 10-second loop, verified arriving live in AWS IoT Core's
MQTT test client.

---

## 6. Debug commands manual

The technique that cracked every hard bug in this project (the FreeRTOS scheduler bug in the
base setup, and the `-28928` chain here): **reproduce with real hardware feedback, not more
hypothesizing from source reading alone.** `printf` gets you far; a debug probe gets you the
rest of the way once printf output goes silent or a fault has no visible cause.

### 6.1 Starting a session

Run and Debug panel (`Ctrl+Shift+D`) → **"Pico Debug (Cortex-Debug)"** → F5. The project's
`launch.json` (generated by the Pico extension) already wires up OpenOCD + the Debug Probe
correctly; `runToEntryPoint: "main"` means it auto-stops at `main()`.

**Debug Console syntax**: type raw GDB commands directly, no `-exec` prefix (that's a
cpptools/GDB convention, not cortex-debug's).

### 6.2 Basic breakpoints

```
break <function_name>
break <file.c>:<line_number>
continue
```

Function-name breakpoints work fine on `static` (file-local) functions too, as long as they
weren't inlined away — GDB will report `Breakpoint N at 0x...: file ..., line ...` on success.
If it *doesn't* print a real file/line, the function was likely inlined; fall back to a
`file:line` breakpoint on a specific statement instead.

### 6.3 Inspecting values

```
p <expression>          # print a variable, e.g. p ssl->state
p *ssl->conf             # dereference and print a whole struct
p/x <expression>         # print in hex
x/8xw <address>          # dump 8 words in hex starting at an address
```

**"Value optimized out"**: happens even in nominally "Debug" (`-Og`) builds once a local
variable's live range ends or it gets kept purely in a register the compiler reused. If this
happens for a function's return value right at its `return` statement, use `finish` instead
(§6.5) — it reads the value from the CPU's actual return-value register, which works regardless
of local-variable debug-info gaps.

### 6.4 Conditional breakpoints (essential for noisy call sites)

A plain breakpoint on a line hit by both benign and real-failure paths (e.g. a shared `if (ret
!= 0)` check that's also hit by expected `MBEDTLS_ERR_SSL_WANT_READ` returns) will make you
manually continue through many false positives. Filter it:

```
break ssl_tls.c:4676 if ret != 0
```

Caveat: if the variable in the condition (`ret` here) is itself sometimes optimized out at that
exact PC, the condition may not evaluate reliably — in that case, breakpoint one function up the
call stack instead, or use the auto-continue pattern below.

### 6.5 Auto-continue without stopping (critical for live network code)

**Do not manually pause-and-resume a live network/TLS session for more than a few seconds** —
pausing breaks the real-time assumptions of WiFi/DHCP/TCP/TLS state machines (timeouts fire on
both sides while frozen), and can leave connection objects in a corrupted state that produces
misleading *new* errors on the next attempt (this happened here: a manually-paused session left
the MQTT client in a state that produced `ERR_ISCONN` on the next attempt, a total red herring
that needed a clean power-cycle to rule out).

Instead, have GDB print and resume automatically:

```
break ssl_tls.c:8161
commands
finish
continue
end
```

`commands` attaches a script to the most recently defined breakpoint, run automatically every
time it's hit — no human interaction, no pause. `finish` here both prints the return value
*and* resumes execution up one frame; `continue` then resumes fully. This can be used to
"scan" through a sequence of expected/benign return values (e.g. repeated `WANT_READ`) until an
unexpected one shows up, without manually driving each step.

### 6.6 Decoding error codes

**mbedtls** (`MBEDTLS_ERR_*`): defined as negative hex constants (e.g.
`MBEDTLS_ERR_SSL_BAD_INPUT_DATA = -0x7100`). Fastest path: convert the decimal value to hex,
then grep the mbedtls headers directly rather than guessing from memory:

```bash
python3 -c "print(hex(28928))"          # -> 0x7100
grep -rn "0x7100" lib/mbedtls/include/mbedtls/*.h
```

**lwIP** (`err_t`, `ERR_*`): a small, densely-packed enum in `lwip/err.h` — `ERR_OK=0,
ERR_MEM=-1, ERR_BUF=-2, ERR_TIMEOUT=-3, ERR_RTE=-4, ERR_INPROGRESS=-5, ERR_VAL=-6,
ERR_WOULDBLOCK=-7, ERR_USE=-8, ERR_ALREADY=-9, ERR_ISCONN=-10, ERR_CONN=-11, ...`. Cheap to just
read the header directly rather than memorize.

### 6.7 Reading the mbedtls SSL state machine

`ssl->state` (an `int`, cast to the `mbedtls_ssl_states` enum) tells you exactly which handshake
phase was active at any breakpoint. The enum in `mbedtls/ssl.h` starts at 0 and is a plain
sequential list — `p ssl->state` gives you a number, cross-reference against the enum
declaration order directly (`grep -n -A20 "MBEDTLS_SSL_HELLO_REQUEST" mbedtls/ssl.h`) rather than
trusting memorized values, since the exact ordering can shift between mbedtls versions.

### 6.8 General workflow used throughout

1. Get a `printf`-based baseline first — cheap, no debugger setup, and often sufficient.
2. When `printf` output goes silent or a fault has no visible cause, escalate to the debugger
   rather than keep guessing from source code alone.
3. Set the narrowest breakpoint that answers the current question, not the broadest one —
   broad/unconditional breakpoints on frequently-hit lines waste many round-trips confirming
   benign hits.
4. For live network/timing-sensitive code, always prefer auto-continuing scripted breakpoints
   (§6.5) over manual pause-inspect-resume.
5. After a debugging session that involved real pauses, always re-test with a **clean
   power-cycle and no debugger attached** before trusting any result — a paused session can
   itself corrupt state and produce misleading follow-up symptoms.

---

## 7. Final architecture

| File | Role |
|---|---|
| `wifi_task.c/h` | WiFi station connect + auto-reconnect; `wifi_wait_connected()` |
| `time_task.c/h` | SNTP time sync; `time_wait_synced()`; drives `settimeofday()` |
| `aws_iot_task.c/h` | DNS resolve, mutual-TLS MQTT connect, periodic publish, auto-reconnect |
| `led.c/h` | LED as WiFi connection indicator (on `wifi_task`'s state, not a separate blink) |
| `temp_task.c/h` | Internal temperature sensor; `temp_task_get_last_celsius()` feeds telemetry |
| `sensor_task.c/h` | I2C MPU6050 example (independent of the AWS IoT path) |
| `aws_credentials.h` (committed) / `aws_credentials.c` (generated, gitignored) | Embedded cert/key/root-CA byte arrays |
| `generate_aws_credentials.py` | Regenerates `aws_credentials.c` from `certs/*.pem` |
| `wifi_credentials.h` (gitignored) / `.h.example` (committed) | WiFi SSID/password |
| `lwipopts.h` | lwIP configuration, including the SNTP/MQTT/TLS-specific tuning from this document |
| `mbedtls_config.h` | mbedtls configuration, including every fix from §5.5-5.6 |
| `certs/` (gitignored) | Raw PEM files downloaded from AWS |

## 8. Known tradeoffs / future work

- **TLS hostname verification is disabled** (§5.6) — the pragmatic choice given lwIP's `mqtt.c`
  API gap, not a default that should be assumed safe in a different context. Revisit if this
  code is ever adapted to a scenario with a less trusted network path.
- **Policy is broader than strictly necessary** — the original SDK-test permissions
  (`sdk/test/*` topics, `sdk-java`/`basicPubSub`/`sdk-nodejs-*` client IDs) are still present
  alongside the device's own. Harmless but worth trimming for least-privilege scoping later.
- **`MEM_SIZE`** in `lwipopts.h` (4000 bytes, inherited from the upstream HTTP-client reference)
  hasn't been specifically re-validated for the heavier MQTT+TLS+SNTP combination now running
  concurrently — no symptoms of exhaustion seen, but worth keeping in mind if new lwIP-level
  failures appear under load.
- **`sensor_task`'s MPU6050 is not yet wired to actual hardware** — prints a retry warning every
  cycle, independent of the AWS IoT path; fine to leave running, or wire up real hardware, or
  remove if not needed.
