#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "tcpip_adapter.h"

#include "debug.h"
#include "storage.h"

#define GW_HOSTNAME   "ske02-gw"
#define GW_AP_PASS    "ske02setup"
#define STA_TIMEOUT_MS 30000
#define STA_MAX_TRIES 10 /* association attempts before the portal */

#define EVT_GOT_IP   (1 << 0)
#define EVT_LOST     (1 << 1)

static EventGroupHandle_t sEvents;
static bool sPortalMode = false;
static char sApSsid[24];
static wifi_scan_config_t sScanCfg = { .show_hidden = 0 };
static uint8_t sStaFails;
static bool sStaEverConnected;

/*
 * Event-driven join, the canonical IDF pattern:
 *   WIFI_EVENT_STA_START          -> esp_wifi_connect()
 *   WIFI_EVENT_STA_DISCONNECTED   -> retry while tries remain (each failed
 *                                    association fires this event; giving up
 *                                    on the FIRST one, like the previous
 *                                    version did, dropped a perfectly good
 *                                    credentials set into the portal)
 *   IP_EVENT_STA_GOT_IP           -> joined
 * After a successful join every disconnect is auto-retried forever.
 */
static void on_event(void *arg, esp_event_base_t base, int32_t id,
                     void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
            return;
        }
        if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            if (sPortalMode)
                return; /* mode switch noise, not a real disconnect */
            if (sStaEverConnected)
            {
                DBG("wifi: connection lost, reconnecting\n");
                esp_wifi_connect();
                return;
            }
            if (++sStaFails < STA_MAX_TRIES)
            {
                DBG("wifi: assoc retry %u\n", (unsigned)sStaFails);
                esp_wifi_connect();
                return;
            }
            DBG("wifi: sta join failed\n");
            xEventGroupSetBits(sEvents, EVT_LOST);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        sStaEverConnected = true;
        sStaFails = 0;
        xEventGroupSetBits(sEvents, EVT_GOT_IP);
        DBG("wifi: got ip\n");
    }
}

static void start_portal(void)
{
    wifi_config_t ap = {0};
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(sApSsid, sizeof(sApSsid), "SKE02-GW-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    strcpy((char *)ap.ap.ssid, sApSsid);
    ap.ap.ssid_len = strlen(sApSsid);
    strcpy((char *)ap.ap.password, GW_AP_PASS);
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    tcpip_adapter_init();
    /* AP+STA: the STA interface is what actually scans; a pure AP mode
     * makes esp_wifi_scan_start fail with ESP_ERR_WIFI_MODE */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap);
    esp_wifi_start();
    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
    {
        tcpip_adapter_ip_info_t ip = {
            .ip = { .addr = PP_HTONL(0x0A000001U) }, /* 10.0.0.1 */
            .netmask = { .addr = PP_HTONL(0xFFFFFF00U) },
            .gw = { .addr = PP_HTONL(0x0A000001U) },
        };
        tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
    }
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
    sPortalMode = true;
    DBG("wifi: portal %s pass %s ip 10.0.0.1\n", sApSsid, GW_AP_PASS);
}

static void wifi_manager_task(void *arg)
{
    const gw_settings_t *st = storage_get();
    (void)arg;

    if (st->ssid[0])
    {
        wifi_config_t sta = {0};
        EventBits_t bits;
        strcpy((char *)sta.sta.ssid, st->ssid);
        strcpy((char *)sta.sta.password, st->pass);
        tcpip_adapter_init();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(ESP_IF_WIFI_STA, &sta);
        esp_wifi_start(); /* the connect comes on WIFI_EVENT_STA_START */
        DBG("wifi: joining \"%s\"\n", st->ssid);
        bits = xEventGroupWaitBits(sEvents, EVT_GOT_IP | EVT_LOST, pdTRUE,
                                   pdFALSE, pdMS_TO_TICKS(STA_TIMEOUT_MS));
        if (bits & EVT_GOT_IP)
        {
            /* joined: STAY on STA - falling through to start_portal()
             * here ALSO raised the AP next to the working STA link, and
             * a browser still sitting on that AP kept seeing the portal */
            tcpip_adapter_ip_info_t ip;
            tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
            DBG("wifi: sta mode, ip %s\n", ip4addr_ntoa(&ip.ip));
            vTaskDelete(NULL); /* reconnects are handled by on_event */
        }
        else
            esp_wifi_stop();
    }

    start_portal();
    vTaskDelete(NULL); /* the portal stays until reboot */
}

void wifi_start(void)
{
    wifi_init_config_t wificfg = WIFI_INIT_CONFIG_DEFAULT();
    sEvents = xEventGroupCreate();
    tcpip_adapter_init();
    esp_event_loop_create_default();
    esp_wifi_init(&wificfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    xTaskCreate(wifi_manager_task, "wifi", 3072, NULL, 3, NULL);
}

bool wifi_sta_connected(void)
{
    return !sPortalMode;
}

const char *wifi_ap_ssid(void)
{
    return sPortalMode ? sApSsid : NULL;
}

void wifi_scan_async(void)
{
    esp_wifi_scan_start(&sScanCfg, false);
}
