#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "debug.h"
#include "espcons.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "storage.h"
#include "wifi.h"
#include "web.h"
#include "ske02.h"
#include "ota_sec.h"
#include "hex_store.h"

/* UART0 шлюза (совпадает с ske02.c) */
#define EC_UART UART_NUM_0

extern const char g_gw_version[44]; /* main.c: метка версии */

#define EC_PIN_REQ 14 /* <- PB1: LOW = USB-UART у нас (инверсная, \
                       * подтяжка вверх из SDK; GPIO14 не страп) */
#define EC_PIN_ACK 12 /* -> PB3: HIGH = сессия окончена */

#define EC_MAX_NET 10 /* кэш SSID после SCAN (для JOIN-мастера) */
#define HIST_MAX 16  /* история команд (стрелки вверх/вниз) */
#define HIST_LEN 32  /* максимальная длина команды в истории */

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
static uint8_t sEscState; /* 0=норма, 1=ESC, 2=ESC[ */

bool espcons_is_active(void)
{
    return sActive;
}

/* состояния JOIN-мастера */
enum
{
    JOIN_IDLE,
    JOIN_SSID,
    JOIN_PASS
};
static uint8_t sJoinState = JOIN_IDLE;
static char sJoinSsid[33];
static char sJoinPass[65];

/* имена команд для TAB-автодополнения (литералы во flash);
 * новые команды добавляем в нижнем регистре */
static const char *const kCmds[] = {
    "help", "info", "wifi", "scan", "nets", "join",
    "forget", "echo", "ap", "url", "check", "download",
    "update_gateway", "update_meter", "ping",
    "reboot", "exit", "quit", NULL};

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

bool espcons_check_enter(void);
static void upd_url_init(void); /* определена ниже, рядом с cmd_url */

bool espcons_check_enter(void)
{
    if (sActive)
        return true;
    if (!gpio_get_level(EC_PIN_REQ)) /* активный уровень LOW */
    {
        char line[64];
        sActive = true;
        upd_url_init(); /* адрес сервера обновлений из NVS */
        uart_flush(EC_UART);
        uart_write_bytes(EC_UART, "\r\nGateway console\r\n", 26);
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
             ap.ssid, ap.rssi > -60 ? "strong" : ap.rssi > -75 ? "ok"
                                                               : "weak",
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

/* ---- UPDATE: скачать .skpak с сервера при выключенном вебе --------
 * Философия: остановить все пожиратели кучи (httpd стека 8КБ,
 * ske02 мониторинг уже на паузе — консольный режим), затем качать.
 * ~35КБ доступно против ~12КБ нужных на TCP-клиент.               */

#include "esp_ota_ops.h"
#include "spi_flash.h"
#include "uart_avr.h"
#include "urclock.h"

static ota_sec_t sUpdSec;
static uint8_t sUpdHdr[OTA_SEC_HDR];
static uint32_t sUpdHdrHave;
static bool sUpdHdrDone;
static uint32_t sUpdFed; /* расшифрованных байт после заголовка */
static uint8_t sUpdDest; /* 1=esp 2=avr 3=bundle */
static esp_ota_handle_t sUpdOta;
static bool sUpdOtaOpen, sUpdHexOpen;
static uint32_t sUpdEspLen, sUpdFedEsp;
static uint8_t sUpdLenHave;
static uint8_t sUpdLenb[4];
static volatile bool sRunAbort; /* Ctrl+C во время run_in_task */

/* приёмник расшифрованного потока — как dl_sink в upd.c, но static */
static int upd_sink(uint8_t *d, uint32_t n, char *err, size_t errcap)
{
    while (n)
    {
        if (!sUpdHdrDone)
        {
            uint32_t need = OTA_SEC_HDR - sUpdHdrHave;
            uint32_t take = n < need ? n : need;
            memcpy(sUpdHdr + sUpdHdrHave, d, take);
            sUpdHdrHave += take;
            d += take;
            n -= take;
            if (sUpdHdrHave < OTA_SEC_HDR)
                return 0;
            uint32_t max = 2 * 1024 * 1024; /* любой тип, лимит бандла */
            int rc = ota_sec_begin(&sUpdSec, sUpdHdr, max);
            if (rc != OTA_SEC_OK)
            {
                snprintf(err, errcap, "hdr: %s", ota_sec_str(rc));
                return -1;
            }
            sUpdHdrDone = true;
            sUpdDest = sUpdSec.type; /* автоопределение из пакета */
            if (sUpdDest == 1)
            {
                /* пакет только ESP: образ = весь plain_len */
                if (esp_ota_begin(esp_ota_get_next_update_partition(NULL),
                                  sUpdSec.plain_len, &sUpdOta) != ESP_OK)
                {
                    snprintf(err, errcap, "ota begin");
                    return -1;
                }
                sUpdOtaOpen = true;
            }
            else if (sUpdDest == 2)
            {
                /* пакет только AVR: весь plain_len — hex */
                if (!hex_store_begin(sUpdSec.plain_len))
                {
                    snprintf(err, errcap, "hex store");
                    return -1;
                }
                sUpdHexOpen = true;
            }
            continue;
        }
        if (sUpdDest == 3 && sUpdLenHave < 4)
        {
            uint32_t need = 4 - sUpdLenHave;
            uint32_t take = n < need ? n : need;
            if (ota_sec_feed(&sUpdSec, d, take) != 0)
            {
                snprintf(err, errcap, "decrypt fail");
                return -1;
            }
            memcpy(sUpdLenb + sUpdLenHave, d, take);
            sUpdLenHave += take;
            d += take;
            n -= take;
            sUpdFed += take;
            if (sUpdLenHave < 4)
                return 0;
            memcpy(&sUpdEspLen, sUpdLenb, 4);
            if (sUpdEspLen == 0 || sUpdEspLen + 4 >= sUpdSec.plain_len)
            {
                snprintf(err, errcap, "bad esp len");
                return -1;
            }
            if (esp_ota_begin(esp_ota_get_next_update_partition(NULL),
                              sUpdEspLen, &sUpdOta) != ESP_OK)
            {
                snprintf(err, errcap, "ota begin");
                return -1;
            }
            sUpdOtaOpen = true;
            if (!hex_store_begin(sUpdSec.plain_len - 4 - sUpdEspLen))
            {
                snprintf(err, errcap, "hex store");
                return -1;
            }
            sUpdHexOpen = true;
            continue;
        }
        {
            uint32_t take = n;
            if (ota_sec_feed(&sUpdSec, d, take) != 0)
            {
                snprintf(err, errcap, "decrypt fail");
                return -1;
            }
            if (sUpdDest == 1 ||
                (sUpdDest == 3 && sUpdFed < sUpdEspLen + 4))
            {
                uint32_t pos = sUpdDest == 3 ? sUpdFed - 4 : sUpdFed;
                uint32_t to_esp = sUpdDest == 3 ? sUpdEspLen - pos : take;
                if (to_esp > take)
                    to_esp = take;
                if (to_esp && esp_ota_write(sUpdOta, d, to_esp) != ESP_OK)
                {
                    snprintf(err, errcap, "ota write");
                    return -1;
                }
                sUpdFedEsp += to_esp;
                if (sUpdDest == 3 && to_esp < take)
                    if (!hex_store_chunk((const char *)(d + to_esp),
                                         take - to_esp))
                    {
                        snprintf(err, errcap, "hex write");
                        return -1;
                    }
            }
            else
            {
                if (!hex_store_chunk((const char *)d, take))
                {
                    snprintf(err, errcap, "hex write");
                    return -1;
                }
            }
            sUpdFed += n;
            d += n;
            n = 0;
        }
    }
    return 0;
}

/* простой HTTP GET c сервера (гасим httpd → куча ~35КБ) */
/* прогресс скачивания: пишет upd_http_get, печатает цикл ожидания
 * run_in_task (для остальных операций sUpdTotal остаётся 0) */
static volatile uint32_t sUpdTotal; /* всего байт в файле (Content-Length) */
static volatile uint32_t sUpdPos;   /* принято байт (заголовок + тело) */

static long upd_http_get(const char *url,
                         char *err, size_t errcap)
{
    char host[64], path[96], hdr[512], req[224];
    struct hostent *he;
    struct sockaddr_in sa;
    int fd = -1;
    size_t hlen = 0;
    long clen = -1, total = 0;
    uint16_t port = 80;
    int rc = -1;
    const char *p, *slash;
    char *colon;

    sUpdTotal = 0;
    sUpdPos = 0;
    p = url;
    if (!strncmp(p, "http://", 7))
        p += 7;
    slash = strchr(p, '/');
    if (!slash || slash == p || strlen(slash) > sizeof(path) - 1)
    {
        snprintf(err, errcap, "bad url");
        return -1;
    }
    memcpy(host, p, slash - p);
    host[slash - p] = 0;
    strcpy(path, slash);
    colon = strchr(host, ':');
    if (colon)
    {
        port = (uint16_t)atoi(colon + 1);
        *colon = 0;
    }
    he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
    {
        snprintf(err, errcap, "dns fail");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        snprintf(err, errcap, "socket");
        return -1;
    }
    {
        struct timeval ct = {.tv_sec = 8, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof(ct));
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        {
            snprintf(err, errcap, "no connect");
            goto out;
        }
    }
    {
        /* короткий recv-таймаут: циклы ниже успевают увидеть sRunAbort
         * (Ctrl+C), а общий бюджет молчания держим счётчиком stalls (20 с) */
        struct timeval rt = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    if (send(fd, req, strlen(req), 0) <= 0)
    {
        snprintf(err, errcap, "send");
        goto out;
    }
    {
        int stalls = 0;
        for (;;)
        {
            int n;
            if (sRunAbort)
            {
                snprintf(err, errcap, "aborted");
                goto out;
            }
            n = recv(fd, hdr + hlen, 1, 0);
            if (n == 0)
            {
                snprintf(err, errcap, "hdr break");
                goto out;
            }
            if (n < 0)
            {
                if (++stalls >= 20)
                {
                    snprintf(err, errcap, "hdr timeout");
                    goto out;
                }
                continue;
            }
            stalls = 0;
            hlen++;
            if (hlen >= sizeof(hdr))
            {
                snprintf(err, errcap, "hdr too big");
                goto out;
            }
            if (hlen >= 4 && !memcmp(hdr + hlen - 4, "\r\n\r\n", 4))
                break;
        }
    }
    hdr[hlen] = 0;
    if (strncmp(hdr, "HTTP/1.", 7) || hdr[9] != '2')
    {
        snprintf(err, errcap, "http %c%c%c", hdr[9], hdr[10], hdr[11]);
        goto out;
    }
    {
        char *cl = strstr(hdr, "Content-Length:");
        if (cl)
            clen = atol(cl + 15);
    }
    if (clen <= 0)
    {
        snprintf(err, errcap, "no length");
        goto out;
    }
    sUpdTotal = (uint32_t)hlen + (uint32_t)clen;
    {
        uint8_t buf[512];
        int stalls = 0;
        while (total < clen)
        {
            uint32_t want = (uint32_t)clen - total;
            int n;
            if (sRunAbort)
            {
                snprintf(err, errcap, "aborted");
                goto out;
            }
            if (want > sizeof(buf))
                want = sizeof(buf);
            n = recv(fd, buf, want, 0);
            if (n == 0)
            {
                snprintf(err, errcap, "eof");
                goto out;
            }
            if (n < 0)
            {
                if (++stalls >= 20)
                {
                    snprintf(err, errcap, "timeout");
                    goto out;
                }
                continue;
            }
            stalls = 0;
            if (upd_sink(buf, (uint32_t)n, err, errcap) != 0)
            {
                if (!err[0])
                    snprintf(err, errcap, "sink");
                rc = -2;
                goto out;
            }
            total += n;
            sUpdPos = (uint32_t)hlen + (uint32_t)total;
            vTaskDelay(1);
        }
    }
    rc = total;
out:
    close(fd);
    return rc;
}

/* скачать .skpak по полному URL при выключенном вебе (для DOWNLOAD);
 * результат: образ ESP в слоте + hex прибора в разделе, применение —
 * UPDATE_GATEWAY / UPDATE_METER */

/* результат скачивания из отдельной задачи */
static long sUpdResult;
static char sUpdErr[48];

/* запуск fn(arg) в отдельной задаче с большим стеком: у meter_task всего
 * 3.5К — не хватает для AES/SHA/Ed25519 + HTTP буферов.
 * Ожидание прерывается Ctrl+C: флаг sRunAbort проверяют циклы задачи
 * (приём, страницы urclock), после её выхода возвращаем false */
static void (*sRunFn)(void *);
static SemaphoreHandle_t sRunDoneSem;

bool espcons_abort_pending(void)
{
    return sRunAbort;
}

static void run_helper_task(void *arg)
{
    sRunFn(arg);
    xSemaphoreGive(sRunDoneSem);
    vTaskDelete(NULL);
}

/* стереть строку прогрессбара перед следующим сообщением */
static void progress_wipe(void)
{
    reply("\r                                                  \r");
}

static bool run_in_task(void (*fn)(void *), void *arg,
                        uint16_t stack, const char *name)
{
    uint8_t ch;
    uint8_t lastPct = 200; /* sentinel: прогресс ещё не печатали */

    if (!sRunDoneSem)
        sRunDoneSem = xSemaphoreCreateBinary();
    xSemaphoreTake(sRunDoneSem, 0);
    sRunAbort = false;
    sUpdTotal = 0;
    sRunFn = fn;
    if (xTaskCreate(run_helper_task, name, stack, arg, 3, NULL) != pdPASS)
    {
        reply("task create failed\r\n");
        return false;
    }
    while (xSemaphoreTake(sRunDoneSem, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        /* прогрессбар скачивания: \r[####----] 47% 512K/1047K;
         * шаг 5%: при терминалах, разворачивающих CR в CRLF (Tera Term),
         * строк меньше, а перерисовка дешевле */
        if (sUpdTotal > 0)
        {
            uint8_t pct = (uint8_t)(sUpdPos * 100 / sUpdTotal);
            uint8_t step = pct / 5;
            if (step != lastPct)
            {
                char pb[64];
                char bar[31];
                uint8_t fill = (uint8_t)((uint32_t)pct * 30 / 100);
                uint8_t i;
                lastPct = step;
                for (i = 0; i < 30; i++)
                    bar[i] = i < fill ? '#' : '-';
                bar[30] = 0;
                snprintf(pb, sizeof(pb), "\r[%s] %3u%% %uK/%uK",
                         bar, pct,
                         (unsigned)(sUpdPos >> 10),
                         (unsigned)(sUpdTotal >> 10));
                reply(pb);
            }
        }
        while (uart_read_bytes(EC_UART, &ch, 1, 0) == 1)
        {
            if (ch != 0x03)
                continue; /* прочий ввод во время операции гасим */
            if (lastPct != 200)
                progress_wipe();
            reply("^C aborting...\r\n");
            sRunAbort = true;
            /* задача сама выйдет по флагу; ждём её завершения */
            xSemaphoreTake(sRunDoneSem, portMAX_DELAY);
            sRunAbort = false;
            reply("aborted\r\n");
            return false;
        }
    }
    if (lastPct != 200)
        progress_wipe();
    return true;
}

/* скачивание + проверка подписи + закрытие OTA: целиком в задаче 8К */
static void upd_run(void *arg)
{
    char err[48];
    int rc;

    err[0] = 0;
    sUpdResult = upd_http_get((const char *)arg, err, sizeof(err));
    if (sUpdResult >= 0 && !sUpdHdrDone)
    {
        strlcpy(err, "file too short", sizeof(err));
        sUpdResult = -1;
    }
    if (sUpdResult >= 0)
    {
        rc = ota_sec_finish(&sUpdSec);
        if (rc != OTA_SEC_OK)
        {
            snprintf(err, sizeof(err), "signature: %s", ota_sec_str(rc));
            sUpdResult = -1;
        }
    }
    if (sUpdResult >= 0 && sUpdOtaOpen && esp_ota_end(sUpdOta) != ESP_OK)
    {
        strlcpy(err, "ota end fail", sizeof(err));
        sUpdResult = -1;
    }
    /* неудача (в т.ч. прерывание): закрыть частичные приёмники,
     * чтобы следующий DOWNLOAD начал с чистого состояния */
    if (sUpdResult < 0)
    {
        if (sUpdOtaOpen)
            esp_ota_end(sUpdOta);
        if (sUpdHexOpen)
            hex_store_begin(0);
    }
    strlcpy(sUpdErr, err, sizeof(sUpdErr));
}

/* прошивка прибора из сохранённого hex — та же последовательность,
 * что у кнопки "Обновить (прибор)" в вебе (web.c avr_flash_task);
 * urboot-сессия гасит WiFi на время записи */
static bool sAvrOk;
static urclock_result_t sAvrRes;

static void avr_run(void *arg)
{
    (void)arg;
    avr_flash_begin();
    sAvrOk = urclock_program_file(NULL, &sAvrRes) == ESP_OK;
    avr_flash_end();
}

/* скачать .skpak по полному URL при выключенном вебе (для DOWNLOAD) */
static void upd_download(const char *url)
{
    long got;

    while (*url == ' ')
        url++;
    if (!*url || !strstr(url, "://"))
    {
        reply("\r\nbad url\r\n");
        return;
    }

    reply("\r\nstopping web...\r\n");
    web_httpd_enable(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    memset(&sUpdSec, 0, sizeof(sUpdSec));
    sUpdHdrHave = 0;
    sUpdHdrDone = false;
    sUpdFed = 0;
    sUpdEspLen = 0;
    sUpdFedEsp = 0;
    sUpdLenHave = 0;
    sUpdOtaOpen = sUpdHexOpen = false;
    sUpdDest = 0;
    sUpdErr[0] = 0;

    reply("downloading...\r\n");
    /* скачивание, проверка подписи и закрытие OTA — в отдельной задаче
     * (8К стека); веб выключен — куча ~40К */
    if (!run_in_task(upd_run, (void *)url, 8192, "updl"))
        goto restart; /* отказ задачи или Ctrl+C — сообщение уже выше */
    got = sUpdResult;
    if (got < 0)
    {
        reply("\rfailed: ");
        reply(sUpdErr);
        reply("\r\n");
        goto restart;
    }
    {
        char b[96];
        snprintf(b, sizeof(b), "\rok: gw %u B + meter %u B\r\n",
                 (unsigned)sUpdFedEsp,
                 (unsigned)(sUpdFed - (sUpdDest == 3 ? 4 : 0) - sUpdFedEsp));
        reply(b);
        if (sUpdOtaOpen)
            web_ota_staged(sUpdFedEsp);
    }
    reply("web on — flash: UPDATE_GATEWAY / UPDATE_METER\r\n");

restart:
    reply("starting web... ");
    web_start();
    reply("done\r\nESP> ");
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
#include "nvs.h"

/* адрес сервера обновлений (NVS "gw"/"updurl"), как раньше в upd.c */
static char sUpdUrl[97]; /* 96 + null */

static const char *upd_url(void)
{
    return sUpdUrl;
}

static bool upd_set_url(const char *url)
{
    nvs_handle_t h;
    if (strlen(url) >= sizeof(sUpdUrl))
        return false;
    if (nvs_open("gw", NVS_READWRITE, &h) != ESP_OK)
        return false;
    strcpy(sUpdUrl, url);
    bool ok = nvs_set_str(h, "updurl", sUpdUrl) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void upd_url_init(void)
{
    nvs_handle_t h;
    size_t n = sizeof(sUpdUrl);
    sUpdUrl[0] = 0;
    if (nvs_open("gw", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_str(h, "updurl", sUpdUrl, &n) != ESP_OK)
            sUpdUrl[0] = 0;
        nvs_close(h);
    }
}

/* URL <адрес> — сохранить адрес сервера; без аргумента — показать */
static void cmd_url(const char *args)
{
    char b[128];
    while (*args == ' ')
        args++;
    if (!*args)
    {
        snprintf(b, sizeof(b), "\r\nserver: %s\r\n",
                 sUpdUrl[0] ? sUpdUrl : "(not set)");
        reply(b);
        return;
    }
    if (!upd_set_url(args))
    {
        reply("\r\nsave failed\r\n");
        return;
    }
    snprintf(b, sizeof(b), "\r\nsaved: %s\r\n", sUpdUrl);
    reply(b);
}

/* CHECK — тело: GET versions.txt; в отдельной задаче (буфер txt и lwIP
 * не влезают в стек meter_task) */
static void chk_run(void *arg)
{
    const char *url = arg;
    char txt[1536];
    const char *p;

    /* простой GET без ota_sec — просто текст */
    {
        /* переиспользуем upd_http_get но с простым приёмником */
        /* временно подменим контекст на текстовый буфер */
        /* TODO: вынести в общий HTTP-клиент */
        /* пока: используем упрощённый путь */
        char host[64], path[96], hdr[512], req[224];
        struct hostent *he;
        struct sockaddr_in sa;
        int fd = -1;
        size_t hlen = 0;
        long clen = -1, total = 0;
        uint16_t port = 80;
        const char *pp, *slash;
        char *colon;

        pp = url;
        if (!strncmp(pp, "http://", 7))
            pp += 7;
        slash = strchr(pp, '/');
        if (!slash || slash == pp)
        {
            reply("bad url\r\n");
            return;
        }
        memcpy(host, pp, slash - pp);
        host[slash - pp] = 0;
        strcpy(path, slash);
        colon = strchr(host, ':');
        if (colon)
        {
            port = (uint16_t)atoi(colon + 1);
            *colon = 0;
        }
        he = gethostbyname(host);
        if (!he || !he->h_addr_list[0])
        {
            reply("dns fail\r\n");
            return;
        }
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            reply("socket\r\n");
            return;
        }
        {
            struct timeval ct = {.tv_sec = 8, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof(ct));
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
            {
                reply("no connect\r\n");
                close(fd);
                return;
            }
        }
        {
            /* 1 с + счётчик: Ctrl+C замечается циклами ниже */
            struct timeval rt = {.tv_sec = 1, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
        }
        snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 path, host);
        if (send(fd, req, strlen(req), 0) <= 0)
        {
            reply("send\r\n");
            close(fd);
            return;
        }
        {
            int stalls = 0;
            for (;;)
            {
                int n;
                if (sRunAbort)
                {
                    close(fd);
                    return;
                }
                n = recv(fd, hdr + hlen, 1, 0);
                if (n == 0)
                {
                    reply("hdr break\r\n");
                    close(fd);
                    return;
                }
                if (n < 0)
                {
                    if (++stalls >= 15)
                    {
                        reply("hdr timeout\r\n");
                        close(fd);
                        return;
                    }
                    continue;
                }
                stalls = 0;
                hlen++;
                if (hlen >= sizeof(hdr))
                {
                    reply("hdr big\r\n");
                    close(fd);
                    return;
                }
                if (hlen >= 4 && !memcmp(hdr + hlen - 4, "\r\n\r\n", 4))
                    break;
            }
        }
        hdr[hlen] = 0;
        if (strncmp(hdr, "HTTP/1.", 7) || hdr[9] != '2')
        {
            reply("http error\r\n");
            close(fd);
            return;
        }
        {
            char *cl = strstr(hdr, "Content-Length:");
            if (cl)
                clen = atol(cl + 15);
        }
        if (clen <= 0 || clen >= (long)sizeof(txt))
        {
            reply("bad size\r\n");
            close(fd);
            return;
        }
        {
            int stalls = 0;
            while (total < clen)
            {
                int n;
                if (sRunAbort)
                {
                    close(fd);
                    return;
                }
                n = recv(fd, txt + total, (size_t)(clen - total), 0);
                if (n == 0)
                    break;
                if (n < 0)
                {
                    if (++stalls >= 15)
                        break;
                    continue;
                }
                stalls = 0;
                total += n;
            }
        }
        close(fd);
        txt[total] = 0;
        if (total < clen)
        {
            reply("incomplete\r\n");
            return;
        }
    }

    /* печать строк versions.txt */
    p = txt;
    while (*p)
    {
        const char *nl = strchr(p, '\n');
        int l = nl ? (int)(nl - p) : (int)strlen(p);
        if (l > 0)
        {
            if (p[l - 1] == '\r')
                l--;
            if (l > 0 && l < 100)
            {
                char b[104];
                memcpy(b, p, l);
                b[l] = 0;
                reply("  ");
                reply(b);
                reply("\r\n");
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
}

/* CHECK — прочитать versions.txt с сервера, показать список */
static void cmd_check(void)
{
    char url[176];

    if (!sUpdUrl[0])
    {
        reply("\r\nserver not set (URL <address>)\r\n");
        return;
    }
    snprintf(url, sizeof(url), "%s/versions.txt", sUpdUrl);
    reply("\r\nchecking ");
    reply(sUpdUrl);
    reply("...\r\n");
    run_in_task(chk_run, url, 4608, "chk");
}

/* DOWNLOAD — скачать бандл system_full.skpak с сохранённого адреса
 * сервера: образ шлюза в неактивный слот, hex прибора в раздел */
static void cmd_download(void)
{
    char url[176];

    if (!sUpdUrl[0])
    {
        reply("\r\nserver not set (URL <address>)\r\n");
        return;
    }
    snprintf(url, sizeof(url), "%s/system_full.skpak", sUpdUrl);
    reply("\r\nbundle update from ");
    reply(sUpdUrl);
    reply("\r\n");
    upd_download(url);
}

/* UPDATE_GATEWAY — прошить шлюз из отложенного DOWNLOAD образа
 * (последовательность кнопки "Прошить шлюз": слот + перезагрузка) */
static void cmd_upd_gw(void)
{
    if (!web_ota_ready())
    {
        reply("\r\nnothing staged (run DOWNLOAD)\r\n");
        return;
    }
    reply("\r\nswitching boot slot, rebooting...\r\n");
    if (esp_ota_set_boot_partition(
            esp_ota_get_next_update_partition(NULL)) != ESP_OK)
    {
        reply("boot switch failed\r\nESP> ");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* UPDATE_METER — прошить прибор из hex, сохранённого DOWNLOAD'ом
 * (последовательность "Обновить (прибор)": urboot, WiFi гасится) */
static void cmd_upd_meter(void)
{
    char b[96];

    if (!hex_store_stored())
    {
        reply("\r\nno meter hex (run DOWNLOAD)\r\n");
        return;
    }
    reply("\r\nflashing meter...\r\n");
    if (!run_in_task(avr_run, NULL, 6144, "avrfl"))
        return; /* отказ задачи или Ctrl+C — сообщение уже выше */
    snprintf(b, sizeof(b), "meter %s: written %d, verify fail %d\r\n",
             sAvrOk ? "ok" : "FAIL",
             sAvrRes.written, sAvrRes.verify_fail);
    reply(b);
}

/* ping — tcp-проверка достижимости сервера обновлений (DNS + connect);
 * без аргумента адрес берётся из сохранённого URL; ping host[:port] */
static char sPingRes[96];
static uint16_t sPingPort;

static void ping_run(void *arg)
{
    const char *host = arg;
    struct hostent *he;
    struct sockaddr_in sa;
    struct timeval ct = {.tv_sec = 5, .tv_usec = 0};
    int64_t t0;
    int fd;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
    {
        strlcpy(sPingRes, "dns fail", sizeof(sPingRes));
        return;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        strlcpy(sPingRes, "socket", sizeof(sPingRes));
        return;
    }
    if (sRunAbort)
    {
        strlcpy(sPingRes, "aborted", sizeof(sPingRes));
        close(fd);
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof(ct));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(sPingPort);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    t0 = esp_timer_get_time();
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        strlcpy(sPingRes, "connect fail", sizeof(sPingRes));
        close(fd);
        return;
    }
    snprintf(sPingRes, sizeof(sPingRes), "connect ok: %u ms (%s)",
             (unsigned)((esp_timer_get_time() - t0) / 1000),
             ip4addr_ntoa((const ip4_addr_t *)&sa.sin_addr));
    close(fd);
}

static void cmd_ping(const char *args)
{
    char host[80];
    const char *p = sUpdUrl;
    char *colon, *slash;
    uint16_t port = 0;
    char b[112];

    while (*args == ' ')
        args++;
    if (*args)
    {
        strlcpy(host, args, sizeof(host));
        colon = strchr(host, ':');
        if (colon)
        {
            port = (uint16_t)atoi(colon + 1);
            *colon = 0;
        }
    }
    else if (p[0])
    {
        if (!strncmp(p, "http://", 7))
            p += 7;
        strlcpy(host, p, sizeof(host));
        slash = strchr(host, '/');
        if (slash)
            *slash = 0;
        colon = strchr(host, ':');
        if (colon)
        {
            port = (uint16_t)atoi(colon + 1);
            *colon = 0;
        }
    }
    else
    {
        reply("\r\nusage: ping [host[:port]] (default: saved server)\r\n");
        return;
    }
    if (!port)
        port = 80;

    sPingPort = port;
    snprintf(b, sizeof(b), "\r\nping %s:%u...\r\n", host, (unsigned)port);
    reply(b);
    if (!run_in_task(ping_run, host, 3072, "ping"))
        return; /* отказ задачи или Ctrl+C — сообщение уже выше */
    {
        reply(sPingRes);
        reply("\r\n");
    }
}

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
    reply("\r\n  help           this help\r\n"
          "  info           fw/heap/mode/ip/nets\r\n"
          "  wifi           current connection (ssid/rssi/ip)\r\n"
          "  scan           wifi scan (top 10, cached for join)\r\n"
          "  nets           saved networks\r\n"
          "  join           interactive: ssid (number/name/TAB) then pass\r\n"
          "  forget <n>|all remove saved network\r\n"
          "  echo           toggle char echo\r\n"
          "  ap             forget all + reboot into portal\r\n"
          "  url <address>  set/show update server address\r\n"
          "  check          list available updates from server\r\n"
          "  download       fetch system_full bundle (stops web)\r\n"
          "  update_gateway flash gateway from staged image update\r\n"
          "  update_meter   flash meter from staged image update\r\n"
          "  ping           tcp ping saved server (or ping host[:port])\r\n"
          "  reboot         restart gateway\r\n"
          "  exit, quit     finish session, return console to the meter\r\n");
}

/* ---- TAB-автокомплит -------------------------------------------- */

static char line[64]; /* буфер строки (общий для poll и autocomplete) */
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
                if (!strncasecmp(line, "forget ", 7))
                    cmd_forget(line + 7);
                else if (!strncasecmp(line, "url ", 4))
                    cmd_url(line + 4);
                else if (!strcasecmp(line, "url"))
                    cmd_url("");
                else if (!strcasecmp(line, "check"))
                    cmd_check();
                else if (!strcasecmp(line, "download"))
                    cmd_download();
                else if (!strcasecmp(line, "update_gateway"))
                    cmd_upd_gw();
                else if (!strcasecmp(line, "update_meter"))
                    cmd_upd_meter();
                else if (!strncasecmp(line, "ping ", 5))
                    cmd_ping(line + 5);
                else if (!strcasecmp(line, "ping"))
                    cmd_ping("");
                else if (!strcasecmp(line, "help"))
                    cmd_help();
                else if (!strcasecmp(line, "info"))
                    cmd_info();
                else if (!strcasecmp(line, "wifi"))
                    cmd_wifi();
                else if (!strcasecmp(line, "scan"))
                    cmd_scan();
                else if (!strcasecmp(line, "nets"))
                    cmd_nets();
                else if (!strcasecmp(line, "join"))
                    join_start();
                else if (!strcasecmp(line, "echo"))
                {
                    sEcho = !sEcho;
                    reply(sEcho ? "\r\necho on\r\n" : "\r\necho off\r\n");
                }
                else if (!strcasecmp(line, "ap"))
                {
                    reply("\r\ndropping all networks, reboot to portal\r\n");
                    storage_net_delall();
                    storage_set_wifi("", "");
                    storage_save(storage_get());
                    esp_restart();
                }
                else if (!strcasecmp(line, "reboot"))
                {
                    reply("\r\nrebooting\r\n");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
                else if (!strcasecmp(line, "EXIT") || !strcasecmp(line, "QUIT"))
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
