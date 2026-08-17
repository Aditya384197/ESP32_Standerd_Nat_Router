#include "router_core.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "router_log.h"
#include "system_metrics.h"
#include "driver/gpio.h"
#include "hardware_config.h"


#define NS "router"
#define KEY_STA_SSID "sta_ssid"
#define KEY_STA_PASS "sta_pass"
#define KEY_AP_SSID "ap_ssid"
#define KEY_AP_PASS "ap_pass"
#define KEY_AP_CH "ap_ch"
#define KEY_HOP_MODE "hop_mode"
#define HOP_MODE_COMMON 0
#define HOP_MODE_FULL 1
#define HOP_COMMON_CHANNEL_COUNT 3
#define HOP_FULL_CHANNEL_COUNT 13

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static volatile bool s_sta_connected;
static volatile bool s_sta_has_ip;
static volatile uint32_t s_sta_ip_uptime;
static char s_last_reason[48] = "Not connected";
static bool s_napt;
static bool s_sta_reconfiguring;
static esp_timer_handle_t s_napt_retry_timer;
static TaskHandle_t s_hop_task;
static TaskHandle_t s_hw_task;
static volatile uint8_t s_hop_mode = HOP_MODE_COMMON;
static volatile bool s_hop_pending;
static volatile bool s_fan_on;
static uint8_t s_retry_count;
static uint32_t s_hop_backoff_ms = HOP_RETRY_INITIAL_MS;
static uint8_t s_performance = 100;
static void request_hop_reconnect(void);

static void set_napt(bool enable);


static void napt_retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_sta_has_ip || s_napt) {
        return;
    }
    set_napt(true);
    if (!s_napt && s_napt_retry_timer) {
        (void)esp_timer_start_once(s_napt_retry_timer, 1000000ULL);
    }
}

static void router_nvs_get_str(const char *key, char *out, size_t cap, const char *fallback)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            strlcpy(out, fallback, cap);
        }
        nvs_close(h);
    } else {
        strlcpy(out, fallback, cap);
    }
}

static void load_config(char *sta_ssid, char *sta_pass, char *ap_ssid, char *ap_pass, uint8_t *channel)
{
    router_nvs_get_str(KEY_STA_SSID, sta_ssid, 33, DEFAULT_STA_SSID);
    router_nvs_get_str(KEY_STA_PASS, sta_pass, 65, DEFAULT_STA_PASS);
    router_nvs_get_str(KEY_AP_SSID, ap_ssid, 33, DEFAULT_AP_SSID);
    router_nvs_get_str(KEY_AP_PASS, ap_pass, 65, DEFAULT_AP_PASS);
    nvs_handle_t h;
    *channel = 1;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t c;
        if (nvs_get_u8(h, KEY_AP_CH, &c) == ESP_OK && c >= 1 && c <= 13) *channel = c;
        uint8_t mode = HOP_MODE_COMMON;
        if (nvs_get_u8(h, KEY_HOP_MODE, &mode) == ESP_OK && mode <= HOP_MODE_FULL) s_hop_mode = mode;
        nvs_close(h);
    }
}

static void set_ap_network(void)
{
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));
}

static void set_napt(bool enable)
{
    if (!s_ap || enable == s_napt) return;
    esp_err_t e = enable ? esp_netif_napt_enable(s_ap) : esp_netif_napt_disable(s_ap);
    if (e == ESP_OK) s_napt = enable;
}

static void configure_radio(void)
{
    /*
     * ESP32 2.4 GHz supports B/G/N. Keep 802.11b enabled as part of the
     * standard BGN bitmap for maximum AP compatibility. The previous G|N
     * bitmap caused ESP_ERR_INVALID_ARG on the tested ESP-IDF/ESP32 build,
     * aborting the application before normal startup.
     */
    const uint8_t protocol = WIFI_PROTOCOL_11B |
                             WIFI_PROTOCOL_11G |
                             WIFI_PROTOCOL_11N;

    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, protocol);
    if (err != ESP_OK) {
        router_log_write("WARN", "STA Wi-Fi protocol configuration failed");
    }

    err = esp_wifi_set_protocol(WIFI_IF_AP, protocol);
    if (err != ESP_OK) {
        router_log_write("WARN", "AP Wi-Fi protocol configuration failed");
    }

    /*
     * 40 MHz is used only when 802.11n is available. If a driver/configuration
     * rejects HT40, keep the router alive rather than aborting the whole app.
     */
    err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40);
    if (err != ESP_OK) {
        router_log_write("WARN", "STA HT40 configuration failed; continuing");
    }

    err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW40);
    if (err != ESP_OK) {
        router_log_write("WARN", "AP HT40 configuration failed; continuing");
    }
}

static void apply_wifi_config(void)
{
    char sta_ssid[33], sta_pass[65], ap_ssid[33], ap_pass[65];
    uint8_t ap_channel;
    load_config(sta_ssid, sta_pass, ap_ssid, ap_pass, &ap_channel);

    wifi_config_t sta = {0};
    wifi_config_t ap = {0};
    strlcpy((char *)sta.sta.ssid, sta_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, sta_pass, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.failure_retry_cnt = 5;
    sta.sta.threshold.rssi = -127;
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = ap_channel;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CONNECTIONS;
    ap.ap.beacon_interval = 100;
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    configure_radio();
}

static void set_ap_channel_from_sta(uint8_t channel)
{
    if (!s_ap || channel < 1 || channel > 13) return;
    wifi_config_t ap = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &ap) != ESP_OK) return;
    if (ap.ap.channel != channel) {
        ap.ap.channel = channel;
        (void)esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
}

static bool scan_channel_for_target(uint8_t channel, wifi_ap_record_t *best)
{
    wifi_scan_config_t scan = {0};
    wifi_config_t sta = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK || sta.sta.ssid[0] == 0) return false;

    scan.ssid = sta.sta.ssid;
    scan.channel = channel;
    scan.show_hidden = true;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = HOP_SCAN_DWELL_MS;
    scan.scan_time.active.max = HOP_SCAN_DWELL_MAX_MS;

    esp_err_t e = esp_wifi_scan_start(&scan, true);
    if (e != ESP_OK) return false;

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        return false;
    }

    wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
    if (!records) {
        return false;
    }

    bool found = false;
    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            if (strncmp((const char *)records[i].ssid, (const char *)sta.sta.ssid, sizeof(records[i].ssid)) == 0) {
                if (!found || records[i].rssi > best->rssi) {
                    *best = records[i];
                    found = true;
                }
            }
        }
    }
    free(records);
    return found;
}

static void hop_task(void *arg)
{
    (void)arg;
    static const uint8_t common_channels[HOP_COMMON_CHANNEL_COUNT] = {1, 6, 11};
    static const uint8_t full_channels[HOP_FULL_CHANNEL_COUNT] = {1,2,3,4,5,6,7,8,9,10,11,12,13};

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_sta_connected || !s_hop_pending) continue;

        s_hop_pending = false;
        vTaskDelay(pdMS_TO_TICKS(10));
        if (s_sta_connected) continue;

        const uint8_t *channels = s_hop_mode == HOP_MODE_FULL ? full_channels : common_channels;
        const size_t count = s_hop_mode == HOP_MODE_FULL ? HOP_FULL_CHANNEL_COUNT : HOP_COMMON_CHANNEL_COUNT;
        bool connected_attempt = false;

        for (size_t i = 0; i < count && !s_sta_connected; ++i) {
            wifi_ap_record_t best = {0};
            if (!scan_channel_for_target(channels[i], &best)) continue;

            wifi_config_t sta = {0};
            if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK) continue;
            sta.sta.channel = best.primary;
            sta.sta.bssid_set = true;
            memcpy(sta.sta.bssid, best.bssid, sizeof(sta.sta.bssid));

            if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) continue;
            set_ap_channel_from_sta(best.primary);

            if (esp_wifi_connect() == ESP_OK) {
                connected_attempt = true;
                break;
            }
        }

        if (s_sta_connected) {
            s_hop_backoff_ms = HOP_RETRY_INITIAL_MS;
            continue;
        }

        if (!connected_attempt) {
            vTaskDelay(pdMS_TO_TICKS(s_hop_backoff_ms));
            if (s_hop_backoff_ms < HOP_RETRY_MAX_MS) {
                s_hop_backoff_ms *= 2;
                if (s_hop_backoff_ms > HOP_RETRY_MAX_MS) s_hop_backoff_ms = HOP_RETRY_MAX_MS;
            }
        }

        if (!s_sta_connected) {
            s_hop_pending = true;
            xTaskNotifyGive(s_hop_task);
        }
    }
}

static void request_hop_reconnect(void)
{
    if (s_sta_connected || !s_hop_task) return;
    s_hop_pending = true;
    xTaskNotifyGive(s_hop_task);
}

static void hw_status_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_YELLOW_GPIO) |
                        (1ULL << LED_GREEN_GPIO) | (1ULL << FAN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(LED_RED_GPIO, 0);
    gpio_set_level(LED_YELLOW_GPIO, 0);
    gpio_set_level(LED_GREEN_GPIO, 0);
    gpio_set_level(FAN_GPIO, 0);

    for (;;) {
        uint8_t signal = router_sta_signal_percent();
        bool connected = router_sta_connected();
        gpio_set_level(LED_RED_GPIO, connected && signal < 40);
        gpio_set_level(LED_YELLOW_GPIO, connected && signal >= 40 && signal < 70);
        gpio_set_level(LED_GREEN_GPIO, connected && signal >= 70);

        float temp = system_metrics_temperature();
        if (temp < 0.0f) {
            s_fan_on = false;
        } else if (!s_fan_on && temp >= FAN_ON_TEMP_C) {
            s_fan_on = true;
        } else if (s_fan_on && temp <= FAN_OFF_TEMP_C) {
            s_fan_on = false;
        }
        gpio_set_level(FAN_GPIO, s_fan_on ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}



static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_STA_START) {
        request_hop_reconnect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connected = true;
        router_log_write("INFO", "STA connected");
        s_retry_count = 0;
        s_hop_backoff_ms = HOP_RETRY_INITIAL_MS;
        const wifi_event_sta_connected_t *e = data;
        if (e) {
            set_ap_channel_from_sta(e->channel);
            wifi_config_t sta = {0};
            if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK && sta.sta.bssid_set) {
                sta.sta.bssid_set = false;
                (void)esp_wifi_set_config(WIFI_IF_STA, &sta);
            }
            snprintf(s_last_reason, sizeof(s_last_reason), "Connected on channel %u", e->channel);
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        router_log_write("WARN", "STA disconnected");
        s_sta_has_ip = false;
        set_napt(false);
        const wifi_event_sta_disconnected_t *e = data;
        if (e) snprintf(s_last_reason, sizeof(s_last_reason), "Wi-Fi reason %u", e->reason);
        if (!s_sta_reconfiguring) {
            if (s_retry_count < 255) ++s_retry_count;
            request_hop_reconnect();
        }
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != IP_EVENT) return;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_sta_has_ip = true;
        router_log_write("INFO", "STA got IP");
        s_sta_ip_uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        set_napt(true);
        if (!s_napt) {
            strlcpy(s_last_reason, "NAT initialization failed", sizeof(s_last_reason));
            if (s_napt_retry_timer) {
                (void)esp_timer_start_once(s_napt_retry_timer, 1000000ULL);
            }
            return;
        }
        strlcpy(s_last_reason, "Internet uplink ready", sizeof(s_last_reason));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_sta_has_ip = false;
        if (s_napt_retry_timer) {
            esp_timer_stop(s_napt_retry_timer);
        }
        set_napt(false);
    }
}

esp_err_t router_set_performance(uint8_t percent)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    wifi_ps_type_t old_ps = WIFI_PS_NONE;
    int8_t old_tx = 0;
    (void)esp_wifi_get_ps(&old_ps);
    (void)esp_wifi_get_max_tx_power(&old_tx);

    wifi_ps_type_t ps = percent >= 80 ? WIFI_PS_NONE :
                        (percent >= 50 ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
    int8_t tx = (int8_t)(8 + ((uint16_t)(percent - 10) * 72) / 90);

    esp_err_t e = esp_wifi_set_ps(ps);
    if (e == ESP_OK) {
        e = esp_wifi_set_max_tx_power(tx);
    }
    if (e != ESP_OK) {
        (void)esp_wifi_set_ps(old_ps);
        if (old_tx > 0) (void)esp_wifi_set_max_tx_power(old_tx);
        return e;
    }

    e = system_metrics_set_performance(percent);
    if (e != ESP_OK) {
        (void)esp_wifi_set_ps(old_ps);
        if (old_tx > 0) (void)esp_wifi_set_max_tx_power(old_tx);
        return e;
    }

    s_performance = percent;
    return ESP_OK;
}
uint8_t router_get_performance(void){ return s_performance; }
uint8_t router_get_tx_power_quarter_dbm(void)
{
    int8_t tx = 0;
    if (esp_wifi_get_max_tx_power(&tx) != ESP_OK) {
        return 0;
    }
    return tx > 0 ? (uint8_t)tx : 0;
}

void router_core_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    if (!s_sta || !s_ap) abort();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.wifi_task_core_id = 0;
    cfg.static_rx_buf_num = 12;
    cfg.dynamic_rx_buf_num = 32;
    cfg.static_tx_buf_num = 16;
    cfg.cache_tx_buf_num = 32;
    cfg.dynamic_tx_buf_num = 24;
    cfg.rx_ba_win = 12;
    cfg.ampdu_rx_enable = 1;
    cfg.ampdu_tx_enable = 1;
    cfg.amsdu_tx_enable = 1;
    cfg.nvs_enable = 0;
    cfg.sta_disconnected_pm = false;

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    system_metrics_init();
    s_performance = system_metrics_get_performance();
    if (xTaskCreatePinnedToCore(hop_task, "sta_hopper", 6144, NULL, 5, &s_hop_task, 0) != pdPASS) abort();
    if (xTaskCreatePinnedToCore(hw_status_task, "hw_status", 3072, NULL, 1, &s_hw_task, 1) != pdPASS) abort();
    const esp_timer_create_args_t napt_timer_args = {
        .callback = napt_retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "napt_retry"
    };
    ESP_ERROR_CHECK(esp_timer_create(&napt_timer_args, &s_napt_retry_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL));
    apply_wifi_config();
    set_ap_network();
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_err_t perf_err = router_set_performance(s_performance);
    if (perf_err != ESP_OK) {
        router_log_write("WARN", "Performance profile could not be fully applied");
    }
}

esp_netif_t *router_get_sta_netif(void) { return s_sta; }
esp_netif_t *router_get_ap_netif(void) { return s_ap; }
bool router_sta_has_ip(void) { return s_sta_has_ip; }
bool router_sta_connected(void) { return s_sta_connected; }
bool router_napt_enabled(void) { return s_napt; }

int8_t router_sta_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}

uint8_t router_sta_signal_percent(void)
{
    int r = router_sta_rssi();
    if (r <= -100) return 0;
    if (r >= -50) return 100;
    return (uint8_t)((r + 100) * 2);
}

float router_sta_distance_m(void)
{
    int r = router_sta_rssi();
    if (r <= -100) return 0.0f;
    const float tx_dbm = 20.0f;
    const float path_loss_n = 2.7f;
    float d = powf(10.0f, (tx_dbm - (float)r) / (10.0f * path_loss_n));
    if (d < 0.1f) d = 0.1f;
    if (d > 999.0f) d = 999.0f;
    return d;
}

uint8_t router_ap_client_count(void)
{
    wifi_sta_list_t list = {0};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? (uint8_t)list.num : 0;
}

void router_get_sta_config(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (ssid && ssid_len) {
        wifi_config_t sta = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK)
            strlcpy(ssid, (const char *)sta.sta.ssid, ssid_len);
        else
            ssid[0] = 0;
    }
    if (password && password_len) password[0] = 0;
}

esp_err_t router_set_sta_config(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32 ||
        !password || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t old_sta = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &old_sta) != ESP_OK) {
        return ESP_FAIL;
    }

    bool was_connected = s_sta_connected;
    bool was_ip = s_sta_has_ip;

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.failure_retry_cnt = 5;
    sta.sta.threshold.rssi = -127;
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    s_sta_reconfiguring = true;
    if (was_connected) {
        (void)esp_wifi_disconnect();
    }

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (e != ESP_OK) {
        (void)esp_wifi_set_config(WIFI_IF_STA, &old_sta);
        s_sta_reconfiguring = false;
        if (was_connected || was_ip) {
            request_hop_reconnect();
        }
        return e;
    }

    nvs_handle_t h;
    e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, KEY_STA_SSID, ssid);
        if (e == ESP_OK) {
            e = nvs_set_str(h, KEY_STA_PASS, password);
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }

    if (e != ESP_OK) {
        (void)esp_wifi_set_config(WIFI_IF_STA, &old_sta);
        s_sta_has_ip = false;
        s_sta_reconfiguring = false;
        request_hop_reconnect();
        return e;
    }

    s_sta_reconfiguring = false;
    s_retry_count = 0;
    request_hop_reconnect();
    return ESP_OK;
}

void router_get_ap_config(char *ssid, size_t ssid_len, char *password, size_t password_len, uint8_t *channel)
{
    router_nvs_get_str(KEY_AP_SSID, ssid, ssid_len, DEFAULT_AP_SSID);
    if (password && password_len) password[0] = 0;
    if (channel) {
        *channel = 1;
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            uint8_t stored = 1;
            if (nvs_get_u8(h, KEY_AP_CH, &stored) == ESP_OK &&
                stored >= 1 && stored <= 13) {
                *channel = stored;
            }
            nvs_close(h);
        }
    }
}

esp_err_t router_set_ap_config(const char *ssid, const char *password, uint8_t channel)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32 || !password || strlen(password) < 8 || strlen(password) > 63 || channel < 1 || channel > 13) return ESP_ERR_INVALID_ARG;
    wifi_config_t old_ap = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &old_ap) != ESP_OK) {
        memset(&old_ap, 0, sizeof(old_ap));
    }

    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    strlcpy((char*)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ssid);
    uint8_t active_channel = channel;
    if (s_sta_connected) {
        wifi_ap_record_t sta_ap = {0};
        if (esp_wifi_sta_get_ap_info(&sta_ap) == ESP_OK && sta_ap.primary >= 1 && sta_ap.primary <= 13) {
            active_channel = sta_ap.primary;
        }
    }
    ap.ap.channel = active_channel;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CONNECTIONS;
    ap.ap.beacon_interval = 100;
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e != ESP_OK) {
        return e;
    }

    nvs_handle_t h;
    e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        (void)esp_wifi_set_config(WIFI_IF_AP, &old_ap);
        return e;
    }

    e = nvs_set_str(h, KEY_AP_SSID, ssid);
    if (e == ESP_OK) e = nvs_set_str(h, KEY_AP_PASS, password);
    if (e == ESP_OK) e = nvs_set_u8(h, KEY_AP_CH, channel);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);

    if (e != ESP_OK) {
        (void)esp_wifi_set_config(WIFI_IF_AP, &old_ap);
        return e;
    }

    return ESP_OK;
}

uint8_t router_get_channel_mode(void) { return s_hop_mode; }

esp_err_t router_set_channel_mode(uint8_t mode)
{
    if (mode > HOP_MODE_FULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, KEY_HOP_MODE, mode);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    s_hop_mode = mode;
    if (!s_sta_connected) request_hop_reconnect();
    return ESP_OK;
}

bool router_fan_on(void) { return s_fan_on; }

const char *router_get_last_disconnect_reason(void) { return s_last_reason; }
uint32_t router_get_sta_uptime_s(void) { return s_sta_has_ip ? (uint32_t)((esp_timer_get_time() / 1000000ULL) - s_sta_ip_uptime) : 0; }
void router_restart(void) { vTaskDelay(pdMS_TO_TICKS(250)); esp_restart(); }

void router_factory_reset(void)
{
    nvs_flash_erase();
    router_log_clear();
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}
