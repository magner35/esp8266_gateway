#ifndef GW_WEB_H
#define GW_WEB_H

#include <stdbool.h>

void web_start(void); /* call once wifi is brought up */

/* фичи из дерева прибора («Wi-Fi gateway»): httpd поднять/погасить,
 * разрешение на OTA-заливку. До web_start работают как преднастройка */
void web_httpd_enable(bool on);
void web_ota_enable(bool on);

#endif /* GW_WEB_H */
