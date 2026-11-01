/*
 * Created on Fri Oct 30 2026
 *
 * Copyright (c) 2026 by magner mr.jaedong@gmail.com
 */

/*
 * ESP8266 gateway for the SKE-02 flow meter - ESP8266_RTOS_SDK v3.4 port.
 * Tasks: wifi manager, meter (UART console client), httpd, modbus tcp,
 * captive dns (portal mode only).
 */
#include <stdint.h>
#include "version.h"

/* Штамп сборки (дата+время) для отладки; номер версии — GW_FWVERSION
 * в version.h, единообразно с прибором */
const char g_gw_version[44] __attribute__((section(".rodata_desc"))) =
    "SKEGW 3425: " __DATE__ " " __TIME__;

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "lwip/ip4_addr.h"

#include "debug.h"
#include "mb_pass.h"
#include "espcons.h"
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
        /* дерево прибора ждём НЕДОЛГО: веб не должен быть заложником
         * консоли прибора - иначе при мёртвом/несогласованном приборе
         * (например, прибор на другой скорости UART) теряем и веб, и
         * единственный путь его перепрошить. Дерево доедет само, как
         * только консоль оживёт (wake-цикл ske02 продолжается) */
        int waited = 0;
        while (!ske02_ready() && waited < 12000)
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
    /* проверка сервера обновлений ДО старта веба: куча свободна от
     * httpd, статус OTA в меню прибора готов к первому опросу;
     * дальнейшие проверки - раз в сутки */
    espcons_ota_boot_check();
    web_start(); /* портал нуждается в httpd всегда */
    /* captive-DNS стартует из start_portal(): в STA-режиме задачи
     * DNS не существует вовсе - ни стека, ни строки в логе */
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

    /* отложенная прошивка прибора, пережившая ребут: проверяем сами,
     * НЕ завися от опроса прибора (ske02 может ещё быть в ретраях,
     * а прошивка сама чинит прибор — GPIO5-сброс в urboot) */
    vTaskDelay(pdMS_TO_TICKS(5000));
    espcons_ota_pend_check();
    vTaskDelete(NULL);
}

void app_main(void)
{
    dbg_init(); /* UART1 log + silence the SDK console on UART0 */
    xTaskCreate(boot_task, "boot", 3072, NULL, 4, NULL);
    /* отдельной задачи мониторинга НЕТ (экономия ~2.4К кучи):
     * сердцебиение heap/min/hwm печатает meter_task в "m ok",
     * окно NTP проверяет wifi-тик ske02 - как STACKMON в приборе */
}
