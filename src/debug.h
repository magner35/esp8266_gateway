#ifndef GW_DEBUG_H
#define GW_DEBUG_H

/*
 * Debug log on UART1 (GPIO2, TX only) - UART0 belongs to the meter link,
 * and the SDK console (printf / esp_log) would inject garbage into it.
 * dbg_init() also silences esp_log, keeping UART0 clean.
 */

void dbg_init(void);

#ifdef __cplusplus
#define dbg_va(...) __VA_ARGS__
void dbg_printf(const char *fmt, ...);
#else
void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

#define DBG(...) dbg_printf(__VA_ARGS__)

#endif /* GW_DEBUG_H */
