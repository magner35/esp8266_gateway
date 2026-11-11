#ifndef GW_PROTOCOL_H
#define GW_PROTOCOL_H

/*
 * Pure C core of the SKE-02 service console client: listing parser
 * (STREAMING: параметр за параметром через on_param - дерево не
 * копится в ОЗУ, константная часть уходит в NVS через tree_store),
 * value line parser, Cyrillic homoglyph restore, prompt matcher,
 * live values ('m' CSV frames). Freestanding - no SDK includes, so
 * the host test bench compiles the SAME source.
 *
 * The console protocol (include/console.h of the firmware repo):
 *   commands ? l g s x p k w d i e r, CR/LF terminated
 *   every reply ends with the prompt "SKE02> "
 *   l  hierarchical listing: "[L#] Root/Sub" headers and indented
 *      "<id> <type> <name> = <value> [range]" lines; range is [min..max]
 *      or the [v1|v2|..] enum/bool option list; command items are
 *      "<id> CMD <name>" without a value
 *   s  values: numbers only, TIME packed as HHMM, DATE as DDMMYY
 *   m  machine data: MeterData_t as CSV, bitwise members in hex
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* property type codes = prop_type_t of the firmware (+CMD for menu
 * command items, which have no value and are run, not set) */
enum
{
    SKT_FLOAT = 0, SKT_U32, SKT_TIME, SKT_DATE, SKT_U8, SKT_I8, SKT_H8,
    SKT_U16, SKT_H16, SKT_I16, SKT_BOOL, SKT_ENUM, SKT_STRING, SKT_STRINGP,
    SKT_CMD,
    /* новые типы прибора (PR_H32/PR_MAC/PR_IP в fproperty.h) -
     * В КОНЦЕ enum: сдвиг существующих значений ломает NVS-дерево */
    SKT_H32, SKT_MAC, SKT_IP
};

/* internal result codes of the console transactions */
enum
{
    BS_OK = 0, BS_BAD_ID, BS_BAD_TYPE, BS_READONLY, BS_ACCESS, BS_WAIT
};

#define SKE_ERR_TRANSPORT (-1)

/* sized from a real device listing (177 params, 33 submenus, 3132 bytes
 * of rusified option labels) plus headroom; 224 = запас на новые
 * пункты меню прибора (при 192 seenMap уходил за границу) */
#define SKE_MAX_PARAMS    224
#define SKE_NAME_LEN      44   /* menu texts are up to 20 CHARS = 41 UTF-8
                                * bytes (Cyrillic) + NUL */
#define SKE_MENU_LEN      44   /* one pool for L1 sections and submenus */
#define SKE_MAX_MENUS     40
#define SKE_MAX_SECTIONS  12   /* L1 sections tracked in order */
#define SKE_OPT_MAX       16   /* options per parameter */
#define SKE_OPT_SCRATCH   512  /* labels of the CURRENT param only */
#define SKE_PROMPT        "SKE02> "

typedef struct
{
    char name[SKE_NAME_LEN];
    uint32_t value;   /* raw payload, IEEE-754 bits for SKT_FLOAT */
    uint32_t minv;    /* raw numeric range, both 0 when not reported */
    uint32_t maxv;
    uint32_t updated; /* timestamp of the last refresh, 0 = never */
    uint8_t optCnt;   /* enum/bool option count */
    uint8_t type;     /* SKT_* */
    uint8_t section;  /* L1 submenu pool id */
    uint8_t group;    /* nearest submenu pool id */
    uint8_t groupLvl; /* level of the owning submenu (L1=1, L2=2, ...) */
    uint8_t tab2;     /* owning L2 submenu pool id, 0 = section root */
    bool present;
    bool readOnly;
    bool masked;      /* password U32, shown as ****** by the console */
} SkeParam;

/* live measurement values: the 'm' frame = MeterData_t of the firmware */
typedef struct
{
    float frequency, rate_raw, rateMLPM, rate;
    float total_plus, total_minus, total, total_sum;
    float gtotal;
    float kf_value, batch;
    uint32_t pulses_packet, pulses;
    /* канал 2 (COUNT2): поля 9-12 второй строки кадра (могут
     * отсутствовать у старой прошивки прибора - тогда 0) */
    float frequency2, kf_value2;
    uint32_t pulses2;
    uint8_t status, setpoint, isr, isr2;
    /* 7-8 поля кадра 'm': МАРКЕР ПРАВКИ - id последнего изменённого
     * параметра + счётчик правок (прибор пишет в storePropertyValue).
     * Счётчик УШЁЛ НАЗАД = ребут прибора (шлюз делает relist).
     * edit_cnt==0 - полей нет (старая прошивка прибора) */
    uint16_t edit_id;
    uint8_t edit_cnt;
    uint32_t updated; /* 0 = no frame yet */
} SkeValues;

/*
 * Protocol state. Худой: массив параметров и пул опций БОЛЬШЕ не
 * живут здесь - парсер отдаёт каждый параметр в on_param (владелец
 * пишет его в NVS), значения вне листинга - в on_value. Здесь только
 * меню-пул (нужен парсеру для секций), живые значения и скрэтч
 * опций текущего параметра.
 */
typedef struct ProtoCtx
{
    char menus[SKE_MAX_MENUS][SKE_MENU_LEN]; /* L1 sections + submenus */
    uint16_t menuCount;
    uint16_t count;      /* parameter count from 'i' */
    char version[24];  /* "SKE02_10_3525": модель_железо_прошивка */
    char serial[11];   /* заводской номер прибора из 'i' (десятичный) */
    uint8_t sectionIds[SKE_MAX_SECTIONS]; /* pool ids of L1 menus, in order */
    uint8_t sectionNum;

    /* live values + the 'm' frame parser position */
    SkeValues vals;
    uint8_t valsLine;

    /* parser state (valid during a listing capture only) */
    bool inListing;
    uint8_t curSection, curGroup;
    uint8_t hdrName[9];   /* pool id of the last header per level */

    /* prompt matcher position */
    uint8_t promptPos;

    /* timestamp source, set by the owner before each parse pass */
    uint32_t nowMs;

    /* потоковый выход парсера */
    void (*on_param)(struct ProtoCtx *, uint16_t id, const SkeParam *p);
    void (*on_value)(struct ProtoCtx *, uint16_t id, uint32_t raw);
    char optScratch[SKE_OPT_SCRATCH];      /* метки опций текущ. парам. */
    const char *optLabel[SKE_OPT_MAX + 1]; /* указатели в optScratch */
    char strVal[SKE_NAME_LEN];             /* SKT_STRING значение */
    uint8_t seenMap[(SKE_MAX_PARAMS + 7) / 8]; /* id, встреченные в листинге */
    uint16_t liveCount;                    /* сколько разных id видели */
    uint16_t maxId;
} ProtoCtx;

void proto_init(ProtoCtx *ctx);
void proto_reset(ProtoCtx *ctx); /* full wipe (rescan) */

void proto_parse_line(ProtoCtx *ctx, char *line);   /* one 'l'/'g'/'s' line */
void proto_listing_begin(ProtoCtx *ctx);            /* reset stream counters */
void proto_values_line(ProtoCtx *ctx, char *line);  /* one 'm' CSV line */
void proto_values_restart(ProtoCtx *ctx);           /* before an 'm' frame */
bool proto_values_ready(const ProtoCtx *ctx);

/* prompt matcher: incremental, across raw uart bytes */
void proto_prompt_reset(ProtoCtx *ctx);
bool proto_prompt_feed(ProtoCtx *ctx, char c);

/* menu pool accessors (ids -> names, для парсера и tree_store) */
const char *proto_section_name(const ProtoCtx *ctx, uint8_t idx);
int8_t proto_section_index_of(const ProtoCtx *ctx, uint8_t poolId);
const char *proto_group_name(const ProtoCtx *ctx, uint8_t idx);
uint8_t proto_type_size(uint8_t type);

/* value formatting: display text (labels, hh:mm, dd.mm.yy) */
void proto_raw_to_display(uint8_t type, uint32_t raw, char *buf, size_t cap);
void proto_bound_text(const SkeParam *p, bool max, char *buf, size_t cap);

/* console 's' value formatting: packed numbers (TIME=HHMM, DATE=DDMMYY),
 * floats with at most 9 significant digits */
void proto_raw_to_cmd(uint8_t type, uint32_t raw, char *buf, size_t cap);

/* UI input text -> raw u32 по ЗАПИСИ дерева (SkeParam из tree_get +
 * opts - метки enum/bool; compose TIME/DATE поверх p->value, clamp);
 * false => сообщение в err */
bool proto_input_to_raw_rec(const SkeParam *p, const char *const *opts,
                            const char *text, uint32_t *raw,
                            char *err, size_t errcap);

/* civil date helpers (TZ-free unix seconds <-> y/m/d) */
void proto_civil_from_days(int32_t days, int16_t *y, uint8_t *m, uint8_t *d);
int32_t proto_days_from_civil(int16_t y, uint8_t m, uint8_t d);

/* restore pure Cyrillic from the mixed Latin-Cyrillic console output */
void proto_rusify(char *s);

#ifdef __cplusplus
}
#endif

#endif /* GW_PROTOCOL_H */
