/*
 * Обновления с сервера производителя, см. upd.h. HTTP-клиент на сырых
 * сокетах (как ntp.c): DNS -> connect с таймаутом через select ->
 * GET HTTP/1.0 -> поток тела через колбэк. ОЗУ: всё на стеке задачи
 * обновлений (~1.7КБ буферов), образ целиком в памяти не бывает:
 * чанки идут в тот же конвейер ota_sec, что и загрузка из браузера.
 */
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_ota_ops.h"
#include "nvs.h"

#include "debug.h"
#include "hex_store.h"
#include "ota_sec.h"
#include "upd.h"
#include "web.h"

#define UPD_HOST_MAX  64
#define UPD_PATH_MAX  96
#define UPD_HDR_MAX   512
#define UPD_READ_MAX  1024
#define UPD_URL_MAX   96

static char sUrl[UPD_URL_MAX + 1];
static upd_state_t sState;

static const char kDemoJson[] =
    "{\"demo\":true,\"items\":["
    "{\"type\":\"gw\",\"ver\":\"1.4.2-demo\",\"date\":\"2026-10-28\","
    "\"file\":\"gw-demo.skpak\",\"size\":753384,"
    "\"note\":\"ДЕМО: задай адрес сервера\"},"
    "{\"type\":\"meter\",\"ver\":\"2.7.1-demo\",\"date\":\"2026-10-28\","
    "\"file\":\"ske02-demo.skpak\",\"size\":275415,"
    "\"note\":\"ДЕМО: задай адрес сервера\"}]}";

/*----------------------------------------------------------------------------*/

void upd_init(void)
{
    nvs_handle h;
    size_t n = sizeof(sUrl);
    sUrl[0] = 0;
    if (nvs_open("gw", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_str(h, "updurl", sUrl, &n) != ESP_OK)
            sUrl[0] = 0;
        nvs_close(h);
    }
}

const char *upd_url(void)
{
    return sUrl;
}

bool upd_set_url(const char *url)
{
    nvs_handle h;
    if (strlen(url) >= sizeof(sUrl))
        return false;
    if (nvs_open("gw", NVS_READWRITE, &h) != ESP_OK)
        return false;
    strcpy(sUrl, url);
    bool ok = nvs_set_str(h, "updurl", sUrl) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/*----------------------------------------------------------------------------*/
/* HTTP-примитив: GET url, тело -> sink (возврат 0 = продолжать).
 * Возвращает content_length, или -1 с текстом в err. */

static int http_fetch(const char *url, uint32_t max_len,
                      int (*sink)(void *ctx, uint8_t *d, uint32_t n),
                      void *ctx, char *err, size_t errcap)
{
    char host[UPD_HOST_MAX];
    char path[UPD_PATH_MAX];
    char hdr[UPD_HDR_MAX];
    char req[224];
    struct hostent *he;
    struct sockaddr_in sa;
    struct timeval tv;
    int fd = -1;
    const char *p, *slash;
    size_t hlen = 0;
    long clen = -1;
    uint32_t total = 0;
    int rc = -1;

    p = url;
    if (!strncmp(p, "http://", 7))
        p += 7;
    slash = strchr(p, '/');
    if (!slash || slash == p || strlen(slash) > sizeof(path) - 1 ||
        (size_t)(slash - p) >= sizeof(host))
    {
        snprintf(err, errcap, "битый адрес");
        return -1;
    }
    memcpy(host, p, slash - p);
    host[slash - p] = 0;
    strcpy(path, slash);

    /* порт после ':' (192.168.0.72:8080), дефолт 80 */
    {
        int port = 80;
        char *colon = strchr(host, ':');
        if (colon)
        {
            port = atoi(colon + 1);
            *colon = 0; /* отрезать порт от имени хоста */
            if (port <= 0 || port > 65535)
                port = 80;
        }
        sa.sin_port = htons((uint16_t)port);
    }

    he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
    {
        snprintf(err, errcap, "DNS: хост не найден");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    /* порт уже вычислен выше, sockaddr заполнен до gethostbyname */
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        snprintf(err, errcap, "socket");
        return -1;
    }
    /* неблокирующий connect + select: живой TCP-стек соединяется
     * быстро, а мёртвый адрес не должен вешать задачу на SYN-ретраи */
    int nb = 1;
    lwip_ioctl(fd, FIONBIO, &nb);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
        errno != EINPROGRESS)
    {
        snprintf(err, errcap, "connect");
        goto out;
    }
    {
        fd_set w;
        struct timeval to = {.tv_sec = 6, .tv_usec = 0};
        FD_ZERO(&w);
        FD_SET(fd, &w);
        if (select(fd + 1, 0, &w, 0, &to) <= 0)
        {
            snprintf(err, errcap, "сервер не отвечает");
            goto out;
        }
        int soe = 0;
        socklen_t sl = sizeof(soe);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soe, &sl) < 0 || soe)
        {
            snprintf(err, errcap, "connect: %d", soe);
            goto out;
        }
    }
    nb = 0;
    lwip_ioctl(fd, FIONBIO, &nb);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    if (send(fd, req, strlen(req), 0) <= 0)
    {
        snprintf(err, errcap, "send");
        goto out;
    }

    /* заголовки: копим до \r\n\r\n */
    for (;;)
    {
        int n = recv(fd, hdr + hlen, 1, 0);
        if (n <= 0)
        {
            snprintf(err, errcap, "обрыв ответа");
            goto out;
        }
        hlen++;
        if (hlen >= sizeof(hdr))
        {
            snprintf(err, errcap, "заголовок велик");
            goto out;
        }
        if (hlen >= 4 && !memcmp(hdr + hlen - 4, "\r\n\r\n", 4))
            break;
    }
    hdr[hlen] = 0;
    if (strncmp(hdr, "HTTP/1.", 7) || hdr[9] != '2')
    {
        snprintf(err, errcap, "HTTP %c%c%c",
                 hdr[9], hdr[10] ? hdr[10] : '?', hdr[11] ? hdr[11] : '?');
        goto out;
    }
    {
        char *cl = strstr(hdr, "Content-Length:");
        if (cl)
            clen = atol(cl + 15);
        if (clen >= 0 && (uint32_t)clen > max_len)
        {
            snprintf(err, errcap, "файл больше лимита");
            goto out;
        }
    }

    /* тело */
    for (;;)
    {
        uint8_t buf[UPD_READ_MAX];
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0)
            break; /* EOF */
        if (n < 0)
        {
            snprintf(err, errcap, "обрыв передачи");
            goto out;
        }
        if (sink && sink(ctx, buf, (uint32_t)n) != 0)
            goto out; /* причина уже в err */
        total += (uint32_t)n;
        if (clen >= 0 && total >= (uint32_t)clen)
            break;
    }
    if (clen >= 0 && total != (uint32_t)clen)
    {
        snprintf(err, errcap, "файл неполный");
        goto out;
    }
    rc = (int)total;

out:
    close(fd);
    return rc;
}

/*----------------------------------------------------------------------------*/
/* Проверка обновлений: versions.txt -> JSON */

typedef struct
{
    char *buf;
    size_t cap;
    size_t len;
    int rc;
    char *err;
    size_t errcap;
} txt_ctx_t;

static int txt_sink(void *vctx, uint8_t *d, uint32_t n)
{
    txt_ctx_t *c = vctx;
    if (c->len + n + 1 > c->cap)
    {
        snprintf(c->err, c->errcap, "versions.txt велик");
        c->rc = -1;
        return -1;
    }
    memcpy(c->buf + c->len, d, n);
    c->len += n;
    c->buf[c->len] = 0;
    return 0;
}

bool upd_check(char *json, size_t cap)
{
    char url[176];
    char err[48];
    static char txt[1536]; /* одна проверка в момент - общий буфер */
    txt_ctx_t ctx = {.buf = txt, .cap = sizeof(txt) - 1,
                     .err = err, .errcap = sizeof(err)};

    if (!sUrl[0])
    {
        snprintf(json, cap, "%s", kDemoJson);
        return true;
    }
    snprintf(url, sizeof(url), "%s/versions.txt", sUrl);
    txt[0] = 0;
    if (http_fetch(url, sizeof(txt) - 1, txt_sink, &ctx,
                   err, sizeof(err)) < 0)
    {
        snprintf(json, cap, "{\"demo\":false,\"error\":\"%s\"}", err);
        return false;
    }
    /* строки "type ver date file size note..." -> JSON-массив */
    size_t o = (size_t)snprintf(json, cap, "{\"demo\":false,\"items\":[");
    char *line = txt;
    int first = 1;
    while (line && *line && o + 128 < cap)
    {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char t[8], ver[24], date[16], file[48];
        long size = 0;
        /* note = пятое поле без пробелов, обрезаем \r */
        int used = 0;
        if (sscanf(line, "%7s %23s %15s %47s %ld %n",
                   t, ver, date, file, &size, &used) >= 5 && used > 0)
        {
            char *note = line + used;
            char *cr = strchr(note, '\r');
            if (cr)
                *cr = 0;
            if (strchr(note, '"') == NULL) /* кавычек в note не ждём */
                o += (size_t)snprintf(json + o, cap - o,
                                      "%s{\"type\":\"%s\",\"ver\":\"%s\","
                                      "\"date\":\"%s\",\"file\":\"%s\","
                                      "\"size\":%ld,\"note\":\"%s\"}",
                                      first ? "" : ",", t, ver, date, file,
                                      size, note);
            first = 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    o += (size_t)snprintf(json + o, cap - o, "]}");
    return true;
}

/*----------------------------------------------------------------------------*/
/* Скачивание пакета: тот же конвейер ota_sec, что и у загрузки из браузера */

typedef struct
{
    ota_sec_t sec;
    bool is_gw;
    bool hdr_done;
    uint32_t hdr_have;
    uint8_t hdr[OTA_SEC_HDR];
    esp_ota_handle_t ota;
    uint32_t fed;
    int rc;
    char *err;
    size_t errcap;
} dl_ctx_t;

static int dl_sink(void *vctx, uint8_t *d, uint32_t n)
{
    dl_ctx_t *c = vctx;

    while (n)
    {
        if (!c->hdr_done)
        {
            uint32_t need = OTA_SEC_HDR - c->hdr_have;
            uint32_t take = n < need ? n : need;
            memcpy(c->hdr + c->hdr_have, d, take);
            c->hdr_have += take;
            d += take;
            n -= take;
            if (c->hdr_have < OTA_SEC_HDR)
                return 0;
            int max = c->is_gw ? 1024 * 1024 : 0xFFFFFFF;
            int rc = ota_sec_begin(&c->sec, c->hdr, (uint32_t)max);
            if (rc == OTA_SEC_OK && c->is_gw &&
                c->sec.type != OTA_SEC_ESP)
                rc = OTA_SEC_TYPE;
            if (rc == OTA_SEC_OK && !c->is_gw &&
                c->sec.type != OTA_SEC_AVR)
                rc = OTA_SEC_TYPE;
            if (rc == OTA_SEC_OK && !c->is_gw &&
                !hex_store_begin(c->sec.plain_len))
                rc = OTA_SEC_LEN;
            if (rc == OTA_SEC_OK && c->is_gw &&
                esp_ota_begin(esp_ota_get_next_update_partition(NULL),
                              c->sec.plain_len, &c->ota) != ESP_OK)
                rc = OTA_SEC_LEN;
            if (rc != OTA_SEC_OK)
            {
                snprintf(c->err, c->errcap, "%s", ota_sec_str(rc));
                c->rc = rc;
                return -1;
            }
            c->hdr_done = true;
            continue;
        }
        uint32_t take = n;
        int rc = ota_sec_feed(&c->sec, d, take);
        if (rc != OTA_SEC_OK)
        {
            snprintf(c->err, c->errcap, "%s", ota_sec_str(rc));
            c->rc = rc;
            return -1;
        }
        if (c->is_gw)
        {
            if (esp_ota_write(c->ota, d, take) != ESP_OK)
            {
                snprintf(c->err, c->errcap, "ota write");
                c->rc = -100;
                return -1;
            }
        }
        else if (!hex_store_chunk((const char *)d, take))
        {
            snprintf(c->err, c->errcap, "запись раздела");
            c->rc = -100;
            return -1;
        }
        c->fed += take;
        sState.bytes = c->fed;
        d += take;
        n = 0;
    }
    return 0;
}

static void upd_task(void *arg)
{
    char url[176];
    char err[48];
    dl_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.is_gw = sState.rc; /* стартовый тип прокинули через rc */
    c.err = err;
    c.errcap = sizeof(err);
    err[0] = 0;

    snprintf(url, sizeof(url), "%s/%s", sUrl, sState.file);
    int got = http_fetch(url, 2 * 1024 * 1024, dl_sink, &c,
                         err, sizeof(err));
    if (got < 0)
    {
        sState.ok = false;
        snprintf(sState.err, sizeof(sState.err), "%s", err);
    }
    else if (!c.hdr_done)
    {
        sState.ok = false;
        snprintf(sState.err, sizeof(sState.err), "пакет меньше заголовка");
    }
    else
    {
        int rc = ota_sec_finish(&c.sec);
        if (rc != OTA_SEC_OK)
        {
            sState.ok = false;
            snprintf(sState.err, sizeof(sState.err), "%s", ota_sec_str(rc));
            if (c.is_gw)
                esp_ota_end(c.ota);
            if (!c.is_gw)
                hex_store_begin(0);
        }
        else if (c.is_gw && esp_ota_end(c.ota) != ESP_OK)
        {
            sState.ok = false;
            snprintf(sState.err, sizeof(sState.err), "ota end");
        }
        else
        {
            sState.ok = true;
            sState.bytes = c.fed;
            if (c.is_gw)
                web_ota_staged(c.fed); /* кнопка "Прошить шлюз" */
        }
    }
    DBG("upd: %s %s: %s (%u Б)\n", c.is_gw ? "gw" : "meter", sState.file,
        sState.ok ? "OK" : sState.err, (unsigned)sState.bytes);
    sState.running = false;
    vTaskDelete(NULL);
}

const upd_state_t *upd_state(void)
{
    return &sState;
}

bool upd_download_start(const char *file, const char *type)
{
    if (sState.running || !sUrl[0])
        return false;
    if (!file[0] || strlen(file) >= sizeof(sState.file))
        return false;
    /* имя файла без путей и ".." */
    for (const char *p = file; *p; p++)
    {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
              ch == '-'))
            return false;
    }
    bool is_gw = !strcmp(type, "gw");
    if (!is_gw && strcmp(type, "meter"))
        return false;

    memset(&sState, 0, sizeof(sState));
    sState.running = true;
    strcpy(sState.file, file);
    sState.rc = is_gw; /* прокидываем тип в задачу */
    if (xTaskCreate(upd_task, "upd", 4096, NULL, 3, NULL) != pdPASS)
    {
        sState.running = false;
        return false;
    }
    return true;
}
