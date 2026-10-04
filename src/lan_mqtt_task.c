/**
 * @file lan_mqtt_task.c
 * @brief PIR events -> Mosquitto on the Raspberry Pi 4B (PIR-MQTT-VMS-Pico.md §3).
 *
 * Sole owner of the second lwIP MQTT client (the first is aws_iot_task's) and of the
 * strong pir_on_motion_*() overrides. The hooks only queue the edge; this task publishes:
 *
 *   status      retained, "online"; the broker publishes the LWT "offline"
 *   pir/state   retained clock reference: on connect, after each drained batch of events,
 *               and every 30 s
 *   pir/event   one message per start/stop, boot_ms = the edge time from the ISR
 *
 * "Retry, never drop" (§3.4) needs more than mqtt_publish() returning ERR_OK: that only
 * means queued in lwIP's output ring. On a disconnect, mqtt_close() deletes every request
 * still waiting for its PUBACK without calling its callback, and lwIP never retransmits.
 * So an event leaves the outbox only once its PUBACK has arrived, and whatever is still
 * unacknowledged is sent again, in order, after a reconnect. A PUBACK lost on the way back
 * gives a duplicate, which the Pi drops by (seq, event).
 *
 * Up to EVENT_WINDOW events are in flight at once, on a connection with Nagle disabled.
 * Sending one event per round trip, each followed by a pir/state that Nagle made the next
 * event wait behind, fell seconds behind a burst (§4.5, finding 1). The broker sends
 * PUBACKs in the order it received the publishes (MQTT 3.1.1 §4.6), so the acknowledged
 * events are always the oldest ones in the outbox.
 */

#include "lan_mqtt_task.h"
#include "lan_mqtt_config.h"
#include "pir_task.h"
#include "time_task.h"
#include "watchdog_task.h"
#include "wifi_task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/altcp.h"
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"   /* struct mqtt_client_s: its TCP connection, for Nagle */
#include "lwip/ip_addr.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* ---- configuration ------------------------------------------------------ */

#define TOPIC_STATUS         LAN_MQTT_TOPIC_BASE "/status"
#define TOPIC_EVENT          LAN_MQTT_TOPIC_BASE "/pir/event"
#define TOPIC_STATE          LAN_MQTT_TOPIC_BASE "/pir/state"
#define TOPIC_CMD_ALL        LAN_MQTT_TOPIC_BASE "/pir/cmd/#"
#define TOPIC_CMD_RETRIGGER  LAN_MQTT_TOPIC_BASE "/pir/cmd/retrigger"

#define KEEP_ALIVE_S         30          /* broker publishes the LWT after ~1.5x this */
#define HEARTBEAT_MS         30000u      /* pir/state, even when nothing moves (§3.3, rule 5) */
#define EVENT_QUEUE_LEN      16u         /* hooks -> task hand-off */
#define OUTBOX_LEN           48u         /* events kept until their PUBACK arrives */
#define EVENT_WINDOW         4u          /* events in flight; MQTT_REQ_MAX_IN_FLIGHT is 8 */
#define ACK_QUEUE_LEN        16u
#define PAYLOAD_MAX          160u        /* the longest event is ~120 bytes */
#define LINK_POLL_MS         1000u       /* how soon an idle task notices a disconnect */
#define CONNECT_WAIT_MS      10000u
#define BACKOFF_MIN_MS       1000u
/* The broker is on the LAN, so a retry costs one SYN. A short cap gets the Pico back
 * within seconds of the Pi returning from a Wi-Fi drop (VMS FoundAndFixed #52: 40 s to
 * 3 min), while its queued events are still inside the Pi's 60 s staleness limit. */
#define BACKOFF_MAX_MS       5000u
#define ERR_MEM_BACKOFF_MS   50u         /* output ring or request slots full */
#define RETAINED_TRIES       20u         /* status/state: give up after ~1 s of ERR_MEM */

/* lwIP reports ERR_TIMEOUT for a request after MQTT_REQ_TIMEOUT s (checked every
 * MQTT_CYCLIC_TIMER_INTERVAL s); allow a little longer than that for a PUBACK. */
#define ACK_WAIT_MS          ((MQTT_REQ_TIMEOUT + 2 * MQTT_CYCLIC_TIMER_INTERVAL) * 1000u)

#define BIT_CONNECTED        (1u << 0)
#define BIT_CONNECT_DONE     (1u << 1)

/* ---- state -------------------------------------------------------------- */

typedef struct {
    uint64_t time_ms;        /* edge time from the ISR, ms since boot (never wraps) */
    uint32_t seq;            /* motion_count: a start and its stop share it */
    uint32_t duration_ms;    /* stop only */
    bool     start;
} lan_event_t;

/* PUBACK result for one event publish, from the lwIP callback to the task. */
typedef struct {
    uint32_t id;
    err_t    err;
} lan_ack_t;

typedef enum {
    OUT_PENDING,             /* not sent on the current connection */
    OUT_SENT,                /* published, waiting for its PUBACK */
    OUT_DONE,                /* acknowledged; removed once it is the oldest */
} lan_out_state_t;

typedef struct {
    lan_event_t     ev;
    uint32_t        id;      /* of the latest publish; its PUBACK carries it back */
    TickType_t      sent_at;
    lan_out_state_t state;
} lan_out_t;

static QueueHandle_t      s_events;
static QueueHandle_t      s_acks;
static EventGroupHandle_t s_conn;
static TaskHandle_t       s_task;
static mqtt_client_t     *s_client;
static volatile uint32_t  s_dropped;        /* events lost because both buffers were full */
static volatile bool      s_in_retrigger;   /* incoming publish is on TOPIC_CMD_RETRIGGER */
static uint32_t           s_next_id = 1;

/* Ring of events, oldest first. Only the task touches it. */
static lan_out_t          s_outbox[OUTBOX_LEN];
static uint32_t           s_out_head;
static uint32_t           s_out_count;

static uint64_t boot_ms_now(void)
{
    return time_us_64() / 1000u;
}

static void lan_wake(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* ---- PIR hooks (pir task context) ---------------------------------------- */

static void lan_enqueue(bool start, uint64_t time_ms, uint32_t duration_ms)
{
    pir_status_t st;
    lan_event_t ev = {
        .seq         = pir_get_status(&st) ? st.motion_count : 0u,
        .time_ms     = time_ms,
        .duration_ms = duration_ms,
        .start       = start,
    };
    /* Never block the pir task. The task moves events on into the outbox at once, so this
     * queue only fills when the outbox is full too; the event is then counted in
     * "dropped". */
    if (s_events == NULL || xQueueSend(s_events, &ev, 0) != pdTRUE) {
        s_dropped++;
    }
    lan_wake();
}

void pir_on_motion_start(uint64_t time_ms)
{
    lan_enqueue(true, time_ms, 0u);
}

void pir_on_motion_stop(uint64_t time_ms, uint32_t duration_ms)
{
    lan_enqueue(false, time_ms, duration_ms);
}

/* ---- lwIP callbacks (tcpip thread context) ------------------------------- */

static void lan_connection_cb(mqtt_client_t *client, __unused void *arg,
                              mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED) {
        /* lwIP's MQTT client leaves Nagle on, so each small publish waited for the TCP
         * ACK of the one before (§4.5, finding 1). */
        if (client->conn != NULL) {
            altcp_nagle_disable(client->conn);
        }
        printf("[lan_mqtt] connected to %s:%d\n", LAN_MQTT_BROKER_IP, LAN_MQTT_PORT);
        xEventGroupSetBits(s_conn, BIT_CONNECTED | BIT_CONNECT_DONE);
        return;
    }

    /* 4/5 = bad user/password or not authorised; 256 = TCP closed; 257 = timeout. */
    printf("[lan_mqtt] connection status=%d\n", (int)status);
    xEventGroupClearBits(s_conn, BIT_CONNECTED);
    xEventGroupSetBits(s_conn, BIT_CONNECT_DONE);

    /* lwIP drops pending requests without calling their callbacks; the task has to
     * notice the disconnect itself. */
    lan_wake();
}

static void lan_event_pub_cb(void *arg, err_t result)
{
    lan_ack_t ack = { .id = (uint32_t)(uintptr_t)arg, .err = result };
    xQueueSend(s_acks, &ack, 0);
    lan_wake();
}

static void lan_sub_cb(__unused void *arg, err_t result)
{
    if (result != ERR_OK) {
        printf("[lan_mqtt] subscribe to %s refused (err=%d)\n", TOPIC_CMD_ALL, result);
    }
}

static void lan_inpub_topic_cb(__unused void *arg, const char *topic, __unused u32_t tot_len)
{
    s_in_retrigger = (strcmp(topic, TOPIC_CMD_RETRIGGER) == 0);
}

static void lan_inpub_data_cb(__unused void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    if (s_in_retrigger && len >= 1u && (data[0] == '0' || data[0] == '1')) {
        bool retriggerable = (data[0] == '1');
        pir_task_set_retrigger(retriggerable);   /* one gpio_put(), safe here */
        printf("[lan_mqtt] retrigger mode -> %s\n",
               retriggerable ? "retriggerable" : "single trigger");
    }
    if (flags & MQTT_DATA_FLAG_LAST) {
        s_in_retrigger = false;
    }
}

/* ---- outbox (task context) ------------------------------------------------ */

static lan_out_t *lan_out_at(uint32_t i)
{
    return &s_outbox[(s_out_head + i) % OUTBOX_LEN];
}

/* Move events from the hooks' queue into the outbox, as far as it has room. Also runs
 * while disconnected, so the outbox, not the small hand-off queue, does the buffering. */
static void lan_pull_events(void)
{
    lan_event_t ev;
    while (s_out_count < OUTBOX_LEN && xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        lan_out_t *out = lan_out_at(s_out_count++);
        out->ev    = ev;
        out->state = OUT_PENDING;
    }
}

/* Sleep up to `ticks`, waking early for new events, PUBACKs and disconnects. Every wait in
 * this task goes through here, so it is also where the task checks in with the watchdog. */
static void lan_idle(TickType_t ticks)
{
    watchdog_checkin(WATCHDOG_CLIENT_LAN_MQTT);
    lan_pull_events();
    ulTaskNotifyTake(pdTRUE, ticks);
    lan_pull_events();
}

/* After a reconnect: everything that was in flight goes out again, in order. */
static void lan_rewind(void)
{
    for (uint32_t i = 0; i < s_out_count; i++) {
        lan_out_t *out = lan_out_at(i);
        if (out->state == OUT_SENT) {
            out->state = OUT_PENDING;
        }
    }
}

/* Applies the PUBACKs that arrived and removes acknowledged events from the front.
 * Returns the number removed, or -1 if lwIP reported a failed publish. */
static int lan_handle_acks(void)
{
    lan_ack_t ack;
    bool failed = false;

    while (xQueueReceive(s_acks, &ack, 0) == pdTRUE) {
        for (uint32_t i = 0; i < s_out_count; i++) {
            lan_out_t *out = lan_out_at(i);
            if (out->state != OUT_SENT || out->id != ack.id) {
                continue;   /* not this one, or left over from an earlier connection */
            }
            if (ack.err != ERR_OK) {
                printf("[lan_mqtt] %s #%" PRIu32 " not acknowledged (err=%d)\n",
                       out->ev.start ? "start" : "stop", out->ev.seq, ack.err);
                failed = true;
                break;
            }
            /* Edge to PUBACK: how late the Pi got it, plus one LAN round trip. */
            uint64_t now64 = boot_ms_now();
            printf("[lan_mqtt] sent %s #%" PRIu32 ", %" PRIu64 " ms after the edge\n",
                   out->ev.start ? "start" : "stop", out->ev.seq,
                   now64 - out->ev.time_ms);
            out->state = OUT_DONE;
            break;
        }
    }

    int removed = 0;
    while (s_out_count > 0 && lan_out_at(0)->state == OUT_DONE) {
        s_out_head = (s_out_head + 1u) % OUTBOX_LEN;
        s_out_count--;
        removed++;
    }
    return failed ? -1 : removed;
}

static bool lan_ack_overdue(void)
{
    TickType_t now = xTaskGetTickCount();
    for (uint32_t i = 0; i < s_out_count; i++) {
        lan_out_t *out = lan_out_at(i);
        if (out->state == OUT_SENT && (now - out->sent_at) > pdMS_TO_TICKS(ACK_WAIT_MS)) {
            return true;
        }
    }
    return false;
}

/* ---- publishing (task context) -------------------------------------------- */

static bool lan_connected(void)
{
    return (xEventGroupGetBits(s_conn) & BIT_CONNECTED) != 0;
}

static err_t lan_publish(const char *topic, const char *payload, int len, bool retain,
                         mqtt_request_cb_t cb, void *arg)
{
    cyw43_arch_lwip_begin();
    err_t err = mqtt_publish(s_client, topic, payload, (u16_t)len, 1 /* QoS 1 */,
                             retain ? 1 : 0, cb, arg);
    cyw43_arch_lwip_end();
    return err;
}

/* status and pir/state: fire-and-forget. Both are retained and republished often, so
 * one lost to a full output ring is replaced by the next. */
static void lan_publish_retained(const char *topic, const char *payload, int len)
{
    if (len < 0 || (size_t)len >= PAYLOAD_MAX) {
        printf("[lan_mqtt] %s payload too long (%d), not sent\n", topic, len);
        return;
    }
    for (uint32_t i = 0; i < RETAINED_TRIES && lan_connected(); i++) {
        err_t err = lan_publish(topic, payload, len, true, NULL, NULL);
        if (err != ERR_MEM) {
            if (err != ERR_OK) {
                printf("[lan_mqtt] publish %s failed, err=%d\n", topic, err);
            }
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(ERR_MEM_BACKOFF_MS));
    }
}

/* pir/state, with boot_ms = now: this is the Pi's clock reference (§3.3, rule 3). */
static void lan_publish_state(void)
{
    char buf[PAYLOAD_MAX];
    int len;
    uint64_t now64 = boot_ms_now();
    pir_status_t st;

    if (pir_get_status(&st)) {
        len = snprintf(buf, sizeof buf,
                       "{\"motion\":%s,\"changed_ms\":%" PRIu64 ",\"count\":%" PRIu32
                       ",\"dropped\":%" PRIu32 ",\"boot_ms\":%" PRIu64 "}",
                       st.motion ? "true" : "false",
                       st.last_change_ms,
                       st.motion_count, s_dropped, now64);
    } else {
        len = snprintf(buf, sizeof buf,
                       "{\"warming_up\":true,\"boot_ms\":%" PRIu64 "}", now64);
    }
    lan_publish_retained(TOPIC_STATE, buf, len);
}

static int lan_format_event(char *buf, size_t size, const lan_event_t *ev)
{
    uint64_t now64 = boot_ms_now();
    uint64_t at64  = ev->time_ms;   /* the edge, not now (rule 2) */

    char duration[32] = "";
    if (!ev->start) {
        snprintf(duration, sizeof duration, ",\"duration_ms\":%" PRIu32, ev->duration_ms);
    }

    /* UTC of the edge, for logs only - the Pi times events by boot_ms. */
    char ts[32] = "";
    if (time_is_synced()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t now_utc = (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
        snprintf(ts, sizeof ts, ",\"ts\":%" PRIu64, now_utc - (now64 - at64));
    }

    return snprintf(buf, size, "{\"seq\":%" PRIu32 ",\"event\":\"%s\",\"boot_ms\":%" PRIu64 "%s%s}",
                    ev->seq, ev->start ? "start" : "stop", at64, duration, ts);
}

/* Publish the oldest unsent events, in order, until EVENT_WINDOW are in flight. Returns
 * ERR_MEM when lwIP has no room right now; the rest go out on a later pass. */
static err_t lan_fill_window(void)
{
    uint32_t in_flight = 0;
    for (uint32_t i = 0; i < s_out_count; i++) {
        if (lan_out_at(i)->state == OUT_SENT) {
            in_flight++;
        }
    }

    for (uint32_t i = 0; i < s_out_count && in_flight < EVENT_WINDOW; i++) {
        lan_out_t *out = lan_out_at(i);
        if (out->state != OUT_PENDING) {
            continue;
        }

        char buf[PAYLOAD_MAX];
        int len = lan_format_event(buf, sizeof buf, &out->ev);
        if (len < 0 || (size_t)len >= sizeof buf) {
            /* Can't happen with these field widths; keeping it would block the outbox. */
            printf("[lan_mqtt] event payload too long (%d), dropped\n", len);
            s_dropped++;
            out->state = OUT_DONE;
            continue;
        }

        uint32_t id = s_next_id++;
        if (s_next_id == 0u) {
            s_next_id = 1u;
        }
        err_t err = lan_publish(TOPIC_EVENT, buf, len, false, lan_event_pub_cb,
                                (void *)(uintptr_t)id);
        if (err != ERR_OK) {
            return err;
        }
        out->id      = id;
        out->sent_at = xTaskGetTickCount();
        out->state   = OUT_SENT;
        in_flight++;
    }
    return ERR_OK;
}

/* ---- connection ----------------------------------------------------------- */

static void lan_drop_connection(void)
{
    cyw43_arch_lwip_begin();
    mqtt_disconnect(s_client);
    cyw43_arch_lwip_end();
}

static bool lan_connect(const ip_addr_t *broker, const struct mqtt_connect_client_info_t *info)
{
    xEventGroupClearBits(s_conn, BIT_CONNECTED | BIT_CONNECT_DONE);
    xQueueReset(s_acks);

    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(s_client, broker, LAN_MQTT_PORT, lan_connection_cb, NULL, info);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        printf("[lan_mqtt] mqtt_client_connect failed, err=%d\n", err);
        return false;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONNECT_WAIT_MS);
    while (!(xEventGroupGetBits(s_conn) & BIT_CONNECT_DONE)) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            break;
        }
        lan_idle(deadline - now);   /* keeps taking events in while it waits */
    }
    if (xEventGroupGetBits(s_conn) & BIT_CONNECTED) {
        return true;
    }

    /* Refused or timed out: put the client back in the disconnected state, or the next
     * mqtt_client_connect() returns ERR_ISCONN. */
    lan_drop_connection();
    return false;
}

/* One connected session; returns when the connection is gone. */
static void lan_session(void)
{
    /* Order matters (§3.2, §3.3 rule 4): status and a fresh pir/state go out before any
     * queued event, so the Pi's first clock reference after a reboot is current. */
    lan_publish_retained(TOPIC_STATUS, "online", 6);
    lan_publish_state();

    cyw43_arch_lwip_begin();
    err_t err = mqtt_subscribe(s_client, TOPIC_CMD_ALL, 1, lan_sub_cb, NULL);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        printf("[lan_mqtt] subscribe failed, err=%d\n", err);
    }

    lan_rewind();
    uint32_t delivered = 0;   /* events acknowledged since the last pir/state */
    TickType_t next_heartbeat = xTaskGetTickCount() + pdMS_TO_TICKS(HEARTBEAT_MS);

    while (lan_connected()) {
        lan_pull_events();

        int removed = lan_handle_acks();
        if (removed < 0) {
            lan_drop_connection();   /* resent in order on the next connection */
            break;
        }
        delivered += (uint32_t)removed;

        if (lan_ack_overdue()) {
            printf("[lan_mqtt] no PUBACK within %u s, reconnecting\n",
                   (unsigned)(ACK_WAIT_MS / 1000u));
            lan_drop_connection();
            break;
        }

        err = lan_fill_window();

        /* pir/state once a batch of events is fully delivered (§3.2), and as the
         * heartbeat; either resets the heartbeat timer. */
        TickType_t now = xTaskGetTickCount();
        if ((delivered > 0 && s_out_count == 0) || (int32_t)(now - next_heartbeat) >= 0) {
            lan_publish_state();
            delivered = 0;
            next_heartbeat = now + pdMS_TO_TICKS(HEARTBEAT_MS);
        }

        /* Capped so an idle session notices a dropped connection within a second; short
         * when lwIP was out of room, since its freeing up wakes nobody. */
        TickType_t wait = next_heartbeat - now;
        if (wait > pdMS_TO_TICKS(LINK_POLL_MS)) {
            wait = pdMS_TO_TICKS(LINK_POLL_MS);
        }
        if (err != ERR_OK && wait > pdMS_TO_TICKS(ERR_MEM_BACKOFF_MS)) {
            wait = pdMS_TO_TICKS(ERR_MEM_BACKOFF_MS);
        }
        lan_idle(wait);
    }
}

static void lan_wait_wifi(void)
{
    while (!wifi_is_connected()) {
        lan_idle(pdMS_TO_TICKS(LINK_POLL_MS));
    }
}

static void lan_mqtt_task(__unused void *params)
{
    ip_addr_t broker;
    if (!ipaddr_aton(LAN_MQTT_BROKER_IP, &broker)) {
        panic("lan_mqtt: bad LAN_MQTT_BROKER_IP '%s'", LAN_MQTT_BROKER_IP);
    }

    /* Must come before any lwIP call: cyw43_arch_init() - which creates the async context
     * that cyw43_arch_lwip_begin() locks - runs inside wifi_task after the scheduler
     * starts, and this task (priority +2) is scheduled before wifi_task (+1). Locking
     * first dereferenced the still-NULL context: a BusFault at 0xF0000004. */
    lan_wait_wifi();

    cyw43_arch_lwip_begin();
    s_client = mqtt_client_new();
    if (s_client != NULL) {
        mqtt_set_inpub_callback(s_client, lan_inpub_topic_cb, lan_inpub_data_cb, NULL);
    }
    cyw43_arch_lwip_end();
    if (s_client == NULL) {
        panic("lan_mqtt: failed to create MQTT client");
    }

    static const struct mqtt_connect_client_info_t info = {
        .client_id   = LAN_MQTT_CLIENT_ID,
        .client_user = LAN_MQTT_USER,
        .client_pass = LAN_MQTT_PASSWORD,
        .keep_alive  = KEEP_ALIVE_S,
        .will_topic  = TOPIC_STATUS,
        .will_msg    = "offline",
        .will_qos    = 1,
        .will_retain = 1,
        .tls_config  = NULL,   /* plain TCP on the LAN */
    };

    uint32_t backoff_ms = BACKOFF_MIN_MS;

    for (;;) {
        /* WiFi only - deliberately not time_wait_synced(), so the LAN path keeps working
         * without internet (§2). */
        lan_wait_wifi();

        if (!lan_connect(&broker, &info)) {
            printf("[lan_mqtt] broker not reachable, retrying in %lu ms\n",
                   (unsigned long)backoff_ms);
            TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(backoff_ms);
            for (TickType_t now = xTaskGetTickCount(); (int32_t)(until - now) > 0;
                 now = xTaskGetTickCount()) {
                lan_idle(until - now);
            }
            backoff_ms = (backoff_ms * 2u > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : backoff_ms * 2u;
            continue;
        }

        backoff_ms = BACKOFF_MIN_MS;
        lan_session();
        printf("[lan_mqtt] disconnected, %lu event(s) waiting\n",
               (unsigned long)(s_out_count + uxQueueMessagesWaiting(s_events)));
    }
}

bool lan_mqtt_task_start(UBaseType_t priority)
{
    s_events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(lan_event_t));
    s_acks   = xQueueCreate(ACK_QUEUE_LEN, sizeof(lan_ack_t));
    s_conn   = xEventGroupCreate();
    if (s_events == NULL || s_acks == NULL || s_conn == NULL) {
        return false;
    }

    return xTaskCreate(lan_mqtt_task, "LanMqtt", 1024, NULL, priority, &s_task) == pdPASS;
}
