#ifndef HEX_PARSER_H
#define HEX_PARSER_H

#include <stdint.h>
#include "esp_err.h"

/*
 * Парсер Intel HEX - переработка components/hex_parser из OTA_update_AVR_using_ESP32.
 *
 * Оригинал вырезал байты данных из записей в один плоский буфер, игнорируя
 * адреса записей и контрольные суммы - для(ATmega128) это не годится:
 *  - hex от avr-objcopy содержит записи extended address (тип 02/04) и
 *    данные по абсолютным байтовым адресам;
 *  - файл 128 КБ не влезает в кучу ESP8266 целиком.
 *
 * Поэтому парсер потоковый: файл читается построчно, байты собираются в
 * одну страницу (AVR_PAGE_SIZE), страница отдаётся колбэку, неполная
 * страница в конце добивается 0xFF. Второй проход по тому же файлу
 * используется для верификации.
 */

/* Колбэк получения страницы образа: addr выровнен на границу страницы,
 * len == размеру страницы (хвост добит 0xFF). */
typedef void (*hex_page_cb)(uint32_t addr, const uint8_t *page, void *user);

typedef struct
{
    uint32_t total_bytes;   /* байт данных в файле */
    uint32_t min_addr;      /* младший/старший записанный адрес */
    uint32_t max_addr;
    int      pages;         /* страниц отдано колбэку */
} hex_stats_t;

/**
 * @brief Разобрать .hex файл, отдавая образ постранично в cb
 *
 * @param path     файл на /spiffs
 * @param cb       приёмник страниц (256 Б, 0xFF-добитые)
 * @param user     контекст колбэка
 * @param stats    итоги разбора (может быть NULL)
 *
 * @return ESP_OK / ESP_FAIL (ошибки формата - в журнал, с номером строки)
 */
esp_err_t hex_file_parse(const char *path, hex_page_cb cb, void *user,
                         hex_stats_t *stats);

#endif /* HEX_PARSER_H */
