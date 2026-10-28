#ifndef GW_UPD_H
#define GW_UPD_H

/*
 * Обновления с сервера производителя: обычный HTTP (целостность и
 * аутентичность пакета гарантирует Ed25519-подпись .skpak, TLS не
 * нужен и по ОЗУ не влезает). На сервере - каталог с versions.txt
 * (строки "gw|meter <версия> <дата> <файл> <размер> <примечание>")
 * и файлами .skpak.
 *
 * URL сервера настраивается из веба (NVS). Пустой URL = ДЕМО-режим:
 * /api/upd/check возвращает фиксированный список, скачивание честно
 * отказывает ("сервер не настроен") - интерфейс можно гонять целиком.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void upd_init(void);              /* при старте: URL из NVS */
const char *upd_url(void);        /* "" = не настроен (демо) */
bool upd_set_url(const char *url);/* записать в NVS (<=96 байт) */

/* Синхронная проверка: собирает JSON-ответ в buf. Возврат - true/ok. */
bool upd_check(char *json, size_t cap);

/* Фоновое скачивание файла с сервера в цель:
 *   type "gw"    -> неактивный OTA-слот (потом /api/ota/flash)
 *   type "meter" -> раздел avrhex (потом /api/avr/flash)
 * Один файл за раз; статус - upd_state(). */
typedef struct
{
    volatile bool running;
    bool ok;
    int rc;           /* 0 или текст в err */
    uint32_t bytes;
    char file[48];
    char err[96];
} upd_state_t;

const upd_state_t *upd_state(void);
bool upd_download_start(const char *file, const char *type);

#endif /* GW_UPD_H */
