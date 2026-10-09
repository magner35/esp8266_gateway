#include "tree_store.h"

#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"

#include "debug.h"

/*
 * NVS namespace "tree": блобы "p<id>" (TreeHdr + NUL-строки:
 * name, section, group, tab2, opt*) и "meta" ({magic, count}).
 * Пишет только таск прибора (листинг / OK-ответ 's'), читают httpd
 * и modbus - NVS-операции потокобезопасны сами по себе.
 */

#define TREE_NS    "tree"

/* формат записи фиксируется размером заголовка: если упаковка
 * сломалась (20 вместо 19) - сборка обязана упасть здесь */
/* тип с отрицательным размером не соберётся, если упаковка сломалась
 * (портабельно: и C, и C++ — файл включается в хост-тест как C++) */
typedef char tree_hdr_must_be_21_bytes[(sizeof(TreeHdr) == 21) ? 1 : -1];
#define TREE_MAGIC 0x54524546u /* "TREE" v2: v1=NUL-джойн menus, v2=строки по 44Б */

static nvs_handle_t sNvs;
static bool sWriting;
static uint16_t sWrote;
static int sFirstErr;
/* буфер записи: только таск прибора, один писатель */
static char sBlob[sizeof(TreeHdr) + TREE_STR_MAX];

/* дописать строку в буфер, гарантировать NUL; вернуть новый хвост */
static char *put_str(char *w, char *end, const char *s)
{
    size_t room = (size_t)(end - w);
    size_t n = strlen(s);
    if (n >= room)
        n = (room > 0) ? room - 1 : 0;
    memcpy(w, s, n);
    w[n] = 0;
    return w + n + 1;
}

void tree_init(void)
{
    if (nvs_open(TREE_NS, NVS_READWRITE, &sNvs) != ESP_OK)
        sNvs = 0;
}

void tree_begin(void)
{
    sWriting = true;
    sWrote = 0;
    sFirstErr = 0;
    /*
     * БЕЗ erase_all: стирание в начале захвата уничтожало РАБОЧЕЕ
     * дерево, и сорвавшийся повтор (сбой секции) оставлял меню пустым.
     * От мусора защищает пропуск неизменившихся записей: стабильное
     * дерево не пишется вовсе, 64К разделу хватает надолго.
     */
}

void tree_abort(void)
{
    sWriting = false;
}

void tree_commit(const ProtoCtx *ctx, uint16_t count)
{
    uint32_t meta[2] = { TREE_MAGIC, count };
    /* menus-таблица: строки фиксированной длины SKE_MENU_LEN, пишутся
     * ПРЯМО из контекста - без промежуточной копии (экономия 1.7КБ статики) */
    nvs_set_blob(sNvs, "menus", ctx->menus,
                 (size_t)ctx->menuCount * SKE_MENU_LEN);
    if (!sWriting)
        return;
    sWriting = false;
    if (!sNvs)
        return;
    /* были отказы записи (место) - НЕ публикуем счётчик: иначе meta
     * с count=184 при реально записанных 80 даёт страницу с дырами */
    if (sFirstErr)
        return;
    nvs_set_blob(sNvs, "meta", meta, sizeof(meta));
    nvs_commit(sNvs);
    DBG("tree: %u params stored (wrote %u, err %d)\n",
        (unsigned)count, (unsigned)sWrote, sFirstErr);
}

void tree_write(uint16_t id, const ProtoCtx *ctx, const SkeParam *p)
{
    char key[8];
    TreeHdr h;
    char *w = sBlob + sizeof(h);
    char *end = sBlob + sizeof(sBlob);
    const char *names[4];
    uint8_t k;

    if (!sWriting || !sNvs)
        return;

    names[0] = p->name;   /* имя параметра; секция/группа/вкладка -
                           * только id (см. h.section), имена лежат
                           * в общей menus-таблице: -60..100Б с записи */

    h.type = p->type;
    h.present = p->present;
    h.readOnly = p->readOnly;
    h.masked = p->masked;
    h.groupLvl = p->groupLvl;
    h.optCnt = p->optCnt;
    h.section = p->section;
    h.group = p->group;
    h.tab2 = p->tab2;
    h.value = p->value;
    h.minv = p->minv;
    h.maxv = p->maxv;
    memcpy(sBlob, &h, sizeof(h));

    w = put_str(w, end, names[0]);
    for (k = 0; k < p->optCnt && w < end; k++)
        w = put_str(w, end, ctx->optLabel[k]);

    snprintf(key, sizeof(key), "p%u", (unsigned)id);
    {
        /* пропускаем запись если блоб не изменился: повторные листинги
         * пишут почти ничего, flash-операций (и подвешиваний CPU) меньше */
        static char old[sizeof(sBlob)];
        size_t oldLen = sizeof(old);
        if (nvs_get_blob(sNvs, key, old, &oldLen) == ESP_OK &&
            oldLen == (size_t)(w - sBlob) &&
            !memcmp(old, sBlob, oldLen))
        {
            sWrote++;
            return;
        }
        esp_err_t rc = nvs_set_blob(sNvs, key, sBlob, (size_t)(w - sBlob));
        if (rc == ESP_OK)
            sWrote++;
        else if (!sFirstErr)
        {
            sFirstErr = (int)rc;
            DBG("tree: write p%u rc=%d size=%u\n",
                (unsigned)id, (int)rc, (unsigned)(w - sBlob));
        }
    }
    /* коммит один раз в tree_commit, не на каждом параметре */
}

uint16_t tree_count(void)
{
    uint32_t meta[2] = { 0, 0 };
    size_t len = sizeof(meta);
    if (!sNvs || nvs_get_blob(sNvs, "meta", meta, &len) != ESP_OK ||
        meta[0] != TREE_MAGIC)
        return 0;
    return (uint16_t)meta[1];
}

bool tree_get(uint16_t id, TreeRec *r)
{
    char key[8];
    TreeHdr h;
    size_t len = sizeof(r->raw);
    char *w;
    char *end;
    uint8_t k;

    if (!sNvs)
        return false;
    snprintf(key, sizeof(key), "p%u", (unsigned)id);
    if (nvs_get_blob(sNvs, key, r->raw, &len) != ESP_OK ||
        len < sizeof(h) + 2)   /* hdr + хотя бы NUL имени: короткие
                                  имена ("V01>") давали 26 Б и
                                  отсекались прежней проверкой +8 */
        return false;

    memcpy(&h, r->raw, sizeof(h));
    memset(&r->p, 0, sizeof(r->p));
    r->p.type = h.type;
    r->p.present = h.present;
    r->p.readOnly = h.readOnly;
    r->p.masked = h.masked;
    r->p.groupLvl = h.groupLvl;
    r->p.optCnt = h.optCnt;
    r->p.value = h.value;
    r->p.minv = h.minv;
    r->p.maxv = h.maxv;

    r->section = h.section;
    r->group = h.group;
    r->tab2 = h.tab2;
    w = r->raw + sizeof(h);
    end = r->raw + len;

    strncpy(r->p.name, w, SKE_NAME_LEN - 1);
    r->p.name[SKE_NAME_LEN - 1] = 0;
    w += strlen(w) + 1;
    for (k = 0; k < h.optCnt && w < end; k++)
    {
        r->opts[k] = w;
        w += strlen(w) + 1;
    }
    return true;
}

void tree_wipe(void)
{
    if (!sNvs)
        return;
    nvs_erase_all(sNvs);
    nvs_commit(sNvs);
    DBG("tree: wiped (space recovered)\n");
}

void tree_set_value(uint16_t id, uint32_t raw)
{
    char key[8];
    TreeHdr h;
    size_t len = sizeof(sBlob);

    if (!sNvs)
        return;
    snprintf(key, sizeof(key), "p%u", (unsigned)id);
    if (nvs_get_blob(sNvs, key, sBlob, &len) != ESP_OK || len < sizeof(h))
        return;
    memcpy(&h, sBlob, sizeof(h));
    h.value = raw;
    memcpy(sBlob, &h, sizeof(h));
    nvs_set_blob(sNvs, key, sBlob, len);
    nvs_commit(sNvs);
}

void tree_dump_blob(uint16_t id)
{
    char key[8];
    size_t len = sizeof(sBlob);
    snprintf(key, sizeof(key), "p%u", (unsigned)id);
    if (nvs_get_blob(sNvs, key, sBlob, &len) != ESP_OK)
        return;
    for (size_t i = 0; i < len; i++)
        printf("%02x ", (unsigned char)sBlob[i]);
    printf("\n");
}

/* menus-таблица: строки фиксированной длины SKE_MENU_LEN, вернуть count */
uint8_t tree_menus_load(char *buf, size_t cap)
{
    size_t len = cap;
    if (!sNvs || nvs_get_blob(sNvs, "menus", buf, &len) != ESP_OK)
    {
        if (cap)
            buf[0] = 0;
        return 0;
    }
    return (uint8_t)(len / SKE_MENU_LEN);
}

/* id (1-based, как pool-id прибора) -> имя; 0 -> "" */
const char *tree_menu_name(const char *buf, uint8_t id)
{
    return id ? buf + (size_t)(id - 1) * SKE_MENU_LEN : "";
}
