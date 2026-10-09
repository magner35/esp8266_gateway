#ifndef GW_TREE_STORE_H
#define GW_TREE_STORE_H

/*
 * Дерево параметров прибора в NVS: константная часть (имена, опции,
 * границы) не живёт в ОЗУ - по одной записи на параметр ("p<id>"),
 * плюс "meta" со счётчиком. Запись ведётся ПОТОКОМ во время листинга
 * (парсер отдаёт параметр за параметром), чтение - тоже по записи:
 * страница настроек и modbus читают ровно то, что нужно, без больших
 * буферов. Данные читаются в TreeRec вызывающего (на его стеке) -
 * общих статических буферов нет, потокобезопасно.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* заголовок NVS-записи (СТРОГО упакованный: 19 байт), строки следуют
 * за ним. GCC (прошивка): __attribute__((packed)); MSVC (хост-тест):
 * pragma pack. Без упаковки выравнивание даёт 20 байт - записи
 * читаются со сдвигом на 1 и первый байт имени теряется */
#if defined(__GNUC__)
#define TREE_PACKED __attribute__((packed))
#else
#define TREE_PACKED
#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
#endif
typedef struct TREE_PACKED
{
    uint8_t type, present, readOnly, masked, groupLvl, optCnt;
    uint8_t section, group, tab2;   /* id в menus-таблице (0 = нет) */
    uint32_t value, minv, maxv;
} TreeHdr;
#if defined(_MSC_VER) && !defined(__GNUC__)
#pragma pack(pop)
#endif

/* строки в записи: name\0 section\0 group\0 tab2\0 opt0..optN\0 */
#define TREE_STR_MAX 736 /* 4 имени (44) + опции (SKE_OPT_SCRATCH 512) + запас */

typedef struct
{
    SkeParam p;                       /* name/value/minv/maxv/flags/... */
    uint8_t section, group, tab2;     /* id в menus-таблице (0 = нет) */
    const char *opts[SKE_OPT_MAX];    /* метки enum/bool, NULL-термин. */
    char raw[sizeof(TreeHdr) + TREE_STR_MAX];
} TreeRec;

/* menus-таблица: имена меню пишутся ОДИН раз ("menus" блоб),
 * записи ссылаются id - вместо ~60-100Б имён в каждой записи */
uint8_t tree_menus_load(char *buf, size_t cap);  /* -> count, имена NUL-джойн */
const char *tree_menu_name(const char *buf, uint8_t id); /* id -> имя */

void tree_init(void);

/* запись листинга: begin -> write* -> commit|abort */
void tree_begin(void);
void tree_write(uint16_t id, const ProtoCtx *ctx, const SkeParam *p);
void tree_commit(const ProtoCtx *ctx, uint16_t count);
void tree_abort(void);

/* чтение */
bool tree_get(uint16_t id, TreeRec *r);   /* false = нет записи */
uint16_t tree_count(void);

/* аварийное освобождение места: стереть всё дерево (пересоберётся
 * листингом при следующей загрузке). Вызывается при NVS-full */
void tree_wipe(void);

/* точечное обновление значения ('s' OK-ответ) */
void tree_set_value(uint16_t id, uint32_t raw);

#endif /* GW_TREE_STORE_H */
