#ifndef GW_DEBUG_H
#define GW_DEBUG_H

/*
 * Debug log on UART1 (GPIO2, TX only) - UART0 belongs to the meter link,
 * and the SDK console (printf / esp_log) would inject garbage into it.
 * dbg_init() also silences esp_log, keeping UART0 clean.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

void dbg_init(void);
void dbg_rom_redirect(void); /* ets_printf -> UART1 (повторять после wifi-рестартов) */

#ifdef __cplusplus
#define dbg_va(...) __VA_ARGS__
void dbg_printf(const char *fmt, ...);
#else
void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

#define DBG(...) dbg_printf(__VA_ARGS__)

/* ---- newlib-мьютекс: сериализация reent-опасных вызовов ------------
 * ESP8266_RTOS_SDK v3.4: CONFIG_FREERTOS_ENABLE_REENT сломан (нет
 * _impure_ptr) — newlib живёт с ОДНИМ reent на все задачи. Любые
 * strtol/atol/sscanf/atoi и %[f] используют внутреннее состояние
 * reent (буферы, freelist dtoa) и при параллельном вызове из двух
 * задач рвут память (IllegalInstruction с мусорным PC — три краша).
 * httpd — однозадачный, ему мьютекс не нужен. */

void nl_lock(void);
void nl_unlock(void);

static inline long nl_atol(const char *s)
{
    long v;
    nl_lock(); v = atol(s); nl_unlock();
    return v;
}

static inline int nl_atoi(const char *s)
{
    int v;
    nl_lock(); v = atoi(s); nl_unlock();
    return v;
}

static inline long nl_strtol(const char *s, char **end, int base)
{
    long v;
    nl_lock(); v = strtol(s, end, base); nl_unlock();
    return v;
}

#endif /* GW_DEBUG_H */
