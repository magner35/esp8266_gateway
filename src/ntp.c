/*
 * SNTP-клиент шлюза. Конфиг в NVS (storage): два сервера (второй -
 * подстраховка первого), смещение UTC (вручную или "Авто" - по
 * публичному IP). Синхронизированное время в дальнейшем уйдёт в
 * прибор для коррекции его таймера DS1307.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/apps/sntp.h"
#include "lwip/sockets.h"
#include "esp_system.h" /* esp_get_free_heap_size */
#include "lwip/netdb.h"

#include "debug.h"
#include "wifi.h" /* wifi_ap_ssid: проверка режима портала */
#include "ntp.h"
#include "storage.h"
#include "nvs.h"

static ntp_cfg_t sCfg;
static bool sSynced = false;

/* окно повторной синхронизации 12 ч (как у push времени в прибор):
 * мягкий ребут шлюза НЕ должен снова дёргать NTP-сервер. Метка
 * последней УСПЕШНОЙ синхронизации - в NVS "gw"/"ntplast"; после
 * потери питания часы шлюза сбрасываются и синхронизация обязательна */
#define NTP_RESYNC_S (12UL * 3600UL)
static bool sRunning;     /* SNTP-опрос активен */
static bool sWindowSkip;  /* в окне 12 ч: время верное, опрос не нужен */
static TickType_t sWaitMeter; /* ждём время прибора (DS1307) с этого тика */

static uint32_t ntp_last_load(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("gw", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u32(h, "ntplast", &v);
        nvs_close(h);
    }
    return v;
}

static void ntp_last_save(uint32_t t)
{
    nvs_handle_t h;
    if (nvs_open("gw", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u32(h, "ntplast", t);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* определённый по IP часовой пояс (часы); sAutoTzOk = уже определён */
static int8_t sAutoTz = 0;
static bool sAutoTzOk = false;
static bool sTzBusy = false;
static TickType_t sTzRetryAfter = 0; /* бэкофф после неудачного цикла */

void ntp_tz_auto_fetch(void); /* вперёд: зовётся из on_sync */

static void on_sync(struct timeval *tv)
{
    (void)tv;
    sSynced = true;
    DBG("ntp: time synced\n");
    /* метка для окна 12 ч (переживает мягкий ребут шлюза) */
    {
        time_t now = time(NULL);
        if (now > 1600000000UL)
            ntp_last_save((uint32_t)now);
    }
    /* сеть точно работает - хорошее время для IP-геолокации */
    ntp_tz_auto_fetch();
}

void ntp_init(void)
{
    storage_get_ntp(&sCfg);
    sntp_set_time_sync_notification_cb(on_sync);
}

/* ---- авто-часовой пояс ------------------------------------------- */

/* HTTP-провайдер (ip-api.com, порт 80) - СЫРЫМ TCP-сокетом: без
 * esp-tls/TLS-кучи, которая на повторных попытках просаживала кучу
 * шлюза почти в ноль. Ответ копится циклом recv до закрытия. */
static bool tz_http_plain(char *buf, size_t cap, const char *host,
                          const char *path)
{
    struct hostent *he;
    struct sockaddr_in sa;
    char req[96];
    size_t len = 0;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    int fd, got;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
    {
        DBG("ntp: tz dns fail %s\n", host);
        return false;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        DBG("ntp: tz socket fail\n");
        return false;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        close(fd);
        DBG("ntp: tz connect fail %s\n", host);
        return false;
    }
    if (send(fd, req, strlen(req), 0) <= 0)
    {
        close(fd);
        DBG("ntp: tz send fail %s\n", host);
        return false;
    }
    while (len < cap - 1)
    {
        got = recv(fd, buf + len, (int)(cap - 1 - len), 0);
        if (got <= 0)
            break;
        len += (size_t)got;
    }
    close(fd);
    buf[len] = 0;
    DBG("ntp: tz %s -> %u bytes: %.40s\n", host, (unsigned)len, buf);
    return len > 0 && strstr(buf, "200");
}

/* ip-api.com: {"offset":10800} секунды */
static bool tz_try_ipapi_com(char *buf, int8_t *out)
{
    char *o;
    long sec;
    int8_t h;
    if (!tz_http_plain(buf, 512, "ip-api.com", "/json/?fields=offset"))
        return false;
    o = strstr(buf, "\"offset\":");
    if (!o)
        return false;
    sec = strtol(o + 9, NULL, 10);
    h = (int8_t)((sec + 1800) / 3600);
    if (h < -12 || h > 14)
        return false;
    *out = h;
    return true;
}

/* ipwho.is (HTTP): "utc":"+03:00" в объекте timezone */
static bool tz_try_ipwho(char *buf, int8_t *out)
{
    char *o;
    int hh = 0, mm = 0;
    char sign = '+';
    int8_t h;
    if (!tz_http_plain(buf, 512, "ipwho.is", "/"))
        return false;
    o = strstr(buf, "\"utc\"");
    if (!o)
        return false;
    o = strchr(o + 5, '"');
    if (!o)
        return false;
    if (sscanf(o + 1, "%c%2d:%2d", &sign, &hh, &mm) != 3 &&
        sscanf(o + 1, "%c%2d%2d", &sign, &hh, &mm) != 3)
        return false;
    h = (int8_t)(hh + (mm >= 30 ? 1 : 0));
    if (sign == '-')
        h = (int8_t)-h;
    if (h < -12 || h > 14)
        return false;
    *out = h;
    return true;
}

static void tz_fetch_task(void *arg)
{
    /* два провайдера, до 3 проходов: сеть в момент запуска может
     * ещё не маршрутизировать (GOT_IP сразу после DHCP) */
    char buf[512];
    int attempt;
    int8_t h = 0;
    (void)arg;

    for (attempt = 0; attempt < 3 && !h; attempt++)
    {
        if (tz_try_ipapi_com(buf, &h))
            break;
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (tz_try_ipwho(buf, &h))
            break;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (h)
    {
        sAutoTz = h;
        sAutoTzOk = true;
        DBG("ntp: auto tz = UTC%+d\n", (int)h);
    }
    else
    {
        /* БЭКОФФ: неудача бьёт по куче (TLS-попытки), повтор не раньше
         * чем через час; "Сохранить" сбрасывает ожидание */
        sTzRetryAfter = xTaskGetTickCount() + pdMS_TO_TICKS(3600UL * 1000);
        DBG("ntp: tz lookup failed, retry in 1h\n");
    }
    sTzBusy = false;
    vTaskDelete(NULL);
}

void ntp_tz_auto_fetch(void)
{
    if (!sCfg.on || !sCfg.autoTz || sAutoTzOk || sTzBusy)
        return;
    if ((int32_t)(xTaskGetTickCount() - sTzRetryAfter) < 0)
        return; /* бэкофф после неудачи */
    /* авто-TZ (HTTP к ip-api.com) - 4К стек задачи + DNS + TCP:
     * при дефиците кучи пропускаем, timezone можно задать вручную */
    if (esp_get_free_heap_size() < 12000)
    {
        DBG("ntp: tz auto skip (heap %u low)\n",
            (unsigned)esp_get_free_heap_size());
        sTzRetryAfter = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        return;
    }
    sTzBusy = true;
    if (xTaskCreate(tz_fetch_task, "tzq", 4096, NULL, 3, NULL) != pdPASS)
        sTzBusy = false;
}

/* общий запуск SNTP-опроса (без проверок) */
static void ntp_start_poll(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, sCfg.s1);
    if (sCfg.s2[0])
        sntp_setservername(1, sCfg.s2);
    else
        sntp_setservername(1, NULL);
    sntp_init();
    sRunning = true;
    sWindowSkip = false;
    DBG("ntp: poll %s%s%s\n", sCfg.s1, sCfg.s2[0] ? " + " : "",
        sCfg.s2[0] ? sCfg.s2 : "");
}

void ntp_apply(void)
{
    sntp_stop();
    sRunning = false;
    sWindowSkip = false;
    sSynced = false;
    if (!sCfg.on || !sCfg.s1[0])
        return;
    /* AP-режим (портал): НИКАКИХ сетевых сервисов времени -
     * даже если STA получил IP, портал жрёт кучу и это не штатный режим */
    if (wifi_ap_ssid())
    {
        DBG("ntp: skip in portal mode\n");
        return;
    }
    /* часы шлюза сброшены (потеря питания): сначала время ПРИБОРА -
     * строка "clock" из его 'i' вызовет ntp_apply снова. Не дождались
     * за 5 минут (прибора нет/мёртв RTC) - синхронизируемся по NTP */
    {
        uint32_t last = ntp_last_load();
        uint32_t now = (uint32_t)time(NULL);
        if (now < 1600000000UL)
        {
            if (sWaitMeter == 0)
            {
                sWaitMeter = xTaskGetTickCount();
                DBG("ntp: clock lost, waiting for meter rtc\n");
                return;
            }
            if ((xTaskGetTickCount() - sWaitMeter) <
                pdMS_TO_TICKS(5UL * 60 * 1000))
                return; /* прибор ещё не ответил */
            DBG("ntp: meter rtc silent 5 min, fallback to NTP\n");
        }
        else
        {
            sWaitMeter = 0;
            /* окно 12 ч: синхронизировались недавно и часы живы
             * (мягкий ребут или время прибора) - сервер не дёргаем */
            if (last > 1600000000UL && now > last &&
                now - last < NTP_RESYNC_S)
            {
                if (!sWindowSkip) /* лог один раз, а не каждые 30 с */
                    DBG("ntp: synced %lus ago, in 12h window - skip poll\n",
                        (unsigned long)(now - last));
                sWindowSkip = true;
                sSynced = true; /* время уже верное */
                return;
            }
        }
    }
    ntp_start_poll();
    ntp_tz_auto_fetch();
}

/* ручная синхронизация (кнопка в вебе): окно 12 ч игнорируем */
void ntp_force_sync(void)
{
    if (!sCfg.on || !sCfg.s1[0] || wifi_ap_ssid())
        return;
    sntp_stop();
    sSynced = false;
    ntp_start_poll();
    DBG("ntp: manual sync requested\n");
}

/* периодический вызов (stat_task, 30 с): переоценить - окно 12 ч
 * истекло или часы всё ещё ждут (прибор молчит - 5-мин fallback) */
void ntp_poll_window(void)
{
    if (sRunning || sSynced)
        return;
    if (sWindowSkip || (uint32_t)time(NULL) < 1600000000UL)
        ntp_apply();
}

const ntp_cfg_t *ntp_cfg(void)
{
    return &sCfg;
}

void ntp_set(const ntp_cfg_t *c)
{
    bool autoChanged = (c->autoTz != sCfg.autoTz);
    sCfg = *c;
    storage_set_ntp(&sCfg);
    /* переключение "Авто" взад-вперёд - способ перезапросить пояс */
    if (autoChanged && c->autoTz)
        sAutoTzOk = false;
    sTzRetryAfter = 0; /* "Сохранить" сбрасывает бэкофф */
    ntp_apply();
}

bool ntp_synced(void)
{
    return sSynced;
}

/* действующий пояс: авто (по IP) или ручной */
int8_t ntp_tz_effective(void)
{
    return (sCfg.autoTz && sAutoTzOk) ? sAutoTz : sCfg.tz;
}

bool ntp_tz_resolved(void)
{
    return !sCfg.autoTz || sAutoTzOk;
}

/* UTC-секунды с эпохи 1970; имеет смысл только после синхронизации */
uint32_t ntp_epoch_utc(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}
