#include "radio.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

_Static_assert(RADIO_MAX_AIR <= ESP_NOW_MAX_DATA_LEN_V2, "frame exceeds ESP-NOW v2 limit");

static const char *TAG = "radio";

#define TX_QUEUE_DEPTH     8u
#define RX_QUEUE_DEPTH     8u
/* Worst case on air: a full frame at LR 250 kbps is ~10 ms, plus MAC retries. */
#if defined(CONFIG_BRIDGE_RATE_LR_250K) || defined(CONFIG_BRIDGE_RATE_LR_500K)
#define SEND_TIMEOUT_MS    150u
#else
#define SEND_TIMEOUT_MS    50u
#endif
#define BEACON_PERIOD_MS   200u
#define BEACON_JITTER_MS   50u
#define INIT_ATTEMPTS      3
#define TX_TASK_STACK      4096u
#define TX_TASK_PRIO       19

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
    uint16_t len;
    uint8_t  data[LINK_MAX_PAYLOAD];
} tx_item_t;

/* ---- state (guarded by s_lock unless noted) ------------------------------ */

static pair_role_t s_role;
static pair_keys_t s_keys;
static uint16_t    s_net_id;
static uint8_t     s_my_mac[6];

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static link_sm_t  s_sm;
static link_seq_t s_seq;
static bool       s_peer_valid;
static uint8_t    s_peer_mac[6];
static uint32_t   s_peer_added_ms;
static pair_session_t s_session; /* valid while s_peer_valid */
static uint32_t   s_my_nonce;
static uint32_t   s_echo;
static int8_t     s_rssi;
static int8_t     s_noise;
static link_hb_t  s_remote;
static uint32_t   s_rx_data;
static uint32_t   s_tx_data;  /* TX task only */

static atomic_uint s_epoch;
static bool        s_config_error; /* set during bring-up only */
static int8_t      s_tx_power_qdbm; /* applied at bring-up */

/* Queues and send-completion signalling (lock-free). */
static QueueHandle_t s_tx_q;
static StaticQueue_t s_tx_q_buf;
static uint8_t       s_tx_q_store[TX_QUEUE_DEPTH * sizeof(tx_item_t)];

static QueueHandle_t s_rx_q;
static StaticQueue_t s_rx_q_buf;
static uint8_t       s_rx_q_store[RX_QUEUE_DEPTH * sizeof(radio_rx_t)];

static SemaphoreHandle_t s_send_done;
static StaticSemaphore_t s_send_done_buf;
static atomic_bool       s_send_ok;

static StaticTask_t s_tx_tcb;
static StackType_t  s_tx_stack[TX_TASK_STACK];

static inline uint32_t now_ms(void)
{
    return health_uptime_ms();
}

static inline bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t new_nonce(void)
{
    uint32_t n;
    do {
        n = esp_random();
    } while (n == 0u);
    return n;
}

static void lock(void)
{
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    (void)xSemaphoreGive(s_lock);
}

/* ---- ESP-NOW callbacks (Wi-Fi task context: copy and return) -------------- */

static radio_rx_t s_cb_item; /* only touched from the Wi-Fi task */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > (int)RADIO_MAX_AIR) {
        health_fault(FLT_AIR_RX_BAD);
        return;
    }
    memcpy(s_cb_item.src, info->src_addr, 6);
    memcpy(s_cb_item.dst, info->des_addr, 6);
    s_cb_item.rssi  = (int8_t)info->rx_ctrl->rssi;
    s_cb_item.noise = (int8_t)info->rx_ctrl->noise_floor;
    s_cb_item.len   = (uint16_t)len;
    memcpy(s_cb_item.data, data, (size_t)len);
    if (xQueueSend(s_rx_q, &s_cb_item, 0) != pdTRUE) {
        health_fault(FLT_AIR_RX_QFULL);
    }
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    atomic_store(&s_send_ok, status == ESP_NOW_SEND_SUCCESS);
    (void)xSemaphoreGive(s_send_done);
}

/* ---- peer management (lock held) ------------------------------------------ */

static void apply_rate(const uint8_t *mac)
{
#if defined(CONFIG_BRIDGE_RATE_LR_500K) || defined(CONFIG_BRIDGE_RATE_LR_250K)
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_LR,
#if defined(CONFIG_BRIDGE_RATE_LR_500K)
        .rate = WIFI_PHY_RATE_LORA_500K,
#else
        .rate = WIFI_PHY_RATE_LORA_250K,
#endif
        .ersu = false,
        .dcm  = false,
    };
    const esp_err_t err = esp_now_set_peer_rate_config(mac, &rc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rate config failed: %s", esp_err_to_name(err));
        health_fault(FLT_INIT);
    }
#else
    (void)mac;
#endif
}

static bool add_peer_locked(const uint8_t mac[6], uint32_t peer_nonce)
{
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    memcpy(p.lmk, s_keys.lmk, ESP_NOW_KEY_LEN);
    p.channel = 0; /* current channel */
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = true;

    esp_err_t err = esp_now_add_peer(&p);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        err = esp_now_mod_peer(&p);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add peer failed: %s", esp_err_to_name(err));
        health_fault(FLT_INIT);
        return false;
    }
    apply_rate(mac);
    memcpy(s_peer_mac, mac, 6);
    if (s_role == PAIR_ROLE_GROUND) {
        s_session.ground_nonce = s_my_nonce;
        s_session.air_nonce    = peer_nonce;
    } else {
        s_session.ground_nonce = peer_nonce;
        s_session.air_nonce    = s_my_nonce;
    }
    s_peer_valid    = true;
    s_peer_added_ms = now_ms();
    link_seq_reset(&s_seq);
    ESP_LOGI(TAG, "peer " MACSTR " authenticated, encrypted peer added", MAC2STR(mac));
    return true;
}

static void drop_peer_locked(void)
{
    if (s_peer_valid) {
        (void)esp_now_del_peer(s_peer_mac);
    }
    s_peer_valid = false;
    memset(&s_session, 0, sizeof(s_session));
    link_seq_reset(&s_seq);
    memset(&s_remote, 0, sizeof(s_remote));
    s_rssi     = 0;
    s_noise    = 0;
    s_my_nonce = new_nonce();
    s_echo     = 0;
}

static void on_lost_locked(void)
{
    health_fault(FLT_LINK_LOST);
    ESP_LOGW(TAG, "link LOST (peer silent %u ms) -> searching",
             (unsigned)(now_ms() - s_sm.last_rx_ms));
    drop_peer_locked();
    (void)xQueueReset(s_tx_q); /* never deliver stale data after a gap */
    atomic_fetch_add(&s_epoch, 1u);
    health_set_led(LED_SEARCHING);
}

static void note_event_locked(link_event_t ev)
{
    switch (ev) {
    case LINK_EV_CONNECTED:
        ESP_LOGI(TAG, "link CONNECTED to " MACSTR, MAC2STR(s_peer_mac));
        health_set_led(LED_CONNECTED);
        break;
    case LINK_EV_RECOVERED:
        ESP_LOGI(TAG, "link recovered");
        health_set_led(LED_CONNECTED);
        break;
    case LINK_EV_DEGRADED:
        ESP_LOGW(TAG, "link DEGRADED");
        health_set_led(LED_DEGRADED);
        break;
    case LINK_EV_LOST:
        on_lost_locked();
        break;
    default:
        break;
    }
}

/* ---- transmit (TX task only) ------------------------------------------------ */

/* `sess` is NULL for broadcast beacons (they carry their own HMAC); every
 * unicast frame gets a session-bound tag appended after the CRC. */
static bool send_frame(const uint8_t *dest, const pair_session_t *sess, link_type_t type,
                       uint16_t seq, const uint8_t *payload, uint16_t len)
{
    static uint8_t buf[RADIO_MAX_AIR]; /* TX task only */
    size_t n = link_frame_encode(buf, LINK_MAX_FRAME, type, s_net_id, seq, payload, len);
    if (n == 0u) {
        health_fault(FLT_AIR_TX_ERR);
        return false;
    }
    if (sess != NULL) {
        pair_frame_tag(&s_keys, sess, buf, n, &buf[n]);
        n += PAIR_FRAME_TAG_LEN;
    }
    (void)xSemaphoreTake(s_send_done, 0); /* discard a late completion */
    const esp_err_t err = esp_now_send(dest, buf, n);
    if (err != ESP_OK) {
        health_fault(FLT_AIR_TX_ERR);
        return false;
    }
    if (xSemaphoreTake(s_send_done, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) != pdTRUE) {
        health_fault(FLT_AIR_TX_TIMEOUT);
        return false;
    }
    if (!atomic_load(&s_send_ok)) {
        health_fault(FLT_AIR_TX_FAIL);
        return false;
    }
    return true;
}

static uint8_t txbuf_pct(void)
{
    return (uint8_t)((uxQueueSpacesAvailable(s_tx_q) * 100u) / TX_QUEUE_DEPTH);
}

static void tx_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    static tx_item_t item;
    uint16_t tx_seq      = (uint16_t)esp_random();
    uint32_t next_hb     = 0;
    uint32_t next_beacon = 0;

    for (;;) {
        (void)esp_task_wdt_reset();
        const bool have = xQueueReceive(s_tx_q, &item, pdMS_TO_TICKS(10)) == pdTRUE;
        const uint32_t now = now_ms();

        uint8_t   peer[6];
        pair_session_t sess;
        bool      peer_valid;
        bool      up;
        bool      searching;
        link_hb_t hb = {0};
        pair_beacon_t bc = {0};

        lock();
        note_event_locked(link_sm_tick(&s_sm, now));
        if (s_peer_valid && s_sm.state == LINK_SEARCHING &&
            (uint32_t)(now - s_peer_added_ms) >= s_sm.lost_ms) {
            /* Candidate authenticated but never answered: start over. */
            ESP_LOGW(TAG, "candidate " MACSTR " did not complete pairing", MAC2STR(s_peer_mac));
            drop_peer_locked();
        }
        peer_valid = s_peer_valid;
        up         = link_sm_is_up(&s_sm);
        searching  = s_sm.state == LINK_SEARCHING;
        memcpy(peer, s_peer_mac, 6);
        sess         = s_session;
        hb.state     = (uint8_t)s_sm.state;
        hb.rssi_dbm  = s_rssi;
        hb.noise_dbm = s_noise;
        hb.txbuf_pct = txbuf_pct();
        hb.rx_lost   = s_seq.lost > 0xFFFFu ? 0xFFFFu : (uint16_t)s_seq.lost;
        hb.rx_errors = (uint16_t)(health_fault_count(FLT_AIR_RX_BAD) & 0xFFFFu);
        bc.role  = (uint8_t)s_role;
        memcpy(bc.mac, s_my_mac, 6);
        bc.nonce = s_my_nonce;
        bc.echo  = s_echo;
        unlock();

        if (have) {
            if (up && peer_valid) {
                if (send_frame(peer, &sess, LINK_T_DATA, tx_seq++, item.data, item.len)) {
                    s_tx_data++;
                }
            } else {
                health_fault(FLT_AIR_TX_NOLINK);
            }
        }

        if (peer_valid && due(now, next_hb)) {
            uint8_t p[LINK_HB_LEN];
            (void)link_hb_encode(&hb, p, sizeof(p));
            (void)send_frame(peer, &sess, LINK_T_HEARTBEAT, tx_seq++, p, (uint16_t)sizeof(p));
            next_hb = now + CONFIG_BRIDGE_HEARTBEAT_MS;
        }

        if (searching && due(now, next_beacon)) {
            uint8_t p[PAIR_BEACON_LEN];
            if (pair_beacon_encode(&s_keys, s_net_id, &bc, p, sizeof(p)) == PAIR_BEACON_LEN) {
                (void)send_frame(BROADCAST, NULL, LINK_T_BEACON, 0, p, (uint16_t)sizeof(p));
            }
            next_beacon = now + BEACON_PERIOD_MS + (esp_random() % BEACON_JITTER_MS);
        }
    }
}

/* ---- receive (single consumer task) ---------------------------------------- */

static void handle_beacon_locked(const radio_rx_t *in, const link_frame_t *f)
{
    pair_beacon_t b;
    if (pair_beacon_decode(&s_keys, s_net_id, f->payload, f->len, &b) != PAIR_OK) {
        health_fault(FLT_AUTH_FAIL);
        return;
    }
    if (b.role != (uint8_t)pair_opposite_role(s_role)) {
        return; /* same-role unit on this network: never pair */
    }
    if (memcmp(b.mac, in->src, 6) != 0) {
        health_fault(FLT_AUTH_FAIL);
        return;
    }
    if (link_sm_is_up(&s_sm)) {
        /* Our peer searching again while its heartbeats have stopped means it
         * restarted: fail over immediately instead of waiting for LOST. A
         * beacon while the link is healthy (e.g. replayed) is ignored. */
        if (s_sm.state == LINK_DEGRADED && s_peer_valid && memcmp(b.mac, s_peer_mac, 6) == 0) {
            note_event_locked(link_sm_force_lost(&s_sm));
        } else {
            return;
        }
    }
    s_echo = b.nonce;
    if (b.echo == s_my_nonce && !s_peer_valid) {
        (void)add_peer_locked(b.mac, b.nonce);
    }
}

size_t radio_rx_process(const radio_rx_t *in, const uint8_t **payload)
{
    if (in->len < LINK_HDR_LEN + LINK_CRC_LEN || in->len > RADIO_MAX_AIR) {
        health_fault(FLT_AIR_RX_BAD);
        return 0;
    }
    /* Unicast types carry a trailing session tag; beacons do not. */
    const bool is_beacon = (in->data[1] & 0x0Fu) == LINK_T_BEACON;
    size_t flen = in->len;
    if (!is_beacon) {
        if (flen < LINK_HDR_LEN + LINK_CRC_LEN + PAIR_FRAME_TAG_LEN) {
            health_fault(FLT_AIR_RX_BAD);
            return 0;
        }
        flen -= PAIR_FRAME_TAG_LEN;
    }

    link_frame_t f;
    const link_err_t e = link_frame_decode(in->data, flen, s_net_id, &f);
    if (e != LINK_OK) {
        if (e != LINK_E_NET) { /* another bridge pair on the channel is normal */
            health_fault(FLT_AIR_RX_BAD);
        }
        return 0;
    }

    const uint32_t now = now_ms();
    size_t out = 0;
    lock();
    if (is_beacon) {
        handle_beacon_locked(in, &f);
    } else if (!s_peer_valid || memcmp(in->src, s_peer_mac, 6) != 0 ||
               memcmp(in->dst, s_my_mac, 6) != 0) {
        /* Unicast types must come from the authenticated peer, addressed to us. */
        health_fault(FLT_PEER_MISMATCH);
    } else if (!pair_frame_verify(&s_keys, &s_session, in->data, flen, &in->data[flen])) {
        /* Forged, from a previous session, or wrong key. */
        health_fault(FLT_AUTH_FAIL);
    } else if (link_seq_check(&s_seq, f.seq) == LINK_SEQ_ACCEPT) {
        s_rssi  = in->rssi;
        s_noise = in->noise;
        note_event_locked(link_sm_on_peer_rx(&s_sm, now));
        if (f.type == LINK_T_HEARTBEAT) {
            if (!link_hb_decode(f.payload, f.len, &s_remote)) {
                health_fault(FLT_AIR_RX_BAD);
            }
        } else if (f.len > 0u) {
            *payload = f.payload;
            out = f.len;
            s_rx_data++;
        }
    }
    unlock();
    return out;
}

bool radio_rx_wait(radio_rx_t *out, uint32_t timeout_ms)
{
    return xQueueReceive(s_rx_q, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool radio_submit(const uint8_t *data, size_t len)
{
    static tx_item_t item; /* serial RX task only */
    if (len == 0u || len > LINK_MAX_PAYLOAD) {
        health_fault(FLT_AIR_TX_ERR);
        return false;
    }
    lock();
    const bool up = link_sm_is_up(&s_sm) && s_peer_valid;
    unlock();
    if (!up) {
        health_fault(FLT_AIR_TX_NOLINK);
        return false;
    }
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    if (xQueueSend(s_tx_q, &item, 0) != pdTRUE) {
        static tx_item_t oldest;
        (void)xQueueReceive(s_tx_q, &oldest, 0);
        health_fault(FLT_AIR_TX_QFULL);
        if (xQueueSend(s_tx_q, &item, 0) != pdTRUE) {
            return false;
        }
    }
    return true;
}

void radio_snapshot(radio_snapshot_t *o)
{
    memset(o, 0, sizeof(*o));
    lock();
    o->state      = s_sm.state;
    o->peer_valid = s_peer_valid;
    memcpy(o->peer_mac, s_peer_mac, 6);
    o->rssi       = s_rssi;
    o->noise      = s_noise;
    o->rem_rssi   = s_remote.rssi_dbm;
    o->rem_noise  = s_remote.noise_dbm;
    o->rem_txbuf  = s_remote.txbuf_pct;
    o->rx_lost    = s_seq.lost;
    o->rx_dup     = s_seq.dup;
    o->rx_data    = s_rx_data;
    o->losses     = s_sm.losses;
    unlock();
    o->tx_data   = s_tx_data;
    o->tx_power_qdbm = s_tx_power_qdbm;
    o->txbuf_pct = txbuf_pct();
}

uint32_t radio_link_epoch(void)
{
    return atomic_load(&s_epoch);
}

/* ---- bring-up ------------------------------------------------------------- */

#define TRY(x)                                                                  \
    do {                                                                        \
        const esp_err_t err_ = (x);                                             \
        if (err_ != ESP_OK) {                                                   \
            ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(err_));          \
            return err_;                                                        \
        }                                                                       \
    } while (0)

static esp_err_t wifi_espnow_up(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        TRY(nvs_flash_erase());
        err = nvs_flash_init();
    }
    TRY(err);
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        TRY(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        TRY(err);
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&cfg));
    TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    TRY(esp_wifi_start());
    TRY(esp_wifi_set_ps(WIFI_PS_NONE)); /* latency: never doze */
#if defined(CONFIG_BRIDGE_RATE_LR_500K) || defined(CONFIG_BRIDGE_RATE_LR_250K)
    TRY(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                               WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
#endif
    /* Regulatory configuration is a build error, not a transient one: flag it
     * so the caller halts instead of retrying / rebooting. */
    err = esp_wifi_set_country_code(CONFIG_BRIDGE_COUNTRY, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "country code \"%s\" rejected: %s", CONFIG_BRIDGE_COUNTRY, esp_err_to_name(err));
        s_config_error = true;
        return err;
    }
    err = esp_wifi_set_channel(CONFIG_BRIDGE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel %d not allowed in country \"%s\": %s", CONFIG_BRIDGE_WIFI_CHANNEL,
                 CONFIG_BRIDGE_COUNTRY, esp_err_to_name(err));
        s_config_error = true;
        return err;
    }
    TRY(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    /* 20 dBm maps to the API maximum (84 quarter-dBm); the driver then clamps
     * to the chip and country limits. Log what was actually applied. */
    TRY(esp_wifi_set_max_tx_power((int8_t)(CONFIG_BRIDGE_TX_POWER_DBM >= 20 ? 84 : CONFIG_BRIDGE_TX_POWER_DBM * 4)));
    int8_t applied = 0;
    TRY(esp_wifi_get_max_tx_power(&applied));
    s_tx_power_qdbm = applied;
    ESP_LOGI(TAG, "TX power %d.%02d dBm (requested %d dBm, country %s), rate %s", applied / 4,
             (applied % 4) * 25, CONFIG_BRIDGE_TX_POWER_DBM, CONFIG_BRIDGE_COUNTRY,
#if defined(CONFIG_BRIDGE_RATE_LR_250K)
             "LR 250 kbps"
#elif defined(CONFIG_BRIDGE_RATE_LR_500K)
             "LR 500 kbps"
#else
             "1 Mbps"
#endif
    );
    TRY(esp_wifi_get_mac(WIFI_IF_STA, s_my_mac));

    TRY(esp_now_init());
    TRY(esp_now_register_recv_cb(on_recv));
    TRY(esp_now_register_send_cb(on_sent));
    TRY(esp_now_set_pmk(s_keys.pmk));

    esp_now_peer_info_t bcast = {0};
    memcpy(bcast.peer_addr, BROADCAST, 6);
    bcast.channel = 0;
    bcast.ifidx   = WIFI_IF_STA;
    bcast.encrypt = false; /* broadcast cannot be encrypted; beacons carry an HMAC */
    TRY(esp_now_add_peer(&bcast));
    apply_rate(BROADCAST);
    return ESP_OK;
}

static void wifi_espnow_down(void)
{
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
}

bool radio_init(pair_role_t role, const pair_keys_t *keys, uint16_t net_id)
{
    s_role      = role;
    s_keys      = *keys;
    s_net_id    = net_id;

    s_lock      = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_send_done = xSemaphoreCreateBinaryStatic(&s_send_done_buf);
    s_tx_q = xQueueCreateStatic(TX_QUEUE_DEPTH, sizeof(tx_item_t), s_tx_q_store, &s_tx_q_buf);
    s_rx_q = xQueueCreateStatic(RX_QUEUE_DEPTH, sizeof(radio_rx_t), s_rx_q_store, &s_rx_q_buf);
    configASSERT(s_lock && s_send_done && s_tx_q && s_rx_q);

    link_sm_init(&s_sm, CONFIG_BRIDGE_DEGRADED_MS, CONFIG_BRIDGE_LOST_MS);
    link_seq_reset(&s_seq);
    s_my_nonce = new_nonce();

    for (int attempt = 1;; attempt++) {
        if (wifi_espnow_up() == ESP_OK) {
            break;
        }
        wifi_espnow_down();
        if (s_config_error) {
            return false; /* not transient: the caller halts */
        }
        health_fault(FLT_INIT);
        if (attempt >= INIT_ATTEMPTS) {
            health_fatal(FLT_INIT, "Wi-Fi/ESP-NOW bring-up");
        }
        vTaskDelay(pdMS_TO_TICKS(100u << attempt)); /* 200, 400 ms backoff */
    }
    ESP_LOGI(TAG, "ESP-NOW up: mac " MACSTR " ch %d (%s) net 0x%04X", MAC2STR(s_my_mac),
             CONFIG_BRIDGE_WIFI_CHANNEL, CONFIG_BRIDGE_COUNTRY, net_id);
    health_set_led(LED_SEARCHING);
    return true;
}

void radio_start(void)
{
    TaskHandle_t h = xTaskCreateStatic(tx_task, "air_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO,
                                       s_tx_stack, &s_tx_tcb);
    if (h == NULL) {
        health_fatal(FLT_INIT, "air_tx task");
    }
}
