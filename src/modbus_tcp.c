/*
 * Modbus TCP slave (port 502, FC 03/04/06/10) on BSD sockets. The same
 * register map as the Arduino build:
 *
 * Input (FC04): 0 status bitmap, 1 param count, 2-3 uptime s, 4 RSSI,
 *               5 s since last transaction, 6 heap KiB, 7-10 IP octets
 * Holding (FC03/06/10): 0 CONTROL (1 rescan, 2 device reboot, 3 gw reboot),
 *               1 poll step x100ms (inert, no background poll anymore),
 *               100+2*id parameter value, low word first
 */
#include "modbus_tcp.h"

#include "tree_store.h"

#include <lwip/sockets.h>
#include <string.h>

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "tcpip_adapter.h"

#include "debug.h"
#include "protocol.h"
#include "wifi.h"
#include "ske02.h"

#define MB_PORT       502
#define MB_MAX_CONN   2
#define MB_RXBUF      300
#define MB_IDLE_MS    10000

#define MB_EX_ILLEGAL_FUNCTION 1
#define MB_EX_ILLEGAL_ADDRESS  2
#define MB_EX_ILLEGAL_VALUE    3
#define MB_EX_SLAVE_FAILURE    4

#define MB_HOLD_PARAM_BASE 100

/* 32-bit tick snapshots read across tasks - use the protocol mutex */
static uint32_t tick_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static uint16_t input_read(uint16_t addr)
{
    switch (addr)
    {
    case 0:
    {
        tcpip_adapter_ip_info_t ip;
        int bits = (ske02_link_up() ? 1 : 0) | (ske02_ready() ? 2 : 0);
        if (wifi_sta_connected())
            bits |= 4;
        else
            bits |= 8;
        (void)ip;
        return (uint16_t)bits;
    }
    case 1:
    {
        uint16_t c;
        xSemaphoreTake(ske02_lock(), portMAX_DELAY);
        c = ske02_ctx()->count;
        xSemaphoreGive(ske02_lock());
        return c;
    }
    case 2:
        return (uint16_t)(tick_ms() / 1000 & 0xFFFF);
    case 3:
        return (uint16_t)(tick_ms() / 1000 >> 16);
    case 4:
        return 0; /* RSSI: filled in from the wifi task data */
    case 5:
    {
        uint32_t age = 0xFFFF;
        xSemaphoreTake(ske02_lock(), portMAX_DELAY);
        if (ske02_ctx()->vals.updated)
            age = (tick_ms() - ske02_ctx()->vals.updated) / 1000;
        xSemaphoreGive(ske02_lock());
        return age > 0xFFFF ? 0xFFFF : (uint16_t)age;
    }
    case 6:
        return (uint16_t)(esp_get_free_heap_size() >> 10);
    case 7:
    case 8:
    case 9:
    case 10:
    {
        tcpip_adapter_ip_info_t ip;
        if (wifi_sta_connected())
            tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
        else
            tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
        {
            uint32_t a = ip.ip.addr;
            return (uint16_t)((a >> (8 * (addr - 7))) & 0xFF);
        }
    }
    default:
        return 0;
    }
}

static uint16_t hold_read(uint16_t addr)
{
    if (addr <= 1)
        return 0; /* control echoes nothing; poll step is inert */
    if (addr < MB_HOLD_PARAM_BASE)
        return 0;
    {
        uint16_t id = (addr - MB_HOLD_PARAM_BASE) >> 1;
        bool high = (addr - MB_HOLD_PARAM_BASE) & 1;
        uint16_t v = 0;
        TreeRec rec;
        if (tree_get(id, &rec) && rec.p.present &&
            proto_type_size(rec.p.type) > 0)
            v = high ? (uint16_t)(rec.p.value >> 16)
                     : (uint16_t)(rec.p.value & 0xFFFF);
        return v;
    }
}

static int hold_validate(uint16_t addr)
{
    if (addr <= 1)
        return 0;
    if (addr < MB_HOLD_PARAM_BASE)
        return MB_EX_ILLEGAL_ADDRESS;
    {
        uint16_t id = (addr - MB_HOLD_PARAM_BASE) >> 1;
        bool high = (addr - MB_HOLD_PARAM_BASE) & 1;
        int rc = MB_EX_ILLEGAL_ADDRESS;
        TreeRec rec;
        if (tree_get(id, &rec) && rec.p.present &&
            proto_type_size(rec.p.type) > 0)
        {
            rc = 0;
            if (high && proto_type_size(rec.p.type) < 4)
                rc = MB_EX_ILLEGAL_VALUE;
        }
        return rc;
    }
}

/* returns a modbus exception code, 0 on success */
static int hold_write(uint16_t addr, uint16_t val)
{
    if (addr == 0)
    {
        ske_req_t r;
        switch (val)
        {
        case 1:
            r.cmd = SKEQ_RESCAN;
            ske02_request(&r, 30000);
            return 0;
        case 2:
            r.cmd = SKEQ_REBOOT;
            ske02_request(&r, 15000);
            return 0;
        case 3:
            DBG("modbus: gateway restart via CONTROL\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            return 0;
        default:
            return 0;
        }
    }
    if (addr == 1)
        return 0; /* inert since the on-demand refresh redesign */

    {
        int rc = hold_validate(addr);
        if (rc)
            return rc;
        {
            uint16_t id = (addr - MB_HOLD_PARAM_BASE) >> 1;
            bool high = (addr - MB_HOLD_PARAM_BASE) & 1;
            static uint16_t pendLo[SKE_MAX_PARAMS];
            static uint16_t pendHi[SKE_MAX_PARAMS];
            static uint8_t pendWord[SKE_MAX_PARAMS];
            uint8_t need;
            ske_req_t r;
            char packed[24];
            uint32_t raw;
            TreeRec rec;
            bool have = tree_get(id, &rec);

            if (!high)
            {
                pendLo[id] = val;
                pendWord[id] |= 1;
            }
            else
            {
                pendHi[id] = val;
                pendWord[id] |= 2;
            }
            need = (have && proto_type_size(rec.p.type) == 4) ? 3 : 1;
            if ((pendWord[id] & need) != need)
                return 0; /* buffered until the pair completes */
            raw = (need == 3)
                      ? ((uint32_t)pendLo[id] | ((uint32_t)pendHi[id] << 16))
                      : pendLo[id];
            if (!have)
                return MB_EX_ILLEGAL_ADDRESS;
            proto_raw_to_cmd(rec.p.type, raw, packed, sizeof(packed));
            pendWord[id] = 0;

            r.cmd = SKEQ_SET;
            r.id = id;
            strncpy(r.text, packed, sizeof(r.text) - 1);
            r.text[sizeof(r.text) - 1] = 0;
            ske02_request(&r, 15000);
            if (r.result == SKE_ERR_TRANSPORT)
                return MB_EX_SLAVE_FAILURE;
            if (r.result == BS_BAD_ID)
                return MB_EX_ILLEGAL_ADDRESS;
            if (r.result != BS_OK)
                return MB_EX_ILLEGAL_VALUE;
            return 0;
        }
    }
}

static uint16_t process_frame(const uint8_t *in, uint16_t inLen, uint8_t *out)
{
    uint16_t tid, len;
    uint8_t fc;
    const uint8_t *pdu;
    uint8_t *o = out + 7;
    uint8_t exc = 0;
    uint16_t i;

    if (inLen < 8)
        return 0;
    tid = (in[0] << 8) | in[1];
    len = (in[4] << 8) | in[5];
    if (in[2] || in[3] || len < 2 || (uint16_t)(6 + len) > inLen)
        return 0; /* not modbus tcp */
    /*
     * MBAP layout: tid(0-1) pid(2-3) len(4-5) UNIT(6) FC(7)...
     * in[6] is the unit id - reading the function code from there put
     * the unit (usually 1) into the switch and every request fell into
     * the default branch answering "illegal function".
     */
    fc = in[7];
    pdu = in + 7;

    switch (fc)
    {
    case 3:
    case 4:
    {
        uint16_t addr, qty, v;
        /* len counts the unit id too: unit(1) + fc/addr/qty(5) = 6.
         * Checking against 5 rejected every well-formed request. */
        if (len != 6)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        addr = (pdu[1] << 8) | pdu[2];
        qty = (pdu[3] << 8) | pdu[4];
        if (qty < 1 || qty > 125)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        o[0] = fc;
        o[1] = qty * 2;
        for (i = 0; i < qty; i++)
        {
            v = (fc == 3) ? hold_read(addr + i) : input_read(addr + i);
            o[2 + 2 * i] = v >> 8;
            o[3 + 2 * i] = v & 0xFF;
        }
        out[4] = 0;
        out[5] = (uint8_t)(2 + qty * 2 + 1);
        break;
    }
    case 6:
    {
        uint16_t addr, val;
        if (len != 6) /* unit(1) + fc/addr/val(5) */
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        addr = (pdu[1] << 8) | pdu[2];
        val = (pdu[3] << 8) | pdu[4];
        exc = hold_write(addr, val);
        if (!exc)
        {
            memcpy(o, pdu, 5);
            out[4] = 0;
            out[5] = 6;
        }
        break;
    }
    case 16:
    {
        uint16_t addr, qty, bc;
        if (len < 7)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        addr = (pdu[1] << 8) | pdu[2];
        qty = (pdu[3] << 8) | pdu[4];
        bc = pdu[6];
        if (qty < 1 || qty > 123 || bc != qty * 2 || len < 7 + bc)
        {
            exc = MB_EX_ILLEGAL_VALUE;
            break;
        }
        for (i = 0; i < qty && !exc; i++)
            exc = hold_validate(addr + i);
        for (i = 0; i < qty && !exc; i++)
            exc = hold_write(addr + i, (pdu[7 + 2 * i] << 8) | pdu[8 + 2 * i]);
        if (!exc)
        {
            o[0] = fc;
            memcpy(o + 1, pdu + 1, 4);
            out[4] = 0;
            out[5] = 6; /* unit + fc + addr + qty */
        }
        break;
    }
    default:
        exc = MB_EX_ILLEGAL_FUNCTION;
        break;
    }

    if (exc)
    {
        o[0] = fc | 0x80;
        o[1] = exc;
        out[4] = 0;
        out[5] = 3; /* unit + fc|0x80 + exception code */
    }
    out[0] = tid >> 8;
    out[1] = tid & 0xFF;
    out[2] = out[3] = 0;
    out[6] = in[6];
    /*
     * Total frame = MBAP header (6 bytes) + the length field, which
     * itself already counts the unit id. Returning 7 + len sent one
     * extra trailing byte with EVERY response and desynced masters on
     * their next transaction.
     */
    return 6 + out[5];
}

static void modbus_client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t buf[MB_RXBUF];
    uint8_t out[MB_RXBUF];
    uint16_t pos = 0;
    TickType_t lastAct = xTaskGetTickCount();

    /* close dead clients instead of parking recv() forever */
    struct timeval tv = { .tv_sec = MB_IDLE_MS / 1000, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;)
    {
        int n = recv(fd, buf + pos, sizeof(buf) - pos, 0);
        if (n <= 0)
            break;
        pos += n;
        lastAct = xTaskGetTickCount();

        while (pos >= 7)
        {
            uint16_t len = (buf[4] << 8) | buf[5];
            uint16_t total = 6 + len;
            if (total > MB_RXBUF)
                goto done;
            if (pos < total)
                break;
            {
                uint16_t outLen = process_frame(buf, total, out);
                if (outLen)
                    send(fd, out, outLen, 0);
            }
            memmove(buf, buf + total, pos - total);
            pos -= total;
        }
        if (xTaskGetTickCount() - lastAct > pdMS_TO_TICKS(MB_IDLE_MS))
            break;
    }
done:
    close(fd);
    vTaskDelete(NULL);
}

static void modbus_task(void *arg)
{
    int srv;
    struct sockaddr_in bindAddr;
    (void)arg;

    srv = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (srv < 0)
    {
        DBG("modbus: socket failed\n");
        vTaskDelete(NULL);
        return;
    }
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(MB_PORT);
    if (bind(srv, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) < 0 ||
        listen(srv, MB_MAX_CONN) < 0)
    {
        DBG("modbus: bind/listen failed\n");
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    DBG("modbus: listening on tcp/502\n");

    for (;;)
    {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &clilen);
        char name[16];
        if (fd < 0)
            continue;
        snprintf(name, sizeof(name), "mbc%d", fd % 10);
        xTaskCreate(modbus_client_task, name, 3072, (void *)(intptr_t)fd, 4,
                    NULL);
    }
}

void modbus_start(void)
{
    xTaskCreate(modbus_task, "modbus", 3072, NULL, 4, NULL);
}
