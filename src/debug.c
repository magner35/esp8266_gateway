#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "esp8266/eagle_soc.h"
#include "esp8266/uart_register.h"

/*
 * UART1 is TX-only (GPIO2) - exactly what a debug channel needs. The
 * SDK log (and newlib printf) go to UART0, which is the meter console
 * line: silence them or every wifi/lwip info line lands in the meter.
 */

/* esp_log выводит посимвольно через putchar (UART0 = линия прибора);
 * переносим ВЕСЬ вывод SDK (включая бинарный WiFi-драйвер, который
 * зовёт esp_log_write напрямую, мимо CONFIG_LOG) на UART1 */
static int dbg_log_putchar(int c)
{
    char ch = (char)c;
    uart_write_bytes(UART_NUM_1, &ch, 1);
    return (unsigned char)c;
}

/* ROM-путь: ets_printf из ROM может зваться внутренностями ROM
 * (паника и т.п.). Символ не экспортирован в esp8266.rom.ld - зовём
 * по адресу (0x4000242c, тот же символ в eboot/Arduino-ядре) */
#define ETS_INSTALL_PUTC1 ((void (*)(void (*)(char)))0x4000242c)

static void dbg_rom_putc(char c)
{
    uart_write_bytes(UART_NUM_1, &c, 1);
}

void dbg_rom_redirect(void)
{
    ETS_INSTALL_PUTC1(dbg_rom_putc);
}

/* ГЛАВНЫЙ путь выхлопа wifi-драйвера: SDK подменяет ROM-овский
 * ets_printf своей версией (components/esp8266/source/ets_printf.c),
 * и её weak-символ ets_putc пишет ПРЯМО в FIFO-регистры UART0 -
 * мимо esp_log, драйвера и putc1. Перекрываем сильным символом:
 * весь ets_printf (логи "wifi:pm stop", "wifi:state ...") идёт на
 * UART1. Пишем регистрами, а не драйвером: вызов может случиться
 * из паники/прерывания, где uart_write_bytes нельзя */
static void dbg_uart1_reg_putc(uint32_t c)
{
    uint32_t wait = 0;
    while (((READ_PERI_REG(UART_STATUS(UART_NUM_1)) &
             (UART_TXFIFO_CNT << UART_TXFIFO_CNT_S)) >>
            UART_TXFIFO_CNT_S) >= 126)
    {
        if (++wait > 2000000) /* не виснуть, если UART молчит */
            return;
    }
    WRITE_PERI_REG(UART_FIFO(UART_NUM_1), c);
}

int ets_putc(int c)
{
    if (c == '\n') /* CRLF, как в оригинальном ets_putc */
        dbg_uart1_reg_putc('\r');
    dbg_uart1_reg_putc((uint32_t)(unsigned char)c);
    return (unsigned char)c;
}

void dbg_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
    };
    uart_driver_install(UART_NUM_1, 0, 256, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &cfg);
    esp_log_set_putchar(dbg_log_putchar);
    dbg_rom_redirect();
}

void dbg_printf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    uart_write_bytes(UART_NUM_1, buf, n);
}
