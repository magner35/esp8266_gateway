#ifndef GW_WIFI_H
#define GW_WIFI_H

/*
 * WiFi manager: STA join with saved credentials, AP captive portal
 * fallback, async scan for the portal page. Exposes the connection
 * state for the web info endpoint.
 */

#include <stdbool.h>

void wifi_start(void);          /* call after storage_init() */
bool wifi_sta_connected(void);
const char *wifi_ap_ssid(void); /* NULL unless the portal is up */
void wifi_scan_async(void);     /* results served once, then discarded */

#endif /* GW_WIFI_H */
