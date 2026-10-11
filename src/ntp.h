#ifndef GW_NTP_H
#define GW_NTP_H

#include <stdint.h>
#include <stdbool.h>

#include "storage.h"

void ntp_init(void);               /* загрузить конфиг, повесить колбэк */
void ntp_apply(void);              /* запустить/остановить SNTP по конфигу */
const ntp_cfg_t *ntp_cfg(void);
void ntp_set(const ntp_cfg_t *c);  /* сохранить в NVS и применить */
bool ntp_synced(void);
uint32_t ntp_epoch_utc(void);
void ntp_tz_auto_fetch(void); /* разово определить пояс по IP */
int8_t ntp_tz_effective(void);     /* действующий пояс (авто/ручной) */
bool ntp_tz_resolved(void);        /* пояс известен (не ждём IP-геолок) */

#endif /* GW_NTP_H */
