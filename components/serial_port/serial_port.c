#include "serial_port.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "health.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if defined(CONFIG_BRIDGE_PORT_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#endif

#define SPORT_RX_BUF 4096
#define SPORT_TX_BUF 4096

/* ---- build-time configuration checks ------------------------------------ */

#if defined(CONFIG_BRIDGE_PORT_USB_SERIAL_JTAG)
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#error "The ESP-IDF console is on USB-Serial-JTAG, which carries MAVLink. Set ESP_CONSOLE_SECONDARY_NONE / a UART console."
#endif
#else
#if CONFIG_BRIDGE_UART_NUM >= SOC_UART_HP_NUM
#error "CONFIG_BRIDGE_UART_NUM does not exist on this chip"
#endif
#if defined(CONFIG_ESP_CONSOLE_UART) && (CONFIG_ESP_CONSOLE_UART_NUM == CONFIG_BRIDGE_UART_NUM)
#error "The ESP-IDF console shares the MAVLink UART. Select ESP_CONSOLE_NONE or another UART."
#endif
#endif

#if defined(CONFIG_BRIDGE_PORT_USB_SERIAL_JTAG)

/* ---- USB-Serial-JTAG ------------------------------------------------------ *
 *
 * The USB host only drains the IN endpoint while a program has the port open,
 * but the device cannot see that. Anything handed to the driver would sit there
 * and be delivered, stale, whenever a GCS opens the port, and neither the
 * driver nor the hardware FIFO can be flushed. So output is held in our own
 * timestamped queue and passed to the driver one frame at a time, only after
 * the previous frame has fully drained. Frames older than SPORT_MAX_AGE_MS are
 * discarded, so at most one frame can ever be delivered late. */

#define SPORT_DRV_TX_BUF  512u   /* > one frame; the driver holds at most one */
#define SPORT_Q_DEPTH     16u
#define SPORT_Q_CHUNK     320u   /* largest single write (radio frame payload) */
#define SPORT_MAX_AGE_MS  1000u

typedef struct {
    uint32_t t_ms;
    uint16_t len;
    uint8_t  data[SPORT_Q_CHUNK];
} out_chunk_t;

/* Writer-task only: sport_write / sport_service are never called concurrently. */
static out_chunk_t s_q[SPORT_Q_DEPTH];
static uint32_t    s_q_head;
static uint32_t    s_q_count;

esp_err_t sport_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = SPORT_DRV_TX_BUF,
        .rx_buffer_size = SPORT_RX_BUF,
    };
    return usb_serial_jtag_driver_install(&cfg);
}

size_t sport_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    const int n = usb_serial_jtag_read_bytes(buf, (uint32_t)cap, pdMS_TO_TICKS(timeout_ms));
    return n > 0 ? (size_t)n : 0u;
}

static void q_pop(void)
{
    s_q_head = (s_q_head + 1u) % SPORT_Q_DEPTH;
    s_q_count--;
}

void sport_service(void)
{
    if (!usb_serial_jtag_is_connected()) {
        /* Cable unplugged: nothing queued can ever be delivered in time. */
        for (; s_q_count > 0u; q_pop()) {
            health_fault(FLT_SERIAL_TX_STALE);
        }
        return;
    }
    const uint32_t now = health_uptime_ms();
    while (s_q_count > 0u && (uint32_t)(now - s_q[s_q_head].t_ms) >= SPORT_MAX_AGE_MS) {
        health_fault(FLT_SERIAL_TX_STALE); /* nobody is reading the port */
        q_pop();
    }
    if (s_q_count == 0u || usb_serial_jtag_wait_tx_done(0) != ESP_OK) {
        return; /* previous frame still in the driver / USB FIFO */
    }
    const out_chunk_t *c = &s_q[s_q_head];
    const int n = usb_serial_jtag_write_bytes(c->data, c->len, 0);
    if (n != (int)c->len) {
        health_fault(FLT_SERIAL_TX_DROP); /* cannot happen with an idle driver */
    }
    q_pop();
}

bool sport_tx_pending(void)
{
    return s_q_count > 0u;
}

bool sport_write(const uint8_t *buf, size_t len)
{
    if (len == 0u || len > SPORT_Q_CHUNK || !usb_serial_jtag_is_connected()) {
        health_fault(FLT_SERIAL_TX_DROP);
        return false;
    }
    if (s_q_count == SPORT_Q_DEPTH) {
        health_fault(FLT_SERIAL_TX_DROP); /* host too slow: keep the newest data */
        q_pop();
    }
    out_chunk_t *c = &s_q[(s_q_head + s_q_count) % SPORT_Q_DEPTH];
    c->t_ms = health_uptime_ms();
    c->len  = (uint16_t)len;
    memcpy(c->data, buf, len);
    s_q_count++;
    sport_service();
    return true;
}

const char *sport_name(void)
{
    return "USB-Serial-JTAG";
}

#else

/* ---- UART ----------------------------------------------------------------- */

#define PORT ((uart_port_t)CONFIG_BRIDGE_UART_NUM)

static const char *TAG = "sport";

static QueueHandle_t s_evq;

esp_err_t sport_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_BRIDGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
#if defined(CONFIG_BRIDGE_UART_FLOWCTRL)
        .flow_ctrl           = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 100,
#else
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#endif
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(PORT, SPORT_RX_BUF, SPORT_TX_BUF, 16, &s_evq, 0);
    if (err == ESP_OK) {
        err = uart_param_config(PORT, &cfg);
    }
    if (err == ESP_OK) {
#if defined(CONFIG_BRIDGE_UART_FLOWCTRL)
        err = uart_set_pin(PORT, CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO,
                           CONFIG_BRIDGE_UART_RTS_GPIO, CONFIG_BRIDGE_UART_CTS_GPIO);
#else
        err = uart_set_pin(PORT, CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#endif
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART%d %d baud tx=%d rx=%d", CONFIG_BRIDGE_UART_NUM,
                 CONFIG_BRIDGE_UART_BAUD, CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO);
    }
    return err;
}

static void drain_events(void)
{
    uart_event_t ev;
    /* Bounded: the event queue has 16 slots. */
    for (int i = 0; i < 16 && xQueueReceive(s_evq, &ev, 0) == pdTRUE; i++) {
        switch (ev.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            health_fault(FLT_SERIAL_RX_OVERFLOW);
            /* Data is already lost; resynchronise as the driver docs require. */
            (void)uart_flush_input(PORT);
            (void)xQueueReset(s_evq);
            return;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            health_fault(FLT_SERIAL_RX_FRAMING);
            break;
        default:
            break;
        }
    }
}

size_t sport_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    drain_events();
    size_t avail = 0;
    if (uart_get_buffered_data_len(PORT, &avail) != ESP_OK) {
        avail = 0;
    }
    int n;
    if (avail > 0u) {
        n = uart_read_bytes(PORT, buf, (uint32_t)(avail < cap ? avail : cap), 0);
    } else {
        /* Block for the first byte only, then take whatever else arrived. */
        n = uart_read_bytes(PORT, buf, 1, pdMS_TO_TICKS(timeout_ms));
        if (n == 1 && cap > 1u && uart_get_buffered_data_len(PORT, &avail) == ESP_OK && avail > 0u) {
            const size_t more = avail < cap - 1u ? avail : cap - 1u;
            const int m = uart_read_bytes(PORT, buf + 1, (uint32_t)more, 0);
            if (m > 0) {
                n += m;
            }
        }
    }
    return n > 0 ? (size_t)n : 0u;
}

bool sport_write(const uint8_t *buf, size_t len)
{
    size_t free_bytes = 0;
    if (uart_get_tx_buffer_free_size(PORT, &free_bytes) != ESP_OK || free_bytes < len) {
        health_fault(FLT_SERIAL_TX_DROP);
        return false;
    }
    const int n = uart_write_bytes(PORT, buf, len);
    if (n != (int)len) {
        health_fault(FLT_SERIAL_TX_DROP);
        return false;
    }
    return true;
}

void sport_service(void)
{
    /* The UART always drains at line rate; nothing to age out. */
}

bool sport_tx_pending(void)
{
    return false;
}

const char *sport_name(void)
{
    return "UART";
}

#endif
