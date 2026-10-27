#include "storage.h"

#include <string.h>
#include <stdio.h>  /* sprintf для net_key */

#include "nvs_flash.h"
#include "debug.h"

#define SETTINGS_MAGIC  0x534BU /* "SK" */
#define NVS_NAMESPACE   "gw"
#define NVS_KEY         "settings"

static gw_settings_t sSettings = {
    .magic = SETTINGS_MAGIC,
    .poll100ms = 1,
    .rfu = 0,
    .ssid = {0},
    .pass = {0},
};

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    storage_load(&sSettings);
    storage_nets_init();
}

/* ------------------------------------------------------------------ */
/* многосетевое Wi-Fi: NVS ключи net0..net4, каждый = wifi_net_t blob.
 * migration: существующая единичная сеть из gw_settings копируется
 * в net0 при первом старте (когда нет ещё ни одной netN).          */

static void net_key(char *buf, uint8_t idx)
{
    sprintf(buf, "net%u", (unsigned)idx);
}

void storage_nets_init(void)
{
    /* если сетей ещё нет, а единичная есть - мигрируем */
    if (storage_net_count() == 0 && sSettings.ssid[0])
        storage_net_add(sSettings.ssid, sSettings.pass);
}

uint8_t storage_net_count(void)
{
    nvs_handle_t h;
    uint8_t n = 0;
    char key[8];
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return 0;
    for (uint8_t i = 0; i < WIFI_NETS_MAX; i++)
    {
        size_t len = 0;
        net_key(key, i);
        if (nvs_get_blob(h, key, NULL, &len) == ESP_OK && len == sizeof(wifi_net_t))
            n++;
        else
            break; /* сетей дальше нет: плотная упаковка от 0 */
    }
    nvs_close(h);
    return n;
}

bool storage_net_get(uint8_t idx, wifi_net_t *out)
{
    nvs_handle_t h;
    char key[8];
    size_t len = sizeof(wifi_net_t);
    if (idx >= WIFI_NETS_MAX)
        return false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    net_key(key, idx);
    bool ok = (nvs_get_blob(h, key, out, &len) == ESP_OK && len == sizeof(wifi_net_t));
    nvs_close(h);
    return ok;
}

bool storage_net_add(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    char key[8];
    uint8_t n = storage_net_count();
    wifi_net_t net;
    if (n >= WIFI_NETS_MAX)
        return false;
    strncpy(net.ssid, ssid, sizeof(net.ssid) - 1);
    net.ssid[sizeof(net.ssid) - 1] = 0;
    strncpy(net.pass, pass, sizeof(net.pass) - 1);
    net.pass[sizeof(net.pass) - 1] = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    net_key(key, n);
    bool ok = (nvs_set_blob(h, key, &net, sizeof(net)) == ESP_OK &&
               nvs_commit(h) == ESP_OK);
    nvs_close(h);
    DBG("storage: net%u = \"%s\" %s\n", n, ssid, ok ? "ok" : "FAIL");
    return ok;
}

/* удаление: сдвигаем хвост к началу (плотная упаковка) */
bool storage_net_del(uint8_t idx)
{
    nvs_handle_t h;
    char key[8], nkey[8];
    uint8_t n = storage_net_count();
    if (idx >= n)
        return false;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    for (uint8_t i = idx; i < n - 1; i++)
    {
        wifi_net_t net;
        size_t len = sizeof(net);
        net_key(nkey, i + 1);
        if (nvs_get_blob(h, nkey, &net, &len) == ESP_OK)
        {
            net_key(key, i);
            nvs_set_blob(h, key, &net, sizeof(net));
        }
    }
    net_key(key, n - 1);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    DBG("storage: net%u deleted, %u left\n", idx, n - 1);
    return true;
}

void storage_net_delall(void)
{
    nvs_handle_t h;
    char key[8];
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    for (uint8_t i = 0; i < WIFI_NETS_MAX; i++)
    {
        net_key(key, i);
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

int8_t storage_net_find(const char *ssid)
{
    wifi_net_t net;
    uint8_t n = storage_net_count();
    for (uint8_t i = 0; i < n; i++)
        if (storage_net_get(i, &net) && strcmp(net.ssid, ssid) == 0)
            return (int8_t)i;
    return -1;
}

bool storage_load(gw_settings_t *s)
{
    nvs_handle_t h;
    size_t len = sizeof(*s);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = (nvs_get_blob(h, NVS_KEY, s, &len) == ESP_OK &&
               len == sizeof(*s) && s->magic == SETTINGS_MAGIC);
    nvs_close(h);
    if (!ok)
    {
        s->magic = SETTINGS_MAGIC;
        s->poll100ms = 1;
        s->rfu = 0;
        s->ssid[0] = 0;
        s->pass[0] = 0;
    }
    return ok;
}

bool storage_save(const gw_settings_t *s)
{
    nvs_handle_t h;
    bool ok;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    ok = (nvs_set_blob(h, NVS_KEY, s, sizeof(*s)) == ESP_OK &&
          nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

const gw_settings_t *storage_get(void)
{
    return &sSettings;
}

uint32_t storage_timepush_get(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u32(h, "tpush", &v);
        nvs_close(h);
    }
    return v;
}

void storage_timepush_set(uint32_t epoch)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u32(h, "tpush", epoch);
        nvs_commit(h);
        nvs_close(h);
    }
}

void storage_set_wifi(const char *ssid, const char *pass)
{
    strncpy(sSettings.ssid, ssid, sizeof(sSettings.ssid) - 1);
    sSettings.ssid[sizeof(sSettings.ssid) - 1] = 0;
    strncpy(sSettings.pass, pass, sizeof(sSettings.pass) - 1);
    sSettings.pass[sizeof(sSettings.pass) - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* widget layout: one JSON blob {"rev":N,"cfg":[...]} in its own key.
 * The content is opaque to the firmware - the page owns the format,
 * the gateway only stores and serves it verbatim.                   */

#define WKEY "widgets"

bool widgets_load(char *buf, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap - 1;
    bool ok;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    ok = (nvs_get_blob(h, WKEY, buf, &len) == ESP_OK && len < cap);
    nvs_close(h);
    if (ok)
        buf[len] = 0;
    return ok;
}

bool widgets_store(const char *json, size_t len)
{
    nvs_handle_t h;
    bool ok;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    ok = (nvs_set_blob(h, WKEY, json, len) == ESP_OK &&
          nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

/* ---- NTP-настройки ---------------------------------------------- */

#define NTP_MAGIC 0x4E55 /* "NT" v2: +autoTz */

void storage_get_ntp(ntp_cfg_t *c)
{
    nvs_handle_t h;
    size_t len = sizeof(*c);
    ntp_cfg_t def = {
        .magic = NTP_MAGIC, .on = 0, .autoTz = 0, .tz = 3,
        .s1 = "pool.ntp.org", .s2 = "time.google.com",
    };
    *c = def;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return;
    if (nvs_get_blob(h, "ntp", c, &len) == ESP_OK &&
        len == sizeof(*c) && c->magic == NTP_MAGIC)
    {
        /* строковые поля обязаны быть NUL-завершёнными */
        c->s1[sizeof(c->s1) - 1] = 0;
        c->s2[sizeof(c->s2) - 1] = 0;
    }
    else
        *c = def;
    nvs_close(h);
}

bool storage_set_ntp(const ntp_cfg_t *c)
{
    nvs_handle_t h;
    bool ok;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    ok = (nvs_set_blob(h, "ntp", c, sizeof(*c)) == ESP_OK &&
          nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

/* ---- отложенная установка OTA ---- */

uint32_t storage_get_otapend(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return 0;
    nvs_get_u32(h, "otapend", &v);
    nvs_close(h);
    return v;
}

bool storage_set_otapend(uint32_t size)
{
    nvs_handle_t h;
    bool ok;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    if (size)
        ok = nvs_set_u32(h, "otapend", size) == ESP_OK;
    else
        ok = nvs_erase_key(h, "otapend") == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
