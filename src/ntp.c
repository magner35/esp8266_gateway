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
#include "esp_http_client.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "debug.h"
#include "ntp.h"
#include "storage.h"

static ntp_cfg_t sCfg;
static bool sSynced = false;

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

/* HTTPS-провайдер (ipapi.co) через esp_http_client: буфер в стеке
 * задачи; сертификат не проверяется (SKIP_SERVER_CERT_VERIFY) */
static bool tz_http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_err_t er;
    int rd, code;
    size_t len = 0;
    bool ok = false;

    if (!h)
    {
        DBG("ntp: tz client init fail\n");
        return false;
    }
    er = esp_http_client_open(h, 0);
    if (er == ESP_OK)
    {
        esp_http_client_fetch_headers(h);
        code = esp_http_client_get_status_code(h);
        while (len < cap - 1)
        {
            rd = esp_http_client_read(h, buf + len, (int)(cap - 1 - len));
            if (rd <= 0)
                break;
            len += (size_t)rd;
        }
        buf[len] = 0;
        DBG("ntp: tz %s http %d, %u bytes\n", url, code, (unsigned)len);
        ok = (code == 200 && len > 0);
    }
    else
        DBG("ntp: tz open fail %s, err %d\n", url, (int)er);
    esp_http_client_cleanup(h);
    return ok;
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

/* ipapi.co: "utc_offset":"+0300" (пробел после ключа, БЕЗ двоеточия;
 * worldtimeapi-формат "+03:00" тоже принимаем) */
static bool tz_try_ipapi_co(char *buf, int8_t *out)
{
    char *o;
    int hh = 0, mm = 0;
    char sign = '+';
    int8_t h;
    if (!tz_http_get("https://ipapi.co/json/", buf, 512))
        return false;
    o = strstr(buf, "\"utc_offset\"");
    if (!o)
        return false;
    o = strchr(o + 12, '"'); /* открывающая кавычка значения */
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
        if (tz_try_ipapi_co(buf, &h))
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
    sTzBusy = true;
    if (xTaskCreate(tz_fetch_task, "tzq", 4096, NULL, 3, NULL) != pdPASS)
        sTzBusy = false;
}

void ntp_apply(void)
{
    sntp_stop();
    sSynced = false;
    if (!sCfg.on || !sCfg.s1[0])
        return;
    /* второй сервер не обязателен: один тоже валиден */
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, sCfg.s1);
    if (sCfg.s2[0])
        sntp_setservername(1, sCfg.s2);
    else
        sntp_setservername(1, NULL);
    sntp_init();
    DBG("ntp: poll %s%s%s\n", sCfg.s1, sCfg.s2[0] ? " + " : "",
        sCfg.s2[0] ? sCfg.s2 : "");
    ntp_tz_auto_fetch();
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
