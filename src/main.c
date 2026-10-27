/*
 * ESP8266 gateway for the SKE-02 flow meter - ESP8266_RTOS_SDK v3.4 port.
 * Tasks: wifi manager, meter (UART console client), httpd, modbus tcp,
 * captive dns (portal mode only).
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "lwip/ip4_addr.h"

#include "debug.h"
#include "dns.h"
#include "mb_pass.h"
#include "ske02.h"
#include "storage.h"
#include "tree_store.h"
#include "ntp.h"
#include "hex_store.h"
#include "web.h"
#include "wifi.h"

static void boot_task(void *arg)
{
    (void)arg;
    DBG("gateway rtos: boot\n");
    storage_init();
    ntp_init();   /* конфиг из NVS; SNTP стартует только по IP (в AP не стартует) */
    hex_store_init();
    ske02_start();
    {
        int waited = 0;
        while (!ske02_ready() && waited < 180000)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        DBG("gateway: tree %s at boot (%d ms)\n",
            ske02_ready() ? "ready" : "pending", waited);
    }
    DBG("gateway: settings fully loaded, starting WiFi\n");
    wifi_start();
    vTaskDelay(pdMS_TO_TICKS(2000));
    web_start(); /* портал нуждается в httpd всегда */
    dns_start(); /* captive-redirect тоже */
    /*
     * AP-режим (портал): экономим кучу — Modbus TCP мост не нужен
     * (SCADA не придёт на портал), heap-стек не нужен (диагностика).
     * WiFi-скан уже on-demand (malloc при заходе на страницу Wi-Fi).
     * NTP не стартует (SNTP активируется только по IP_EVENT_STA_GOT_IP).
     */
    if (!wifi_ap_ssid())
        mb_pass_start(); /* прозрачный мост :502 — только в STA */
    else
        DBG("gateway: AP mode - modbus bridge off (heap)\n");
    vTaskDelete(NULL);
}

/*
 * Сводка состояния раз в 30 с на UART1: видно без браузера, жив ли
 * WiFi, какой адрес и что с heap — когда "страница недоступна",
 * но прибор обменивается.
 */
static void stat_task(void *arg)
{
    tcpip_adapter_ip_info_t ip;
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));
    for (;;)
    {
        if (wifi_ap_ssid())
        {
            DBG("gw: portal, heap %u\n",
                (unsigned)esp_get_free_heap_size());
        }
        else if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK)
        {
            wifi_ap_record_t ap;
            int rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                rssi = ap.rssi;
            DBG("gw: sta %s rssi %d heap %u min %u\n",
                ip4addr_ntoa(&ip.ip), rssi,
                (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size());
        }
        else
            DBG("gw: no ip, heap %u min %u\n",
                (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    dbg_init(); /* UART1 log + silence the SDK console on UART0 */
    xTaskCreate(boot_task, "boot", 3072, NULL, 4, NULL);
    xTaskCreate(stat_task, "stat", 2048, NULL, 2, NULL);
}
