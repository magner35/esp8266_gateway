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
#include "dns.h"
#include "storage.h"
#include "ntp.h"

#define GW_HOSTNAME   "ske02-gw"
#define GW_AP_PASS    "ske02setup"
#define STA_TIMEOUT_MS   30000 /* все сети: полный таймаут (быстрый цикл валит драйвер) */
#define STA_MAX_TRIES     10   /* ретраев ассоциации на сеть (как было до правок AP) */

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
    if (base == WIFI_EVENT)
    {
        /* диагностика портала: кто подключился/отвалился */
        if (id == WIFI_EVENT_AP_STACONNECTED)
            DBG("wifi: station connected\n");
        if (id == WIFI_EVENT_AP_STADISCONNECTED)
            DBG("wifi: station disconnected\n");
        if (id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
            return;
        }
        if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            if (data)
                DBG("wifi: sta down, reason %d\n",
                    (int)((const system_event_sta_disconnected_t *)data)->reason);
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
                vTaskDelay(pdMS_TO_TICKS(10)); /* дать idle покормить WDT */
                esp_wifi_connect();
                return;
            }
            DBG("wifi: sta join failed\n");
            xEventGroupSetBits(sEvents, EVT_LOST);
        }
        return;
    }
    if (base == IP_EVENT)
    {
        if (id == IP_EVENT_STA_GOT_IP)
        {
            sStaEverConnected = true;
            sStaFails = 0;
            xEventGroupSetBits(sEvents, EVT_GOT_IP);
            ntp_apply();   /* появился интернет - запуск SNTP */
            if (data)
                DBG("wifi: got ip %s\n",
                    ip4addr_ntoa(&((const ip_event_got_ip_t *)data)->ip_info.ip));
            else
                DBG("wifi: got ip\n");
            return;
        }
        /* DHCP выдал станции адрес — канал до неё работает.
         * ВАЖНО: compat-слой этого SDK постит AP_STAIPASSIGNED
         * с data=NULL (event_send_compat.inc, HANDLE_SYS_EVENT
         * без ARG) — разыменование_NULL здесь роняло таск цикла
         * событий в немую панику и hardware WDT (rst cause:4). */
        if (id == IP_EVENT_AP_STAIPASSIGNED)
        {
            DBG("wifi: dhcp gave station an address\n");
            return;
        }
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
    ap.ap.max_connection = 2;
    /*
     * ОТКРЫТАЯ сеть: WPA2-AP требовал лишнюю кучу под crypto-buffers
     * (esp_wifi_set_storage default), на грани OOM. Портал существует
     * только пока потерян домашний WiFi — открытая точка приемлема.
     */
    ap.ap.authmode = WIFI_AUTH_OPEN;

    tcpip_adapter_init();
    /*
     * AP+STA: the STA interface is what actually scans; a pure AP mode
     * makes esp_wifi_scan_start fail with ESP_ERR_WIFI_MODE.
     * Адрес и DHCP — ДЕФОЛТ адаптера (192.168.4.1 + пул): ручная
     * установка 10.0.0.1 гонилась с асинхронным AP-start событием,
     * адаптер возвращал свой дефолт — клиенты получали адрес из
     * чужой подсети, и 10.0.0.1 был недостижим.
     */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap);
    /* B+G: проприетарный 11n ESP8266 — отдельный источник падений AP */
    esp_wifi_set_protocol(WIFI_IF_AP,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    esp_wifi_start();
    dbg_rom_redirect(); /* wifi-драйвер мог вернуть свой ets-putc */
    esp_wifi_set_ps(WIFI_PS_NONE);   /* явно: PS + softAP = wdt */
    sPortalMode = true;
    dns_start(); /* captive-redirect: существует только в портале,
                  * в STA-режиме задачи DNS нет вовсе */
    DBG("wifi: portal %s open ip 192.168.4.1\n", sApSsid);
}

static void wifi_manager_task(void *arg)
{
    (void)arg;
    uint8_t nets = storage_net_count();

    /* НЕТ сохранённых сетей → ТОЛЬКО портал (AP). STA-интерфейс в APSTA
     * ничего не подключает (список пуст) — скан работает, интернета нет. */
    if (nets == 0)
    {
        start_portal();
        vTaskDelete(NULL);
    }

    /* Есть сохранённые сети → ТОЛЬКО STA, бесконечный перебор.
     * Никогда не падаем в портал: после подключения event handler
     * сам реконнектит; если ни одна не подключилась — круг снова. */
    {
        wifi_config_t sta = {0};
        wifi_net_t net;
        EventBits_t bits;
        tcpip_adapter_init();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_ps(WIFI_PS_NONE);
        for (;;)
        {
            for (uint8_t i = 0; i < nets; i++)
            {
                if (!storage_net_get(i, &net) || !net.ssid[0])
                    continue;
                sStaFails = 0;
                sStaEverConnected = false;
                strcpy((char *)sta.sta.ssid, net.ssid);
                strcpy((char *)sta.sta.password, net.pass);
                esp_wifi_set_config(ESP_IF_WIFI_STA, &sta);
                esp_wifi_start();
                dbg_rom_redirect(); /* wifi-драйвер мог вернуть свой ets-putc */
                DBG("wifi: joining \"%s\" (%u/%u)\n", net.ssid, i + 1, nets);
                bits = xEventGroupWaitBits(sEvents, EVT_GOT_IP | EVT_LOST, pdTRUE,
                                           pdFALSE, pdMS_TO_TICKS(STA_TIMEOUT_MS));
                if (bits & EVT_GOT_IP)
                {
                    tcpip_adapter_ip_info_t ip;
                    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
                    DBG("wifi: sta mode, ip %s\n", ip4addr_ntoa(&ip.ip));
                    vTaskDelete(NULL); /* реконнектами управляет on_event */
                }
                esp_wifi_stop();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            DBG("wifi: all %u networks failed, retry round\n", nets);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void wifi_start(void)
{
    wifi_init_config_t wificfg = WIFI_INIT_CONFIG_DEFAULT();
    sEvents = xEventGroupCreate();
    tcpip_adapter_init();
    esp_event_loop_create_default();
    esp_wifi_init(&wificfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    /* ANY_ID: нужны и STA_GOT_IP, и AP_STAIPASSIGNED (портал) */
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
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
