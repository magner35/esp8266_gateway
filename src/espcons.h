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
bool espcons_link_ready(void);    /* RDY (PB2->GPIO13): true = консоль
                                     прибора свободна, можно слать в UART */

/* OTA из меню прибора (маркеры otadev/otagw из консоли прибора) */
void espcons_ota_menu(bool device); /* прошить прибор (true) или шлюз */
void espcons_ota_check(void);       /* проверка сервера (сутки между) */
int espcons_ota_version(void);      /* 0 = нет обновления, иначе версия */
void espcons_ota_pend_check(void);  /* исполнить отложенную прошивку прибора */
void espcons_ota_boot_check(void);  /* старт: проверить сервер ДО веба */

#endif /* GW_ESPCONS_H */
