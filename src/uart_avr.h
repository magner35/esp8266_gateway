#ifndef GW_UART_AVR_H
#define GW_UART_AVR_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* см. uart_avr.c: тонкий адаптер над UART0 шлюза */

esp_err_t avr_uart_init(void);
void avr_reset_pulse(void);   /* вход в загрузчик: BOOT + 1 Мбауд */
int  avr_send(const uint8_t *data, size_t len);
int  avr_recv(uint8_t *buf, size_t len, uint32_t timeout_ms);
void avr_drain(void);

void avr_flash_begin(void);   /* пауза обмена + UART программатору */
void avr_flash_end(void);     /* вернуть 115200 + продолжить обмен */

#endif /* GW_UART_AVR_H */
