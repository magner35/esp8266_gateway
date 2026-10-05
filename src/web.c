/*
 * Web server on esp_http_server: the portal (AP mode), the monitoring
 * window and the settings dashboard. Pages are generated literals from
 * pages.h; the JSON API serves the meter cache under the ske02 mutex.
 */
#include "web.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "tcpip_adapter.h"

#include "debug.h"
#include "pages.h"
#include "protocol.h"
#include "ske02.h"
#include "storage.h"
#include "wifi.h"

static httpd_handle_t sServer;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

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

static void send_page(httpd_req_t *req, const char *page)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, strlen(page));
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
    send_page(req, PAGE_APP);
    return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    static TickType_t scanStart; /* when the current scan was kicked off */
    char out[2048];
    size_t off = 0;
    wifi_ap_record_t aps[16];
    uint16_t n = 16;
    int i;

    if (strchr(req->uri, '?'))
    { /* ?restart=1 - drop the previous results, rescan */
        wifi_scan_async();
        scanStart = xTaskGetTickCount();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"running\":true}", 16);
        return ESP_OK;
    }
    /* a full channel sweep takes ~2 s: report "running" while it is in
     * flight, the portal page polls us every 1.5 s */
    if (xTaskGetTickCount() - scanStart < pdMS_TO_TICKS(2500))
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"running\":true}", 16);
        return ESP_OK;
    }
    if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK)
        n = 0;
    off += snprintf(out + off, sizeof(out) - off, "{\"nets\":[");
    for (i = 0; i < n && off < sizeof(out) - 128; i++)
    {
        char esc[64];
        json_escape((const char *)aps[i].ssid, esc, sizeof(esc));
        off += snprintf(out + off, sizeof(out) - off,
                        "%s{\"s\":\"%s\",\"r\":%d,\"e\":%d}",
                        i ? "," : "", esc, aps[i].rssi,
                        aps[i].authmode != WIFI_AUTH_OPEN);
    }
    off += snprintf(out + off, sizeof(out) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, off);
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
    storage_save(storage_get());
    DBG("web: creds for \"%s\" saved (pass %u chars)\n", ssid,
        (unsigned)strlen(pass));

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
                 portal ? 1 : 0, sta ? st->ssid : "",
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

    /*
     * link/ready ride this 1 Hz endpoint so the UI badge refreshes with
     * the monitoring poll - no separate /api/info traffic.
     */
    xSemaphoreTake(ske02_lock(), portMAX_DELAY);
    link = ske02_link_up() ? 1 : 0;
    ready = ske02_ready() ? 1 : 0;
    v = ske02_values();
    if (!v)
    {
        xSemaphoreGive(ske02_lock());
        snprintf(out, sizeof(out), "{\"ok\":0,\"link\":%d,\"ready\":%d}",
                 link, ready);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, strlen(out));
        return ESP_OK;
    }
    off += snprintf(out + off, sizeof(out) - off,
                    "{\"ok\":1,\"age\":%lu,\"link\":%d,\"ready\":%d,"
                    "\"frequency\":%.2f,\"rate_raw\":%f,\"rate_fast\":%f,"
                    "\"rateMLPM\":%f,\"rate\":%f,"
                    "\"total_plus\":%f,\"total_minus\":%f,"
                    "\"total\":%f,\"total_sum\":%f,"
                    "\"totalml_plus\":%f,\"totalml_minus\":%f,"
                    "\"gtotal\":%f,\"gtotalml\":%f,"
                    "\"kf_value\":%f,\"batch\":%f,"
                    "\"pulses_packet\":%lu,\"pulses\":%lu,"
                    "\"status\":%u,\"setpoint\":%u,\"isr\":%u}",
                    (unsigned long)((xTaskGetTickCount() * portTICK_PERIOD_MS -
                                     v->updated) / 1000),
                    link, ready,
                    (double)v->frequency, (double)v->rate_raw,
                    (double)v->rate_fast, (double)v->rateMLPM,
                    (double)v->rate, (double)v->total_plus,
                    (double)v->total_minus, (double)v->total,
                    (double)v->total_sum, (double)v->totalml_plus,
                    (double)v->totalml_minus, (double)v->gtotal,
                    (double)v->gtotalml, (double)v->kf_value,
                    (double)v->batch, (unsigned long)v->pulses_packet,
                    (unsigned long)v->pulses, v->status, v->setpoint, v->isr);
    xSemaphoreGive(ske02_lock());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, off);
    return ESP_OK;
}

static esp_err_t h_api_params(httpd_req_t *req)
{
    char chunk[1024];
    char esc[128];
    char val[64];
    size_t off;
    uint16_t id;
    bool first;

    httpd_resp_set_type(req, "application/json");

    /*
     * Lock discipline: the meter mutex is held ONLY around plain memory
     * access - an httpd send under the lock can block on TCP backpressure
     * forever, stalling the meter task and killing the console polling.
     */
    {
        size_t off = 0;
        xSemaphoreTake(ske02_lock(), portMAX_DELAY);
        {
            ProtoCtx *ctx = ske02_ctx();
            uint8_t s;
            off += snprintf(chunk + off, sizeof(chunk) - off,
                            "{\"count\":%u,\"sections\":[",
                            (unsigned)ctx->count);
            for (s = 0; s < ctx->sectionNum; s++)
            {
                json_escape(proto_section_name(ctx, s), esc, sizeof(esc));
                off += snprintf(chunk + off, sizeof(chunk) - off,
                                "%s\"%s\"", s ? "," : "", esc);
            }
            off += snprintf(chunk + off, sizeof(chunk) - off,
                            "],\"params\":[");
        }
        xSemaphoreGive(ske02_lock());
        httpd_resp_send_chunk(req, chunk, off);
    }

    first = true;
    for (id = 0; id < ske02_ctx()->count; id++)
    {
        SkeParam *p;
        uint8_t sz;
        uint32_t age;
        xSemaphoreTake(ske02_lock(), portMAX_DELAY);
        p = proto_param(ske02_ctx(), id);
        if (!p || (!p->present && !p->name[0]))
        {
            xSemaphoreGive(ske02_lock());
            continue;
        }
        sz = proto_type_size(p->type);
        age = p->updated
                  ? (xTaskGetTickCount() * portTICK_PERIOD_MS - p->updated) /
                        1000
                  : 0xFFFF;
        off = snprintf(chunk, sizeof(chunk), "%s{\"i\":%u,\"t\":%u,\"r\":%lu",
                       first ? "" : ",", (unsigned)id, p->type,
                       (unsigned long)p->value);
        json_escape(p->name, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"n\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"s\":%d",
                        (int)proto_section_index_of(ske02_ctx(), p->section));
        json_escape(proto_group_name(ske02_ctx(), p->group), esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"g\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"gl\":%u",
                        p->groupLvl);
        json_escape(p->tab2 ? proto_group_name(ske02_ctx(), p->tab2) : "", esc,
                    sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"tb\":\"%s\"", esc);
        proto_value_text(ske02_ctx(), id, val, sizeof(val));
        json_escape(val, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"v\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off,
                        ",\"w\":%d,\"cx\":%d,\"m\":%d",
                        (p->present && !p->readOnly && sz > 0) ? 1 : 0,
                        p->type == SKT_CMD ? 1 : 0, p->masked ? 1 : 0);
        proto_bound_text(p, false, val, sizeof(val));
        json_escape(val, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"lo\":\"%s\"", esc);
        proto_bound_text(p, true, val, sizeof(val));
        json_escape(val, esc, sizeof(esc));
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"hi\":\"%s\"", esc);
        off += snprintf(chunk + off, sizeof(chunk) - off, ",\"o\":[");
        for (uint8_t k = 0; k < p->optCnt; k++)
        {
            const char *o = proto_opt_text(ske02_ctx(), p, k);
            json_escape(o ? o : "", esc, sizeof(esc));
            off += snprintf(chunk + off, sizeof(chunk) - off, "%s\"%s\"",
                            k ? "," : "", esc);
        }
        off += snprintf(chunk + off, sizeof(chunk) - off, "],\"a\":%lu}",
                        (unsigned long)age);
        xSemaphoreGive(ske02_lock());
        first = false;
        httpd_resp_send_chunk(req, chunk, off);
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0); /* end chunked */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* action endpoints (POST)                                             */

static bool do_unlock(httpd_req_t *req, char *body)
{
    char pw[10];
    ske_req_t r;
    (void)req;
    if (!field(body, "pw", pw, sizeof(pw)) || !*pw)
        return true;
    r.cmd = SKEQ_UNLOCK;
    strncpy(r.text, pw, sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    return ske02_request(&r, 15000);
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

    if (!do_unlock(req, body))
    {
        ske_req_t bad = {.result = BS_ACCESS, .err = "неверный пароль меню"};
        return reply_ok_err(req, &bad, NULL);
    }

    /* UI text -> packed console value, then queue the 's' command */
    xSemaphoreTake(ske02_lock(), portMAX_DELAY);
    {
        bool ok = proto_input_to_raw(ske02_ctx(), (uint16_t)atoi(ids), vtext,
                                     &raw, err, sizeof(err));
        SkeParam *p = proto_param(ske02_ctx(), (uint16_t)atoi(ids));
        if (ok && p)
            proto_raw_to_cmd(p->type, raw, packed, sizeof(packed));
        else
            packed[0] = 0;
    }
    xSemaphoreGive(ske02_lock());
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

    xSemaphoreTake(ske02_lock(), portMAX_DELAY);
    proto_value_text(ske02_ctx(), r.id, val, sizeof(val));
    xSemaphoreGive(ske02_lock());
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
    if (!do_unlock(req, body))
    {
        ske_req_t bad = {.result = BS_ACCESS, .err = "неверный пароль меню"};
        return reply_ok_err(req, &bad, NULL);
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

/* the widget layout: {"rev":N,"cfg":[...]} stored in NVS verbatim.
 * The 4 KB buffers are heap-only: keeping them static would eat 8 KB
 * of the ESP8266 DRAM for two rarely used endpoints. */
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
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"ok\":false}", 12);
        return ESP_FAIL;
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
    httpd_resp_set_hdr(req, "Location", "http://10.0.0.1/");
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
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 8192;


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
