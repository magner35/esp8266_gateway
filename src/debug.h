#ifndef GW_DEBUG_H
#define GW_DEBUG_H

#include <Arduino.h>

/*
 * Two console layouts:
 *  - production: UART0 talks to the meter, the debug log goes to UART1
 *    (GPIO2, TX only);
 *  - GW_DEBUG_USB (bench bring-up): the debug log goes to UART0, i.e. the
 *    USB port of the devboard; the meter UART link is disabled so the log
 *    stays readable.
 */

#if defined(GW_DEBUG_USB)
#define DBG(...) Serial.printf("[gw] " __VA_ARGS__)
#define GW_SKE_ENABLED 0
#else
#define DBG(...) Serial1.printf("[gw] " __VA_ARGS__)
#define GW_SKE_ENABLED 1
#endif

#endif /* GW_DEBUG_H */
