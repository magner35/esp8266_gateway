#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"

#include "debug.h"
#include "espcons.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "storage.h"
#include "wifi.h"

/* UART0 шлюза (совпадает с ske02.c) */
#define EC_UART UART_NUM_0

extern const char g_gw_version[44]; /* main.c: метка версии */

#define EC_PIN_REQ 14   /* <- PB1: LOW = USB-UART у нас (инверсная,
                          * подтяжка вверх из SDK; GPIO14 не страп) */
#define EC_PIN_ACK 12   /* -> PB3: HIGH = сессия окончена */

static bool sActive;
static bool sEcho = true;

void espcons_init(void)
{
    gpio_config_t in = {.pin_bit_mask = 1ULL << EC_PIN_REQ,
                        .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&in);
    gpio_config_t out = {.pin_bit_mask = 1ULL << EC_PIN_ACK,
                         .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_set_level(EC_PIN_ACK, 0);
}

bool espcons_check_enter(void)
{
    if (sActive)
        return true;
    if (!gpio_get_level(EC_PIN_REQ)) /* активный уровень LOW */
    {
        char line[64];
        DBG("espcons: req LOW, enter\r\n");
        sActive = true;
        DBG("espcons: t1 flush\r\n");
        uart_flush(EC_UART);
        DBG("espcons: t2 banner\r\n");
        uart_write_bytes(EC_UART, "\r\nESP8266 gateway console\r\n", 26);
        DBG("espcons: t3 snprintf\r\n");
        snprintf(line, sizeof(line), "fw %s, heap %u\r\n", g_gw_version,
                 (unsigned)esp_get_free_heap_size());
        DBG("espcons: t4 fw line\r\n");
        uart_write_bytes(EC_UART, line, strlen(line));
        uart_write_bytes(EC_UART, "ESP> ", 5);
        DBG("espcons: session open\r\n");
    }
    return sActive;
}

static void reply(const char *s)
{
    uart_write_bytes(EC_UART, s, strlen(s));
}

static void cmd_info(void)
{
    char b[160];
    tcpip_adapter_ip_info_t ip;
    bool ap = wifi_ap_ssid() != NULL;
    reply("\r\n");
    snprintf(b, sizeof(b), "fw    %s\r\n", g_gw_version);
    reply(b);
    snprintf(b, sizeof(b), "heap  %u (min %u)\r\n",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    reply(b);
    snprintf(b, sizeof(b), "mode  %s\r\n", ap ? "AP (portal)" : "STA");
    reply(b);
    if (!ap && tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK)
    {
        snprintf(b, sizeof(b), "ip    %s\r\n", ip4addr_ntoa(&ip.ip));
        reply(b);
    }
    snprintf(b, sizeof(b), "nets  %u saved\r\n", storage_net_count());
    reply(b);
}

static void cmd_wifi(void)
{
    char b[160];
    wifi_ap_record_t ap;
    tcpip_adapter_ip_info_t ip;
    if (wifi_ap_ssid())
    {
        snprintf(b, sizeof(b), "\r\nAP \"%s\" 192.168.4.1\r\n",
                 wifi_ap_ssid());
        reply(b);
        return;
    }
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    {
        reply("\r\nSTA not connected\r\n");
        return;
    }
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
    snprintf(b, sizeof(b), "\r\nSTA \"%s\" %s rssi %d ip %s\r\n",
             ap.ssid, ap.rssi > -60 ? "strong" : ap.rssi > -75 ? "ok" : "weak",
             ap.rssi, ip4addr_ntoa(&ip.ip));
    reply(b);
}

static void cmd_scan(void)
{
    wifi_scan_config_t cfg = {0};
    wifi_ap_record_t rec[12];
    uint16_t n = 12;
    char b[128];
    int i;
    reply("\r\nscanning...");
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK)
    {
        reply(" failed\r\n");
        return;
    }
    snprintf(b, sizeof(b), " %u networks\r\n", n);
    reply(b);
    for (i = 0; i < n && i < 12; i++)
    {
        snprintf(b, sizeof(b), "%-28s rssi %d\r\n", rec[i].ssid,
                 rec[i].rssi);
        reply(b);
    }
}

static void cmd_nets(void)
{
    char b[64];
    wifi_net_t net;
    uint8_t n = storage_net_count();
    uint8_t i;
    snprintf(b, sizeof(b), "\r\n%u saved:\r\n", n);
    reply(b);
    for (i = 0; i < n; i++)
    {
        if (!storage_net_get(i, &net))
            continue;
        snprintf(b, sizeof(b), "  %u: %s\r\n", i + 1, net.ssid);
        reply(b);
    }
}

/* JOIN <ssid> <pass>: сохранить сеть и перезагрузиться (wifi_start
 * при загрузке перебирает сохранённые). Пустой пароль = открытая */
static void cmd_join(const char *args)
{
    char ssid[sizeof(((wifi_net_t *)0)->ssid)];
    char pass[sizeof(((wifi_net_t *)0)->pass)];
    const char *sp = args;
    const char *pp;
    size_t sl, pl;

    while (*sp == ' ')
        sp++;
    pp = strchr(sp, ' ');
    if (!pp || !*sp)
    {
        reply("\r\nusage: JOIN <ssid> <pass>\r\n");
        return;
    }
    sl = (size_t)(pp - sp);
    pp++;
    while (*pp == ' ')
        pp++;
    pl = strlen(pp);
    if (sl == 0 || sl >= sizeof(ssid) || pl >= sizeof(pass))
    {
        reply("\r\nERR: ssid/pass too long\r\n");
        return;
    }
    memcpy(ssid, sp, sl);
    ssid[sl] = 0;
    strcpy(pass, pp);

    if (storage_net_find(ssid) >= 0)
        storage_net_del((uint8_t)storage_net_find(ssid));
    if (!storage_net_add(ssid, pass))
    {
        reply("\r\nERR: storage full, FORGET first\r\n");
        return;
    }
    reply("\r\nsaved, joining...\r\n");
    esp_wifi_stop(); /* та же причина, что и в AP: сброс автоконнекта */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart(); /* wifi_start подхватит новую сеть */
}

/* FORGET <n> или FORGET ALL: удалить сеть(и) */
static void cmd_forget(const char *args)
{
    char b[64];
    while (*args == ' ')
        args++;
    if (!strcasecmp(args, "ALL"))
    {
        /* как при FORGET-сигнале из меню прибора */
        storage_net_delall();
        storage_set_wifi("", "");
        storage_save(storage_get());
        reply("\r\nall networks forgotten\r\n");
        return;
    }
    {
        int n = atoi(args);
        if (n < 1 || n > storage_net_count())
        {
            reply("\r\nusage: FORGET <n>|ALL (see NETS)\r\n");
            return;
        }
        storage_net_del((uint8_t)(n - 1));
        snprintf(b, sizeof(b), "\r\nnetwork %d forgotten\r\n", n);
        reply(b);
    }
}

static void cmd_help(void);

/* END: сообщить ATmega (GPIO12 HIGH), дождаться возврата мультиплексора
 * (PB1/GPIO14 упадёт в течение ~1 с), отпустить ACK */
static void cmd_end(void)
{
    int waits;
    gpio_set_level(EC_PIN_ACK, 1);
    reply("\r\nESP> closing...\r\n");
    for (waits = 0; waits < 30; waits++)
    {
        if (gpio_get_level(EC_PIN_REQ)) /* линия отпущена = HIGH */
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    gpio_set_level(EC_PIN_ACK, 0);
    if (!gpio_get_level(EC_PIN_REQ))
    {
        reply("ESP> meter did not take the line back\r\nESP> ");
        return;
    }
    sActive = false;
    DBG("espcons: session closed\r\n");
}

static void cmd_help(void)
{
    reply("\r\n  HELP           this help\r\n"
          "  INFO           fw/heap/mode/ip/nets\r\n"
          "  WIFI           current connection (ssid/rssi/ip)\r\n"
          "  SCAN           wifi scan (top 12)\r\n"
          "  NETS           saved networks\r\n"
          "  JOIN <ssid> <pass>  save network + reboot to join\r\n"
          "  FORGET <n>|ALL     remove saved network\r\n"
          "  ECHO           toggle char echo\r\n"
          "  AP             FORGET ALL + reboot into portal (careful!)\r\n"
          "  REBOOT         restart gateway\r\n"
          "  END | BYE      finish session, return USB-UART to the meter\r\n");
}

/* эхо по умолчанию ВКЛ (как в консоли прибора): терминалы шлют слепые,
 * приборная консоль тоже эхает; ECHO переключает */
void espcons_poll(void)
{
    static char line[64];
    static uint8_t len;
    uint8_t ch;
    char ech[2];
    while (sActive)
    {
        int n = uart_read_bytes(EC_UART, &ch, 1, pdMS_TO_TICKS(200));
        if (n <= 0)
            return; /* цикл meter продолжит: промпт не перерисовываем */
        if (ch == '\r')
        {
            if (sEcho)
                reply("\r\n");
            line[len] = 0;
            if (len)
            {
                if (!strncasecmp(line, "JOIN ", 5))
                    cmd_join(line + 5);
                else if (!strncasecmp(line, "FORGET ", 7))
                    cmd_forget(line + 7);
                else if (!strcasecmp(line, "HELP"))
                    cmd_help();
                else if (!strcasecmp(line, "INFO"))
                    cmd_info();
                else if (!strcasecmp(line, "WIFI"))
                    cmd_wifi();
                else if (!strcasecmp(line, "SCAN"))
                    cmd_scan();
                else if (!strcasecmp(line, "NETS"))
                    cmd_nets();
                else if (!strcasecmp(line, "ECHO"))
                {
                    sEcho = !sEcho;
                    reply(sEcho ? "\r\necho on\r\n" : "\r\necho off\r\n");
                }
                else if (!strcasecmp(line, "AP"))
                {
                    reply("\r\ndropping all networks, reboot to portal\r\n");
                    /* ровно те же действия, что и при FORGET-сигнале
                     * из меню прибора (ske02.c wifi_send_line): */
                    storage_net_delall();
                    storage_set_wifi("", "");    /* legacy ssid/pass */
                    storage_save(storage_get());
                    esp_restart();
                }
                else if (!strcasecmp(line, "REBOOT"))
                {
                    reply("\r\nrebooting\r\n");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
                else if (!strcasecmp(line, "END") || !strcasecmp(line, "BYE"))
                {
                    cmd_end();
                    if (!sActive)
                        return;
                }
                else
                    reply("\r\nERR cmd (HELP for list)\r\n");
                len = 0;
                if (sActive)
                    reply("ESP> ");
            }
            else
                reply("\r\nESP> ");
            continue;
        }
        if (ch == '\n')
            continue; /* только \r - разделитель, \n от эха терминала */
        if ((ch == 8 || ch == 0x7F) && len)
        {
            len--;
            if (sEcho)
                reply("\b \b");
            continue;
        }
        if (len < sizeof(line) - 1 && ch >= ' ' && ch < 127)
        {
            line[len++] = (char)ch;
            if (sEcho)
            {
                ech[0] = (char)ch;
                ech[1] = 0;
                reply(ech);
            }
        }
    }
}
