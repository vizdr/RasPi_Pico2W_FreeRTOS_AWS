# PIR Motion Events: Raspberry Pi Pico 2 W (FreeRTOS) → Raspberry Pi 4B (Raspberry Pi OS Trixie)

**Setup:** Both boards are in the same IPv4 subnet, connected via Wi‑Fi to a FRITZ!Box 7583. A PIR motion sensor is attached to the Pico 2 W, which runs FreeRTOS with lwIP and the CYW43 Wi‑Fi driver.
**Goal:** Make the PIR information available on the Pi 4B with minimal delay.

---

## Contents

1. [Analysis](#1-analysis)
2. [Transport options](#2-transport-options)
3. [Recommended architecture: MQTT](#3-recommended-architecture-mqtt)
4. [Step-by-step plan](#4-step-by-step-plan)
5. [Alternative evaluated: WebSocket](#5-alternative-evaluated-websocket)
6. [Alternative evaluated: WebRTC DataChannel](#6-alternative-evaluated-webrtc-datachannel)
7. [Overall comparison](#7-overall-comparison)
8. [Recommendation](#8-recommendation)
9. [Sources](#9-sources)

---

## 1. Analysis

The goal calls for an **event push** from the Pico 2 W to the Pi 4B. If the Pi polled the Pico instead, it would add latency and load. The Pico should therefore send each PIR edge as it happens.

### Sources of end-to-end delay

| Source | Typical delay | Can you control it? |
|---|---|---|
| PIR module detection (e.g. HC-SR501, AM312) | ~100 ms to several s, set by the module | Partly (module type, sensitivity) |
| GPIO IRQ → FreeRTOS task | µs | Yes |
| **CYW43 Wi‑Fi power save on the Pico** | **up to ~100–300 ms** (DTIM beacon wake-ups) | **Yes, the main lever** |
| Wi‑Fi airtime through the FRITZ!Box | 2–10 ms | Mostly fixed |
| **Wi‑Fi power save on the Pi 4B** | tens to hundreds of ms | **Yes** |
| Broker → subscriber on the Pi | < 1 ms | — |

With power save turned off on both boards, events should reach a process on the Pi in about **5–20 ms** after the Pico sees the GPIO edge.

---

## 2. Transport options

| Option | Latency | Delivery guarantee | Effort on FreeRTOS/lwIP | Verdict |
|---|---|---|---|---|
| **MQTT (Mosquitto on Pi 4B)** | ms | QoS 1, retained state, LWT for "Pico offline" | Low (coreMQTT or lwIP `apps/mqtt`) | **Recommended** |
| Raw UDP push | lowest | none (seq numbers and ACKs are up to you) | Very low | Fallback or minimal prototype |
| Raw TCP socket | ms | TCP, but framing and reconnect logic are up to you | Medium | Rebuilds what MQTT already gives you |
| Pi polls the Pico's lwIP httpd | at least the poll interval | — | Low | Too slow and wasteful |
| WebSocket | ms | DIY | Medium | See [section 5](#5-alternative-evaluated-websocket) |
| WebRTC DataChannel | ms after setup | DIY | Large | See [section 6](#6-alternative-evaluated-webrtc-datachannel) |

### Why MQTT fits best

- The broker on the Pi makes the data available to **any local process** through `localhost`.
- A **retained** state topic gives a newly started consumer the current state immediately.
- **LWT** plus keep-alive provides liveness detection with no extra code.
- The same publish path carries over directly to **AWS IoT Core** later (coreMQTT / coreMQTT-Agent).

---

## 3. Recommended architecture: MQTT

```
PIR ──GPIO IRQ──► ISR ──xQueueSendFromISR──► pir_task ──► mqtt_task (sole owner of the client)
                                                              │  Wi‑Fi, TCP 1883, QoS 1
                                                              ▼
                                      Pi 4B: Mosquitto ──► consumer service(s) via localhost
```

---

## 4. Step-by-step plan

### Step 1: Prepare the network on the FRITZ!Box 7583

1. Assign fixed IPv4 addresses to both boards: *Heimnetz → Netzwerk → Netzwerkverbindungen* → edit each device → enable *"Diesem Netzwerkgerät immer die gleiche IPv4-Adresse zuweisen"*. Menu labels can differ slightly between FRITZ!OS versions.
2. Make sure neither board is on the **Gastnetz**, because the guest network isolates clients from each other.
3. Optional: the Pico can resolve `raspi.fritz.box` through the FRITZ!Box DNS using lwIP `dns_gethostbyname()`. A fixed IP is simpler and more deterministic.
4. Check connectivity with `ping <pico-ip>` from the Pi. lwIP answers ICMP by default.

### Step 2: Set up the broker on the Pi 4B (Trixie)

```bash
sudo apt install mosquitto mosquitto-clients
sudo mosquitto_passwd -c /etc/mosquitto/passwd pico
sudo chown mosquitto: /etc/mosquitto/passwd && sudo chmod 600 /etc/mosquitto/passwd
```

Mosquitto 2.x only listens on localhost until you add a listener. Create `/etc/mosquitto/conf.d/lan.conf`:

```
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
```

Then restart and test:

```bash
sudo systemctl enable --now mosquitto && sudo systemctl restart mosquitto
mosquitto_sub -h localhost -u pico -P '<pw>' -v -t 'home/#'
```

If you've enabled nftables or ufw, open TCP 1883 for the LAN subnet only.

### Step 3: Define the topic and payload contract

| Topic | QoS | Retained | Content |
|---|---|---|---|
| `home/pico2w-01/pir/event` | 1 | no | `{"seq":42,"state":1,"t_us":123456789}`, where `state` is 1 for motion start and 0 for end |
| `home/pico2w-01/pir/state` | 1 | **yes** | current state |
| `home/pico2w-01/status` | 1 | **yes** | `online` / `offline`, with `offline` set as the **LWT** |

- `seq` lets the Pi detect lost events.
- `t_us` is `time_us_64()` on the Pico.
- **Timestamps:** let the Pi stamp each message on receipt, which is accurate to a few ms. SNTP on the Pico (lwIP `sntp`, with the FRITZ!Box as server) is optional and only needed if you want absolute event times from the device itself.

### Step 4: Write the Pico 2 W firmware (FreeRTOS + lwIP)

1. **Build configuration:** use `pico_cyw43_arch_lwip_sys_freertos` with `NO_SYS=0`, and `LWIP_SOCKET=1` if you use coreMQTT over sockets.
2. **Handle the PIR in an ISR and do all real work in a task:**

   ```c
   static QueueHandle_t pir_q;

   static void pir_isr(uint gpio, uint32_t events) {
       pir_evt_t e = { .rising = (events & GPIO_IRQ_EDGE_RISE) != 0,
                       .t_us   = time_us_64() };
       BaseType_t hp = pdFALSE;
       xQueueSendFromISR(pir_q, &e, &hp);
       portYIELD_FROM_ISR(hp);
   }

   // init:
   // gpio_set_irq_enabled_with_callback(PIR_PIN,
   //         GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &pir_isr);
   ```

   `pir_task` handles debouncing and state, assigns `seq`, and forwards the event to `mqtt_task`.
3. **`mqtt_task` is the only task that touches the MQTT client:**
   - **coreMQTT / coreMQTT-Agent** over an lwIP sockets transport is the cleanest fit for multiple tasks.
   - The **lwIP `apps/mqtt`** client needs less code. It uses the raw API, so wrap every call in `cyw43_arch_lwip_begin()` / `cyw43_arch_lwip_end()`.
   - Settings: keep-alive 30 s, QoS 1, LWT as in Step 3, reconnect with exponential backoff, `TCP_NODELAY` on the socket.
4. **Turn off Wi‑Fi power save after joining.** This is the biggest latency gain:

   ```c
   cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);   // or CYW43_NONE_PM for minimum latency
   ```

5. **Make it robust:**
   - Monitor the link with `cyw43_tcpip_link_status()` and reconnect Wi‑Fi first, then MQTT.
   - After a reconnect, republish the current state to the retained topic.
   - Feed the hardware watchdog from a supervisor task.

### Step 5: Build the consumer on the Pi 4B

- **Quick check:** `mosquitto_sub -v -t 'home/pico2w-01/#'`
- **Service:** write a small subscriber as a systemd unit. Options are C++ with `libmosquitto-dev` or Paho MQTT C++, or Python with `python3-paho-mqtt` (v2 callback API). It should:
  - log events to journald
  - check `seq` for gaps
  - forward events to your application
- Any other process on the Pi can subscribe to `localhost:1883`, so the broker also works as the **IPC layer**.

### Step 6: Tune and measure latency

1. **Turn off Wi‑Fi power save on the Pi 4B.** Trixie uses NetworkManager:

   ```bash
   nmcli connection modify "<SSID-profile>" 802-11-wireless.powersave 2
   nmcli connection up "<SSID-profile>"
   iw dev wlan0 get power_save   # should report: off
   ```

   Connecting the Pi to the FRITZ!Box by Ethernet removes this issue entirely and halves the Wi‑Fi hops.
2. **Measure round-trip time.** The two clocks aren't synchronized, so measure a round trip instead. The Pi publishes `…/ping` with a timestamp, the Pico echoes it on `…/pong`, and the Pi logs **RTT/2**. Run it for a few hours and look at p50, p99 and max values, not just the average.
3. **Compare power-save settings.** Run the test with PM on and off on each board to quantify the effect.

### Step 7: Hardening (optional, after the prototype works)

- Use TLS on port 8883 with mbedTLS on the Pico, the same stack as for AWS IoT Core.
- Use per-device credentials and Mosquitto ACLs, e.g. the Pico may only write to `home/pico2w-01/#`.
- Add a FreeRTOS-side queue to buffer events during short disconnects. Watch for stale-state cases.
- Add an optional periodic heartbeat if you want liveness faster than keep-alive × 1.5.

### Expected result

A PIR edge reaches a subscriber on the Pi 4B in about **5–20 ms** after the Pico sees the GPIO edge, with power save off on both sides. You also get:

- reliable delivery (QoS 1)
- offline detection (LWT)
- immediate state for new consumers (retained topic)

The main remaining delay is the PIR module's own detection time.

---

## 5. Alternative evaluated: WebSocket

**Verdict:** a reasonable option, especially if the data should end up in a browser. It is not faster than MQTT, and you'd have to write the delivery semantics that MQTT already provides.

### How it would work

The Pico should be the **client** and connect outbound to a WebSocket server on the Pi 4B. The reasons are the same as for MQTT: the Pico owns reconnect logic, and the Pi is the stable, always-on endpoint. Making the Pico the server would also work, but lwIP's `httpd` has no WebSocket upgrade built in, so you'd write a server on raw sockets and handle the extra state on the MCU.

### Effort on the Pico (FreeRTOS + lwIP sockets)

There's no WebSocket client in lwIP or the Pico SDK. A minimal client is roughly 300–500 lines of C:

- **Handshake:** an HTTP `GET` with `Upgrade: websocket` and a random `Sec-WebSocket-Key`. Check the `101` response and verify `Sec-WebSocket-Accept` (SHA‑1 plus Base64, both available in mbedTLS).
- **Framing:** the client must **mask** every frame with a 4-byte key. Server frames arrive unmasked. Handle text/binary, ping/pong and close frames.
- **Task model:** one `ws_task` owns the socket. The PIR ISR → queue path stays the same as in the MQTT design.
- **TLS (`wss://`):** optional on the LAN, with the same mbedTLS cost as MQTT over TLS.

### Pi 4B side

- Python `websockets` (asyncio) for a quick prototype.
- C++ with Boost.Beast or libwebsockets for a production service.
- The same server can also deliver events to browsers. This is WebSocket's main advantage.

### Latency

It's the same as MQTT. Both use one persistent TCP connection, and frame overhead is only 2–14 bytes. Set `TCP_NODELAY` on the Pico socket. Wi‑Fi power save remains the dominant factor.

### What you lose compared to MQTT

| Feature | MQTT | Plain WebSocket |
|---|---|---|
| Delivery acknowledgement | QoS 1/2 | You add app-level ACKs plus `seq` |
| Current state for new consumers | Retained message | You cache the state in the server |
| "Device offline" notification | LWT | You detect a ping/pong timeout yourself |
| Fan-out to several local processes | Built in (pub/sub) | You write the broadcast logic |
| Path to AWS IoT Core | Direct | IoT Core accepts MQTT over WSS, but not custom WebSocket protocols |

### Hybrid option

Keep **MQTT on the Pico** and enable a **WebSocket listener in Mosquitto** on the Pi:

```
# /etc/mosquitto/conf.d/ws.conf
listener 9001
protocol websockets
```

Browsers or web dashboards then subscribe directly to the same topics with MQTT.js. You get WebSocket reach without writing a WebSocket client on the MCU.

---

## 6. Alternative evaluated: WebRTC DataChannel

**Verdict:** technically possible on Pico-class hardware, but heavily over-engineered for one PIR signal inside a single subnet. You'd still need a signaling channel, usually MQTT or HTTP.

### Stack required on the Pico

A WebRTC data channel needs this whole stack:

- ICE (STUN, optionally TURN)
- DTLS
- SCTP over DTLS over UDP
- SDP offer/answer
- a separate **signaling** channel to exchange the SDP

### Feasibility

It has been done with **libpeer**, a portable WebRTC implementation in C built on BSD sockets for IoT and embedded devices, including the RP2040 Pico W. Its Pico example demonstrates ping-pong over a datachannel. Some caveats:

- **Dependencies:** mbedtls, libsrtp, usrsctp and cJSON. libsrtp needs a patch for the Pico W.
- **Target:** the example targets the **RP2040**. A port to the RP2350 (Pico 2 W) is plausible, but I haven't seen it verified.
- **Signaling:** libpeer supports **WHIP and MQTT**. In practice that means you'd run an MQTT broker anyway, just to set up WebRTC.

### Costs on a Cortex-M33 at 150 MHz (rough estimates)

- **Flash/RAM:** usrsctp plus DTLS plus ICE adds hundreds of KB of flash and a large share of RAM. That fits in 4 MB flash and 520 KB SRAM, but it competes with the application, lwIP buffers and FreeRTOS stacks.
- **Connection setup:** the DTLS handshake (ECDHE) plus ICE plus the SCTP association can take **seconds** on the MCU. Every reconnect pays this cost again.
- **Complexity:** you'd be debugging three protocol layers you don't need on a flat LAN.

### Latency

Once established, a data channel set to `ordered=false, maxRetransmits=0` behaves like UDP and avoids TCP head-of-line blocking. On a lightly loaded home Wi‑Fi with near-zero packet loss, that saves practically nothing compared with MQTT or WebSocket. Both options are in the same 5–20 ms range.

### Where WebRTC actually makes sense

- **Media streams**, i.e. video and audio.
- **Peer-to-peer across NAT** without a relay, e.g. a remote browser talking directly to a device.

Neither applies to Pico → Pi in one subnet. If the Pi already runs a WebRTC session to browsers for video, the better pattern is to have the **Pi** forward PIR events into that session's data channel, not the Pico.

---

## 7. Overall comparison

| Criterion | MQTT (recommended) | WebSocket | WebRTC DataChannel |
|---|---|---|---|
| LAN latency (power save off) | 5–20 ms | 5–20 ms | 5–20 ms after setup |
| Connection setup | ~ms (TCP), longer with TLS | ~ms (TCP + HTTP upgrade) | seconds (ICE + DTLS + SCTP) |
| Delivery and state semantics | QoS, retained, LWT built in | DIY | DIY (SCTP gives reliability, not state) |
| Pico code to write | Small (coreMQTT or lwIP MQTT) | Medium (custom client) | Large (port, patches, signaling) |
| Pico footprint | Small | Small | Large |
| Extra infrastructure | Broker | WebSocket server | Signaling (MQTT or HTTP) + optional STUN |
| Browser access | Through Mosquitto's WebSocket listener | Native | Native |
| Cloud path (AWS IoT Core) | Direct | Only as MQTT over WSS | No |

---

## 8. Recommendation

1. **Use MQTT** as the Pico → Pi transport, following the plan in [section 4](#4-step-by-step-plan).
2. If you need a browser view, **add Mosquitto's WebSocket listener** instead of a WebSocket client on the Pico.
3. Choose a **plain WebSocket** only if you want a single custom server on the Pi that serves the web UI and receives device data, and you accept writing the ACK, state and liveness logic yourself.
4. **Don't use WebRTC** for this use case. Consider it only for media, or for a peer-to-peer browser connection across NAT, and then preferably terminate it on the Pi 4B rather than on the Pico.

---

## 9. Sources

- [libpeer documentation](https://docsearch.algolia.com/mcp/docs/repo/sepfy/libpeer)
- [sepfy/libpeer – ESP Component Registry](https://components.espressif.com/components/sepfy/libpeer)
- [libpeer README (fork mirror)](https://github.com/9527cpp/libpeer)
- Upstream repository: [github.com/sepfy/libpeer](https://github.com/sepfy/libpeer)
