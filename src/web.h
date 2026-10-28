#ifndef GW_WEB_H
#define GW_WEB_H

#include <stdbool.h>
#include <stdint.h>

void web_start(void); /* call once wifi is brought up */

/* фичи из дерева прибора («Wi-Fi gateway»): httpd поднять/погасить,
 * разрешение на OTA-заливку. До web_start работают как преднастройка */
void web_httpd_enable(bool on);
void web_ota_enable(bool on);

/* образ прибран в неактивный OTA-слот вне веб-загрузки (upd.c) */
void web_ota_staged(uint32_t bytes);

#endif /* GW_WEB_H */
