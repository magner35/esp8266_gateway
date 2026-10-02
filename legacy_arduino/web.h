#ifndef GW_WEB_H
#define GW_WEB_H

/* Web server: WiFi setup portal (AP mode, captive) and the SKE-02
 * parameter dashboard (STA mode). */

void webSetup(bool portalMode);
void webLoop(void);

#endif /* GW_WEB_H */
