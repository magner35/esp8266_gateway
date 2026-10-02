#ifndef GW_STORAGE_H
#define GW_STORAGE_H

/* Persistent gateway settings in the NVS flash partition. */

#include <stdint.h>
#include <stdbool.h>

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

#endif /* GW_STORAGE_H */
