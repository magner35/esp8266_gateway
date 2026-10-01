#include "ske02.h"
#include "crc8.h"
#include "debug.h"
#include "storage.h"

#define CONSOLE_SOF 0xAA
#define SKE_MAX_REPLY 160      /* 16 records * 7 bytes + headroom */
#define SKE_TXT_LINE 96        /* longest 'l' line is ~78 bytes */
#define SKE_TXT_TIMEOUT_MS 8000
#define SKE_TXT_QUIET_MS 600
#define SKE_PING_PERIOD_MS 1000
#define SKE_WAKE_RETRY_MISS 3
#define SKE_POLL_MISS 5

enum
{
    ST_WAKE,   /* device absent/just booted: PING until it answers */
    ST_NAMES,  /* capturing the text 'l' listing (names, sections, RO) */
    ST_SWEEP,  /* binary READ_MANY sweep filling types/values */
    ST_POLL    /* steady state: one READ_MANY chunk per poll period */
};

static SkeParam sParams[SKE_MAX_PARAMS];
static char sSections[SKE_MAX_SECTIONS][SKE_SECTION_LEN];
static uint16_t sSectionCount;
static uint16_t sCount;        /* parameter count from PING, capped */
static char sVersion[20];
static uint32_t sLastOk;
static uint8_t sState;
static bool sReady;
static uint32_t sT;            /* timestamp of the last step */
static uint16_t sCursor;       /* next READ_MANY start id */
static uint8_t sMisses;

/* text listing capture */
static bool sTxtActive;
static bool sTxtStarted;
static uint32_t sTxtDeadline, sTxtQuiet;
static char sLine[SKE_TXT_LINE];
static uint16_t sLinePos;
static uint8_t sCurSection;

#if GW_SKE_ENABLED
#define SKE_UNUSED
#else
#define SKE_UNUSED __attribute__((unused)) /* bench build: device code is dead */
#endif

/* ------------------------------------------------------------------ */
/* low level framing                                                   */

static void skeFlushRx(void)
{
    while (Serial.available())
        Serial.read();
}

static bool skeExpired(uint32_t deadline)
{
    return (int32_t)(millis() - deadline) >= 0;
}

static int skeRxByte(uint32_t deadline)
{
    while (!skeExpired(deadline))
    {
        if (Serial.available())
            return Serial.read();
        yield();
    }
    return -1;
}

/* Read one reply frame for cmd/id; skips stray bytes before SOF. */
static bool SKE_UNUSED skeReadFrame(uint8_t cmd, uint16_t id, uint8_t *rx, uint8_t *rxLen,
                                    uint32_t deadline)
{
    int c = -1;
    while (!skeExpired(deadline))
    {
        c = skeRxByte(deadline);
        if (c < 0)
            return false;
        if (c == CONSOLE_SOF)
            break;
    }
    if (c != CONSOLE_SOF)
        return false;

    uint8_t hdr[4];
    for (uint8_t i = 0; i < 4; i++)
    {
        c = skeRxByte(deadline);
        if (c < 0)
            return false;
        hdr[i] = (uint8_t)c;
    }
    if (hdr[0] != (uint8_t)(cmd | 0x80))
        return false;
    if ((uint16_t)(hdr[1] | (hdr[2] << 8)) != id)
        return false;

    uint8_t len = hdr[3];
    if (len > SKE_MAX_REPLY)
        return false;
    for (uint8_t i = 0; i < len; i++)
    {
        c = skeRxByte(deadline);
        if (c < 0)
            return false;
        rx[i] = (uint8_t)c;
    }
    c = skeRxByte(deadline);
    if (c < 0)
        return false;
    if (gwCrc8Init(gwCrc8(hdr, 4), rx, len) != (uint8_t)c)
        return false;

    *rxLen = len;
    return true;
}

static bool skeTransact(uint8_t cmd, uint16_t id, const uint8_t *pl, uint8_t plen,
                        uint8_t *rx, uint8_t *rxLen, uint8_t retries)
{
#if !GW_SKE_ENABLED
    (void)rx;
    (void)rxLen;
    return false; /* bench build: no meter link, fail fast */
#else
    for (uint8_t attempt = 0; attempt <= retries; attempt++)
    {
        if (attempt)
            delay(60); /* let the device console task catch up */
        skeFlushRx();

        uint8_t head[5] = { CONSOLE_SOF, cmd, (uint8_t)id, (uint8_t)(id >> 8), plen };
        Serial.write(head, 5);
        if (plen)
            Serial.write(pl, plen);
        uint8_t crc = gwCrc8Init(gwCrc8(head + 1, 4), pl, plen);
        Serial.write(crc);
        Serial.flush();

        uint32_t deadline = millis() + SKE_TIMEOUT_MS;
        if (skeReadFrame(cmd, id, rx, rxLen, deadline))
            return true;
    }
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* cache helpers                                                       */

uint8_t skeTypeSize(uint8_t type)
{
    static const uint8_t sz[14] = { 4, 4, 4, 4, 1, 1, 1, 2, 2, 2, 1, 1, 0, 0 };
    return (type < 14) ? sz[type] : 0;
}

static uint8_t valuePayload(uint8_t type, uint32_t raw, uint8_t out[4])
{
    switch (type)
    {
    case SKT_FLOAT:
    case SKT_U32:
    case SKT_TIME:
    case SKT_DATE:
        out[0] = (uint8_t)raw;
        out[1] = (uint8_t)(raw >> 8);
        out[2] = (uint8_t)(raw >> 16);
        out[3] = (uint8_t)(raw >> 24);
        return 4;
    case SKT_U16:
    case SKT_H16:
    case SKT_I16:
        out[0] = (uint8_t)raw;
        out[1] = (uint8_t)(raw >> 8);
        return 2;
    default:
        out[0] = (uint8_t)raw;
        return 1;
    }
}

static uint8_t addSection(const char *name)
{
    for (uint16_t i = 0; i < sSectionCount; i++)
        if (!strncmp(sSections[i], name, SKE_SECTION_LEN))
            return (uint8_t)(i + 1);
    if (sSectionCount >= SKE_MAX_SECTIONS)
        return 0;
    strncpy(sSections[sSectionCount], name, SKE_SECTION_LEN - 1);
    sSections[sSectionCount][SKE_SECTION_LEN - 1] = 0;
    sSectionCount++;
    return (uint8_t)sSectionCount;
}

/* ------------------------------------------------------------------ */
/* binary commands                                                     */

static bool SKE_UNUSED skePing(void)
{
    uint8_t rx[32];
    uint8_t n = 0;
    if (!skeTransact(BC_PING, 0, NULL, 0, rx, &n, 1))
        return false;
    if (n < 2)
        return false;

    uint16_t cnt = (uint16_t)(rx[n - 2] | (rx[n - 1] << 8));
    if (cnt > SKE_MAX_PARAMS)
    {
        DBG("ske02: %u params exceed the cache, capping to %u\n",
            (unsigned)cnt, (unsigned)SKE_MAX_PARAMS);
        cnt = SKE_MAX_PARAMS;
    }
    size_t vl = n - 2;
    if (vl >= sizeof(sVersion))
        vl = sizeof(sVersion) - 1;
    memcpy(sVersion, rx, vl);
    sVersion[vl] = 0;

    sCount = cnt;
    sLastOk = millis();
    return true;
}

/* One READ_MANY chunk; returns the id of the last parsed record. */
static bool SKE_UNUSED skeReadMany(uint16_t start, uint16_t *lastId)
{
    uint8_t pl = SKE_CHUNK;
    uint8_t rx[SKE_MAX_REPLY];
    uint8_t n = 0;
    if (!skeTransact(BC_READ_MANY, start, &pl, 1, rx, &n, 2))
        return false;
    if (!n || rx[0] != BS_OK)
        return false;

    uint8_t i = 1;
    while (i + 3 <= n)
    {
        uint16_t rid = (uint16_t)(rx[i] | (rx[i + 1] << 8));
        uint8_t t = rx[i + 2];
        uint8_t sz = skeTypeSize(t);
        if (i + 3 + sz > n)
            break; /* desync - drop the tail, resync on the next chunk */
        if (rid < sCount)
        {
            SkeParam *p = &sParams[rid];
            p->type = t;
            p->present = true;
            p->value = 0;
            for (uint8_t k = 0; k < sz && k < 4; k++)
                p->value |= (uint32_t)rx[i + 3 + k] << (8 * k);
            p->updated = millis();
        }
        *lastId = rid;
        i += 3 + sz;
    }
    sLastOk = millis();
    return true;
}

int skeCommitRaw(uint16_t id, uint32_t raw)
{
    SkeParam *p = skeGet(id);
    if (!p || !p->present)
        return BS_BAD_ID;
    if (skeTypeSize(p->type) == 0)
        return BS_BAD_TYPE;

    uint8_t pl[4];
    uint8_t plen = valuePayload(p->type, raw, pl);
    uint8_t rx[12];
    uint8_t n = 0;
    if (!skeTransact(BC_WRITE, id, pl, plen, rx, &n, 2))
        return SKE_ERR_TRANSPORT;
    if (!n)
        return SKE_ERR_TRANSPORT;
    if (rx[0] != BS_OK)
        return rx[0];

    /* the WRITE reply mirrors the clamped value actually stored */
    if (n >= 2)
    {
        p->type = rx[1];
        uint8_t sz = skeTypeSize(rx[1]);
        if (sz && n >= 2 + sz)
        {
            p->value = 0;
            for (uint8_t k = 0; k < sz && k < 4; k++)
                p->value |= (uint32_t)rx[2 + k] << (8 * k);
            p->updated = millis();
        }
    }
    sLastOk = millis();
    return BS_OK;
}

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

    double d = strtod(text, NULL);
    uint32_t raw = 0;
    switch (p->type)
    {
    case SKT_FLOAT:
    {
        float f = (float)d;
        memcpy(&raw, &f, 4);
        break;
    }
    case SKT_U32:
    case SKT_TIME:
    case SKT_DATE:
        if (d < 0)
            d = 0;
        if (d > 4294967295.0)
            d = 4294967295.0;
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
    default: /* U8/H8/U16/H16/BOOL/ENUM - the device clamps to range anyway */
        if (d < 0)
            d = 0;
        if (d > 65535)
            d = 65535;
        raw = (uint16_t)d;
        break;
    }

    int rc = skeCommitRaw(id, raw);
    if (rc == SKE_ERR_TRANSPORT)
        err = F("прибор не отвечает");
    else if (rc == BS_BAD_ID)
        err = F("нет такого параметра");
    else if (rc == BS_BAD_TYPE)
        err = F("неверный тип/длина");
    else if (rc == BS_READONLY)
        err = F("только для чтения");
    return rc == BS_OK;
}

void skeRebootDevice(void)
{
    uint8_t rx[8];
    uint8_t n = 0;
    skeTransact(BC_REBOOT, 0, NULL, 0, rx, &n, 1);
    sState = ST_WAKE;
    sT = 0;
    sReady = false;
}

void skeRescan(void)
{
    memset(sParams, 0, sizeof(sParams));
    memset(sSections, 0, sizeof(sSections));
    sSectionCount = 0;
    sVersion[0] = 0;
    sCount = 0;
    sCursor = 0;
    sMisses = 0;
    sTxtStarted = false;
    sState = ST_WAKE;
    sT = 0;
    sReady = false;
}

/* ------------------------------------------------------------------ */
/* text listing ('l') capture: names, sections, read-only flags        */

static void parseTextLine(char *s)
{
    /* strip CR/LF */
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
    if (!len)
        return;

    if (s[0] == '-' && s[1] == '-')
    {
        char *e = strstr(s, " --");
        if (e)
        {
            *e = 0;
            sCurSection = addSection(s + 3);
        }
        return;
    }
    if (s[0] < '0' || s[0] > '9')
        return; /* help text / noise */

    char *e;
    long id = strtol(s, &e, 10);
    if (id < 0 || id >= SKE_MAX_PARAMS)
        return;
    while (*e == ' ')
        e++;
    char *name = strchr(e, ' '); /* end of the type token */
    if (!name)
        return;
    *name++ = 0;
    while (*name == ' ')
        name++;
    char *eq = strstr(name, " = ");
    if (!eq)
        return;

    /* read only view items end with " *" after the value */
    bool ro = (len > 0 && s[len - 1] == '*');

    *eq = 0;
    char *t = eq;
    while (t > name && t[-1] == ' ')
        *--t = 0;

    SkeParam *p = &sParams[id];
    strncpy(p->name, name, SKE_NAME_LEN - 1);
    p->name[SKE_NAME_LEN - 1] = 0;
    p->readOnly = ro;
    p->section = sCurSection;
}

static void SKE_UNUSED textStart(void)
{
    skeFlushRx();
    static const char cmd[] = "l\r\n";
    Serial.write((const uint8_t *)cmd, 3);
    Serial.flush();
    sLinePos = 0;
    sTxtActive = true;
    sTxtStarted = true;
    sTxtDeadline = millis() + SKE_TXT_TIMEOUT_MS;
    sTxtQuiet = millis() + SKE_TXT_QUIET_MS;
    sCurSection = 0;
}

static void SKE_UNUSED textFeed(void)
{
    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r')
        {
            if (sLinePos)
            {
                sLine[sLinePos] = 0;
                parseTextLine(sLine);
                sLinePos = 0;
            }
        }
        else if (sLinePos < sizeof(sLine) - 1)
        {
            sLine[sLinePos++] = c;
        }
        sTxtQuiet = millis() + SKE_TXT_QUIET_MS;
    }
    if (skeExpired(sTxtQuiet) || skeExpired(sTxtDeadline))
    {
        if (sLinePos)
        {
            sLine[sLinePos] = 0;
            parseTextLine(sLine);
        }
        sTxtActive = false;
        DBG("ske02: names done, %u sections\n", (unsigned)sSectionCount);
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

void skeBegin(void)
{
#if GW_SKE_ENABLED
    Serial.setRxBufferSize(512);
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
        if (millis() - sT >= SKE_PING_PERIOD_MS)
        {
            sT = millis();
            if (skePing())
            {
                DBG("ske02: %s, %u params\n", sVersion, (unsigned)sCount);
                sCursor = 0;
                sState = ST_NAMES;
            }
        }
        break;

    case ST_NAMES:
        if (!sTxtStarted)
            textStart();
        else
        {
            /* capture finished - move on to the binary sweep */
            sState = ST_SWEEP;
            sCursor = 0;
            sT = 0;
        }
        break;

    case ST_SWEEP:
        if (millis() - sT >= 30)
        {
            sT = millis();
            uint16_t last = sCursor;
            if (skeReadMany(sCursor, &last))
            {
                sCursor = (uint16_t)(last + 1);
                sMisses = 0;
                if (sCursor >= sCount)
                {
                    sCursor = 0;
                    sReady = true;
                    sState = ST_POLL;
                    DBG("ske02: sweep done\n");
                }
            }
            else if (++sMisses >= SKE_WAKE_RETRY_MISS)
            {
                sMisses = 0;
                sState = ST_WAKE;
                sT = 0;
            }
        }
        break;

    case ST_POLL:
        if (millis() - sT >= (uint32_t)storagePoll100ms() * 100UL)
        {
            sT = millis();
            uint16_t last = sCursor;
            if (skeReadMany(sCursor, &last))
            {
                sMisses = 0;
                sCursor = (uint16_t)(last + 1);
                if (sCursor >= sCount)
                    sCursor = 0;
            }
            else if (++sMisses >= SKE_POLL_MISS)
            {
                sMisses = 0;
                sReady = false;
                sState = ST_WAKE;
                sT = 0;
            }
        }
        break;
    }
#endif
}

bool skeLinkUp(void)
{
    return sLastOk && (millis() - sLastOk) < 10000;
}

bool skeReady(void) { return sReady; }
uint16_t skeCount(void) { return sCount; }
const char *skeVersion(void) { return sVersion[0] ? sVersion : "-"; }
uint32_t skeLastOkMs(void) { return sLastOk; }

SkeParam *skeGet(uint16_t id)
{
    return (id < sCount && id < SKE_MAX_PARAMS) ? &sParams[id] : NULL;
}

const char *skeSectionName(uint8_t idx)
{
    return (idx >= 1 && idx <= sSectionCount) ? sSections[idx - 1] : "";
}

String skeValueText(uint16_t id)
{
    SkeParam *p = skeGet(id);
    if (!p)
        return String();
    if (!p->present)
        return String('-');

    char buf[24];
    switch (p->type)
    {
    case SKT_FLOAT:
    {
        float f;
        memcpy(&f, &p->value, 4);
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
    case SKT_I8:
        itoa((int8_t)p->value, buf, 10);
        break;
    case SKT_I16:
        itoa((int16_t)p->value, buf, 10);
        break;
    default:
        ultoa(p->value, buf, 10);
        break;
    }
    return String(buf);
}
