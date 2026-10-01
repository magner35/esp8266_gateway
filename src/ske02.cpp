#include "ske02.h"
#include "debug.h"
#include "storage.h"

/*
 * Client of the SKE-02 service console, TEXT protocol only (the binary
 * protocol was removed from the firmware). See include/console.h of the
 * firmware repository:
 *
 *   commands  ? l g s w d i e r, CR/LF terminated
 *   every reply ends with the prompt "SKE02> " - the transaction marker
 *   (no prompt after the 'r' reboot reply)
 *   l         hierarchical listing: "[L#] Root/Sub" headers and indented
 *             "<id> <type> <name> = <value> [range]" lines; range is
 *             [min..max] or the [v1|v2|..] enum/bool option list
 *   g <id>    one parameter line, same format, no indent
 *   s <id> v  "OK " + the resulting line, or "ERR id|value|type|read only";
 *             values are numbers only (TIME/DATE = raw unix seconds)
 *   i         model/fw/serial/params/freeheap/ticks
 *   e         toggle local echo (off is what a host tool wants)
 *
 * Discovery: wake ('e' until "echo off", then 'i' for the version and
 * the parameter count) -> one full 'l' capture (names, menu structure,
 * types, ranges, options AND the values themselves). Steady state: the
 * listing is re-issued every poll period - one command refreshes every
 * value; the web/modbus serving pauses while it streams (skeCapturing).
 */

#define SKE_TXT_LINE 208     /* worst header: [L#] + 4 Cyrillic menu names
                              * (4*41+3 = 167); long enum range lists may
                              * still truncate - only their tail is lost */
#define SKE_TXT_PROMPT "SKE02> "
#define SKE_LIST_TIMEOUT_MS 15000
#define SKE_LIST_QUIET_MS 900
#define SKE_CMD_TIMEOUT_MS 1200
#define SKE_WAKE_PERIOD_MS 1000
#define SKE_WAKE_MISS 3
#define SKE_POLL_MISS 3
#define SKE_POLL_MIN_MS 5000UL /* a full listing takes >1 s on the wire;
                                * keep the serving pauses sparse */

enum
{
    ST_WAKE,   /* no console yet: echo off + 'i' until it answers */
    ST_LIST,   /* capturing the 'l' listing (first time or a poll) */
    ST_POLL    /* steady state: periodic re-listing */
};

static SkeParam sParams[SKE_MAX_PARAMS];
static char sMenus[SKE_MAX_MENUS][SKE_MENU_LEN]; /* L1 sections + submenus */
static uint16_t sMenuCount;
static char sOptPool[SKE_OPT_POOL];
static uint16_t sOptPoolUsed;
static uint16_t sCount;        /* parameter count from 'i', capped */
static char sVersion[20];
static uint32_t sLastOk;
static uint8_t sState;
static bool sReady;
static uint32_t sT;            /* timestamp of the last step */
static uint8_t sMisses;

/* listing capture (streamed, prompt-terminated) */
static bool sTxtActive;
static uint32_t sTxtDeadline, sTxtQuiet;
static char sLine[SKE_TXT_LINE];
static uint16_t sLinePos;
static bool sInListing;        /* menu fields are only valid while the
                                * hierarchical listing is being parsed */

/* menu structure tracking while parsing */
static uint8_t sCurSection;
static uint8_t sCurGroup;
static uint8_t sHdrName[9];    /* pool id of the last header per level */
static uint8_t sSectionIds[SKE_MAX_SECTIONS];
static uint8_t sSectionNum;

#if GW_SKE_ENABLED
#define SKE_UNUSED
#else
#define SKE_UNUSED __attribute__((unused)) /* bench build: device code is dead */
#endif

/* ------------------------------------------------------------------ */
/* low level uart + prompt scanner                                     */

static void skeFlushRx(void)
{
    while (Serial.available())
        Serial.read();
}

static bool skeExpired(uint32_t deadline)
{
    return (int32_t)(millis() - deadline) >= 0;
}

/* incremental matcher for the "SKE02> " prompt marker */
static uint8_t sPromptPos;

static void promptReset(void)
{
    sPromptPos = 0;
}

static bool SKE_UNUSED promptFeed(char c)
{
    if (SKE_TXT_PROMPT[sPromptPos] == c)
    {
        sPromptPos++;
        if (SKE_TXT_PROMPT[sPromptPos] == 0)
        {
            sPromptPos = 0;
            return true;
        }
    }
    else
    {
        sPromptPos = (c == SKE_TXT_PROMPT[0]) ? 1 : 0;
    }
    return false;
}

/*
 * Blocking line reader until the prompt (or a deadline). Every assembled
 * line goes to cb. Echo, if still on, produces harmless extra lines.
 */
typedef void (*skeLineCb)(char *line);

static bool SKE_UNUSED txtReadUntil(skeLineCb cb, uint32_t deadline)
{
    char line[SKE_TXT_LINE];
    uint16_t pos = 0;
    promptReset();
    while (!skeExpired(deadline))
    {
        while (Serial.available())
        {
            char c = (char)Serial.read();
            if (promptFeed(c))
                return true;
            if (c == '\n' || c == '\r')
            {
                if (pos)
                {
                    line[pos] = 0;
                    if (cb)
                        cb(line);
                    pos = 0;
                }
            }
            else if (pos < sizeof(line) - 1)
            {
                line[pos++] = c;
            }
        }
        yield();
    }
    return false;
}

static bool SKE_UNUSED txtCommand(const char *cmd, skeLineCb cb, uint16_t timeoutMs)
{
    skeFlushRx();
    Serial.print(cmd);
    Serial.write('\r');
    Serial.flush();
    return txtReadUntil(cb, millis() + timeoutMs);
}

/* ------------------------------------------------------------------ */
/* cache helpers                                                       */

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

uint8_t skeTypeSize(uint8_t type)
{
    static const uint8_t sz[14] = { 4, 4, 4, 4, 1, 1, 1, 2, 2, 2, 1, 1, 0, 0 };
    return (type < 14) ? sz[type] : 0;
}

/* days <-> civil date (Howard Hinnant's algorithms, TZ-free); the meter
 * stores TIME/DATE as unix seconds and edits only part of the value */
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

static bool rusWordByte(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u >= 0x80;
}

/* in place; the buffer must have room for the worst-case expansion
 * (every letter of a Cyrillic word turning into a 2-byte sequence) */
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

/* ------------------------------------------------------------------ */
/* the parameter line parser (shared by 'l' and 'g')                  */
/*
 *   [L1] Root/Sub                      header
 *         007 U8  Name = 42 [3..10]    param: indent = owner level + 1
 *         008 E   Mode = On [Off|On]
 *         009 U8  View = 5 *           read only views end with " *"
 */

static char sTokBuf[8];

/* display value -> raw u32; the date/time-of-day part keeps the cached one */
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
            if (!strncmp(tmp, text, sizeof(tmp)))
                return k;
        }
        return p->value;
    }
    default:
        return (uint32_t)strtol(text, NULL, 10);
    }
}

/* parse and apply one "<id> <type> <name> = <value> [range] [*]" line */
static void parseParamLine(char *s)
{
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
    if (!len)
        return;

    if (s[0] == '[' && s[1] == 'L') /* submenu header [L#] Root/Sub/.. */
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
        rusifyText(path); /* menu names arrive as a Latin-Cyrillic mix */
        char *last = path;
        for (char *p = path; *p; p++)
            if (*p == '/')
                last = p + 1;
        sCurGroup = addNamed(last);
        if (lvl < sizeof(sHdrName))
            sHdrName[lvl] = sCurGroup;

        /* the L1 section is strictly the SECOND path component */
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

    /*
     * The indent is the authoritative owner: params print at walk level
     * (owner + 1), 2 spaces per level. A parent-level param can follow a
     * deeper subtree without any new header, so the last header alone
     * would misattribute it. 'g' replies have no indent (owner 0 level
     * info is irrelevant - the cache already knows the menu structure).
     */
    uint16_t ind = 0;
    while (s[ind] == ' ')
        ind++;
    uint8_t owner = (ind >= 2) ? (uint8_t)(ind / 2 - 1) : 0;

    char *e;
    long id = strtol(s + ind, &e, 10);
    if (e == s + ind || id < 0 || id >= SKE_MAX_PARAMS)
        return; /* not a param line */
    while (*e == ' ')
        e++;
    char *tokEnd = strchr(e, ' '); /* end of the type token */
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

    /* value+range tail; read only views end with " *" (len = entry length,
     * the live strlen(s) is already cut at the type token) */
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
    char *rb = strrchr(tail, '['); /* options/range at the line end */
    /* NB: *tokEnd = 0 above already cut s at the type token, so the live
     * strlen(s) is useless here - use the length captured at entry */
    if (rb && len && s[len - 1] == ']')
    {
        s[len - 1] = 0; /* drop the closing bracket first, then split */
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

    /*
     * The value text is copied out FIRST: rusifyText() expands strings
     * in place and would run over the range/option bytes further along
     * the line. The option list is parsed from the raw range too, with
     * each label rusified into the pool on its own.
     */
    char valBuf[48];
    strncpy(valBuf, tail, sizeof(valBuf) - 1);
    valBuf[sizeof(valBuf) - 1] = 0;

    SkeParam *p = &sParams[id];
    p->type = type;

    /*
     * Options live in the meter's flash: store them on the FIRST listing
     * that brings them. Re-listings (the periodic poll) only refresh
     * values - re-storing would append pool duplicates until overflow.
     */
    if (sInListing && !p->optCnt && range &&
        (type == SKT_BOOL || type == SKT_ENUM))
    {
        /* option list [v1|v2|..]: all-or-nothing into the pool */
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
                p->optPool = (uint16_t)(off + 1); /* 0 = "no options" */
                p->optCnt = cnt;
            }
            else
            {
                sOptPoolUsed = off; /* roll the partial list back */
            }
        }
    }
    else if (sInListing && range && strstr(range, ".."))
    {
        /* numeric range [min..max] */
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
        /*
         * The owning L2 menu comes from the header walk (parents precede
         * children there); parameter order alone cannot recover it, because
         * the firmware prints each submenu's subtrees before its own params.
         */
        p->tab2 = (owner >= 2) ? sHdrName[2] : 0;
    }

    p->present = (skeTypeSize(type) > 0);
    p->value = textToRaw(p, valBuf);
    p->updated = millis();
}

/* ------------------------------------------------------------------ */
/* text commands                                                       */

static char sInfoLine[96];

static void infoLineCb(char *line)
{
    /* keep the last "serial ... params N ..." line */
    if (!strncmp(line, "serial", 6))
    {
        strncpy(sInfoLine, line, sizeof(sInfoLine) - 1);
        sInfoLine[sizeof(sInfoLine) - 1] = 0;
    }
    else if (!strncmp(line, "model", 5))
    {
        /* "model SKE02 fw 3425 hw X" -> "SKE02/3425" */
        char model[8] = { 0 };
        char fw[8] = { 0 };
        char *m = line + 5;
        while (*m == ' ')
            m++;
        char *w = strchr(m, ' ');
        if (w)
            *w = 0;
        strncpy(model, m, sizeof(model) - 1);
        char *f = w ? strstr(w + 1, "fw ") : NULL;
        if (f)
        {
            f += 3;
            char *we = strchr(f, ' ');
            if (we)
                *we = 0;
            strncpy(fw, f, sizeof(fw) - 1);
        }
        if (model[0] && fw[0])
            snprintf(sVersion, sizeof(sVersion), "%s/%s", model, fw);
    }
}

static char sEchoLast[24];

static void echoLineCb(char *line)
{
    if (!strncmp(line, "echo", 4))
    {
        strncpy(sEchoLast, line, sizeof(sEchoLast) - 1);
        sEchoLast[sizeof(sEchoLast) - 1] = 0;
    }
}

static bool SKE_UNUSED skeEchoOff(void)
{
    /* 'e' TOGGLES the echo: apply until the reply says "echo off"; the
     * leading empty line drops any half-typed line from boot noise */
    for (uint8_t attempt = 0; attempt < 3; attempt++)
    {
        sEchoLast[0] = 0;
        txtCommand("", NULL, SKE_CMD_TIMEOUT_MS);
        if (txtCommand("e", echoLineCb, SKE_CMD_TIMEOUT_MS) &&
            !strncmp(sEchoLast, "echo off", 8))
            return true;
    }
    return false;
}

static bool SKE_UNUSED skeInfo(void)
{
    sInfoLine[0] = 0;
    if (!txtCommand("i", infoLineCb, SKE_CMD_TIMEOUT_MS))
        return false;
    char *p = strstr(sInfoLine, "params");
    if (!p)
        return false;
    long cnt = strtol(p + 6, NULL, 10);
    if (cnt <= 0)
        return false;
    if (cnt > SKE_MAX_PARAMS)
        cnt = SKE_MAX_PARAMS;
    sCount = (uint16_t)cnt;
    sLastOk = millis();
    return true;
}

static void SKE_UNUSED textStart(void)
{
    skeFlushRx();
    Serial.print(F("l"));
    Serial.write('\r');
    Serial.flush();
    sTxtActive = true;
    sInListing = true;
    sTxtDeadline = millis() + SKE_LIST_TIMEOUT_MS;
    sTxtQuiet = millis() + SKE_LIST_QUIET_MS;
    sCurSection = 0;
    sCurGroup = 0;
    memset(sHdrName, 0, sizeof(sHdrName));
}

/* streamed listing capture; ends on the prompt (or quiet/deadline) */
static void SKE_UNUSED textFeed(void)
{
    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (promptFeed(c))
        {
            if (sLinePos)
            {
                sLine[sLinePos] = 0;
                parseParamLine(sLine);
                sLinePos = 0;
            }
            sTxtActive = false;
            sInListing = false;
            sLastOk = millis();
            sMisses = 0;
            if (sState == ST_LIST)
            {
                sReady = true;
                sState = ST_POLL;
                sT = millis();
                DBG("ske02: listing done (%u params)\n", (unsigned)sCount);
            }
            return;
        }
        if (c == '\n' || c == '\r')
        {
            if (sLinePos)
            {
                sLine[sLinePos] = 0;
                parseParamLine(sLine);
                sLinePos = 0;
            }
        }
        else if (sLinePos < sizeof(sLine) - 1)
        {
            sLine[sLinePos++] = c;
        }
        sTxtQuiet = millis() + SKE_LIST_QUIET_MS;
    }
    if (skeExpired(sTxtQuiet) || skeExpired(sTxtDeadline))
    {
        sTxtActive = false; /* prompt lost - the caller counts a miss */
        sInListing = false;
        DBG("ske02: listing timed out\n");
    }
}

/* 's <id> <v>': reply "OK <line>" or "ERR ..." */
static int sSetStatus; /* 0 ok, else BS_* / transport */

static void setValueCb(char *line)
{
    if (!strncmp(line, "ERR", 3))
    {
        if (strstr(line, "read only"))
            sSetStatus = BS_READONLY;
        else if (strstr(line, "id"))
            sSetStatus = BS_BAD_ID;
        else
            sSetStatus = BS_BAD_TYPE; /* value/type/syntax */
        return;
    }
    if (!strncmp(line, "OK ", 3))
        parseParamLine(line + 3); /* updates the cache incl. the value */
}

/* raw -> the textual value the console parser understands */
static void rawToTextCmd(uint8_t type, uint32_t raw, char *buf, size_t cap)
{
    if (type == SKT_FLOAT)
    {
        float f;
        memcpy(&f, &raw, 4);
        /* at most 9 significant digits per the console parser */
        dtostrf(f, 1, 6, buf);
        char *dot = strchr(buf, '.');
        if (dot)
        {
            char *end = buf + strlen(buf) - 1;
            while (end > dot && *end == '0')
                *end-- = 0;
            if (end == dot)
                *end = 0;
            int digits = 0;
            for (char *q = buf; *q; q++)
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
            strcpy(buf, "0");
        return;
    }
    ultoa(raw, buf, 10); /* ints and TIME/DATE unix seconds */
}

int skeCommitRaw(uint16_t id, uint32_t raw)
{
    SkeParam *p = skeGet(id);
    if (!p)
        return BS_BAD_ID;

    char val[24];
    rawToTextCmd(p->type, raw, val, sizeof(val));
    char cmd[SKE_NAME_LEN + 24];
    snprintf(cmd, sizeof(cmd), "s %u %s", (unsigned)id, val);
    sSetStatus = 0;
    if (!txtCommand(cmd, setValueCb, SKE_CMD_TIMEOUT_MS))
        return SKE_ERR_TRANSPORT;
    if (sSetStatus == 0)
        sLastOk = millis();
    return sSetStatus;
}

/*
 * Write from UI text. TIME is "hh:mm" and DATE is "dd.mm.yy": like the
 * device menu, only the edited part of the unix value changes (the date
 * part stays for TIME, the time-of-day stays for DATE); the console 's'
 * command itself takes the raw seconds.
 */
bool skeSetFromText(uint16_t id, const char *text, String &err)
{
    SkeParam *p = skeGet(id);
    if (!p)
    {
        err = F("нет такого параметра");
        return false;
    }
    if (!p->present || skeTypeSize(p->type) == 0)
    {
        err = F("параметр недоступен");
        return false;
    }
    if (p->readOnly)
    {
        err = F("только для чтения");
        return false;
    }
    if (!text || !*text)
    {
        err = F("пустое значение");
        return false;
    }

    uint32_t raw = 0;
    if (p->type == SKT_TIME || p->type == SKT_DATE)
    {
        int v1, v2, v3 = 0;
        uint32_t cur = p->value;
        if (p->type == SKT_TIME)
        {
            if (sscanf(text, "%d:%d", &v1, &v2) != 2 || v1 < 0 || v1 > 23 ||
                v2 < 0 || v2 > 59)
            {
                err = F("формат чч:мм");
                return false;
            }
            raw = cur - (cur % 86400UL) + (uint32_t)v1 * 3600UL + (uint32_t)v2 * 60UL;
        }
        else
        {
            if (sscanf(text, "%d.%d.%d", &v1, &v2, &v3) != 3 || v1 < 1 || v1 > 31 ||
                v2 < 1 || v2 > 12 || v3 < 0 || v3 > 99)
            {
                err = F("формат дд.мм.гг");
                return false;
            }
            raw = (uint32_t)daysFromCivil((int16_t)(2000 + v3), (uint8_t)v2,
                                          (uint8_t)v1) * 86400UL + (cur % 86400UL);
        }
    }
    else
    {
        /* the console accepts plain numbers; enum/bool = option index */
        double d = strtod(text, NULL);
        switch (p->type)
        {
        case SKT_FLOAT:
        {
            float f = (float)d;
            memcpy(&raw, &f, 4);
            break;
        }
        case SKT_U32:
            if (d < 0)
                d = 0;
            if (d > 999999)
                d = 999999;
            raw = (uint32_t)d;
            break;
        case SKT_I8:
            if (d < -128) d = -128;
            if (d > 127) d = 127;
            raw = (uint8_t)(int8_t)d;
            break;
        case SKT_I16:
            if (d < -32768) d = -32768;
            if (d > 32767) d = 32767;
            raw = (uint16_t)(int16_t)d;
            break;
        default:
            if (d < 0)
                d = 0;
            if (d > 65535)
                d = 65535;
            raw = (uint16_t)d;
            break;
        }
    }

    int rc = skeCommitRaw(id, raw);
    if (rc == SKE_ERR_TRANSPORT)
        err = F("прибор не отвечает");
    else if (rc == BS_BAD_ID)
        err = F("нет такого параметра");
    else if (rc == BS_BAD_TYPE)
        err = F("неверное значение");
    else if (rc == BS_READONLY)
        err = F("только для чтения");
    return rc == BS_OK;
}

void skeRebootDevice(void)
{
    /* 'r': "rebooting", NO prompt afterwards */
    skeFlushRx();
    Serial.print(F("r"));
    Serial.write('\r');
    Serial.flush();
    delay(200); /* let the reply drain */
    sState = ST_WAKE;
    sT = 0;
    sReady = false;
}

void skeRescan(void)
{
    memset(sParams, 0, sizeof(sParams));
    memset(sMenus, 0, sizeof(sMenus));
    memset(sOptPool, 0, sizeof(sOptPool));
    sMenuCount = 0;
    sOptPoolUsed = 0;
    sSectionNum = 0;
    sVersion[0] = 0;
    sCount = 0;
    sMisses = 0;
    sTxtActive = false;
    sState = ST_WAKE;
    sT = 0;
    sReady = false;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

void skeBegin(void)
{
#if GW_SKE_ENABLED
    Serial.setRxBufferSize(1024);
    Serial.begin(SKE_UART_BAUD);
#endif
    skeRescan();
}

void skePoll(void)
{
#if GW_SKE_ENABLED
    if (sTxtActive)
    {
        textFeed();
        return;
    }

    switch (sState)
    {
    case ST_WAKE:
        if (millis() - sT >= SKE_WAKE_PERIOD_MS)
        {
            sT = millis();
            if (skeEchoOff() && skeInfo())
            {
                DBG("ske02: %s, %u params\n", sVersion, (unsigned)sCount);
                textStart();
                sState = ST_LIST;
            }
            else if (++sMisses >= SKE_WAKE_MISS)
            {
                sMisses = 0;
            }
        }
        break;

    case ST_LIST:
        /* the capture ended without a prompt (timeout in textFeed) */
        if (++sMisses >= SKE_WAKE_MISS)
        {
            sMisses = 0;
            sState = ST_WAKE;
            sT = 0;
        }
        else
        {
            textStart(); /* retry the listing */
        }
        break;

    case ST_POLL:
    {
        uint32_t period = (uint32_t)storagePoll100ms() * 100UL;
        if (period < SKE_POLL_MIN_MS)
            period = SKE_POLL_MIN_MS;
        if (millis() - sT >= period)
        {
            sT = millis();
            textStart(); /* one command refreshes every value */
            sState = ST_LIST;
        }
        break;
    }
    }
#endif
}

bool skeLinkUp(void)
{
    return sLastOk && (millis() - sLastOk) < 15000;
}

bool skeReady(void) { return sReady; }
bool skeCapturing(void) { return sTxtActive; }
uint16_t skeCount(void) { return sCount; }
const char *skeVersion(void) { return sVersion[0] ? sVersion : "-"; }
uint32_t skeLastOkMs(void) { return sLastOk; }

SkeParam *skeGet(uint16_t id)
{
    return (id < sCount && id < SKE_MAX_PARAMS) ? &sParams[id] : NULL;
}

const char *skeSectionName(uint8_t idx)
{
    return (idx < sSectionNum) ? sMenus[sSectionIds[idx] - 1] : "";
}

int8_t skeSectionIndexOf(uint8_t poolId)
{
    for (uint8_t i = 0; i < sSectionNum; i++)
        if (sSectionIds[i] == poolId)
            return (int8_t)i;
    return -1;
}

const char *skeGroupName(uint8_t idx)
{
    return (idx >= 1 && idx <= sMenuCount) ? sMenus[idx - 1] : "";
}

uint16_t skeSectionCount(void)
{
    return sSectionNum;
}

/* format a raw value (or bound) as display text for the parameter type */
static String rawToText(uint8_t type, uint32_t raw)
{
    char buf[24];
    switch (type)
    {
    case SKT_FLOAT:
    {
        float f;
        memcpy(&f, &raw, 4);
        if (!isfinite(f))
            return String('?');
        dtostrf(f, 1, 3, buf);
        char *dot = strchr(buf, '.');
        if (dot)
        {
            char *e = buf + strlen(buf) - 1;
            while (e > dot && *e == '0')
                *e-- = 0;
            if (e == dot)
                *e = 0;
        }
        break;
    }
    case SKT_TIME:
    {
        uint32_t v = raw % 86400UL;
        snprintf(buf, sizeof(buf), "%02lu:%02lu",
                 (unsigned long)(v / 3600), (unsigned long)(v % 3600 / 60));
        break;
    }
    case SKT_DATE:
    {
        int16_t y;
        uint8_t m, d;
        civilFromDays(raw / 86400L, &y, &m, &d);
        snprintf(buf, sizeof(buf), "%02u.%02u.%02u", d, m, (uint16_t)(y % 100));
        break;
    }
    case SKT_I8:
        itoa((int8_t)raw, buf, 10);
        break;
    case SKT_I16:
        itoa((int16_t)raw, buf, 10);
        break;
    default:
        ultoa(raw, buf, 10);
        break;
    }
    return String(buf);
}

String skeValueText(uint16_t id)
{
    SkeParam *p = skeGet(id);
    if (!p)
        return String();
    if (!p->present)
        return String('-');
    /* enum/bool values are shown as their option label, like on the LCD */
    if ((p->type == SKT_ENUM || p->type == SKT_BOOL) && p->optCnt)
    {
        uint32_t v = p->value;
        const char *s = skeOptText(p, v < p->optCnt ? (uint8_t)v : 0);
        return String(s ? s : "-");
    }
    return rawToText(p->type, p->value);
}

String skeBoundText(const SkeParam *p, bool max)
{
    if (!p || (p->minv == 0 && p->maxv == 0))
        return String();
    return rawToText(p->type, max ? p->maxv : p->minv);
}

uint8_t skeOptCount(const SkeParam *p)
{
    return p ? p->optCnt : 0;
}

const char *skeOptText(const SkeParam *p, uint8_t idx)
{
    if (!p || !p->optCnt || idx >= p->optCnt || !p->optPool)
        return NULL;
    const char *s = sOptPool + p->optPool - 1;
    while (idx--)
        s += strlen(s) + 1;
    return s;
}
