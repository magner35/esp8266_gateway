#ifndef GW_ESPCONS_H
#define GW_ESPCONS_H

/*
 * Консоль шлюза на USB-UART (режим USBESP: ATmega отдала линию).
 * Вход: PB0 -> GPIO15 HIGH; выход: END -> GPIO12 -> PB3 -> ATmega
 * возвращает мультиплексор, GPIO15 падает.
 */
#include <stdbool.h>

void espcons_init(void);
bool espcons_check_enter(void); /* true = линия наша: уходим в poll */
void espcons_poll(void);        /* читает/исполняет до выхода или таймаута */
bool espcons_is_active(void);   /* true = консоль шлюза заняла UART */
bool espcons_abort_pending(void); /* true = пользователь нажал Ctrl+C */

#endif /* GW_ESPCONS_H */
