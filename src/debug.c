#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_log.h"

/*
 * UART1 is TX-only (GPIO2) - exactly what a debug channel needs. The
 * SDK log (and newlib printf) go to UART0, which is the meter console
 * line: silence them or every wifi/lwip info line lands in the meter.
 */

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

    /*
     * Логи SDK выключены на КОМПИЛЯЦИИ (sdkconfig:
     * CONFIG_LOG_DEFAULT_LEVEL_NONE) — esp_log_level_set здесь был
     * no-op, потому что CONFIG_LOG_SET_LEVEL не включён. UART0 чист
     * для консоли измерителя, наши DBG идут на UART1.
     */
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
