#include "modbus_tcp.h"
#include "ske02.h"
#include "debug.h"
#include "storage.h"
#include <ESP8266WiFi.h>

/*
 * Register map (full tables in README.md). Word order follows the
 * firmware convention: 32-bit values occupy two registers, LOW word
 * at the LOW address.
 *
 * Input registers (FC04, read only):
 *   0     gateway status bitmap: b0 UART link, b1 table ready,
 *         b2 WiFi STA connected, b3 AP portal active
 *   1     SKE-02 parameter count
 *   2-3   gateway uptime, s (u32)
 *   4     WiFi RSSI, dBm (i16)
 *   5     seconds since the last successful console transaction
 *   6     free heap, KiB
 *   7-10  IP address octets
 *
 * Holding registers (FC03/06/10):
 *   0     CONTROL: 1 rescan the device, 2 reboot the device,
 *         3 restart the gateway
 *   1     console poll period, 100 ms units (1..255)
 *   2-99  reserved, read 0
 *   100 + 2*id      parameter <id> raw value, low word
 *   100 + 2*id + 1  parameter <id> raw value, high word (u32/float only)
 *
 * Parameter write semantics: a multi-register write (FC10) covering
 * both words commits immediately; single writes (FC06) are buffered
 * per word and commit when the second word of the pair arrives.
 * IEEE-754 float values map onto the two words as raw LE u32 bits.
 */

#define MB_PORT            502
#define MB_MAX_CLIENTS     2
#define MB_RXBUF           300
#define MB_IDLE_MS         10000
#define MB_HOLD_PARAM_BASE 100

/* modbus exception codes */
#define MB_EX_ILLEGAL_FUNCTION 1
#define MB_EX_ILLEGAL_ADDRESS  2
#define MB_EX_ILLEGAL_VALUE    3
#define MB_EX_SLAVE_FAILURE    4

extern bool gwApMode; /* defined in main.cpp */

static WiFiServer sServer(MB_PORT);
static WiFiClient sClient[MB_MAX_CLIENTS];
static uint8_t sBuf[MB_MAX_CLIENTS][MB_RXBUF];
static uint16_t sPos[MB_MAX_CLIENTS];
static uint32_t sLastAct[MB_MAX_CLIENTS];
static uint32_t sStartTime;

/* pending single-word writes towards the device */
static uint8_t sPendWord[SKE_MAX_PARAMS]; /* b0 low, b1 high */
static uint16_t sPendLo[SKE_MAX_PARAMS];
static uint16_t sPendHi[SKE_MAX_PARAMS];

static uint16_t sLastControl;
static bool sRestartRequest;

static uint32_t upTimeSec32(void)
{
    return (millis() - sStartTime) / 1000UL;
}

static uint16_t inputRead(uint16_t addr)
{
    switch (addr)
    {
    case 0:
        return (skeLinkUp() ? 1 : 0) | (skeReady() ? 2 : 0) |
               (((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) ? 4 : 0) |
               (gwApMode ? 8 : 0);
    case 1:
        return skeCount();
    case 2:
        return (uint16_t)(upTimeSec32() & 0xFFFF);
    case 3:
        return (uint16_t)(upTimeSec32() >> 16);
    case 4:
        return (uint16_t)WiFi.RSSI();
    case 5:
    {
        uint32_t age = skeLastOkMs() ? (millis() - skeLastOkMs()) / 1000UL : 0xFFFF;
        return (age > 0xFFFF) ? 0xFFFF : (uint16_t)age;
    }
    case 6:
        return (uint16_t)(ESP.getFreeHeap() >> 10);
    default:
        if (addr >= 7 && addr <= 10)
        {
            IPAddress ip = gwApMode ? WiFi.softAPIP() : WiFi.localIP();
            return ip[addr - 7];
        }
        return 0;
    }
}

static uint16_t holdRead(uint16_t addr)
{
    if (addr == 0)
        return sLastControl;
    if (addr == 1)
        return storagePoll100ms();
    if (addr < MB_HOLD_PARAM_BASE)
        return 0;

    uint16_t id = (addr - MB_HOLD_PARAM_BASE) >> 1;
    bool high = (addr - MB_HOLD_PARAM_BASE) & 1;
    SkeParam *p = skeGet(id);
    if (!p || !p->present || skeTypeSize(p->type) == 0)
        return 0;
    return high ? (uint16_t)(p->value >> 16) : (uint16_t)(p->value & 0xFFFF);
}

static int holdValidate(uint16_t addr)
{
    if (addr <= 1)
        return 0; /* control and poll period always writable */
    if (addr < MB_HOLD_PARAM_BASE)
        return MB_EX_ILLEGAL_ADDRESS;

    uint16_t id = (addr - MB_HOLD_PARAM_BASE) >> 1;
    bool high = (addr - MB_HOLD_PARAM_BASE) & 1;
    SkeParam *p = skeGet(id);
    if (!p || !p->present || skeTypeSize(p->type) == 0)
        return MB_EX_ILLEGAL_ADDRESS;
    if (high && skeTypeSize(p->type) < 4)
        return MB_EX_ILLEGAL_VALUE; /* one-word parameter has no high word */
    return 0;
}

static void doControl(uint16_t code)
{
    switch (code)
    {
    case 1:
        skeRescan();
        break;
    case 2:
        skeRebootDevice();
        break;
    case 3:
        sRestartRequest = true;
        break;
    default:
        return; /* unknown codes are ignored */
    }
    sLastControl = code;
}

/* Returns a modbus exception code, 0 on success. */
static int holdWrite(uint16_t addr, uint16_t val)
{
    if (addr == 0)
    {
        doControl(val);
        return 0;
    }
    if (addr == 1)
    {
        storageSetPoll100ms((uint8_t)val);
        storageSave();
        return 0;
    }

    int rc = holdValidate(addr);
    if (rc)
        return rc;

    uint16_t id = (addr - MB_HOLD_PARAM_BASE) >> 1;
    bool high = (addr - MB_HOLD_PARAM_BASE) & 1;
    SkeParam *p = skeGet(id);

    if (!high)
    {
        sPendLo[id] = val;
        sPendWord[id] |= 1;
    }
    else
    {
        sPendHi[id] = val;
        sPendWord[id] |= 2;
    }

    uint8_t need = (skeTypeSize(p->type) == 4) ? 3 : 1;
    if ((sPendWord[id] & need) == need)
    {
        uint32_t raw = (skeTypeSize(p->type) == 4)
                           ? ((uint32_t)sPendLo[id] | ((uint32_t)sPendHi[id] << 16))
                           : sPendLo[id];
        sPendWord[id] = 0;
        int crc = skeCommitRaw(id, raw);
        if (crc == SKE_ERR_TRANSPORT)
            return MB_EX_SLAVE_FAILURE; /* the meter is not answering */
        if (crc == BS_BAD_ID)
            return MB_EX_ILLEGAL_ADDRESS;
        if (crc != BS_OK)
            return MB_EX_ILLEGAL_VALUE; /* bad type or read only */
    }
    return 0;
}

/* Process one complete frame; returns the reply length or 0 to drop. */
static uint16_t mbProcessFrame(const uint8_t *in, uint16_t inLen, uint8_t *out)
{
    if (inLen < 8)
        return 0;
    uint16_t tid = (uint16_t)((in[0] << 8) | in[1]);
    uint16_t pid = (uint16_t)((in[2] << 8) | in[3]);
    uint16_t len = (uint16_t)((in[4] << 8) | in[5]);
    if (pid != 0 || len < 2 || (uint16_t)(6 + len) > inLen || len > 254)
        return 0; /* not Modbus TCP */

    const uint8_t *pdu = in + 7;
    uint8_t pduLen = (uint8_t)(len - 1);
    uint8_t *o = out + 7;
    uint8_t olen = 0;
    uint8_t exc = 0;
    uint8_t fc = pdu[0];

    switch (fc)
    {
    case 3: /* read holding registers */
    case 4: /* read input registers */
    {
        if (pduLen != 5)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
        uint16_t qty = (uint16_t)((pdu[3] << 8) | pdu[4]);
        if (qty < 1 || qty > 125)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        o[0] = fc;
        o[1] = (uint8_t)(qty * 2);
        for (uint16_t i = 0; i < qty; i++)
        {
            uint16_t v = (fc == 3) ? holdRead(addr + i) : inputRead(addr + i);
            o[2 + 2 * i] = (uint8_t)(v >> 8);
            o[3 + 2 * i] = (uint8_t)v;
        }
        olen = (uint8_t)(2 + qty * 2);
        break;
    }

    case 6: /* write single register */
    {
        if (pduLen != 5)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
        uint16_t val = (uint16_t)((pdu[3] << 8) | pdu[4]);
        exc = (uint8_t)holdWrite(addr, val);
        if (!exc)
        {
            memcpy(o, pdu, 5);
            olen = 5;
        }
        break;
    }

    case 16: /* write multiple registers */
    {
        if (pduLen < 7)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
        uint16_t qty = (uint16_t)((pdu[3] << 8) | pdu[4]);
        uint8_t bc = pdu[6];
        if (qty < 1 || qty > 123 || bc != qty * 2 || pduLen < 7 + bc)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        /* validate everything first, then apply */
        for (uint16_t i = 0; i < qty && !exc; i++)
            exc = (uint8_t)holdValidate(addr + i);
        for (uint16_t i = 0; i < qty && !exc; i++)
        {
            uint16_t val = (uint16_t)((pdu[7 + 2 * i] << 8) | pdu[8 + 2 * i]);
            exc = (uint8_t)holdWrite(addr + i, val);
        }
        if (!exc)
        {
            o[0] = fc;
            o[1] = pdu[1];
            o[2] = pdu[2];
            o[3] = pdu[3];
            o[4] = pdu[4];
            olen = 5;
        }
        break;
    }

    default:
        exc = MB_EX_ILLEGAL_FUNCTION;
        break;
    }

    if (exc)
    {
        o[0] = (uint8_t)(fc | 0x80);
        o[1] = exc;
        olen = 2;
    }

    out[0] = (uint8_t)(tid >> 8);
    out[1] = (uint8_t)tid;
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint8_t)((olen + 1) >> 8);
    out[5] = (uint8_t)(olen + 1);
    out[6] = in[6]; /* unit id passthrough */
    return (uint16_t)(7 + olen);
}

void mbSetup(void)
{
    sStartTime = millis();
    sServer.begin();
    sServer.setNoDelay(true);
    DBG("modbus: listening on tcp/502\n");
}

void mbLoop(void)
{
    /* accept new clients into free slots */
    while (sServer.hasClient())
    {
        WiFiClient c = sServer.accept();
        uint8_t slot = MB_MAX_CLIENTS;
        for (uint8_t i = 0; i < MB_MAX_CLIENTS; i++)
        {
            if (!sClient[i] || !sClient[i].connected())
            {
                if (sClient[i])
                    sClient[i].stop();
                slot = i;
                break;
            }
        }
        if (slot == MB_MAX_CLIENTS)
        {
            c.stop(); /* all slots busy */
        }
        else
        {
            sClient[slot] = c;
            sPos[slot] = 0;
            sLastAct[slot] = millis();
        }
    }

    static uint8_t out[MB_RXBUF];
    for (uint8_t i = 0; i < MB_MAX_CLIENTS; i++)
    {
        if (!sClient[i] || !sClient[i].connected())
            continue;

        if (sClient[i].available())
        {
            while (sClient[i].available())
            {
                if (sPos[i] >= MB_RXBUF)
                {
                    sClient[i].stop(); /* frame overflow */
                    sPos[i] = 0;
                    break;
                }
                sBuf[i][sPos[i]++] = (uint8_t)sClient[i].read();
            }
            sLastAct[i] = millis();
        }

        while (sPos[i] >= 7)
        {
            uint16_t len = (uint16_t)((sBuf[i][4] << 8) | sBuf[i][5]);
            uint16_t total = (uint16_t)(6 + len);
            if (total > MB_RXBUF)
            {
                sClient[i].stop();
                sPos[i] = 0;
                break;
            }
            if (sPos[i] < total)
                break; /* wait for the rest of the frame */

            uint16_t outLen = mbProcessFrame(sBuf[i], total, out);
            if (outLen)
                sClient[i].write(out, outLen);

            memmove(sBuf[i], sBuf[i] + total, sPos[i] - total);
            sPos[i] -= total;
        }

        if (millis() - sLastAct[i] > MB_IDLE_MS)
            sClient[i].stop();
    }
}

bool mbConsumeRestartRequest(void)
{
    if (sRestartRequest)
    {
        sRestartRequest = false;
        return true;
    }
    return false;
}
