# PIR Motion Sensor → MQTT → VMS: Analysis and Plan

> **Outdated (renamed 2026-10-03).** Superseded by [PIR-MQTT-VMS-Pico.md](PIR-MQTT-VMS-Pico.md)
> for the Pico 2 W, and by `PIR-MQTT-VMS-PI4.md` in the VMS repository for the Raspberry Pi 4B.
> Kept for reference only; don't implement from it.

**Status:** plan only, nothing below is implemented yet · **Date:** 2026-10-02

**Repositories involved:**

- this repository (`blink_freertos`): the Pico 2 W firmware with the Makeblock Me PIR Motion Sensor v1.1
- [vizdr/VMS](https://github.com/vizdr/VMS): the video management system on the Raspberry Pi 4B (USB camera `cam-01`, ONVIF camera `cam-02`, AWS KVS)

**Goal:** motion start/stop events from the PIR sensor on the Pico 2 W reach the Raspberry Pi
4B with minimal delay over a local MQTT broker. There they become **one of the selectable
triggers for recording the USB camera** in VMS. PIR messages that arrive **while a recording
is already running are ignored.** The PIR state is visible in a local GUI and available to
any other process on the Pi.

---

## Contents

1. [How we got here: three options](#1-how-we-got-here-three-options)
2. [Analysis](#2-analysis)
3. [Design](#3-design)
4. [Step-by-step plan](#4-step-by-step-plan)
5. [Open decisions](#5-open-decisions)
6. [Consistency record: what earlier plans said and what changed](#6-consistency-record-what-earlier-plans-said-and-what-changed)
7. [Verified facts and sources](#7-verified-facts-and-sources)

[Appendix A: Sizing POST_SEC](#appendix-a-sizing-post_sec)

---

## 1. How we got here: three options

| Option | PIR edge → consumer | Verdict |
|---|---|---|
| **A. Pico → AWS IoT Core** (new `…/motion` topic, DynamoDB, API, web UI on S3) | ~0.5 s to the database, plus 1–5 s of browser polling | Too slow for a live view; cloud-only consumers |
| **B. Pico → Mosquitto on the Pi 4B** (local web or QML GUI, own camera service) | ~5–30 ms, pushed to subscribers (with Wi‑Fi power save off) | Fast. Its own camera service would conflict with VMS, see §2.3 |
| **C. B + VMS integration** (PIR becomes a VMS recording trigger) | ~5–30 ms to the trigger service; the clip is cut by VMS's existing path | **Chosen.** This document |

Even in option C, recording quality doesn't depend on sub-second trigger latency, because VMS
clips include **12 s of pre-roll** from footage already stored. Low latency matters for the
live GUI panel and for any other local consumer. For recording, it only has to be well under
the pre-roll, which it is by orders of magnitude.

The remaining fixed delay is the **PIR module's own detection time** (100 ms to a few s). No
transport removes that.

---

## 2. Analysis

### 2.1 Pico 2 W firmware: current state and constraints

| Item | Current state (file) | Consequence |
|---|---|---|
| PIR driver | `pir.c/h`: raw GPIO IRQ on the shared `IO_IRQ_BANK0` (coexists with CYW43), level-based edge handling, S1 mode pin | Done. Nothing to change |
| PIR service | `pir_task.c/h`: queue, 30 s warm-up, start/stop state, `seq` = `motion_count` (start and its stop share it), weak hooks | Hooks need a **timestamp parameter** (today: `pir_on_motion_start(void)`, `pir_on_motion_stop(duration_ms)`) |
| Weak hooks | One strong override per program | **One owner** of the PIR events: the new LAN MQTT task. Any further consumer (e.g. AWS) is fed from the broker, not from a second override |
| MQTT client | lwIP `apps/mqtt` raw API, already used by `aws_iot_task.c` (TLS to AWS) | Reuse it for a **second client** to Mosquitto. No coreMQTT and no sockets (`LWIP_SOCKET 0`) |
| MQTT client limits | `MQTT_OUTPUT_RINGBUF_SIZE` 256 B, `MQTT_REQ_MAX_IN_FLIGHT` 4 (`mqtt_opts.h`) | Bursts can return `ERR_MEM`. Events must be **retried, not dropped** |
| lwIP heap | `MEM_SIZE 4000` (`lwipopts.h`). `mqtt_client_new()` takes ~0.5 KB from it (`mem_calloc`: 256 B ring buffer, 128 B rx buffer, request list) | A second client plus a larger ring buffer needs `MEM_SIZE` ≈ 16000 |
| TCP connections | `MEMP_NUM_TCP_PCB` default 5 | Enough for AWS plus the local broker |
| Timers | Each MQTT client runs a cyclic `sys_timeout`; `MEMP_NUM_SYS_TIMEOUT 16` | Raise by 2 for headroom |
| Wi‑Fi power save | Already off: `cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM)` (`wifi_task.c:54`) | Nothing to do on the Pico |
| Time | SNTP from `pool.ntp.org` (`time_task.c`); `aws_iot_task` blocks in `time_wait_synced()` | The local path **must not wait for SNTP**, or it stops working without internet. Events carry `boot_ms` always and UTC `ts` only when `time_is_synced()` |
| Shared lwIP core lock | An AWS TLS **reconnect** runs the mbedTLS handshake inside lwIP | Can delay local publishes during that handshake (estimate: ~1 s). Rare, only on reconnect. Removable later by routing AWS through a Mosquitto bridge (§5, D6) |
| JSON payloads | `snprintf` into fixed buffers; the length goes straight to `mqtt_publish()` | Add the `len < 0 || len >= sizeof(buf)` guard to every payload |

### 2.2 Raspberry Pi 4B (Raspberry Pi OS Trixie): packages

Checked against the Debian archive for trixie.

| Need | Package (trixie) | Note |
|---|---|---|
| Broker | `mosquitto` 2.0.21, `mosquitto-clients` | |
| Python MQTT client | `python3-paho-mqtt`, `python3-asyncio-mqtt` | VMS runs its Python code in `venv-adapter`, so install `aiomqtt` or `paho-mqtt` there with pip |
| QML GUI (optional) | `python3-pyside6.qtquick` (PySide6 6.8.2), `qml6-module-qtquick-controls`, `qt6-wayland` | **Qt's own MQTT module is not packaged in trixie.** Use paho-mqtt as the backend |

### 2.3 VMS: how recording works today

| Fact (VMS source) | Consequence for the PIR |
|---|---|
| **cam‑01 (USB) is owned by MediaMTX.** A GStreamer pipeline reads `/dev/video*` and publishes into MediaMTX, and everything else reads from MediaMTX (`adapter/bin/publish-cam01.sh`) | No second process may open the camera, so a `motion` daemon or our own ffmpeg recorder is ruled out. The PIR must trigger **VMS's own** recording |
| **A recording is a clip cut from KVS afterwards.** Publish `{"timestamp","stream","labels"}` to `adapter/<thing>/event` → IoT Rule → `cloud/lambda/clip_to_s3.py` cuts **ts−12 s … ts+33 s** → S3 + `clips` table → both GUIs | A PIR trigger is **one publish to that topic**. Clips appear in the existing GUIs; no new AWS resources |
| **KVS returns only footage it already has.** `event_watcher.py` therefore delays the publish by `PUBLISH_DELAY_SEC = 38` (33 s post-roll + margin). The database duration is nominal; only probing the media shows a short clip | The PIR path must defer its publish the same way, and verification must **probe the media** (`ffprobe`) |
| **No footage unless the KVS producer runs.** `kvs-cam01.service` runs only when started, because it costs money. This is VMS's open "ingest question" (`Camera-Features.md` §9, Phase 5) | A PIR trigger while the producer is stopped yields no clip. A **decision** (§5, D3) |
| **Detection modes:** `recordingMode` ∈ {`manual`, `motion`, `cellMotion`, `human`} per camera in the `cameras` table. `set_camera_mode.py` and the admin app (`POST /api/cameras/<id>/mode`) reject every non-manual mode without `onvifHost`. `list_cameras.py` returns `supportsDetection = bool(onvifHost)` | cam‑01 can only be `manual` today. **PIR becomes cam‑01's first automatic trigger: a new mode `pir`** |
| **Gating:** `ClipGate` (rising edge + 60 s cooldown); `adapter/bin/replay-gate.py` replays recorded event logs through it | "Ignore while recording runs" is a **different gate**: busy for exactly the clip session. Keep the replay-testable pattern |
| **One AWS MQTT session only.** The IoT policy allows client id `adapter-01` (held by `agent.py`). The watcher publishes over the **HTTP data plane** (`iot-data:Publish`), granted on `topic/adapter/adapter-01/event` in `cloud/iam/kvs-producer-policy.json` | A local Mosquitto connection doesn't touch that. The PIR watcher publishes like `event_watcher.py`: **no new IAM grant** |
| **Recording state is not visible on the Pi.** Manual recording lives only in the cloud client (start time held in the browser; `record_clip` is called at Stop) | "Recording runs" can only include manual recordings if the client announces them (§5, D2) |
| **Configuration:** `/etc/adapter/adapter.env` read by `adapter/config.py`, which reads **all keys at import time and raises if one is missing** | New MQTT keys must be read **lazily in the PIR watcher only**, or every VMS process breaks on a Pi without them |
| **Ports in use:** MediaMTX 8554 (RTSP), 8888 (HLS), 8889 (WebRTC), 1935 (RTMP), 8890 (SRT), 127.0.0.1:9997 (API); onvif-admin 8080 | Mosquitto 1883 (and 9001 if WebSockets is ever added) are free |
| **Conventions:** one process per concern, registry as the single source of truth, user systemd units, heartbeat state files in `$XDG_RUNTIME_DIR/vms/` (`aws_state.py`), `--dry-run`, `FoundAndFixed.md` | The PIR integration follows all of them |
| **GetClip limit:** returns "the first 100 MB or the first 200 fragments" of the range (AWS API reference) | Long clips are **truncated silently**. Cap the clip length (§3.4) |

---

## 3. Design

### 3.1 Architecture

```
Pico 2 W ──MQTT 1883──► Mosquitto (Pi 4B) ──localhost──► kvs-pir-watcher (new VMS user unit)
 pir/event {seq,start|stop,boot_ms,ts}                     │ ClipSession gate per camera
 pir/state (retained), status (LWT)                        │ ("ignore while recording runs")
 ◄── pir/cmd/retrigger                                     ▼ after post-roll + ingest margin
                                     iot-data publish adapter/adapter-01/event
                                     {"timestamp","endTimestamp","stream":"cam-01","labels":["pir"]}
                                                           ▼ existing, unchanged path
                                     IoT Rule ─► clip_to_s3 ─► S3 + clips table ─► both VMS GUIs

Other local consumers (admin-app panel, QML kiosk, scripts) subscribe to the same broker.
aws_iot_task (telemetry over TLS to AWS IoT Core) stays as it is.
```

### 3.2 Topic contract

| Topic | QoS / retained | Publisher | Payload |
|---|---|---|---|
| `home/pico2w-01/status` | 1 / **retained**, LWT = `offline` | Pico | `online` / `offline` |
| `home/pico2w-01/pir/event` | 1 / no | Pico | `{"seq":42,"event":"start","boot_ms":123456,"ts":1759312345123}`; `stop` adds `"duration_ms"`; `ts` only when SNTP has synced |
| `home/pico2w-01/pir/state` | 1 / **retained** | Pico | `{"motion":true,"since_boot_ms":…,"count":42}` |
| `home/pico2w-01/pir/cmd/retrigger` | 1 / no | admin GUI → Pico | `1` / `0` → `pir_task_set_retrigger()` |

Rules:

- **Only `pir/event` triggers recording.** Retained `pir/state` is replayed to every new
  subscriber, so it must never open a session. Otherwise a watcher restart while someone
  stands in the hall would create a phantom clip.
- **`seq` pairs a start with its stop** and exposes gaps. QoS 1 may deliver a message twice,
  so consumers deduplicate by `(seq, event)`.

### 3.3 The clip session: "ignore while recording runs"

One `ClipSession` per camera whose `recordingMode` is `pir`:

```
IDLE ──start(seq=N)──► RECORDING(N, t0)
RECORDING: start/stop with seq≠N, or a repeated start(N) ─► IGNORED (logged + counted)
           stop(N) at t1                     ─► POST_ROLL until t1 + POST_SEC
           t0 + MAX_SEC reached, or status=offline ─► close at that moment
POST_ROLL end ─► wait INGEST_MARGIN_SEC ─► publish [t0 − 12 s, end] ─► IDLE
```

- **Precise definition of "recording runs":** from the triggering `start` until the clip's
  end (variant B: stop + POST_SEC or the cap; variant A: t0 + 33 s). Everything that arrives
  in between is ignored. In single-trigger PIR mode that means the repeated start/stop pairs;
  in retriggerable mode (the firmware default) it is mostly QoS 1 duplicates.
- **POST_SEC** is the extra footage kept after `stop`. Starting value ≈ 4–5 s; how to size it
  is in [Appendix A](#appendix-a-sizing-post_sec).
- **INGEST_MARGIN_SEC ≈ 5 s**, the same margin VMS uses (38 − 33). It only delays the publish
  so that KVS has the footage; it adds nothing to the clip.
- **Producer check:** if `camera_control.get_stream_status("cam-01") != "active"`, log
  *"skipped: producer not running"* and stay IDLE. Publishing anyway would create a clip
  with no media, which is the kind of silent failure VMS documents.
- **Events keep flowing during POST_ROLL.** The publish wait runs as a background task, as
  in `event_watcher.py`, so the subscription keeps draining.
- **Known limitation:** a watcher restart in the middle of a session loses that session's
  clip. Acceptable for a first version; the session state could be persisted later.

### 3.4 Clip window: two variants

| | **A: fixed 45 s** | **B: follows the motion (recommended)** |
|---|---|---|
| Window | t0 − 12 s … t0 + 33 s | t0 − 12 s … stop + POST_SEC, capped at t0 + MAX_SEC |
| Cloud change | none | `clip_to_s3.py` accepts an optional `endTimestamp` (~5 lines, backward-compatible) |
| Long presence | only 33 s covered | covered until it ends (up to the cap) |
| "Recording runs" lasts | 33 s after the trigger | the whole session |

**Choosing POST_SEC (variant B).** VMS needs 33 s of post-roll because ONVIF detectors give
no end-of-activity signal. The PIR does: in retriggerable mode its `stop` already comes one
hold time after the last movement. POST_SEC therefore only covers fragment rounding and
anything the camera sees beyond the PIR's field. Start at about 4–5 s; the full reasoning, the
formula and the measurement procedure are in [Appendix A](#appendix-a-sizing-post_sec).

**Choosing MAX_SEC (variant B).** GetClip silently stops at 100 MB or 200 fragments.

- cam‑01 encodes at 1 Mbit/s (`VIDEO_BITRATE`), so 180 s is about 23 MB.
- With `GOP=30` at `CAM_FPS_OUT=15`, a keyframe comes every 2 s. Assuming one KVS fragment
  per GOP, 200 fragments is about 400 s.
- So MAX_SEC = 180 s (plus 12 s of pre-roll) stays well inside both limits. Verify the
  fragment duration on the real stream before raising it.

### 3.5 Timestamps

- **Default: the Pi's receive time.** KVS uses `PRODUCER_TIMESTAMP`, i.e. the Pi's clock, and
  the network delay is milliseconds.
- **Exception: backlogged events.** If the Pico's `ts` is present and more than 2 s older
  than the receive time (an event delivered from the queue after a reconnect, or by the
  broker's persistent session after a watcher restart), use the Pico's `ts`. The clip then
  stays centred on the real event.

### 3.6 Registry and configuration

**Registry** (`cameras` table):

- new capability attribute `pirTopic` (cam‑01: `home/pico2w-01/pir/event`);
- `recordingMode` gains the value `pir`;
- capability gating: `pir` requires `pirTopic`, the ONVIF modes require `onvifHost`.

**`/etc/adapter/adapter.env`:** `MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD_FILE`.
Read with `config.get()` **inside `pir_watcher.py` only**; never add them as module-level
constants in `config.py`.

**Watcher MQTT session:** a fixed client id with a persistent session (`clean_session=False`),
so QoS 1 events published during a watcher restart are delivered afterwards. §3.5 then
handles their age.

---

## 4. Step-by-step plan

Each phase ends with a check that proves it works.

### Phase 0: Network and broker (Pi 4B)

1. On the FRITZ!Box:
   - give both boards fixed IPv4 addresses, and keep them off the guest network;
   - optionally enable the FRITZ!Box as the LAN time server.
2. Pi 4B network: **Ethernet** (preferred), or Wi‑Fi with power save off
   (`nmcli connection modify <profile> 802-11-wireless.powersave 2`).
3. `sudo apt install mosquitto mosquitto-clients`, then in `/etc/mosquitto/conf.d/`:
   - `listener 1883` with `password_file` (users `pico`, `vms`, `gui`);
   - an `acl_file`:
     - `pico` writes `home/pico2w-01/#` and reads `home/pico2w-01/pir/cmd/#`;
     - `vms` reads `home/#`;
     - `gui` reads `home/#` and writes `…/pir/cmd/#`;
   - `persistence true`.
4. **Check:** `mosquitto_sub -v -t 'home/#'` on the Pi receives a `mosquitto_pub` sent from
   another LAN host. `iw dev wlan0 get power_save` reports `off` (if on Wi‑Fi).

### Phase 1: Pico 2 W firmware (this repository)

1. **Hooks with timestamps:** `pir_on_motion_start(uint32_t time_ms)` and
   `pir_on_motion_stop(uint32_t time_ms, uint32_t duration_ms)`. Pass the ISR timestamp
   through from `pir_apply()`, and update README-PIR.md §5.
2. **New `lan_mqtt_task.c/h`**, the sole owner of the second lwIP MQTT client.
   - **Connection:**
     - wait for Wi‑Fi only (`wifi_wait_connected()`), **not** for SNTP;
     - plain TCP (`tls_config = NULL`), user and password, keep-alive 30 s;
     - LWT: `status` = `offline`, retained, QoS 1.
   - **On connect:**
     - publish `status` = `online` (retained);
     - publish `pir/state` (retained) from `pir_get_status()`;
     - subscribe to `pir/cmd/#`. `retrigger` calls `pir_task_set_retrigger()`, which is a
       single atomic GPIO write and safe from the lwIP callback.
   - **Event path:**
     - strong hook overrides only `xQueueSend(…, 0)` into a ~16-entry queue; if full, count
       a drop, never block the `pir` task;
     - loop: `xQueuePeek()` → publish → `xQueueReceive()` only after `ERR_OK`; on `ERR_MEM`,
       back off ~50 ms and retry;
     - after each event, republish the retained `pir/state`.
   - **Plumbing:**
     - every lwIP call between `cyw43_arch_lwip_begin()` / `cyw43_arch_lwip_end()`;
     - reconnect with exponential backoff;
     - `snprintf` length guard on every payload.
3. **Configuration:** gitignored `lan_mqtt_config.h` plus `lan_mqtt_config.h.example`
   (broker IP, user, password), same pattern as `wifi_credentials.h`.
4. **`lwipopts.h`:** `MEM_SIZE` → 16000, `MQTT_OUTPUT_RINGBUF_SIZE` → 512,
   `MEMP_NUM_SYS_TIMEOUT` → 18.
5. **`main.c`:** create the task at `tskIDLE_PRIORITY + 2`; add the source to `CMakeLists.txt`.
6. **Check:**
   - a wave produces `start` and `stop` with matching `seq` in `mosquitto_sub` within
     milliseconds;
   - powering off the Pico yields `offline` after ~45 s (1.5 × keep-alive);
   - with the internet unplugged, local events still flow;
   - AWS telemetry still arrives every 10 s.

### Phase 2: Observation (measure before building, as VMS's Phase 0)

1. Log `home/pico2w-01/#` with receive timestamps to `measurements/pir-<date>.jsonl` in the
   VMS repository: one afternoon and one night. A `mosquitto_sub -v -F` line format or a
   20-line script is enough.
2. Repeat for a shorter period with the module in **single-trigger** mode, via
   `pir/cmd/retrigger` = `0`, to see how often "ignore" would actually fire.
3. **Output:** event rates, session durations, idle false-positive rate. These size
   `POST_SEC` and `MAX_SEC` and feed the cost estimate (duty cycle).
4. **For POST_SEC specifically,** also measure the PIR hold time, how far the camera sees
   beyond the PIR's field, and the stop → next-start gaps, as described in
   [Appendix A.5](#a5-turning-phase-2-data-into-a-number).

### Phase 3: VMS mode plumbing (no triggering yet, as VMS's own Phase 1)

1. Add `pirTopic` to the cam‑01 registry row.
2. Add `pir` to the valid modes with capability gating, in **both**:
   - `cloud/lambda/set_camera_mode.py`;
   - the admin app's `POST /api/cameras/<id>/mode`.
3. `cloud/lambda/list_cameras.py` returns `supportedModes`. The cloud client builds its mode
   `<select>` from it instead of the four static options plus `supportsDetection`.
   `recModeHint()` gets a `pir` text that mentions the producer prerequisite.
4. **Check:** cam‑01 accepts `pir` and rejects `motion`; cam‑02 behaves as before; both GUIs
   show the same value.

### Phase 4: Shared trigger module and the gate

1. Move `publish_event()` and the publish-delay logic from `event_watcher.py` into
   `adapter/clip_trigger.py`. `event_watcher.py` imports it, with behaviour unchanged.
   That's one copy instead of two that drift.
2. Add `ClipSession` (§3.3) to `clip_trigger.py`, next to `ClipGate`.
3. Add a replay tool (`adapter/bin/replay-pir.py`, or extend `replay-gate.py`) that runs the
   Phase 2 `.jsonl` through `ClipSession`. It prints clips, ignored events and duty cycle for
   given `POST_SEC` and `MAX_SEC`.
4. **Check:** replaying the Phase 2 logs gives plausible clip counts. The ONVIF watcher still
   produces its 45 s clips (one real detection on cam‑02).

### Phase 5: `kvs-pir-watcher.service` (new VMS user unit)

1. `adapter/pir_watcher.py`:
   - every 60 s, read cameras with `recordingMode == "pir"` from the registry (the same
     cadence as `event_watcher.py`);
   - subscribe to each `pirTopic` on localhost (persistent session, §3.6);
   - one `ClipSession` per camera, with the producer check and the timestamp rule;
   - publish through `clip_trigger` with `labels: ["pir"]`;
   - support `--dry-run`.
2. **Separate process from `event_watcher.py`,** following VMS's rule that a stalled broker
   must not stop ONVIF detection. No shared state is needed, because `recordingMode` has one
   value per camera.
3. **Heartbeat state file** `$XDG_RUNTIME_DIR/vms/pir-state.json`, rewritten every 10 s (the
   `aws_state.py` pattern: a stale file means *unknown*). It holds the Pico online flag,
   motion, session state, last `seq`, gaps and the ignored count.
4. Add the unit to LAUNCH.md A8's unit table.
5. **Check:**
   - `--dry-run` while waving logs exactly one session plus the ignored events, with no
     publish;
   - with the producer running, a real clip `clips/cam-01/…mp4` labelled `pir` appears;
   - `ffprobe` shows a media duration ≈ the window, and the footage starts before the hand
     enters the frame;
   - with the producer stopped, the log says *"skipped: producer not running"*.

### Phase 6: Variable-length clips (only with variant B)

1. `clip_to_s3.py`: `end = utc(event["endTimestamp"]) if "endTimestamp" in event else ts + 33 s`.
   The pre-roll stays at ts − 12 s. Redeploy following VMS's "Deploying a Lambda".
2. **Check:** an ONVIF trigger still yields 45 s; a PIR session yields t0 − 12 s … stop +
   POST_SEC (probe with `ffprobe`); a session hitting `MAX_SEC` is not truncated by GetClip.

### Phase 7: GUIs

1. **VMS local admin app** (LAN only; already the home of local-hardware concerns). Add a
   "PIR sensor" panel fed by a new `GET /api/pir` that reads `pir-state.json`. It shows:
   - online/offline, motion, session state (idle / recording / post-roll);
   - ignored count, recent events;
   - a retrigger-mode toggle, which publishes `pir/cmd/retrigger`.
2. **VMS cloud client:** the `pir` mode option from Phase 3. Clips labelled `pir` already
   appear in the existing clip list.
3. **Optional QML kiosk** (PySide6 + paho-mqtt, Wayland) for a display on the Pi: the PIR
   state plus the MediaMTX live view.
4. **Check:** a wave changes the admin panel within the heartbeat interval; the cloud client
   lists the new clip.

### Phase 8: Hardening and documentation

1. Hardening:
   - hardware watchdog on the Pico;
   - an alert when `pir-state.json` goes stale;
   - optional TLS on 8883;
   - a one-night soak test with the producer running, comparing clips against the Phase 2
     expectations.
2. VMS documentation:
   - `Camera-Features.md` §9: a new "PIR trigger" phase;
   - LAUNCH.md: Mosquitto setup and the new unit;
   - `config/adapter.env.example`: the MQTT keys;
   - `FoundAndFixed.md` entries for any defect found.
3. This repository:
   - MQTT_RASPI_4B_Pico2W.md, aligned with the implementation (§6);
   - README-PIR.md, README.md.

### Dependencies

Phases 0–2 don't depend on any decision in §5 and can start immediately. Phases 3–7 change
the VMS repository.

---

## 5. Open decisions

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Clip model | A: fixed 45 s, no cloud change · B: follows the motion, small `clip_to_s3` change | **B** |
| D2 | What counts as "recording runs" | only an open PIR session for that camera · also a manual recording from the cloud client (needs the client to announce start/stop via `/cmd`, and `agent.py` to write a runtime file) | **PIR session only** first; manual later if overlaps matter |
| D3 | Producer prerequisite (VMS's ingest question) | (a) keep `kvs-cam01` running while mode = `pir`: consistent with the ONVIF modes, ingest cost 24/7 · (b) PIR starts the producer: pre-roll lost plus seconds of KVS spin-up · (c) VMS's local ring-buffer option: a separate project | **(a) now, (c) later** |
| D4 | Presence longer than `MAX_SEC`, or a return during POST_ROLL that continues past the session's end ([A.4](#a4-coupling-with-ignore-while-recording-runs)) | close the session · open a continuation session while `pir/state` still reports motion | Decide after Phase 2 data; A.4 is the strongest argument for continuation |
| D5 | GUI scope | admin-app panel only · plus QML kiosk | Admin panel first |
| D6 | AWS path of the Pico | keep the direct TLS telemetry path · route everything through a Mosquitto bridge on the Pi (frees TLS and RAM on the Pico, removes the handshake stall, but the cloud then depends on the Pi) | **Keep direct** for now |

---

## 6. Consistency record: what earlier plans said and what changed

| Earlier statement | Status now | Reason |
|---|---|---|
| Motion events to AWS IoT Core on `…/motion`, stored in DynamoDB, served by an API, web UI polling every 2 s (AWS plan) | **Superseded** for motion | Latency (seconds) and cloud-only access; the local path is ~5–30 ms |
| `motion` daemon + `pir-camera.service` drive the USB camera (local plan) | **Dropped** | VMS/MediaMTX owns the camera; the daemon would also clash with onvif-admin on port 8080 |
| Web GUI served by Mosquitto (`http_dir`, WebSockets 9001), SQLite recorder, retained `pir/history` (local plan) | **Dropped** | The VMS admin app is the local GUI; VMS's `clips` table is the recording history |
| Topic contract `status` / `pir/event` / `pir/state` / `pir/cmd` | **Kept** | `pir/history` and the camera topics were removed |
| Pico `lan_mqtt_task`, `lwipopts.h` changes, hook timestamps | **Kept** | Unchanged (Phase 1) |
| MQTT_RASPI_4B_Pico2W.md: coreMQTT over sockets with `LWIP_SOCKET=1` | **Not applicable** | The project already uses lwIP `apps/mqtt`; sockets are disabled |
| MQTT_RASPI_4B_Pico2W.md: ISR via `gpio_set_irq_enabled_with_callback()`, edge type from the event mask | **Not applicable** | `pir.c` uses a raw per-pin handler next to the CYW43 handler, and acts on the sampled level |
| MQTT_RASPI_4B_Pico2W.md: "turn off Wi‑Fi power save on the Pico" | **Already done** | `wifi_task.c:54` |
| MQTT_RASPI_4B_Pico2W.md: "SNTP optional" | **Refined** | SNTP exists, but the local path must not depend on it (§2.1) |
| "Port 9001 is free; MediaMTX uses 8554/8888/9997" | **Corrected** | MediaMTX also uses 8889, 1935, 8890; 1883 and 9001 are still free |
| "Check GetClip's limits before raising MAX_SEC" | **Resolved** | 100 MB or 200 fragments, truncated silently; MAX_SEC = 180 s fits (§3.4) |

---

## 7. Verified facts and sources

**This repository**

- `pir.c/h`, `pir_task.c/h`: driver/service split; hook signatures; `seq` = `motion_count`.
- `wifi_task.c:54`: `CYW43_NONE_PM`.
- `lwipopts.h`: `MEM_SIZE 4000`, `LWIP_SOCKET 0`, `MEMP_NUM_SYS_TIMEOUT 16`.
- `aws_iot_task.c`: lwIP MQTT client; `time_wait_synced()` before TLS.

**Pico SDK 2.3.0 / lwIP**

- `mqtt_opts.h`: `MQTT_OUTPUT_RINGBUF_SIZE 256`, `MQTT_REQ_MAX_IN_FLIGHT 4`,
  `MQTT_VAR_HEADER_BUFFER_LEN 128`.
- `mqtt.c`: `mqtt_client_new()` uses `mem_calloc`; the cyclic timer uses `sys_timeout`.
- `opt.h`: `MEMP_NUM_TCP_PCB 5`.
- `cyw43.h`: `CYW43_DEFAULT_PM` = `CYW43_PERFORMANCE_PM` (PM2).

**VMS** (github.com/vizdr/VMS)

- `adapter/event_watcher.py`: `ClipGate`, `PUBLISH_DELAY_SEC = 38`, `COOLDOWN_SEC = 60`,
  iot-data publish, `REGISTRY_POLL_SEC = 60`.
- `cloud/lambda/clip_to_s3.py`: window ts − 12 s … ts + 33 s.
- `cloud/lambda/set_camera_mode.py`, `adapter/onvif-admin/app.py:516`: modes gated on
  `onvifHost`.
- `cloud/lambda/list_cameras.py`: `supportsDetection`.
- `cloud/iam/kvs-producer-policy.json`: `iot:Publish` on `topic/adapter/adapter-01/event`.
- `adapter/agent.py`: single MQTT session `adapter-01`.
- `adapter/aws_state.py`: heartbeat state-file pattern.
- `adapter/config.py`: keys read at import, `ConfigError` if missing.
- `adapter/camera_control.py`: `get_stream_status()`, unit names.
- `adapter/bin/publish-cam01.sh`: camera owned by the MediaMTX pipeline;
  `VIDEO_BITRATE=1000000`, `GOP=30`, `CAM_FPS_OUT=15`.
- `mediamtx/mediamtx.yml`: ports.
- `Camera-Features.md` §9: detection phases and the Phase 5 ingest question.

**External**

- AWS Kinesis Video Streams API reference, *GetClip*: "the first 100 MB or the first
  200 fragments".
- Debian trixie archive: `mosquitto` 2.0.21, `pyside6` 6.8.2, `python3-paho-mqtt`,
  `python3-asyncio-mqtt`, `motion` 4.7.0, `ffmpeg` 7.1; no Qt MQTT package.

---

## Appendix A: Sizing POST_SEC

### A.1 Definition

POST_SEC is the extra footage kept **after the PIR's `stop` message** arrives (variant B,
§3.4):

```
clip    = [ t0 − 12 s ,  t1 + POST_SEC ]      t0 = start received, t1 = stop received
publish =   t1 + POST_SEC + INGEST_MARGIN_SEC  (≈5 s, the KVS ingest wait)
```

Don't confuse it with INGEST_MARGIN_SEC. That one only delays the *publish*, so that KVS has
received the footage; it adds nothing to the clip.

### A.2 Why it can be much shorter than VMS's 33 s

VMS cuts ONVIF detection clips at ts + 33 s because those detectors latch for only about 5 s
and give **no real end-of-activity signal**. The 33 s is a margin for "the event probably
continued".

The PIR in retriggerable mode (the firmware default) gives a real end. Its output stays high
until one **hold time** (T_hold, set by the module's potentiometer) has passed **after the
last movement**. When `stop` arrives, the clip already contains T_hold seconds of "no motion
detected":

```
last movement ──── T_hold ────► stop (t1) ── POST_SEC ──► clip end
              (already in the clip)          (extra)
```

POST_SEC only has to cover what T_hold doesn't.

### A.3 What POST_SEC must cover

| Component | Why | Typical size |
|---|---|---|
| **Fragment rounding** (T_fragment) | KVS cuts on fragment boundaries. cam‑01 has a keyframe every 2 s (`GOP=30` at `CAM_FPS_OUT=15`), so the end can be rounded off by up to one fragment | ≥ 2 s |
| **The camera sees more than the PIR** (T_camera_extra) | If the camera's field of view reaches further than the PIR's, a person can still be visible after the PIR has lost them | 0 s if the PIR's field covers the camera's view; otherwise measure |
| **Short-gap tolerance** (T_return_gap) | A person who returns within POST_SEC is still inside the same clip (see A.4) | Depends on how returns should be handled |

```
POST_SEC = max( 2 × T_fragment ,  T_camera_extra ,  T_return_gap )
```

**Starting value without measurements: POST_SEC ≈ 4–5 s**, i.e. two fragments, assuming the
PIR's field covers the camera's view.

### A.4 Coupling with "ignore while recording runs"

During POST_ROLL the session still counts as "recording", so a new `start` is **ignored**
(§3.3). That has a consequence:

- A person who returns **within POST_SEC** is in the clip only until t1 + POST_SEC.
- In retriggerable mode, if they keep moving past the session's end, the PIR output stays
  high and **no new `start` arrives**. That activity is never recorded.

So POST_SEC is a trade-off:

- **Longer:** brief returns are covered by the same clip, but every session costs more ingest
  and storage (VMS's duty-cycle cost lever, `COSTS-1.4.md` §7.2).
- **Shorter:** cheaper, but the "returned and stayed" case falls into the gap.

The robust fix does not depend on POST_SEC. At session close, check the retained `pir/state`;
if it still reports `motion: true`, open a continuation session. That is decision **D4**
(§5), and this case is the strongest argument for it.

### A.5 Turning Phase 2 data into a number

From the observation log (`measurements/pir-<date>.jsonl`, Phase 2):

1. **T_hold:** do a single brief wave and record `duration_ms` of that start/stop pair. With
   almost no motion, the duration ≈ T_hold. This is the tail every clip already has.
2. **T_camera_extra:** walk out of the PIR's field on a path the camera can still see, and
   compare the recorded video with the `stop` time.
3. **Gap distribution (T_return_gap):** collect the times between each `stop` and the next
   `start`. If many gaps lie just above a candidate POST_SEC, people often return briefly.
   Then either raise POST_SEC to cover that percentile, or rely on D4's continuation session.
4. **Replay:** run the log through `ClipSession` with the Phase 4 replay tool for several
   POST_SEC values, and compare clips per hour, duty cycle and the number of ignored starts.
   This is the same method VMS used to size `COOLDOWN_SEC` (`adapter/bin/replay-gate.py`).
5. **Verify on real clips (Phase 5/6):** `ffprobe` the clip duration and check visually that
   the person has left the frame before the clip ends.
