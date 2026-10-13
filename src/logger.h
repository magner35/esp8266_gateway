#ifndef GW_LOGGER_SHIM_H
#define GW_LOGGER_SHIM_H
/* shim: референсный urclock/hex_parser печатали через свой logger;
 * здесь всё уходит в наш DBG (UART1) */
#include "debug.h"
#define logI(tag, fmt, ...) DBG("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define logW(tag, fmt, ...) DBG("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define logE(tag, fmt, ...) DBG("%s: " fmt "\n", tag, ##__VA_ARGS__)
#endif
