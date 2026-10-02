#include "storage.h"

#include <string.h>

#include "nvs_flash.h"

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

void storage_set_wifi(const char *ssid, const char *pass)
{
    strncpy(sSettings.ssid, ssid, sizeof(sSettings.ssid) - 1);
    sSettings.ssid[sizeof(sSettings.ssid) - 1] = 0;
    strncpy(sSettings.pass, pass, sizeof(sSettings.pass) - 1);
    sSettings.pass[sizeof(sSettings.pass) - 1] = 0;
}
