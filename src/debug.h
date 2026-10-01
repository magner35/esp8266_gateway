#ifndef GW_DEBUG_H
#define GW_DEBUG_H

#include <Arduino.h>

/*
 * Console layout flags (orthogonal):
 *  - GW_DEBUG_USB: the debug log goes to UART0 (the USB port of the
 *    devboard) instead of UART1/GPIO2.
 *  - GW_SKE_DISABLE: the meter UART link is disabled.
 * GW_DEBUG_USB alone = diagnostic build: log on USB, meter link active
 * (log bytes mix into the meter line - fine for short boot diagnosis).
 */

#if defined(GW_DEBUG_USB)
#define DBG(...) Serial.printf("[gw] " __VA_ARGS__)
#else
#define DBG(...) Serial1.printf("[gw] " __VA_ARGS__)
#endif

#if defined(GW_SKE_DISABLE)
#define GW_SKE_ENABLED 0
#else
#define GW_SKE_ENABLED 1
#endif

#endif /* GW_DEBUG_H */
