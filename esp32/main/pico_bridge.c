/*
 * pico_bridge.c
 * ESP32 side of the UART link to the Pi Pico.
 *
 * TX: GPIO17 (UART2)  →  Pico GPIO5 (UART1 RX)
 * RX: GPIO16 (UART2)  ←  Pico GPIO4 (UART1 TX)
 */
#include "pico_bridge.h"
#include "Skylander.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "PicoBridge";

#define NVS_NAMESPACE "kaos"
#define NVS_KEY_PTYPE "portal_type"

static uint8_t s_portal_type = 2; /* default Traptanium */

#define BRIDGE_UART     UART_NUM_2
#define PIN_TX          17
#define PIN_RX          16

extern SemaphoreHandle_t g_sky_mutex;

static bool s_pico_ready = false;

static void log_dump_edges(const char *label, const uint8_t *dump) {
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, dump, 16, ESP_LOG_INFO);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, dump + SKYLANDER_DUMP_SIZE - 16, 16,
                             ESP_LOG_INFO);
    ESP_LOGI(TAG, "%s dump edges logged", label);
}

static void log_changed_blocks(uint8_t slot, const uint8_t *before,
                               const uint8_t *after) {
    for (uint8_t block = 0; block < SKYLANDER_DUMP_SIZE / 16; block++) {
        size_t offset = (size_t)block * 16;
        if (memcmp(before + offset, after + offset, 16) != 0) {
            ESP_LOGI(TAG, "WRITE_BACK slot %u changed block %02X", slot, block);
        }
    }
}

/* -----------------------------------------------------------------------
 * Send a framed message to the Pico
 * ----------------------------------------------------------------------- */
static void send_frame(kaos_msg_t type, const uint8_t *payload, uint16_t len) {
    static uint8_t buf[SKYLANDER_DUMP_SIZE + 8];
    int n = kaos_build_frame(buf, type, payload, len);
    uart_write_bytes(BRIDGE_UART, (const char *)buf, n);
}

/* -----------------------------------------------------------------------
 * RX task — listens for MSG_WRITE_BACK, MSG_PICO_READY, MSG_DEBUG
 * ----------------------------------------------------------------------- */
static void rx_task(void *arg) {
    kaos_parser_t parser;
    kaos_parser_init(&parser);

    kaos_msg_t  type;
    uint8_t    *payload;
    uint16_t    len;
    uint8_t     byte;

    while (1) {
        int r = uart_read_bytes(BRIDGE_UART, &byte, 1, pdMS_TO_TICKS(20));
        if (r != 1) continue;

        if (!kaos_parser_feed(&parser, byte, &type, &payload, &len)) continue;

        switch (type) {
            case MSG_PICO_READY:
                ESP_LOGI(TAG, "Pico ready");
                s_pico_ready = true;
                break;

            case MSG_WRITE_BACK: {
                ESP_LOGI(TAG, "WRITE_BACK received: payload=%u", (unsigned)len);
                if (len != 1 + SKYLANDER_DUMP_SIZE) {
                    ESP_LOGE(TAG, "WRITE_BACK rejected: expected %u bytes",
                             1 + SKYLANDER_DUMP_SIZE);
                    break;
                }
                uint8_t slot = payload[0];
                log_dump_edges("WRITE_BACK RX", payload + 1);

                xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
                if (slot < PORTAL_SLOT_ENABLED_COUNT && g_skylanders[slot].loaded) {
                    /* Verify UID in write-back matches currently loaded file
                     * to prevent stale write-back from overwriting wrong file */
                    uint8_t wb_uid[4];
                    memcpy(wb_uid, payload + 1, 4);
                    bool uid_match = (memcmp(wb_uid, g_skylanders[slot].uid, 4) == 0);
                    if (uid_match) {
                        log_changed_blocks(slot, g_skylanders[slot].raw_data,
                                           payload + 1);
                        ESP_LOGI(TAG, "writeback slot=%u file=%s", (unsigned)(slot + 1),
                                 g_skylanders[slot].filename);
                        /* The dump was opened successfully when it was loaded.
                         * Do not truncate it before we know the UART write-back
                         * can be written completely. */
                        FILE *f = fopen(g_skylanders[slot].filename, "r+b");
                        if (f) {
                            size_t written = fwrite(payload + 1, 1,
                                                    SKYLANDER_DUMP_SIZE, f);
                            int flush_rc = fflush(f);
                            int close_rc = fclose(f);
                            struct stat st;
                            int stat_rc = stat(g_skylanders[slot].filename, &st);
                            ESP_LOGI(TAG,
                                     "Save slot %d: fwrite=%u/%u fflush=%d fclose=%d size=%ld",
                                     slot, (unsigned)written,
                                     SKYLANDER_DUMP_SIZE, flush_rc, close_rc,
                                     (stat_rc == 0) ? (long)st.st_size : -1L);
                            if (written != SKYLANDER_DUMP_SIZE || flush_rc != 0 ||
                                close_rc != 0 || stat_rc != 0 ||
                                st.st_size != SKYLANDER_DUMP_SIZE) {
                                ESP_LOGE(TAG, "Save failed (errno=%d)", errno);
                            } else {
                                memcpy(g_skylanders[slot].raw_data, payload + 1,
                                       SKYLANDER_DUMP_SIZE);
                                ESP_LOGI(TAG, "Saved slot %d", slot);
                            }
                        } else {
                            ESP_LOGE(TAG, "Cannot open for write: %s (errno=%d)",
                                     g_skylanders[slot].filename, errno);
                        }
                    } else {
                        ESP_LOGW(TAG, "Slot %d write-back UID mismatch — discarded", slot);
                    }
                }
                xSemaphoreGive(g_sky_mutex);
                break;
            }

            case MSG_DEBUG: {
                char msg[48] = {0};
                int dlen = (len < 47) ? len : 47;
                memcpy(msg, payload, dlen);
                ESP_LOGI(TAG, "DBG: %s", msg);
                break;
            }

            default:
                break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void pico_bridge_init(void) {
    uart_config_t cfg = {
        .baud_rate  = KAOS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(BRIDGE_UART, 2048, 2048, 0, NULL, 0);
    uart_param_config(BRIDGE_UART, &cfg);
    uart_set_pin(BRIDGE_UART, PIN_TX, PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(rx_task, "pico_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "Bridge UART2 ready (TX=%d RX=%d @ %d baud)",
             PIN_TX, PIN_RX, KAOS_BAUD);

    /* Load saved portal type from NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v = 2;
        if (nvs_get_u8(nvs, NVS_KEY_PTYPE, &v) == ESP_OK) s_portal_type = v;
        nvs_close(nvs);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    send_frame(MSG_ESP_READY, NULL, 0);
    /* Sync portal type to Pico after boot */
    send_frame(MSG_SET_PORTAL_TYPE, &s_portal_type, 1);
    ESP_LOGI(TAG, "Sent ESP_READY to Pico (portal type %d)", s_portal_type);
}

void pico_bridge_load(uint8_t slot, const uint8_t *raw_dump) {
    static uint8_t payload[1 + SKYLANDER_DUMP_SIZE];
    payload[0] = slot;
    memcpy(payload + 1, raw_dump, SKYLANDER_DUMP_SIZE);
    send_frame(MSG_LOAD, payload, 1 + SKYLANDER_DUMP_SIZE);
    ESP_LOGI(TAG, "Sent LOAD slot %d to Pico", slot);
}

void pico_bridge_unload(uint8_t slot) {
    send_frame(MSG_UNLOAD, &slot, 1);
    ESP_LOGI(TAG, "Sent UNLOAD slot %d to Pico", slot);
}

uint8_t pico_bridge_get_portal_type(void) {
    return s_portal_type;
}

void pico_bridge_set_portal_type(uint8_t type) {
    s_portal_type = type;
    send_frame(MSG_SET_PORTAL_TYPE, &type, 1);
    /* Persist to NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_PTYPE, type);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Portal type → %d", type);
}
