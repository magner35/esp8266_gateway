#ifndef URCLOCK_H
#define URCLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Движок программирования AVR - адаптация components/avr_flash +
 * components/avr_pro_mode из OTA_update_AVR_using_ESP32 под загрузчик
 * urboot (протокол urclock, avrdude -c urclock) на ATmega128.
 *
 * Оригинал говорил по чистому STK500v1 (optiboot: setDeviceParams 0x42,
 * 0x45, enterProgMode 0x50, loadAddress 0x55, PROG_PAGE 0x64 128-байтными
 * блоками). Urprotocol (см. urboot/urprotocol.md) устроен иначе:
 *   - никаких set/get parameter и enterProgMode;
 *   - UR_GET_SYNC отвечает парой байт, КОДИРУЮЩИХ mcuid и 5 флагов
 *     возможностей загрузчика (2040 mcuid * 5 бит);
 *   - страницы flash: UR_PROG_PAGE_FL=0x02 / UR_READ_PAGE_FL=0x03,
 *     адрес - БАЙТНЫЙ little-endian (24 бита для flash >64КБ, как у m128),
 *     длина - один байт (0 = 256), затем данные и UR_EOP;
 *   - STK_CHIP_ERASE=0x52 опционален;
 *   - векторный загрузчик (VBL) требует патча векторов ХОСТОМ: вектор
 *     сброса приложения -> jmp на загрузчик, исходный вектор -> слот
 *     vblvecnum (обе величины читаются из 6-байтовой таблицы на вершине
 *     flash самого загрузчика).
 *
 * Если синхронизация дала классические константы 0x14/0x10 - на AVR сидит
 * обычный STK500v1-загрузчик (optiboot и т.п.): задействуется
 * совместимый режим, по сути оригинальный код проекта.
 */

typedef struct
{
    /* кодированный ответ загрузчика */
    uint8_t  insync, ok;
    bool     compat;            /* true = классический STK500v1 (0x14/0x10) */

    /* декодированные флаги urprotocol (биты UB_*) */
    uint16_t mcuid;
    uint8_t  features;

    /* 6-байтовая таблица на вершине flash (urprotocol-режим) */
    bool     table_valid;
    uint8_t  bl_version;        /* major<<3 | minor */
    uint8_t  bl_cap;            /* байт возможностей UR_* */
    uint8_t  bl_pages;          /* страниц занимает загрузчик */
    uint8_t  vbl_vecnum;        /* слот вектора для VBL, 0 = не VBL */
    uint32_t bl_start;          /* начало области загрузчика (не трогаем) */
} urboot_info_t;

/* Биты флагов urprotocol (urboot.c: UB_FEATURES) */
#define UB_RESERVED_1   1
#define UB_RESERVED_2   2
#define UB_READ_FLASH   4       /* urboot читать flash умеет всегда */
#define UB_FLASH_LL_NOR 8       /* запись без стирания страницы (AND) */
#define UB_CHIP_ERASE  16       /* есть STK_CHIP_ERASE */

/* Биты байта возможностей из таблицы (urboot.c: UR_*) */
#define URT_AUTOBAUD    0x80
#define URT_EEPROM      0x40
#define URT_UPDATE_FL   0x20
#define URT_DUAL        0x10
#define URT_VBLMASK     0x0C
#define URT_VBLPATCH    0x08    /* сам патчит вектора: verify стр.0 не строго */
#define URT_VBLJUMP     0x04    /* вектора патчит хост */
#define URT_PROTECTRST  0x02
#define URT_HAS_CE      0x01

typedef struct
{
    int      written;           /* страниц записано */
    int      skipped;           /* пропущено (0xFF после chip erase) */
    int      verify_fail;       /* страниц с ошибкой верификации */
    uint32_t image_bytes;       /* полезных байт в образе */
    uint32_t ms;                /* длительность операции */
    bool     compat;            /* работали в STK500v1-режиме */
    char     bl_desc[80];       /* описание загрузчика для отчёта */
} urclock_result_t;

/** Синхронизация с загрузчиком: сбросы AVR, UR_GET_SYNC до устойчивого
 *  ответа, чтение таблицы загрузчика. Инфо - urclock_info(). */
esp_err_t urclock_connect(void);

const urboot_info_t *urclock_info(void);

/** Полный цикл: connect -> chip erase -> запись страниц -> верификация ->
 *  leave_progmode. Прогресс - в журнал (UART1). */
esp_err_t urclock_program_file(const char *hex_path, urclock_result_t *res);

/* живой прогресс: phase 0=нет/1=стирание/2=запись/3=verify */
void urclock_progress(int *phase, int *done, int *total);

#endif /* URCLOCK_H */
