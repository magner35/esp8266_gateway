/*
 * Web server on esp_http_server: the portal (AP mode), the monitoring
 * window and the settings dashboard. Pages are generated literals from
 * pages.h; the JSON API serves the meter cache under the ske02 mutex.
 */
#include "web.h"

#include <stdio.h>
#include <stdlib.h>   /* malloc: h_scan буферы */
#include <string.h>

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "tcpip_adapter.h"

#include "debug.h"
#include "pages.h"
#define PAGE_APP_GZ_LEN ((int)sizeof(PAGE_APP_GZ))
#include "protocol.h"
#include "ske02.h"
#include "tree_store.h"
#include "storage.h"
#include "wifi.h"

static httpd_handle_t sServer;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static TreeRec rec;   /* единая на web.c (httpd однопоточен) */

static void json_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            out[o++] = '\\';
            out[o++] = (char)c;
        }
        else if (c < 0x20)
        {
            out[o++] = ' ';
        }
        else
        {
            out[o++] = (char)c; /* UTF-8 passes through */
        }
    }
    out[o] = 0;
}

/* URL-decode a form field in place (plus '+' -> space) */
static void url_decode(char *s)
{
    char *w = s;
    while (*s)
    {
        if (*s == '+')
        {
            *w++ = ' ';
            s++;
        }
        else if (*s == '%' && s[1] && s[2])
        {
            int hi = s[1] <= '9' ? s[1] - '0' : (s[1] | 0x20) - 'a' + 10;
            int lo = s[2] <= '9' ? s[2] - '0' : (s[2] | 0x20) - 'a' + 10;
            *w++ = (char)(hi * 16 + lo);
            s += 3;
        }
        else
        {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/*
 * Extract a urlencoded form field into out (value runs to the next '&').
 * Copies instead of cutting the body in place: a NUL written at the '&'
 * truncates the whole request for every later strstr() call, which made
 * the pass field unfindable after reading ssid - the gateway stored an
 * EMPTY password and every association attempt failed.
 */
static bool field(char *body, const char *name, char *out, size_t cap)
{
    char pat[24];
    char *p;
    char *amp;
    size_t n;
    snprintf(pat, sizeof(pat), "%s=", name);
    p = strstr(body, pat);
    if (!p)
        return false;
    p += strlen(pat);
    amp = strchr(p, '&');
    n = amp ? (size_t)(amp - p) : strlen(p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    url_decode(out);
    return true;
}

/*
 * Страница уходит ЧАНКАМИ по 2 КБ с проверкой результата: если
 * браузер оборвал загрузку (refresh посреди 80 КБ), первый же
 * неудачный chunk прерывает отправку и handler вернёт ошибку —
 * httpd сразу закрывает сессию. Отправка одним куском оставляла
 * поток httpd толкать данные в мёртвый сокет (send : 0 / recv : 0
 * в логе), однопоточный сервер замирал.
 */
static esp_err_t send_page(httpd_req_t *req, const char *page)
{
    /* no-store: после перепрошивки браузер не показывает старую
     * закэшированную страницу (иначе её JS зовёт удалённые URI) */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = strlen(page), off = 0;
    while (off < len)
    {
        size_t k = len - off;
        if (k > 2048)
            k = 2048;
        if (httpd_resp_send_chunk(req, page + off, k) != ESP_OK)
            return ESP_FAIL;
        off += k;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/*
 * Gzip-вариант страницы (~5x меньше): телефон с энергосбережением
 * по B+G не успевает принять 80 КБ за send-таймаут — отправка
 * рвалась на середине. Отдаём PAGE_APP_GZ с Content-Encoding.
 */
static esp_err_t send_page_gz(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    size_t off = 0;
    while (off < (size_t)PAGE_APP_GZ_LEN)
    {
        size_t k = (size_t)PAGE_APP_GZ_LEN - off;
        if (k > 1024)
            k = 1024;
        if (httpd_resp_send_chunk(req, (const char *)(PAGE_APP_GZ + off), k) != ESP_OK)
            return ESP_FAIL;
        off += k;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* pages                                                               */

/*
 * Single-page app: every page URI serves the SAME composed app.html
 * (built by tools/pages.py from the split www/ modules); the router
 * inside picks the section by path/hash, no page reloads on nav.
 */
static esp_err_t h_app(httpd_req_t *req)
{
    esp_err_t r = send_page_gz(req);
    /* полный ли ушёл ответ: при зашумлённом канале обрыв на
     * середине виден только так (предупреждения httpd выключены) */
    DBG("web: page request: send %s\n", r == ESP_OK ? "ok" : "FAILED");
    return r;
}

/*
 * POST /api/forgetwifi - "забыть все сети": стираем сохранённые
 * креды и перезагружаемся. С пустым ssid wifi_manager_task сразу
 * поднимает точку доступа (режим портала).
 */
static esp_err_t h_api_forgetwifi(httpd_req_t *req)
{
    storage_set_wifi("", "");
    storage_save(storage_get());
    DBG("web: wifi credentials erased, rebooting to portal\n");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 9);
    /* дать ответу уйти, потом перезагрузка */
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK; /* not reached */
}

static esp_err_t h_scan(httpd_req_t *req)
{
    static uint8_t sScanBusy;
    /* буферы в КУЧЕ на время скана: в покое не занимают ничего,
     * на стеке httpd их держать нельзя (переполнение) */
    char *out = malloc(2048);
    wifi_ap_record_t *aps = malloc(16 * sizeof(wifi_ap_record_t));
    size_t off = 0;

    if (!out || !aps)
    {
        free(out); free(aps);
        httpd_resp_set_status(req, "503 Busy");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    uint16_t n = 16;
    esp_err_t err = ESP_FAIL;
    int i, attempt;

    /* один скан за раз: параллельный второй уронит httpd на 4+ сек */
    if (sScanBusy)
    {
        httpd_resp_set_status(req, "503 Busy");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    sScanBusy = 1;

    /*
     * Blocking scan (~2 s full channel sweep): the old async scheme with
     * hand-rolled 2.5 s timers lost results and never worked in STA
     * mode. Blocking here ties one httpd worker, acceptable for a
     * manual rescan. Retry: первый скан после старта портала может
     * вернуть STATE, пока wifi-таск устраивается.
     */
    for (attempt = 0; attempt < 2; attempt++)
    {
        wifi_scan_config_t cfg = {0};
        n = 16;
        err = esp_wifi_scan_start(&cfg, true);
        if (err == ESP_OK)
        {
            err = esp_wifi_scan_get_ap_records(&n, aps);
            if (err == ESP_OK)
                break;
        }
        DBG("web: scan attempt %d failed: %d\n", attempt, (int)err);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK)
        n = 0;
    sScanBusy = 0;
    DBG("web: scan done: %u networks\n", (unsigned)n);

    off += snprintf(out + off, 2048 - off, "{\"nets\":[");
    for (i = 0; i < n && off < 2048 - 128; i++)
    {
        char esc[64];
        json_escape((const char *)aps[i].ssid, esc, sizeof(esc));
        off += snprintf(out + off, 2048 - off,
                        "%s{\"s\":\"%s\",\"r\":%d,\"e\":%d}",
                        i ? "," : "", esc, aps[i].rssi,
                        aps[i].authmode != WIFI_AUTH_OPEN);
    }
    off += snprintf(out + off, 2048 - off, "],\"err\":%d}", (int)err);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, off);
    free(out);
    free(aps);
    return ESP_OK;
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[160];
    char head[512];
    char ssid[33];
    char pass[65];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    body[len] = 0;
    if (!field(body, "ssid", ssid, sizeof(ssid)) || !*ssid)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad ssid", 8);
        return ESP_FAIL;
    }
    if (!field(body, "pass", pass, sizeof(pass)))
        pass[0] = 0;

    storage_set_wifi(ssid, pass);
    {
        bool okSave = storage_save(storage_get());
        if (!okSave)
        {
            /* NVS full: стираем дерево (пересоберётся листингом
             * при загрузке) и пробуем сохранить креды ещё раз */
            tree_wipe();
            okSave = storage_save(storage_get());
        }
        DBG("web: creds for \"%s\" (pass %u chars) save %s\n", ssid,
            (unsigned)strlen(pass), okSave ? "OK" : "FAILED");
        if (!okSave)
        {
            /* без сохранённых кредов перезагрузка = снова портал;
             * честно сообщаем об ошибке, НЕ перезагружаем */
            httpd_resp_set_status(req, "500 Storage Error");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req,
                "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\">"
                "<body><h2>&#1054;&#1096;&#1080;&#1073;&#1082;&#1072; &#1089;&#1086;&#1093;&#1088;&#1072;&#1085;&#1077;&#1085;&#1080;&#1103;</h2>"
                "<p>NVS &#1085;&#1077; &#1089;&#1084;&#1086;&#1075; &#1079;&#1072;&#1087;&#1080;&#1089;&#1072;&#1090;&#1100; &#1085;&#1072;&#1089;&#1090;&#1088;&#1086;&#1081;&#1082;&#1080;.</p>"
                "</body></html>", 0);
            return ESP_FAIL;
        }
    }

    /* answer first, restart after the response drains */
    snprintf(head, sizeof(head),
             "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
             "<title>OK</title><meta http-equiv=\"refresh\" content=\"12;url=/\">"
             "</head><body><h2>&#1057;&#1086;&#1093;&#1088;&#1072;&#1085;&#1077;&#1085;&#1086;</h2>"
             "<p>&#1064;&#1083;&#1102;&#1079; &#1087;&#1077;&#1088;&#1077;&#1079;&#1072;&#1075;&#1088;&#1091;&#1078;&#1072;&#1077;&#1090;&#1089;&#1103;.</p>"
             "</body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, head, strlen(head));

    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK; /* not reached */
}

/* ------------------------------------------------------------------ */
/* JSON API                                                            */

static esp_err_t h_api_info(httpd_req_t *req)
{
    char out[512];
    char esc[40];
    tcpip_adapter_ip_info_t ip;
    const gw_settings_t *st = storage_get();
    bool portal = wifi_ap_ssid() != NULL;
    bool sta = wifi_sta_connected();
    int rssi = 0;

    if (portal)
        tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
    else
    {
        wifi_ap_record_t ap;
        tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            rssi = ap.rssi;
    }

    {
        xSemaphoreTake(ske02_lock(), portMAX_DELAY);
        json_escape(ske02_ctx()->version, esc, sizeof(esc));
        snprintf(out, sizeof(out),
                 "{\"fw\":\"%s\",\"count\":%u,\"link\":%d,\"ready\":%d,"
                 "\"ap\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                 "\"uptime\":%lu,\"heap\":%lu}",
                 esc, (unsigned)ske02_ctx()->count,
                 ske02_link_up() ? 1 : 0, ske02_ready() ? 1 : 0,
                 portal ? 1 : 0,
                 portal ? wifi_ap_ssid() : st->ssid,
                 ip4addr_ntoa(&ip.ip), rssi,
                 (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
                 (unsigned long)esp_get_free_heap_size());
        xSemaphoreGive(ske02_lock());
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    return ESP_OK;
}

static esp_err_t h_api_values(httpd_req_t *req)
{
    char out[640];
    size_t off = 0;
    const SkeValues *v;
    int link, ready;
    static SkeValues cached;     /* last snapshot, served without waiting */
    static TickType_t cachedAt;
    const gw_settings_t *st = storage_get();
    bool portal = wifi_ap_ssid() != NULL;

    /*
     * link/ready ride this 1 Hz endpoint so the UI badge refreshes with
     * the monitoring poll - no separate /api/info traffic.
     * SHORT mutex timeout (50 ms): the browser polls at 5 Hz, and a
     * blocking wait here exhausts all 7 httpd sockets when the meter
     * task is busy. Serve the last cached snapshot instead.
     */
    if (xSemaphoreTake(ske02_lock(), pdMS_TO_TICKS(50)) == pdTRUE)
    {
        link = ske02_link_up() ? 1 : 0;
        ready = ske02_ready() ? 1 : 0;
        v = ske02_values();
        if (v)
        {
            cached = *v;
            cachedAt = xTaskGetTickCount();
        }
        xSemaphoreGive(ske02_lock());
    }
    else
    {
        /* meter busy: serve the cached copy (link/ready from last time) */
        v = &cached;
        link = 1;
        ready = 1;
    }
    if (!cached.updated)
    {
        v = NULL;                  /* no data ever received */
        link = ske02_link_up() ? 1 : 0;
        ready = ske02_ready() ? 1 : 0;
    }
    if (!v)
    {
        snprintf(out, sizeof(out), "{\"ok\":0,\"link\":%d,\"ready\":%d}",
                 link, ready);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, strlen(out));
        return ESP_OK;
    }
    /*
     * ПАЧКА снимков: прибор опрашивается 5 Гц, кольцо хранит
     * последние ~1.6 с; ?since=N возвращает всё новее N, при
     * since=0 (загрузка страницы) — только свежий снимок.
     * Ответ собирается ЧАНКАМИ по одному снимку: ни кучи, ни
     * больших буферов — каждый чанк ~550 Б ложится в стек.
     */
    {
        uint32_t since = 0;
        const char *q = strchr(req->uri, '?');
        if (q && !strncmp(q, "?since=", 7))
            since = (uint32_t)strtoul(q + 7, NULL, 10);

        uint32_t ts[8];
        SkeValues vs[8];
        int n = ske02_valring_since(since, ts, vs, 8);
        if (n == 0)
        {
            ts[0] = v->updated;
            vs[0] = *v;
            n = 1;
        }
        if (since == 0 && n > 1)
            n = 1;   /* первое обращение: только свежий снимок */

        httpd_resp_set_type(req, "application/json");
        off = snprintf(out, sizeof(out),
                       "{\"ok\":1,\"age\":%lu,\"link\":%d,\"ready\":%d,"
                       "\"now\":%lu,\"cfg\":%u,\"ap\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"seq\":[",
                       (unsigned long)((xTaskGetTickCount() * portTICK_PERIOD_MS -
                                        v->updated) / 1000),
                       link, ready,
                       (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                       (unsigned)v->cfg_rev,
                       portal ? 1 : 0,
                       portal ? "192.168.4.1" : "",
                       portal ? wifi_ap_ssid() : st->ssid);
        httpd_resp_send_chunk(req, out, off);
        for (int i = 0; i < n; i++)
        {
            const SkeValues *sv = &vs[i];
            off = snprintf(out, sizeof(out),
                           "%s{\"t\":%lu,\"frequency\":%.2f,"
                           "\"rateMLPM\":%f,\"rate\":%f,"
                           "\"totalml_plus\":%f,\"totalml_minus\":%f,"
                           "\"gtotalml\":%f,\"gtotal\":%f,"
                           "\"total_plus\":%f,\"total_minus\":%f,"
                           "\"total\":%f,\"total_sum\":%f,"
                           "\"kf_value\":%f,\"batch\":%f,"
                           "\"pulses\":%lu,\"status\":%u,"
                           "\"setpoint\":%u,\"isr\":%u}",
                           i ? "," : "", (unsigned long)ts[i],
                           (double)sv->frequency,
                           (double)sv->rateMLPM, (double)sv->rate,
                           (double)sv->totalml_plus, (double)sv->totalml_minus,
                           (double)sv->gtotalml, (double)sv->gtotal,
                           (double)sv->total_plus, (double)sv->total_minus,
                           (double)sv->total, (double)sv->total_sum,
                           (double)sv->kf_value, (double)sv->batch,
                           (unsigned long)sv->pulses, sv->status,
                           sv->setpoint, sv->isr);
            if (httpd_resp_send_chunk(req, out, off) != ESP_OK)
                return ESP_FAIL;
        }
        httpd_resp_send_chunk(req, "]}", 2);
        httpd_resp_send_chunk(req, NULL, 0);   /* конец chunked */
    }
    return ESP_OK;
}

/* значение параметра из записи дерева - как proto_value_text прежде */
static void rec_value_text(const TreeRec *r, char *buf, size_t cap)
{
    if (cap)
        buf[0] = 0;
    if (r->p.type == SKT_CMD)
        return;
    if (!r->p.present)
    {
        snprintf(buf, cap, "-");
        return;
    }
    if (r->p.masked)
    {
        snprintf(buf, cap, "******");
        return;
    }
    if (r->p.type == SKT_STRING)
    {
        snprintf(buf, cap, "%s", r->opts[0] ? r->opts[0] : "");
        return;
    }
    if ((r->p.type == SKT_ENUM || r->p.type == SKT_BOOL) && r->p.optCnt)
    {
        uint32_t v = r->p.value;
        snprintf(buf, cap, "%s", r->opts[v < r->p.optCnt ? v : 0]);
        return;
    }
    proto_raw_to_display(r->p.type, r->p.value, buf, cap);
}

static esp_err_t h_api_params(httpd_req_t *req)
{
    /*
     * Тяжёлые буферы - В СТАТИКУ: локально ~2.7КБ + vsnprintf(%f)
     * ~1.5КБ впритык на 8К стека httpd - ответ рвался посередине и
     * сервер зависал. httpd однопоточен - статика безопасна.
     */
    static char chunk[1024];
    static char esc[192];
    static char val[64];
    /* rec — единая static на весь web.c (httpd однопоточна):
     * три локальных static TreeRec ели 6.9КБ статики */
    static char secs[SKE_MAX_SECTIONS][SKE_NAME_LEN];
    size_t off;
    uint16_t id;
    bool first;
    uint8_t secNum = 0;
    uint16_t count;

    httpd_resp_set_type(req, "application/json");

    /* секции: имена записей дерева, в порядке первого появления */
    count = tree_count();
    static char menus[SKE_MAX_MENUS * SKE_MENU_LEN];
    tree_menus_load(menus, sizeof(menus));
    {
        for (id = 0; id < count && secNum < SKE_MAX_SECTIONS; id++)
        {
            uint8_t f;
            if (!tree_get(id, &rec) || !rec.section)
                continue;
            const char *sn = tree_menu_name(menus, rec.section);
            for (f = 0; f < secNum; f++)
                if (!strcmp(secs[f], sn))
                    break;
            if (f == secNum && sn[0])
            {
                strncpy(secs[secNum], sn, SKE_NAME_LEN - 1);
                secs[secNum][SKE_NAME_LEN - 1] = 0;
                secNum++;
            }
        }
    }

    off = snprintf(chunk, sizeof(chunk), "{\"count\":%u,\"sections\":[",
                   (unsigned)count);
    for (uint8_t f = 0; f < secNum; f++)
    {
        json_escape(secs[f], esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, "%s\"%s\"",
                        f ? "," : "", esc);
    }
    off += snprintf(chunk + off, sizeof(chunk) - off, "],\"params\":[");
    httpd_resp_send_chunk(req, chunk, off);

    first = true;
    for (id = 0; id < count; id++)
    {
        uint8_t sz;
        uint8_t f;
        int8_t sidx = -1;
        if (!tree_get(id, &rec))
            continue;
        if (!rec.p.present && !rec.p.name[0])
            continue;
        sz = proto_type_size(rec.p.type);

        off = snprintf(chunk, sizeof(chunk), "%s{\"i\":%u,\"t\":%u,\"r\":%lu",
                       first ? "" : ",", (unsigned)id, rec.p.type,
                       (unsigned long)rec.p.value);
        json_escape(rec.p.name, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"n\":\"%s\"", esc);
        const char *sn = tree_menu_name(menus, rec.section);
        for (f = 0; f < secNum; f++)
            if (!strcmp(secs[f], sn))
            {
                sidx = (int8_t)f;
                break;
            }
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"s\":%d", sidx);
        json_escape(tree_menu_name(menus, rec.group), esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"g\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"gl\":%u",
                        rec.p.groupLvl);
        json_escape(tree_menu_name(menus, rec.tab2), esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"tb\":\"%s\"", esc);
        rec_value_text(&rec, val, sizeof(val));
        json_escape(val, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"v\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off,
                        ",\"w\":%d,\"cx\":%d,\"m\":%d",
                        (rec.p.present && !rec.p.readOnly && sz > 0) ? 1 : 0,
                        rec.p.type == SKT_CMD ? 1 : 0, rec.p.masked ? 1 : 0);
        proto_bound_text(&rec.p, false, val, sizeof(val));
        json_escape(val, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"lo\":\"%s\"", esc);
        proto_bound_text(&rec.p, true, val, sizeof(val));
        json_escape(val, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"hi\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"o\":[");
        for (uint8_t k = 0; k < rec.p.optCnt; k++)
        {
            json_escape(rec.opts[k] ? rec.opts[k] : "", esc, sizeof(esc));
            off += snprintf(chunk + off, sizeof(chunk) - off, "%s\"%s\"",
                            k ? "," : "", esc);
        }
        off += snprintf(chunk + off, sizeof(chunk) - off, "],\"a\":0}");
        first = false;
        httpd_resp_send_chunk(req, chunk, off);
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0); /* end chunked */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* action endpoints (POST)                                             */

/*
 * Unlock with the menu password. Returns true when OK to proceed with
 * the actual command (no password given, or unlock succeeded). On
 * failure fills *out with the error (incl. lockout wait_s) so the
 * caller can report it to the browser instead of a confusing
 * "ERR access" from the subsequent 's'/'x'.
 */
static bool do_unlock(httpd_req_t *req, char *body, ske_req_t *out)
{
    char pw[10];
    ske_req_t r;
    (void)req;
    memset(&r, 0, sizeof(r));
    if (!field(body, "pw", pw, sizeof(pw)) || !*pw)
        return true;              /* no password: try the command bare */
    r.cmd = SKEQ_UNLOCK;
    strncpy(r.text, pw, sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    /*
     * ske02_request returns false for BOTH transport timeout and
     * non-BS_OK results - the return value alone can't tell them
     * apart. Use a sentinel: if r.result stays SKE_ERR_TRANSPORT
     * after the call, nothing was copied back (timeout).
     */
    r.result = SKE_ERR_TRANSPORT;
    ske02_request(&r, 15000);   /* return value not used here */
    if (r.result == SKE_ERR_TRANSPORT)
    {
        if (out) { out->result = SKE_ERR_TRANSPORT; strcpy(out->err, "прибор не отвечает"); }
        return false;
    }
    if (r.result != BS_OK)
    {
        if (out)
            *out = r;             /* BS_ACCESS / BS_WAIT + wait_s */
        return false;
    }
    return true;
}

static esp_err_t reply_ok_err(httpd_req_t *req, const ske_req_t *r,
                              const char *value)
{
    char out[192];
    char esc[96];
    httpd_resp_set_type(req, "application/json");
    if (r->result == BS_OK)
    {
        if (value)
        {
            json_escape(value, esc, sizeof(esc));
            snprintf(out, sizeof(out), "{\"ok\":true,\"value\":\"%s\"}", esc);
        }
        else
        {
            snprintf(out, sizeof(out), "{\"ok\":true}");
        }
        httpd_resp_send(req, out, strlen(out));
        return ESP_OK;
    }
    json_escape(r->err[0] ? r->err : "error", esc, sizeof(esc));
    if (r->wait_s)
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\",\"wait\":%u}",
                 esc, (unsigned)r->wait_s);
    else
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", esc);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, out, strlen(out));
    return ESP_OK;
}

static esp_err_t h_api_set(httpd_req_t *req)
{
    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    ske_req_t r;
    uint32_t raw = 0;
    char packed[24];
    char err[64];
    char val[64];
    char ids[12];
    char vtext[48];

    if (len <= 0)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    body[len] = 0;
    if (!field(body, "id", ids, sizeof(ids)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad id", 6);
        return ESP_FAIL;
    }
    if (!field(body, "v", vtext, sizeof(vtext)))
        vtext[0] = 0;

    {
        ske_req_t unlock_result;
        memset(&unlock_result, 0, sizeof(unlock_result));
        if (!do_unlock(req, body, &unlock_result))
            return reply_ok_err(req, &unlock_result, NULL);
    }

    /* UI text -> packed console value, then queue the 's' command */
    {
        bool ok = tree_get((uint16_t)atoi(ids), &rec) &&
                  proto_input_to_raw_rec(&rec.p, rec.opts, vtext,
                                         &raw, err, sizeof(err));
        if (ok)
            proto_raw_to_cmd(rec.p.type, raw, packed, sizeof(packed));
        else
            packed[0] = 0;
    }
    if (!packed[0])
    {
        ske_req_t bad;
        bad.result = BS_BAD_TYPE;
        snprintf(bad.err, sizeof(bad.err), "%s", err);
        return reply_ok_err(req, &bad, NULL);
    }

    r.cmd = SKEQ_SET;
    r.id = (uint16_t)atoi(ids);
    strncpy(r.text, packed, sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    ske02_request(&r, 15000);

    {
        val[0] = 0;
        if (tree_get(r.id, &rec))
            rec_value_text(&rec, val, sizeof(val));
    }
    return reply_ok_err(req, &r, val);
}

static esp_err_t h_api_run(httpd_req_t *req)
{
    char body[128];
    char ids[12];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    ske_req_t r;
    if (len <= 0)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    body[len] = 0;
    if (!field(body, "id", ids, sizeof(ids)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad id", 6);
        return ESP_FAIL;
    }
    {
        ske_req_t unlock_result;
        memset(&unlock_result, 0, sizeof(unlock_result));
        if (!do_unlock(req, body, &unlock_result))
            return reply_ok_err(req, &unlock_result, NULL);
    }
    r.cmd = SKEQ_RUN;
    r.id = (uint16_t)atoi(ids);
    ske02_request(&r, 15000);
    return reply_ok_err(req, &r, NULL);
}

static esp_err_t h_api_refresh(httpd_req_t *req)
{
    ske_req_t r;
    char out[64];
    r.cmd = SKEQ_REFRESH;
    ske02_request(&r, 20000);
    httpd_resp_set_type(req, "application/json");
    snprintf(out, sizeof(out), "{\"ok\":%d,\"ready\":%d}",
             r.result == BS_OK ? 1 : 0, ske02_ready() ? 1 : 0);
    httpd_resp_send(req, out, strlen(out));
    return ESP_OK;
}

static esp_err_t h_api_rescan(httpd_req_t *req)
{
    ske_req_t r;
    r.cmd = SKEQ_RESCAN;
    ske02_request(&r, 30000);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 10);
    return ESP_OK;
}

static esp_err_t h_api_reboot(httpd_req_t *req)
{
    ske_req_t r;
    r.cmd = SKEQ_REBOOT;
    ske02_request(&r, 15000);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 10);
    return ESP_OK;
}

static esp_err_t h_api_widgets_get(httpd_req_t *req)
{
    char *buf = malloc(4200);
    httpd_resp_set_type(req, "application/json");
    if (!buf)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"ok\":false}", 12);
        return ESP_FAIL;
    }
    if (widgets_load(buf, 4200))
        httpd_resp_send(req, buf, strlen(buf));
    else
        httpd_resp_send(req, "{\"rev\":0,\"cfg\":null}", 20);
    free(buf);
    return ESP_OK;
}

static esp_err_t h_api_widgets_set(httpd_req_t *req)
{
    char *buf = malloc(4200);
    int len;
    httpd_resp_set_type(req, "application/json");
    if (!buf)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"ok\":false}", 12);
        return ESP_FAIL;
    }
    len = httpd_req_recv(req, buf, 4199);
    if (len <= 2 || buf[0] != '{')
    {
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false}", 12);
        return ESP_FAIL;
    }
    buf[len] = 0;
    if (!widgets_store(buf, (size_t)len))
    {
        /* NVS full: освобождаем место деревом (пересоберётся) */
        tree_wipe();
        if (!widgets_store(buf, (size_t)len))
        {
            free(buf);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "{\"ok\":false}", 12);
            return ESP_FAIL;
        }
    }
    free(buf);
    httpd_resp_send(req, "{\"ok\":true}", 11);
    return ESP_OK;
}

/*
 * This SDK's esp_http_server matches URIs by exact strcmp (no wildcard
 * support), so instead of a catch-all the real OS probe URLs are
 * registered one by one; in the portal mode they redirect (which every
 * captive detector treats as "sign in required").
 */
static esp_err_t h_captive(httpd_req_t *req)
{
    if (!wifi_ap_ssid())
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const char *const sCaptiveProbes[] = {
    "/generate_204",  "/gen_204",  "/hotspot-detect.html",
    "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
    "/fwlink", "/redirect", "/check_network_status.txt", NULL,
};

/* ------------------------------------------------------------------ */

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;   /* 26 needed: 17 api/pages + 9 captive */
    /* ВАЖНО: не меньше 8 КБ — h_scan держит на стеке ~2.7 КБ буферов
     * + кадры; на 6 КБ стек переполнялся и затаптывал кучу (куча
     * "уменьшалась", страница портала со сканом ломалась) */
    cfg.stack_size = 8192;
    /*
     * Socket exhaustion fix: the browser polls /api/values at 5 Hz and
     * opens a new connection for POST /api/set on "Применить". Without
     * LRU purge the 7-socket limit is hit (accept errno=23 EMFILE) and
     * the request is dropped. Purge closes the oldest idle connection.
     */
    cfg.lru_purge_enable = true;
    /*
     * Таймауты сокета (в СЕКУНДАХ, поля ..._wait_timeout — не
     * ..._wait_ms, как в IDF v4): зависший send/recv не должен
     * замораживать ОДНОПОТОЧНЫЙ httpd. Дефолт 5с — заметная
     * пауза всего сервера на каждого отвалившегося клиента.
     */
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 4;

    if (httpd_start(&sServer, &cfg) != ESP_OK)
    {
        DBG("web: httpd_start failed\n");
        return;
    }

    httpd_uri_t u;

    u = (httpd_uri_t){.uri = "/", .method = HTTP_GET, .handler = h_app};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/wifi", .method = HTTP_GET, .handler = h_app};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/settings", .method = HTTP_GET, .handler = h_app};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/panel", .method = HTTP_GET, .handler = h_app};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/widgets", .method = HTTP_GET, .handler = h_app};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/scan", .method = HTTP_GET, .handler = h_scan};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/save", .method = HTTP_POST, .handler = h_save};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/info", .method = HTTP_GET,
                      .handler = h_api_info};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/values", .method = HTTP_GET,
                      .handler = h_api_values};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/params", .method = HTTP_GET,
                      .handler = h_api_params};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/set", .method = HTTP_POST,
                      .handler = h_api_set};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/run", .method = HTTP_POST,
                      .handler = h_api_run};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/refresh", .method = HTTP_POST,
                      .handler = h_api_refresh};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/rescan", .method = HTTP_POST,
                      .handler = h_api_rescan};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/reboot", .method = HTTP_POST,
                      .handler = h_api_reboot};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/forgetwifi", .method = HTTP_POST,
                      .handler = h_api_forgetwifi};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/widgets", .method = HTTP_GET,
                      .handler = h_api_widgets_get};
    httpd_register_uri_handler(sServer, &u);
    u = (httpd_uri_t){.uri = "/api/widgets", .method = HTTP_POST,
                      .handler = h_api_widgets_set};
    httpd_register_uri_handler(sServer, &u);
    {
        int i;
        for (i = 0; sCaptiveProbes[i]; i++)
        {
            u = (httpd_uri_t){.uri = sCaptiveProbes[i], .method = HTTP_GET,
                              .handler = h_captive};
            httpd_register_uri_handler(sServer, &u);
        }
    }
    DBG("web: http server started\n");
}
