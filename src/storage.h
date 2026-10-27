#ifndef GW_STORAGE_H
#define GW_STORAGE_H

/* Persistent gateway settings in the NVS flash partition. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    uint16_t magic;
    uint8_t poll100ms; /* legacy field, kept for layout compatibility */
    uint8_t rfu;
    char ssid[33];
    char pass[65];
} gw_settings_t;

void storage_init(void);
bool storage_save(const gw_settings_t *s);
bool storage_load(gw_settings_t *s); /* false = blank/invalid, defaults */
const gw_settings_t *storage_get(void);
void storage_set_wifi(const char *ssid, const char *pass);

/* --- многосетевое хранилище Wi-Fi (NVS net0..netN) --- */
#define WIFI_NETS_MAX 5
typedef struct
{
    char ssid[33];
    char pass[65];
} wifi_net_t;

void storage_nets_init(void);  /* миграция единичной сети -> net0 (однократно) */
uint8_t storage_net_count(void);
bool storage_net_get(uint8_t idx, wifi_net_t *out); /* idx < count */
bool storage_net_add(const char *ssid, const char *pass); /* false = полный */
bool storage_net_del(uint8_t idx);
void storage_net_delall(void);
int8_t storage_net_find(const char *ssid); /* -1 = нет */

/* widget layout blob: opaque JSON owned by the page, served verbatim */
bool widgets_load(char *buf, size_t cap); /* false = not stored yet */
bool widgets_store(const char *json, size_t len);

/* NTP-настройки: два сервера + смещение UTC; on = синхронизировать и
 * (в дальнейшем) корректировать таймер DS1307 прибора */
typedef struct
{
    uint16_t magic;
    uint8_t on;
    uint8_t autoTz; /* часовой пояс определять по IP (ip-api.com) */
    int8_t tz; /* смещение UTC в часах (может быть отрицательным);
                  при autoTz - последнее определённое, для отображения */
    char s1[32];
    char s2[32];
} ntp_cfg_t;

void storage_get_ntp(ntp_cfg_t *c); /* дефолты при пустом NVS */
bool storage_set_ntp(const ntp_cfg_t *c);

/* epoch последней АВТОотправки TIME (NVS, переживает рестарт):
 * защита от повторных пушей после тихого ребута */
uint32_t storage_timepush_get(void);
void storage_timepush_set(uint32_t epoch);

/* отложенная установка OTA: размер образа в staging (0 = нет) */
uint32_t storage_get_otapend(void);
bool storage_set_otapend(uint32_t size);

#endif /* GW_STORAGE_H */
