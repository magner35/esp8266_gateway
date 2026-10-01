/*
 * Host test bench for the gateway's TEXT console parser: the REAL
 * parseParamLine/rusifyText machinery from src/ske02.cpp is mirrored
 * here with small stubs, then fed with the captured device listing.
 * Compile (MSVC): tools\run_parser_test.bat
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define DBG(...) printf(__VA_ARGS__)
static void yield() {}

enum { SKT_FLOAT = 0, SKT_U32, SKT_TIME, SKT_DATE, SKT_U8, SKT_I8, SKT_H8,
       SKT_U16, SKT_H16, SKT_I16, SKT_BOOL, SKT_ENUM, SKT_STRING, SKT_STRINGP };
enum { BS_OK = 0, BS_BAD_ID, BS_BAD_TYPE, BS_READONLY };
#define SKE_MAX_PARAMS 192
#define SKE_NAME_LEN 44
#define SKE_MENU_LEN 44
#define SKE_MAX_MENUS 40
#define SKE_MAX_SECTIONS 12
#define SKE_TXT_LINE 208
#define SKE_OPT_POOL 3584
#define SKE_OPT_MAX 16

struct SkeParam
{
    char name[SKE_NAME_LEN];
    uint32_t value, minv, maxv, updated;
    uint16_t optPool;
    uint8_t optCnt, type, section, group, groupLvl, tab2;
    bool present, readOnly;
};

static SkeParam sParams[SKE_MAX_PARAMS];
static char sMenus[SKE_MAX_MENUS][SKE_MENU_LEN];
static uint16_t sMenuCount;
static char sOptPool[SKE_OPT_POOL];
static uint16_t sOptPoolUsed;
static uint8_t sCurSection, sCurGroup;
static uint8_t sHdrName[9];
static uint8_t sSectionIds[SKE_MAX_SECTIONS];
static uint8_t sSectionNum;
static bool sInListing = true;
static char sTokBuf[8];

static uint8_t typeByToken(const char *tok)
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
    return 0xFF;
}

static uint8_t skeTypeSize(uint8_t type)
{
    static const uint8_t sz[14] = { 4, 4, 4, 4, 1, 1, 1, 2, 2, 2, 1, 1, 0, 0 };
    return (type < 14) ? sz[type] : 0;
}

static void civilFromDays(int32_t days, int16_t *y, uint8_t *m, uint8_t *d)
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

static int32_t daysFromCivil(int16_t y, uint8_t m, uint8_t d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static uint8_t addNamed(const char *name)
{
    for (uint16_t i = 0; i < sMenuCount; i++)
        if (!strncmp(sMenus[i], name, SKE_MENU_LEN))
            return (uint8_t)(i + 1);
    if (sMenuCount >= SKE_MAX_MENUS)
        return 0;
    strncpy(sMenus[sMenuCount], name, SKE_MENU_LEN - 1);
    sMenus[sMenuCount][SKE_MENU_LEN - 1] = 0;
    sMenuCount++;
    return (uint8_t)sMenuCount;
}

static const struct { char lat; uint16_t cp; } sHomoglyph[] = {
    { 'A', 0x0410 }, { 'B', 0x0412 }, { 'E', 0x0415 }, { 'K', 0x041A },
    { 'M', 0x041C }, { 'H', 0x041D }, { 'O', 0x041E }, { 'P', 0x0420 },
    { 'C', 0x0421 }, { 'T', 0x0422 }, { 'X', 0x0425 }, { 'b', 0x042C },
    { 'a', 0x0430 }, { 'e', 0x0435 }, { 'o', 0x043E }, { 'p', 0x0440 },
    { 'c', 0x0441 }, { 'y', 0x0443 }, { 'x', 0x0445 },
};

static bool rusWordByte(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u >= 0x80;
}

static void rusifyText(char *s)
{
    static char buf[SKE_TXT_LINE];
    size_t len = strlen(s);
    size_t in = 0, out = 0;
    while (in < len && out < sizeof(buf) - 3)
    {
        if (!rusWordByte(s[in]))
        {
            buf[out++] = s[in++];
            continue;
        }
        size_t start = in;
        bool hasCyr = false;
        while (in < len && rusWordByte(s[in]))
        {
            unsigned char u = (unsigned char)s[in];
            if (u == 0xD0 || u == 0xD1)
                hasCyr = true;
            in++;
        }
        for (size_t i = start; i < in && out < sizeof(buf) - 3; i++)
        {
            char ch = s[i];
            uint16_t cp = 0;
            if (hasCyr)
                for (uint8_t k = 0; k < sizeof(sHomoglyph) / sizeof(sHomoglyph[0]); k++)
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
    buf[out] = 0;
    strcpy(s, buf);
}

static const char *skeOptText(const SkeParam *p, uint8_t idx)
{
    if (!p || !p->optCnt || idx >= p->optCnt || !p->optPool)
        return NULL;
    const char *s = sOptPool + p->optPool - 1;
    while (idx--)
        s += strlen(s) + 1;
    return s;
}

int8_t skeSectionIndexOf(uint8_t poolId)
{
    for (uint8_t i = 0; i < sSectionNum; i++)
        if (sSectionIds[i] == poolId)
            return (int8_t)i;
    return -1;
}

static uint32_t textToRaw(SkeParam *p, const char *text)
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
            return (uint32_t)daysFromCivil((int16_t)(2000 + y), (uint8_t)m,
                                           (uint8_t)d) * 86400UL + tod;
        }
        return p->value;
    }
    case SKT_BOOL:
    case SKT_ENUM:
    {
        if (!p->optCnt)
            return (uint32_t)strtoul(text, NULL, 10);
        char tmp[44];
        for (uint8_t k = 0; k < p->optCnt; k++)
        {
            const char *o = skeOptText(p, k);
            if (!o)
                break;
            strncpy(tmp, o, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            rusifyText(tmp);
            if (!strcmp(tmp, text))
                return k;
        }
        return p->value;
    }
    default:
        return (uint32_t)strtol(text, NULL, 10);
    }
}

/* verbatim mirror of the gateway parseParamLine */
static void parseParamLine(char *s)
{
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
    if (!len)
        return;

    if (s[0] == '[' && s[1] == 'L')
    {
        char *path = strchr(s, ']');
        if (!path || !sInListing)
            return;
        uint8_t lvl = 0;
        for (char *p = s + 2; p < path; p++)
            if (*p >= '0' && *p <= '9')
                lvl = (uint8_t)(lvl * 10 + (*p - '0'));
        path += 1;
        while (*path == ' ')
            path++;
        rusifyText(path);
        char *last = path;
        for (char *p = path; *p; p++)
            if (*p == '/')
                last = p + 1;
        sCurGroup = addNamed(last);
        if (lvl < sizeof(sHdrName))
            sHdrName[lvl] = sCurGroup;
        char *sec = strchr(path, '/');
        if (sec && sec < last)
        {
            sec++;
            char *end = strchr(sec, '/');
            if (end)
                *end = 0;
            uint8_t id = addNamed(sec);
            sCurSection = id;
            if (id && skeSectionIndexOf(id) < 0 &&
                sSectionNum < SKE_MAX_SECTIONS)
                sSectionIds[sSectionNum++] = id;
        }
        return;
    }

    uint16_t ind = 0;
    while (s[ind] == ' ')
        ind++;
    uint8_t owner = (ind >= 2) ? (uint8_t)(ind / 2 - 1) : 0;

    char *e;
    long id = strtol(s + ind, &e, 10);
    if (e == s + ind || id < 0 || id >= SKE_MAX_PARAMS)
        return;
    while (*e == ' ')
        e++;
    char *tokEnd = strchr(e, ' ');
    if (!tokEnd)
        return;
    *tokEnd = 0;
    strncpy(sTokBuf, e, sizeof(sTokBuf) - 1);
    sTokBuf[sizeof(sTokBuf) - 1] = 0;
    uint8_t type = typeByToken(sTokBuf);
    char *name = tokEnd + 1;
    while (*name == ' ')
        name++;
    char *eq = strstr(name, " = ");
    if (!eq || type == 0xFF)
        return;

    char *tail = eq + 3;
    bool ro = false;
    if (len && s[len - 1] == '*')
    {
        ro = true;
        s[--len] = 0;
        while (len && s[len - 1] == ' ')
            s[--len] = 0;
    }
    char *range = NULL;
    char *rb = strrchr(tail, '[');
    if (rb && len && s[len - 1] == ']')
    {
        s[len - 1] = 0;
        *rb = 0;
        range = rb + 1;
    }
    char *vend = tail + strlen(tail);
    while (vend > tail && vend[-1] == ' ')
        *--vend = 0;

    *eq = 0;
    char *t = eq;
    while (t > name && t[-1] == ' ')
        *--t = 0;

    char valBuf[48];
    strncpy(valBuf, tail, sizeof(valBuf) - 1);
    valBuf[sizeof(valBuf) - 1] = 0;

    SkeParam *p = &sParams[id];
    p->type = type;

    if (sInListing && !p->optCnt && range &&
        (type == SKT_BOOL || type == SKT_ENUM))
    {
        p->optCnt = 0;
        p->optPool = 0;
        uint8_t cnt = 1;
        for (char *q = range; *q; q++)
            if (*q == '|')
                cnt++;
        if (cnt <= SKE_OPT_MAX)
        {
            uint16_t off = sOptPoolUsed;
            uint8_t stored = 0;
            char *q = range;
            while (q && *q)
            {
                char *nq = strchr(q, '|');
                if (nq)
                    *nq = 0;
                char *ws = q + strlen(q);
                while (ws > q && ws[-1] == ' ')
                    *--ws = 0;
                while (*q == ' ')
                    q++;
                char tmp[48];
                size_t l = strlen(q);
                if (l >= sizeof(tmp))
                    l = sizeof(tmp) - 1;
                memcpy(tmp, q, l);
                tmp[l] = 0;
                rusifyText(tmp);
                if (sOptPoolUsed + strlen(tmp) + 1 > SKE_OPT_POOL)
                    break;
                strcpy(sOptPool + sOptPoolUsed, tmp);
                sOptPoolUsed += strlen(tmp) + 1;
                stored++;
                q = nq ? nq + 1 : NULL;
            }
            if (stored == cnt)
            {
                p->optPool = (uint16_t)(off + 1);
                p->optCnt = cnt;
            }
            else
            {
                sOptPoolUsed = off;
            }
        }
    }
    else if (sInListing && range && strstr(range, ".."))
    {
        char *dot = strstr(range, "..");
        *dot = 0;
        char *loS = range;
        char *hiS = dot + 2;
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

    rusifyText(name);
    rusifyText(valBuf);

    if (sInListing)
    {
        strncpy(p->name, name, SKE_NAME_LEN - 1);
        p->name[SKE_NAME_LEN - 1] = 0;
        p->readOnly = ro;
        p->section = sCurSection;
        p->groupLvl = owner;
        p->group = (owner < sizeof(sHdrName)) ? sHdrName[owner] : sCurGroup;
        p->tab2 = (owner >= 2) ? sHdrName[2] : 0;
    }

    p->present = (skeTypeSize(type) > 0);
    p->value = textToRaw(p, valBuf);
    p->updated = 1;
}

int fails = 0;
#define CHECK(cond, ...)                            \
    do                                              \
    {                                               \
        if (!(cond))                                \
        {                                           \
            fails++;                                \
            printf("FAIL: " __VA_ARGS__);           \
            printf("\n");                           \
        }                                           \
    } while (0)

static float asF(uint32_t raw) { float f; memcpy(&f, &raw, 4); return f; }

int main(int argc, char **argv)
{
    /* one-line debug of the range extraction */
    {
        char probe[SKE_TXT_LINE];
        strcpy(probe, "      000 B    Tип = Цeнa имп. [K-фaктop|Цeнa имп.]");
        parseParamLine(probe);
        printf("probe: opts=%u v=%u name='%s'\n",
               sParams[0].optCnt, sParams[0].value, sParams[0].name);
    }

    const char *file = (argc > 1) ? argv[1] : "tools/ske_l.txt";
    FILE *f = fopen(file, "rb");
    if (!f)
    {
        printf("cannot open %s\n", file);
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char work[SKE_TXT_LINE];
        strncpy(work, line, SKE_TXT_LINE - 1);
        work[SKE_TXT_LINE - 1] = 0;
        parseParamLine(work);
    }
    fclose(f);

    for (int dbg = 0; dbg < 4; dbg++)
        printf("[p%d] type=%u opts=%u pool=%u ro=%d v=%u name='%s'\n",
               dbg, sParams[dbg].type, sParams[dbg].optCnt,
               sParams[dbg].optPool, (int)sParams[dbg].readOnly,
               sParams[dbg].value, sParams[dbg].name);
    {
        char out[48] = { 0 };
        const char *q = sOptPool;
        int n = 0;
        while (q < sOptPool + sOptPoolUsed && n < 6)
        {
            printf("[pool] '%s'\n", q);
            q += strlen(q) + 1;
            n++;
        }
        (void)out;
    }

    printf("menus=%u sections=%u poolUsed=%u\n",
           sMenuCount, sSectionNum, sOptPoolUsed);
    CHECK(sSectionNum == 4, "sections=%u", sSectionNum);
    CHECK(sMenuCount >= 33, "menus=%u", sMenuCount);

    /* names */
    CHECK(!strcmp(sParams[0].name, "Тип"), "p0 name='%s'", sParams[0].name);
    CHECK(!strcmp(sParams[2].name, "Значение"), "p2 name");
    int empty = 0;
    for (int i = 0; i < 177; i++)
        if (!sParams[i].name[0])
            empty++;
    CHECK(empty == 0, "empty names=%d", empty);

    /* full long submenu names */
    bool full = false;
    for (uint16_t i = 0; i < sMenuCount; i++)
        if (strstr(sMenus[i],
                   "\xd0\xbd\xd0\xb0\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbb"
                   "\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5"))
            full = true;
    CHECK(full, "full 'направление' submenu");

    /* types */
    CHECK(sParams[0].type == SKT_BOOL, "p0 type=%u", sParams[0].type);
    CHECK(sParams[1].type == SKT_ENUM, "p1 type");
    CHECK(sParams[2].type == SKT_FLOAT, "p2 type");
    CHECK(sParams[152].type == SKT_DATE, "p152 type");
    CHECK(sParams[153].type == SKT_TIME, "p153 type");
    CHECK(sParams[154].type == SKT_I8, "p154 type");

    /* options */
    CHECK(sParams[0].optCnt == 2, "p0 opts=%u", sParams[0].optCnt);
    CHECK(sParams[1].optCnt == 3, "p1 opts=%u", sParams[1].optCnt);
    const char *o = skeOptText(&sParams[0], 0);
    CHECK(o && !strcmp(o, "K-фактор"), "p0 opt0='%s'", o ? o : "(null)");
    o = skeOptText(&sParams[0], 1);
    CHECK(o && !strcmp(o, "Цена имп."), "p0 opt1='%s'", o ? o : "(null)");
    o = skeOptText(&sParams[1], 2);
    CHECK(o && !strcmp(o, "Метр куб."), "p1 opt2='%s'", o ? o : "(null)");
    CHECK(sOptPoolUsed < SKE_OPT_POOL, "pool overflow");
    int withOpts = 0;
    for (int i = 0; i < 177; i++)
        if (sParams[i].optCnt)
            withOpts++;
    CHECK(withOpts >= 60, "enums with options=%d", withOpts);

    /* numeric ranges */
    CHECK(sParams[43].minv == 3 && sParams[43].maxv == 10, "p43 range");
    CHECK(asF(sParams[2].maxv) > 999999.0f && asF(sParams[2].maxv) < 1000000.0f,
          "p2 hi=%f", asF(sParams[2].maxv));
    CHECK((int32_t)sParams[154].minv == -60 && (int32_t)sParams[154].maxv == 60,
          "p154 range");

    /* values */
    CHECK(sParams[0].value == 1, "p0 value=%u (Цена имп.)", sParams[0].value);
    CHECK(asF(sParams[2].value) == 1.0f, "p2 value");
    CHECK(sParams[43].value == 6, "p43 value=%u", sParams[43].value);
    CHECK(sParams[152].value == 946684800UL, "p152 date=%u", sParams[152].value);
    CHECK(sParams[153].value == 3 * 3600UL, "p153 time=%u", sParams[153].value);

    /* read-only marker */
    CHECK(sParams[169].readOnly, "p169 ro"); /* Изм.CRC view */
    CHECK(!sParams[43].readOnly, "p43 not ro");

    /* poll regression: parse the whole listing a SECOND time (the
     * periodic poll re-issues 'l') - options must survive intact */
    sInListing = true;
    f = fopen(file, "rb");
    while (fgets(line, sizeof(line), f))
    {
        char work[SKE_TXT_LINE];
        strncpy(work, line, SKE_TXT_LINE - 1);
        work[SKE_TXT_LINE - 1] = 0;
        parseParamLine(work);
    }
    fclose(f);
    CHECK(sParams[0].optCnt == 2 && sParams[0].value == 1,
          "2nd pass p0 opts=%u v=%u", sParams[0].optCnt, sParams[0].value);
    CHECK(sParams[1].optCnt == 3, "2nd pass p1 opts=%u", sParams[1].optCnt);
    {
        int wo = 0;
        for (int i = 0; i < 177; i++)
            if (sParams[i].optCnt)
                wo++;
        CHECK(wo == withOpts, "2nd pass enums with options=%d (was %d)",
              wo, withOpts);
    }
    CHECK(sOptPoolUsed < SKE_OPT_POOL, "2nd pass pool=%u", sOptPoolUsed);

    printf(fails ? "%d CHECK(s) FAILED\n" : "ALL OK (%d failures)\n", fails);
    return fails ? 1 : 0;
}
