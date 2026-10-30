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

#define EC_MAX_NET 8    /* кэш SSID после SCAN (для JOIN-мастера) */
#define HIST_MAX 16     /* история команд (стрелки вверх/вниз) */
#define HIST_LEN 32     /* максимальная длина команды в истории */

static bool sActive;
static bool sEcho = true;

/* кэш последнего SCAN: SSID для JOIN по номеру/автокомплита */
static char sScanSsid[EC_MAX_NET][33]; /* 32 символа + null */
static uint8_t sScanCount;

/* история команд: кольцо ИНДЕКСОВ (1Б на запись, имена берём из
 * kCmds[] во flash); FORGET хранится как база, аргумент дотачивается */
static uint8_t sHistIdx[HIST_MAX]; /* 16 байт */
static uint8_t sHistHead;
static uint8_t sHistCount;
static int8_t sHistBrowse;
static uint8_t sEscState;   /* 0=норма, 1=ESC, 2=ESC[ */

bool espcons_is_active(void)
{
    return sActive;
}

/* состояния JOIN-мастера */
enum { JOIN_IDLE, JOIN_SSID, JOIN_PASS };
static uint8_t sJoinState = JOIN_IDLE;
static char sJoinSsid[33];
static char sJoinPass[65];

/* имена команд для TAB-автокомплита (литералы во flash) */
static const char *const kCmds[] = {
    "HELP", "INFO", "WIFI", "SCAN", "NETS", "JOIN",
    "FORGET", "ECHO", "AP", "REBOOT", "END", "BYE", NULL};

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
        sActive = true;
        uart_flush(EC_UART);
        uart_write_bytes(EC_UART, "\r\nESP8266 gateway console\r\n", 26);
        snprintf(line, sizeof(line), "fw %s, heap %u\r\n", g_gw_version,
                 (unsigned)esp_get_free_heap_size());
        uart_write_bytes(EC_UART, line, strlen(line));
        uart_write_bytes(EC_UART, "ESP> ", 5);
    }
    return sActive;
}

static void reply(const char *s)
{
    uart_write_bytes(EC_UART, s, strlen(s));
}

static void prompt(const char *p)
{
    char b[16];
    snprintf(b, sizeof(b), "\r\n%s", p);
    reply(b);
}

static void cmd_info(void)
{
    char b[160];
    tcpip_adapter_ip_info_t ip;
    bool ap = wifi_ap_ssid() != NULL;
    prompt("");
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
    char b[96];
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
    /* копим SSID в кэш для JOIN (номера) + печатаем список */
    sScanCount = 0;
    for (i = 0; i < (int)n && sScanCount < EC_MAX_NET; i++)
    {
        strlcpy(sScanSsid[sScanCount], (const char *)rec[i].ssid,
                sizeof(sScanSsid[0]));
        snprintf(b, sizeof(b), "  %u: %-28s rssi %d\r\n",
                 sScanCount + 1, sScanSsid[sScanCount], rec[i].rssi);
        reply(b);
        sScanCount++;
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

/* FORGET <n> или FORGET ALL */
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
}

static void cmd_help(void)
{
    reply("\r\n  HELP           this help\r\n"
          "  INFO           fw/heap/mode/ip/nets\r\n"
          "  WIFI           current connection (ssid/rssi/ip)\r\n"
          "  SCAN           wifi scan (top 8, cached for JOIN)\r\n"
          "  NETS           saved networks\r\n"
          "  JOIN           interactive: ssid (number/name/TAB) then pass\r\n"
          "                     Ctrl+C cancels the wizard\r\n"
          "  FORGET <n>|ALL     remove saved network\r\n"
          "  ECHO           toggle char echo\r\n"
          "  AP             FORGET ALL + reboot into portal\r\n"
          "  REBOOT         restart gateway\r\n"
          "  END | BYE      finish session, return USB-UART to the meter\r\n"
          "  TAB            autocomplete command / network name\r\n");
}

/* ---- TAB-автокомплит -------------------------------------------- */

static char line[64];  /* буфер строки (общий для poll и autocomplete) */
static uint8_t len;

static void tab_complete(void)
{
    const char *match[8];
    uint8_t n = 0;
    uint8_t i;
    /* используем ГЛОБАЛЬНЫЙ len (актуален: line[len]=0 всегда),
     * НЕ заводим локальный - затенение ломало счётчик после TAB */

    /* JOIN-мастер: SSID-шаг - дополняем из кэша SCAN */
    if (sJoinState == JOIN_SSID)
    {
        for (i = 0; i < sScanCount && n < 8; i++)
            if (!strncasecmp(sScanSsid[i], line, len))
                match[n++] = sScanSsid[i];
    }
    else if (sJoinState == JOIN_IDLE)
    {
        /* обычный режим: дополняем имя команды */
        for (i = 0; kCmds[i] && n < 8; i++)
            if (!strncasecmp(kCmds[i], line, len))
                match[n++] = kCmds[i];
    }
    else
        return; /* pass-шаг: не дополняем */

    if (n == 0)
        return;
    if (n == 1)
    {
        /* единственное совпадение: дописать хвост */
        uint8_t ml = strlen(match[0]);
        if (ml > len)
        {
            char tail[40];
            strlcpy(tail, match[0] + len, sizeof(tail));
            reply(tail);
            strlcat(line, match[0] + len, sizeof(line));
            len = strlen(line); /* обновить глобальный счётчик! */
        }
        return;
    }
    /* несколько: напечатать варианты, строку не менять */
    reply("\r\n");
    for (i = 0; i < n; i++)
    {
        reply(match[i]);
        reply("  ");
    }
    reply("\r\n> ");
    reply(line);
}

/* ---- JOIN-мастер: ssid -> pass -> save+reboot -------------------- */

static void join_start(void)
{
    sJoinState = JOIN_SSID;
    sJoinSsid[0] = 0;
    sJoinPass[0] = 0;
    if (sScanCount)
        prompt("ssid (number or name, TAB = list): ");
    else
        prompt("ssid (run SCAN first for list): ");
}

static void join_handle(const char *input)
{
    if (sJoinState == JOIN_SSID)
    {
        /* число = номер из последнего SCAN */
        if (input[0] >= '1' && input[0] <= '9' && !input[1] &&
            (uint8_t)(input[0] - '0') <= sScanCount)
        {
            strlcpy(sJoinSsid, sScanSsid[input[0] - '1'],
                    sizeof(sJoinSsid));
        }
        else
            strlcpy(sJoinSsid, input, sizeof(sJoinSsid));
        if (!sJoinSsid[0])
        {
            reply("\r\nempty ssid, cancelled\r\n");
            sJoinState = JOIN_IDLE;
            return;
        }
        sJoinState = JOIN_PASS;
        prompt("pass (* = echo off, empty = open): ");
        sEcho = false; /* пароль не светим */
        return;
    }
    if (sJoinState == JOIN_PASS)
    {
        strlcpy(sJoinPass, input, sizeof(sJoinPass));
        sEcho = true;
        sJoinState = JOIN_IDLE;
        /* dedup: если SSID уже сохранён - удалить старый */
        {
            int8_t idx = storage_net_find(sJoinSsid);
            if (idx >= 0)
                storage_net_del((uint8_t)idx);
        }
        if (!storage_net_add(sJoinSsid, sJoinPass))
        {
            reply("\r\nERR: storage full, FORGET first\r\n");
            return;
        }
        reply("\r\nsaved, joining...\r\n");
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

/* ---- история команд: только индексы (16Б), имена из flash -------- */

/* найти индекс команды в kCmds по префиксу строки; -1 = не команда */
static int8_t hist_lookup(const char *cmd)
{
    uint8_t i;
    for (i = 0; kCmds[i]; i++)
        if (!strncasecmp(cmd, kCmds[i], strlen(kCmds[i])))
            return (int8_t)i; /* "FORGET 3" матчится по "FORGET" */
    return -1;
}

static void hist_push(const char *cmd)
{
    int8_t idx = hist_lookup(cmd);
    if (idx < 0)
        return; /* не команда таблицы — не сохраняем */
    /* не дублировать подряд */
    if (sHistCount > 0)
    {
        uint8_t prev = (sHistHead + HIST_MAX - 1) % HIST_MAX;
        if (sHistIdx[prev] == (uint8_t)idx)
            return;
    }
    sHistIdx[sHistHead] = (uint8_t)idx;
    sHistHead = (sHistHead + 1) % HIST_MAX;
    if (sHistCount < HIST_MAX)
        sHistCount++;
    sHistBrowse = -1;
}

/* показать историческую команду вместо текущей строки */
static void hist_show(int8_t pos)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        if (sEcho)
            reply("\b \b");
    if (pos < 0)
    {
        len = 0;
        line[0] = 0;
        return;
    }
    strlcpy(line, kCmds[sHistIdx[pos]], sizeof(line));
    len = strlen(line);
    /* FORGET с аргументом: подставить базу + пробел */
    if (!strcasecmp(line, "FORGET"))
        line[len++] = ' ', line[len] = 0;
    if (sEcho)
        reply(line);
}

/* стрелка вверх: старее */
static void hist_up(void)
{
    if (sHistCount == 0)
        return;
    if (sHistBrowse < 0)
        sHistBrowse = (sHistHead + HIST_MAX - 1) % HIST_MAX;
    else
    {
        uint8_t oldest = (sHistHead + HIST_MAX - sHistCount) % HIST_MAX;
        if (sHistBrowse == oldest)
            return;
        if (--sHistBrowse < 0)
            sHistBrowse = HIST_MAX - 1;
    }
    hist_show(sHistBrowse);
}

/* стрелка вниз: новее */
static void hist_down(void)
{
    if (sHistBrowse < 0)
        return;
    sHistBrowse = (sHistBrowse + 1) % HIST_MAX;
    if (sHistBrowse == sHistHead)
        sHistBrowse = -1;
    hist_show(sHistBrowse);
}

/* ---- главный poll ------------------------------------------------ */

void espcons_poll(void)
{
    uint8_t ch;
    char ech[2];
    while (sActive)
    {
        int n = uart_read_bytes(EC_UART, &ch, 1, pdMS_TO_TICKS(200));
        if (n <= 0)
            return;

        /* стрелки: ESC [ A (вверх) / ESC [ B (вниз) */
        if (ch == 0x1B)
        {
            sEscState = 1;
            continue;
        }
        if (sEscState == 1)
        {
            sEscState = (ch == '[' || ch == 'O') ? 2 : 0;
            continue;
        }
        if (sEscState == 2)
        {
            sEscState = 0;
            if (ch == 'A')
                hist_up();
            else if (ch == 'B')
                hist_down();
            continue; /* остальные ESC-коды игнорируем */
        }

        if (ch == 0x03) /* Ctrl+C: прервать JOIN-мастер */
        {
            if (sJoinState != JOIN_IDLE)
            {
                sJoinState = JOIN_IDLE;
                sEcho = true;
                reply("\r\n^C cancelled\r\nESP> ");
                len = 0;
                line[0] = 0;
                continue;
            }
            continue; /* вне мастера Ctrl+C ничего не делает */
        }
        if (ch == '\t')
        {
            DBG("espcons: TAB, len=%u line='%s'\r\n", (unsigned)len, line);
            tab_complete();
            continue;
        }
        if (ch < ' ' && ch != '\r' && ch != '\n' && ch != 8 && ch != 0x7F)
            DBG("espcons: ctrl 0x%02X\r\n", ch);
        if (ch == '\r')
        {
            if (sEcho)
                reply("\r\n");
            line[len] = 0;
            if (sJoinState != JOIN_IDLE)
            {
                /* мастер собирает ввод построчно */
                join_handle(line);
                len = 0;
                if (sJoinState == JOIN_IDLE && sActive)
                    reply("ESP> ");
                else if (sJoinState == JOIN_PASS && sActive)
                    ; /* промпт уже напечатан в join_handle */
                continue;
            }
            if (len)
            {
                hist_push(line); /* в историю до исполнения */
                if (!strncasecmp(line, "FORGET ", 7))
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
                else if (!strcasecmp(line, "JOIN"))
                    join_start();
                else if (!strcasecmp(line, "ECHO"))
                {
                    sEcho = !sEcho;
                    reply(sEcho ? "\r\necho on\r\n" : "\r\necho off\r\n");
                }
                else if (!strcasecmp(line, "AP"))
                {
                    reply("\r\ndropping all networks, reboot to portal\r\n");
                    storage_net_delall();
                    storage_set_wifi("", "");
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
            continue;
        if ((ch == 8 || ch == 0x7F) && len)
        {
            len--;
            line[len] = 0; /* терминировать на новой позиции */
            if (sEcho)
                reply("\b \b");
            continue;
        }
        if (len < sizeof(line) - 1 && ch >= ' ' && ch < 127)
        {
            line[len++] = (char)ch;
            line[len] = 0; /* всегда терминирована: strlen корректен
                            * для TAB и после Enter */
            if (sEcho)
            {
                ech[0] = (char)ch;
                ech[1] = 0;
                reply(ech);
            }
        }
    }
}
