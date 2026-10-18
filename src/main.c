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
    ntp_init();   /* конфиг из NVS; сам SNTP стартует по получении IP */
    hex_store_init(); /* раздел avrhex под образы прибора */
    /*
     * ПРИБОР ПЕРВЫМ: листинг дерева читается ДО старта беспроводных
     * задач - куча девственная (14К буфер гарантирован), flash-паузы
     * NVS никому не мешают. Ждём готовности до 25с; если прибора нет -
     * WiFi поднимется всё равно, листинг догонит в фоне.
     */
    ske02_start();
    /*
     * ПРИБОР ПЕРВЫМ, СТРОГО: WiFi не поднимаем, пока листинг ЭТОЙ
     * загрузки не дошёл до конца (ske02_ready). tree_count() не
     * годится: он читает СТАРОЕ дерево из NVS и всегда > 0 - wifi
     * стартовал мгновенно, его выхлоп на UART0 рвал команды
     * ("I (68..)" для консоли прибора = мусор -> ERR syntax).
     * Страховка от отсутствия прибора - 180с.
     */
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
    /* the wifi task needs a moment to bring an interface up */
    vTaskDelay(pdMS_TO_TICKS(2000));
    web_start();
    dns_start();
    mb_pass_start(); /* единственный Modbus TCP сервер: прозрачный мост :502 */
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

/*
 * Логгер провалов кучи: опрашивает свободную память каждые 100 мс
 * и печатает каждый НОВЫЙ минимум (шаг 512 Б) с аптаймом — по
 * таймштампам видно, какое событие (страница/params/скан) ест кучу.
 */
static uint32_t sLastDipLog;

static void heap_watch_task(void *arg)
{
    uint32_t watermark = UINT32_MAX;
    (void)arg;
    for (;;)
    {
        uint32_t h = esp_get_free_heap_size();
        if (h + 512 < watermark)
        {
            watermark = (h + 256) & ~255u;   /* гистерезис от дребезга */
            /* не чаще раза в минуту: прошивка AVR сыплет дипами на
             * каждую запись страницы и тонет в них */
            {
                uint32_t now_s = (uint32_t)(xTaskGetTickCount() *
                                            portTICK_PERIOD_MS / 1000);
                if (now_s - sLastDipLog >= 60)
                {
                    sLastDipLog = now_s;
                    DBG("gw: heap dip %u at %lus\n", (unsigned)h,
                        (unsigned long)now_s);
                }
            }
        }
        else if (h > watermark + 2048)
            watermark = h;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    dbg_init(); /* UART1 log + silence the SDK console on UART0 */
    xTaskCreate(boot_task, "boot", 3072, NULL, 4, NULL);
    xTaskCreate(stat_task, "stat", 2048, NULL, 2, NULL);
    xTaskCreate(heap_watch_task, "heapw", 2048, NULL, 1, NULL);
}
