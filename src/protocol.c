/*
 * Pure C core of the SKE-02 console client - see protocol.h. Ported
 * verbatim (semantics!) from the verified Arduino implementation, with
 * every hard-won ordering constraint kept:
 *   - entry-line length captured before the type-token cut (the live
 *     strlen is useless after *tokEnd = 0)
 *   - the value text copied out before any rusify (in-place expansion
 *     would run over the range/option bytes further along the line)
 *   - options stored on the first listing only (pool duplicates would
 *     overflow it on every refresh), all-or-nothing per list
 *   - the menu owner taken from the line indent, the L2 tab from the
 *     header walk (the firmware prints subtrees before parent params)
 */
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TXT_LINE 208 /* worst header: [L#] + 4 Cyrillic menu names */

/* ------------------------------------------------------------------ */
/* init / wipe                                                         */

void proto_init(ProtoCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void proto_reset(ProtoCtx *ctx)
{
    uint32_t now = ctx->nowMs;
    proto_init(ctx);
    ctx->nowMs = now;
}

/* ------------------------------------------------------------------ */
/* types                                                               */

static uint8_t type_by_token(const char *tok)
{
    if (!strcmp(tok, "F")) return SKT_FLOAT;
    if (!strcmp(tok, "U32")) return SKT_U32;
    if (!strcmp(tok, "TIME")) return SKT_TIME;
    if (!strcmp(tok, "DATE")) return SKT_DATE;
    if (!strcmp(tok, "U8")) return SKT_U8;
    if (!strcmp(tok, "I8")) return SKT_I8;
    if (!strcmp(tok, "U16")) return SKT_U16;
    if (!strcmp(tok, "I16")) return SKT_I16;
    if (!strcmp(tok, "B")) return SKT_BOOL;
    if (!strcmp(tok, "E")) return SKT_ENUM;
    if (!strcmp(tok, "S")) return SKT_STRING;
    if (!strcmp(tok, "CMD")) return SKT_CMD;
    return 0xFF;
}

uint8_t proto_type_size(uint8_t type)
{
    static const uint8_t sz[15] = { 4, 4, 4, 4, 1, 1, 1, 2, 2, 2, 1, 1, 0, 0, 0 };
    return (type < 15) ? sz[type] : 0;
}

/* ------------------------------------------------------------------ */
/* civil date (Howard Hinnant's algorithms, TZ-free)                   */

void proto_civil_from_days(int32_t days, int16_t *y, uint8_t *m, uint8_t *d)
{
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    *m = (uint8_t)(mp + (mp < 10 ? 3 : -9));
    *y = (int16_t)(yy + (*m <= 2));
}

int32_t proto_days_from_civil(int16_t y, uint8_t m, uint8_t d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/* ------------------------------------------------------------------ */
/* menu pool                                                           */

static uint8_t add_named(ProtoCtx *ctx, const char *name)
{
    uint16_t i;
    for (i = 0; i < ctx->menuCount; i++)
        if (!strncmp(ctx->menus[i], name, SKE_MENU_LEN))
            return (uint8_t)(i + 1);
    if (ctx->menuCount >= SKE_MAX_MENUS)
        return 0;
    strncpy(ctx->menus[ctx->menuCount], name, SKE_MENU_LEN - 1);
    ctx->menus[ctx->menuCount][SKE_MENU_LEN - 1] = 0;
    ctx->menuCount++;
    return (uint8_t)ctx->menuCount;
}

int8_t proto_section_index_of(const ProtoCtx *ctx, uint8_t poolId)
{
    uint8_t i;
    for (i = 0; i < ctx->sectionNum; i++)
        if (ctx->sectionIds[i] == poolId)
            return (int8_t)i;
    return -1;
}

const char *proto_section_name(const ProtoCtx *ctx, uint8_t idx)
{
    return (idx < ctx->sectionNum) ? ctx->menus[ctx->sectionIds[idx] - 1] : "";
}

const char *proto_group_name(const ProtoCtx *ctx, uint8_t idx)
{
    return (idx >= 1 && idx <= ctx->menuCount) ? ctx->menus[idx - 1] : "";
}

SkeParam *proto_param(ProtoCtx *ctx, uint16_t id)
{
    return (id < ctx->count && id < SKE_MAX_PARAMS) ? &ctx->params[id] : NULL;
}

/* ------------------------------------------------------------------ */
/* Cyrillic homoglyph restore                                          */
/*
 * The console emits Cyrillic letters that share an HD44780 glyph with
 * Latin ones as Latin code points (RussianROMLetters in the firmware:
 * A B E K M H O P C T X b a e o p c y x). Restore them so names, menus
 * and options are uniformly Russian and searchable. Word-wise: a Latin
 * letter is converted only inside a word that already holds genuine
 * Cyrillic, so real Latin words (Qmax, UTC, CRC, HW, F01) survive.
 */

static const struct
{
    char lat;
    uint16_t cp;
} sHomoglyph[] = {
    { 'A', 0x0410 }, { 'B', 0x0412 }, { 'E', 0x0415 }, { 'K', 0x041A },
    { 'M', 0x041C }, { 'H', 0x041D }, { 'O', 0x041E }, { 'P', 0x0420 },
    { 'C', 0x0421 }, { 'T', 0x0422 }, { 'X', 0x0425 }, { 'b', 0x042C },
    { 'a', 0x0430 }, { 'e', 0x0435 }, { 'o', 0x043E }, { 'p', 0x0440 },
    { 'c', 0x0441 }, { 'y', 0x0443 }, { 'x', 0x0445 },
};

static bool rus_word_byte(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u >= 0x80;
}

void proto_rusify(char *s)
{
    static char buf[TXT_LINE];
    size_t len = strlen(s);
    size_t in = 0, out = 0;
    while (in < len && out < sizeof(buf) - 3)
    {
        if (!rus_word_byte(s[in]))
        {
            buf[out++] = s[in++];
            continue;
        }
        {
            size_t start = in;
            bool hasCyr = false;
            while (in < len && rus_word_byte(s[in]))
            {
                unsigned char u = (unsigned char)s[in];
                if (u == 0xD0 || u == 0xD1)
                    hasCyr = true;
                in++;
            }
            for (; start < in && out < sizeof(buf) - 3; start++)
            {
                char ch = s[start];
                uint16_t cp = 0;
                uint8_t k;
                if (hasCyr)
                    for (k = 0; k < sizeof(sHomoglyph) / sizeof(sHomoglyph[0]); k++)
                        if (sHomoglyph[k].lat == ch)
                        {
                            cp = sHomoglyph[k].cp;
                            break;
                        }
                if (cp)
                {
                    buf[out++] = (char)(0xC0 | (cp >> 6));
                    buf[out++] = (char)(0x80 | (cp & 0x3F));
                }
                else
                    buf[out++] = ch;
            }
        }
    }
    buf[out] = 0;
    strcpy(s, buf);
}

/* ------------------------------------------------------------------ */
/* value conversions                                                   */

/* display value -> raw u32; the date/time-of-day part keeps the cached one */
static uint32_t text_to_raw(SkeParam *p, const char *text)
{
    switch (p->type)
    {
    case SKT_FLOAT:
    {
        float f = (float)strtod(text, NULL);
        uint32_t raw;
        memcpy(&raw, &f, 4);
        return raw;
    }
    case SKT_TIME:
    {
        int h = 0, m = 0;
        if (sscanf(text, "%d:%d", &h, &m) == 2)
        {
            uint32_t day = p->value - (p->value % 86400UL);
            return day + (uint32_t)h * 3600UL + (uint32_t)m * 60UL;
        }
        return p->value;
    }
    case SKT_DATE:
    {
        int d = 1, m = 1, y = 0;
        if (sscanf(text, "%d.%d.%d", &d, &m, &y) == 3)
        {
            uint32_t tod = p->value % 86400UL;
            return (uint32_t)proto_days_from_civil((int16_t)(2000 + y), (uint8_t)m,
                                                   (uint8_t)d) * 86400UL + tod;
        }
        return p->value;
    }
    case SKT_BOOL:
    case SKT_ENUM:
        /* enum/bool display values (option labels) are matched back to
         * their index by proto_parse_line through the pool */
        return p->value;
    default:
        return (uint32_t)strtol(text, NULL, 10);
    }
}

/* raw -> display text for the parameter type */
static void raw_to_display(uint8_t type, uint32_t raw, char *buf, size_t cap)
{
    if (type == SKT_FLOAT)
    {
        float f;
        char *dot;
        memcpy(&f, &raw, 4);
        snprintf(buf, cap, "%.3f", (double)f);
        dot = strchr(buf, '.');
        if (dot)
        {
            char *e = buf + strlen(buf) - 1;
            while (e > dot && *e == '0')
                *e-- = 0;
            if (e == dot)
                *e = 0;
        }
        if (!buf[0])
            snprintf(buf, cap, "0");
        return;
    }
    if (type == SKT_TIME)
    {
        uint32_t v = raw % 86400UL;
        snprintf(buf, cap, "%02lu:%02lu", (unsigned long)(v / 3600),
                 (unsigned long)(v % 3600 / 60));
        return;
    }
    if (type == SKT_DATE)
    {
        int16_t y;
        uint8_t m, d;
        proto_civil_from_days(raw / 86400L, &y, &m, &d);
        snprintf(buf, cap, "%02u.%02u.%02u", d, m, (unsigned)(y % 100));
        return;
    }
    if (type == SKT_I8)
    {
        snprintf(buf, cap, "%d", (int)(int8_t)raw);
        return;
    }
    if (type == SKT_I16)
    {
        snprintf(buf, cap, "%d", (int)(int16_t)raw);
        return;
    }
    snprintf(buf, cap, "%lu", (unsigned long)raw);
}

void proto_value_text(const ProtoCtx *ctx, uint16_t id, char *buf, size_t cap)
{
    const SkeParam *p = (const SkeParam *)proto_param((ProtoCtx *)ctx, id);
    if (cap)
        buf[0] = 0;
    if (!p)
        return;
    if (p->type == SKT_CMD)
        return;
    if (!p->present)
    {
        snprintf(buf, cap, "-");
        return;
    }
    if (p->masked)
    {
        snprintf(buf, cap, "******");
        return;
    }
    /* enum/bool values are shown as their option label, like on the LCD */
    if ((p->type == SKT_ENUM || p->type == SKT_BOOL) && p->optCnt)
    {
        uint32_t v = p->value;
        const char *s = proto_opt_text(ctx, p, v < p->optCnt ? (uint8_t)v : 0);
        snprintf(buf, cap, "%s", s ? s : "-");
        return;
    }
    raw_to_display(p->type, p->value, buf, cap);
}

void proto_bound_text(const SkeParam *p, bool max, char *buf, size_t cap)
{
    if (cap)
        buf[0] = 0;
    if (!p || (p->minv == 0 && p->maxv == 0))
        return;
    raw_to_display(p->type, max ? p->maxv : p->minv, buf, cap);
}

const char *proto_opt_text(const ProtoCtx *ctx, const SkeParam *p, uint8_t idx)
{
    const char *s;
    if (!p || !p->optCnt || idx >= p->optCnt || !p->optPool)
        return NULL;
    s = ctx->optPool + p->optPool - 1;
    while (idx--)
        s += strlen(s) + 1;
    return s;
}

/* console 's' value: packed numbers (TIME=HHMM, DATE=DDMMYY, the device
 * rebuilds the rest of the timestamp itself), floats <= 9 significant
 * digits per the console parser limit */
void proto_raw_to_cmd(uint8_t type, uint32_t raw, char *buf, size_t cap)
{
    if (type == SKT_FLOAT)
    {
        float f;
        char *dot;
        int digits = 0;
        char *q;
        char *end;
        memcpy(&f, &raw, 4);
        snprintf(buf, cap, "%.6f", (double)f);
        dot = strchr(buf, '.');
        if (dot)
        {
            end = buf + strlen(buf) - 1;
            while (end > dot && *end == '0')
                *end-- = 0;
            if (end == dot)
                *end = 0;
            for (q = buf; *q; q++)
                if (*q >= '0' && *q <= '9')
                    digits++;
            end = buf + strlen(buf) - 1;
            while (digits > 9 && end > dot)
            {
                *end-- = 0;
                digits--;
            }
            if (end == dot)
                *end = 0;
        }
        if (!buf[0])
            snprintf(buf, cap, "0");
        return;
    }
    if (type == SKT_TIME)
    {
        uint32_t v = raw % 86400UL;
        snprintf(buf, cap, "%02u%02u", (unsigned)(v / 3600),
                 (unsigned)(v % 3600 / 60));
        return;
    }
    if (type == SKT_DATE)
    {
        int16_t y;
        uint8_t m, d;
        proto_civil_from_days(raw / 86400L, &y, &m, &d);
        snprintf(buf, cap, "%02u%02u%02u", d, m, (unsigned)(y % 100));
        return;
    }
    snprintf(buf, cap, "%lu", (unsigned long)raw);
}

/* UI input text -> raw; TIME is "hh:mm", DATE is "dd.mm.yy": like the
 * device menu, only the edited part of the unix value changes */
bool proto_input_to_raw(ProtoCtx *ctx, uint16_t id, const char *text,
                        uint32_t *raw, char *err, size_t errcap)
{
    SkeParam *p = proto_param(ctx, id);
    double d;
    if (!p)
    {
        snprintf(err, errcap, "net takogo parametra");
        return false;
    }
    if (!p->present || proto_type_size(p->type) == 0)
    {
        snprintf(err, errcap, "parametr nedostupen");
        return false;
    }
    if (p->readOnly)
    {
        snprintf(err, errcap, "tolko dlya chteniya");
        return false;
    }
    if (!text || !*text)
    {
        snprintf(err, errcap, "pustoe znachenie");
        return false;
    }

    if (p->type == SKT_TIME || p->type == SKT_DATE)
    {
        int v1, v2, v3 = 0;
        uint32_t cur = p->value;
        if (p->type == SKT_TIME)
        {
            if (sscanf(text, "%d:%d", &v1, &v2) != 2 || v1 < 0 || v1 > 23 ||
                v2 < 0 || v2 > 59)
            {
                snprintf(err, errcap, "format hh:mm");
                return false;
            }
            *raw = cur - (cur % 86400UL) + (uint32_t)v1 * 3600UL +
                     (uint32_t)v2 * 60UL;
        }
        else
        {
            if (sscanf(text, "%d.%d.%d", &v1, &v2, &v3) != 3 || v1 < 1 ||
                v1 > 31 || v2 < 1 || v2 > 12 || v3 < 0 || v3 > 99)
            {
                snprintf(err, errcap, "format dd.mm.gg");
                return false;
            }
            *raw = (uint32_t)proto_days_from_civil((int16_t)(2000 + v3),
                                                   (uint8_t)v2, (uint8_t)v1) *
                       86400UL +
                   (cur % 86400UL);
        }
        return true;
    }

    d = strtod(text, NULL);
    switch (p->type)
    {
    case SKT_FLOAT:
    {
        float f = (float)d;
        memcpy(raw, &f, 4);
        break;
    }
    case SKT_U32:
        if (d < 0)
            d = 0;
        if (d > 999999)
            d = 999999;
        *raw = (uint32_t)d;
        break;
    case SKT_I8:
        if (d < -128) d = -128;
        if (d > 127) d = 127;
        *raw = (uint8_t)(int8_t)d;
        break;
    case SKT_I16:
        if (d < -32768) d = -32768;
        if (d > 32767) d = 32767;
        *raw = (uint16_t)(int16_t)d;
        break;
    default: /* the device clamps to range anyway */
        if (d < 0)
            d = 0;
        if (d > 65535)
            d = 65535;
        *raw = (uint16_t)d;
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* the parameter line parser (shared by 'l' and 'g'/'s OK' replies)    */
/*
 *   [L1] Root/Sub                      header
 *         007 U8  Name = 42 [3..10]    param: indent = owner level + 1
 *         008 E   Mode = On [Off|On]
 *         009 U8  View = 5 *           read only views end with " *"
 *         010 CMD  Run something       command item, no value
 */

static void store_options(ProtoCtx *ctx, SkeParam *p, char *range)
{
    uint8_t cnt = 1;
    uint16_t off;
    uint8_t stored = 0;
    char *q = range;
    char *nq;
    char *ws;
    char tmp[48];
    size_t l;

    for (; *q; q++)
        if (*q == '|')
            cnt++;
    if (cnt > SKE_OPT_MAX)
        return;

    off = ctx->optPoolUsed;
    q = range;
    while (q && *q)
    {
        nq = strchr(q, '|');
        if (nq)
            *nq = 0;
        ws = q + strlen(q);
        while (ws > q && ws[-1] == ' ')
            *--ws = 0;
        while (*q == ' ')
            q++;
        l = strlen(q);
        if (l >= sizeof(tmp))
            l = sizeof(tmp) - 1;
        memcpy(tmp, q, l);
        tmp[l] = 0;
        proto_rusify(tmp);
        if (ctx->optPoolUsed + strlen(tmp) + 1 > SKE_OPT_POOL)
            break;
        strcpy(ctx->optPool + ctx->optPoolUsed, tmp);
        ctx->optPoolUsed += strlen(tmp) + 1;
        stored++;
        q = nq ? nq + 1 : NULL;
    }
    if (stored == cnt)
    {
        p->optPool = (uint16_t)(off + 1); /* 0 stays "no options" */
        p->optCnt = cnt;
    }
    else
    {
        ctx->optPoolUsed = off; /* roll the partial list back */
    }
}

void proto_parse_line(ProtoCtx *ctx, char *s)
{
    size_t len = strlen(s);
    uint8_t lvl = 0;
    char *path;
    char *last;
    char *sec;
    char *e;
    long id;
    char *tokEnd;
    static char tokBuf[8];
    uint8_t type;
    char *name;
    char *eq;
    uint16_t ind = 0;
    uint8_t owner;
    char *tail;
    bool ro = false;
    char *range = NULL;
    char *rb;
    char *vend;
    char *t;
    char valBuf[48];
    SkeParam *p;
    uint8_t k;

    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
    if (!len)
        return;

    if (s[0] == '[' && s[1] == 'L') /* submenu header [L#] Root/Sub/.. */
    {
        path = strchr(s, ']');
        if (!path || !ctx->inListing)
            return;
        {
            char *pp;
            for (pp = s + 2; pp < path; pp++)
                if (*pp >= '0' && *pp <= '9')
                    lvl = (uint8_t)(lvl * 10 + (*pp - '0'));
        }
        path++;
        while (*path == ' ')
            path++;
        proto_rusify(path); /* menu names arrive as a Latin-Cyrillic mix */
        last = path;
        {
            char *pp;
            for (pp = path; *pp; pp++)
                if (*pp == '/')
                    last = pp + 1;
        }
        ctx->curGroup = add_named(ctx, last);
        if (lvl < sizeof(ctx->hdrName))
            ctx->hdrName[lvl] = ctx->curGroup;

        /* the L1 section is strictly the SECOND path component */
        sec = strchr(path, '/');
        if (sec && sec < last)
        {
            char *end;
            uint8_t sid;
            sec++;
            end = strchr(sec, '/');
            if (end)
                *end = 0;
            sid = add_named(ctx, sec);
            ctx->curSection = sid;
            if (sid && proto_section_index_of(ctx, sid) < 0 &&
                ctx->sectionNum < SKE_MAX_SECTIONS)
                ctx->sectionIds[ctx->sectionNum++] = sid;
        }
        return;
    }

    /* the indent is the authoritative owner: params print at walk level
     * (owner + 1), 2 spaces per level; a parent-level param can follow a
     * deeper subtree without any new header */
    while (s[ind] == ' ')
        ind++;
    owner = (ind >= 2) ? (uint8_t)(ind / 2 - 1) : 0;

    id = strtol(s + ind, &e, 10);
    if (e == s + ind || id < 0 || id >= SKE_MAX_PARAMS)
        return; /* not a param line */
    while (*e == ' ')
        e++;
    tokEnd = strchr(e, ' '); /* end of the type token */
    if (!tokEnd)
        return;
    *tokEnd = 0;
    strncpy(tokBuf, e, sizeof(tokBuf) - 1);
    tokBuf[sizeof(tokBuf) - 1] = 0;
    type = type_by_token(tokBuf);
    name = tokEnd + 1;
    while (*name == ' ')
        name++;
    eq = strstr(name, " = ");

    if (type == SKT_CMD && !eq)
    {
        /* "<id> CMD <name>" - a menu command item, run via 'x' */
        char *eol = name + strlen(name);
        while (eol > name && (eol[-1] == ' ' || eol[-1] == '*'))
            *--eol = 0;
        proto_rusify(name);
        p = &ctx->params[id];
        if (ctx->inListing)
        {
            strncpy(p->name, name, SKE_NAME_LEN - 1);
            p->name[SKE_NAME_LEN - 1] = 0;
            p->section = ctx->curSection;
            p->groupLvl = owner;
            p->group = (owner < sizeof(ctx->hdrName)) ? ctx->hdrName[owner]
                                                      : ctx->curGroup;
            p->tab2 = (owner >= 2) ? ctx->hdrName[2] : 0;
        }
        p->type = SKT_CMD;
        p->present = true;
        p->updated = ctx->nowMs;
        return;
    }

    if (!eq || type == 0xFF)
        return;

    /*
     * NB: *tokEnd = 0 above already cut s at the type token, so the live
     * strlen(s) is useless - len (captured at entry) addresses the real
     * line end. Read only views end with " *".
     */
    tail = eq + 3;
    if (len && s[len - 1] == '*')
    {
        ro = true;
        s[--len] = 0;
        while (len && s[len - 1] == ' ')
            s[--len] = 0;
    }
    rb = strrchr(tail, '['); /* options/range at the line end */
    if (rb && len && s[len - 1] == ']')
    {
        s[len - 1] = 0; /* drop the closing bracket first, then split */
        *rb = 0;
        range = rb + 1;
    }
    vend = tail + strlen(tail);
    while (vend > tail && vend[-1] == ' ')
        *--vend = 0;

    *eq = 0;
    t = eq;
    while (t > name && t[-1] == ' ')
        *--t = 0;

    /* the value text is copied out FIRST: rusify expands in place and
     * would run over the range/option bytes further along the line */
    strncpy(valBuf, tail, sizeof(valBuf) - 1);
    valBuf[sizeof(valBuf) - 1] = 0;

    p = &ctx->params[id];
    p->type = type;

    /* options live in the meter's flash: store them on the FIRST listing
     * that brings them; re-listings only refresh values - re-storing
     * would append pool duplicates until overflow */
    if (ctx->inListing && !p->optCnt && range &&
        (type == SKT_BOOL || type == SKT_ENUM))
    {
        p->optCnt = 0;
        p->optPool = 0;
        store_options(ctx, p, range);
    }
    else if (ctx->inListing && range && strstr(range, ".."))
    {
        char *dot = strstr(range, "..");
        char *loS, *hiS;
        *dot = 0;
        loS = range;
        hiS = dot + 2;
        while (*loS == ' ')
            loS++;
        while (*hiS == ' ')
            hiS++;
        if (type == SKT_FLOAT)
        {
            float lo = (float)strtod(loS, NULL);
            float hi = (float)strtod(hiS, NULL);
            memcpy(&p->minv, &lo, 4);
            memcpy(&p->maxv, &hi, 4);
        }
        else
        {
            p->minv = (uint32_t)strtol(loS, NULL, 10);
            p->maxv = (uint32_t)strtol(hiS, NULL, 10);
        }
    }

    proto_rusify(name);
    proto_rusify(valBuf);

    if (ctx->inListing)
    {
        strncpy(p->name, name, SKE_NAME_LEN - 1);
        p->name[SKE_NAME_LEN - 1] = 0;
        p->readOnly = ro;
        p->section = ctx->curSection;
        p->groupLvl = owner;
        p->group = (owner < sizeof(ctx->hdrName)) ? ctx->hdrName[owner]
                                                  : ctx->curGroup;
        /* the owning L2 menu comes from the header walk (parents precede
         * children there); parameter order alone cannot recover it */
        p->tab2 = (owner >= 2) ? ctx->hdrName[2] : 0;
    }
    else
        p->readOnly = ro;

    p->present = (proto_type_size(type) > 0);
    if (!strncmp(valBuf, "******", 6))
        p->masked = true; /* password U32s are always shown masked */
    else
    {
        p->masked = false;
        if (type == SKT_BOOL || type == SKT_ENUM)
        {
            /* the display value is the option label: match it back to
             * the index through the (rusified) pool */
            uint32_t v = p->value;
            char tmp[44];
            bool matched = false;
            if (p->optCnt)
            {
                for (k = 0; k < p->optCnt; k++)
                {
                    const char *o = proto_opt_text(ctx, p, k);
                    if (!o)
                        break;
                    strncpy(tmp, o, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = 0;
                    proto_rusify(tmp); /* pool entries are pre-rusified; idempotent */
                    if (!strcmp(tmp, valBuf))
                    {
                        v = k;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched || !p->optCnt)
                p->value = p->optCnt ? v : (uint32_t)strtoul(valBuf, NULL, 10);
        }
        else
            p->value = text_to_raw(p, valBuf);
    }
    p->updated = ctx->nowMs;
}

/* ------------------------------------------------------------------ */
/* live values ('m'): MeterData_t as three CSV lines, prompt-framed    */

void proto_values_restart(ProtoCtx *ctx)
{
    ctx->valsLine = 0;
}

bool proto_values_ready(const ProtoCtx *ctx)
{
    return ctx->valsLine == 3;
}

static bool vals_next(char **pp, char *field, size_t cap)
{
    char *c;
    size_t n;
    if (!*pp || !**pp)
        return false;
    c = strchr(*pp, ',');
    n = c ? (size_t)(c - *pp) : strlen(*pp);
    if (n >= cap)
        n = cap - 1;
    memcpy(field, *pp, n);
    field[n] = 0;
    *pp = c ? c + 1 : NULL;
    return true;
}

static float vals_float(char **pp, char *field, size_t cap)
{
    return vals_next(pp, field, cap) ? (float)strtod(field, NULL) : 0.0f;
}

static uint32_t vals_ulong(char **pp, char *field, size_t cap, int base)
{
    return vals_next(pp, field, cap) ? strtoul(field, NULL, base) : 0;
}

void proto_values_line(ProtoCtx *ctx, char *line)
{
    char f[24];
    char *p;
    SkeValues *v = &ctx->vals;

    switch (ctx->valsLine)
    {
    case 0: /* m,frequency,rate_raw,rate_fast,rateMLPM,rate */
        if (strncmp(line, "m,", 2))
            return; /* console chatter */
        p = line + 2;
        v->frequency = vals_float(&p, f, sizeof(f));
        v->rate_raw = vals_float(&p, f, sizeof(f));
        v->rate_fast = vals_float(&p, f, sizeof(f));
        v->rateMLPM = vals_float(&p, f, sizeof(f));
        v->rate = vals_float(&p, f, sizeof(f));
        ctx->valsLine = 1;
        break;
    case 1: /* totals, 8 fields */
        p = line;
        v->total_plus = vals_float(&p, f, sizeof(f));
        v->total_minus = vals_float(&p, f, sizeof(f));
        v->total = vals_float(&p, f, sizeof(f));
        v->total_sum = vals_float(&p, f, sizeof(f));
        v->totalml_plus = vals_float(&p, f, sizeof(f));
        v->totalml_minus = vals_float(&p, f, sizeof(f));
        v->gtotal = vals_float(&p, f, sizeof(f));
        v->gtotalml = vals_float(&p, f, sizeof(f));
        ctx->valsLine = 2;
        break;
    case 2: /* kf,pulses_packet,pulses,batch,status,setpoint,isr (%02X) */
        p = line;
        v->kf_value = vals_float(&p, f, sizeof(f));
        v->pulses_packet = vals_ulong(&p, f, sizeof(f), 10);
        v->pulses = vals_ulong(&p, f, sizeof(f), 10);
        v->batch = vals_float(&p, f, sizeof(f));
        v->status = (uint8_t)vals_ulong(&p, f, sizeof(f), 16);
        v->setpoint = (uint8_t)vals_ulong(&p, f, sizeof(f), 16);
        v->isr = (uint8_t)vals_ulong(&p, f, sizeof(f), 16);
        ctx->valsLine = 3; /* frame complete */
        v->updated = ctx->nowMs;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* prompt matcher                                                      */

void proto_prompt_reset(ProtoCtx *ctx)
{
    ctx->promptPos = 0;
}

bool proto_prompt_feed(ProtoCtx *ctx, char c)
{
    if (SKE_PROMPT[ctx->promptPos] == c)
    {
        ctx->promptPos++;
        if (SKE_PROMPT[ctx->promptPos] == 0)
        {
            ctx->promptPos = 0;
            return true;
        }
    }
    else
    {
        ctx->promptPos = (c == SKE_PROMPT[0]) ? 1 : 0;
    }
    return false;
}
