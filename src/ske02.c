/*
 * Meter task: UART0 client of the SKE-02 service console.
 *
 *   ST_WAKE  - no console yet: 'e' (echo off) + 'i' (version, count)
 *   ST_LIST  - capturing the 'l' listing (discovery / explicit refresh),
 *              streamed through the protocol parser, prompt-terminated
 *   ST_IDLE  - ready; one small 'm' frame per second (monitoring), the
 *              settings tree stays quiet - 'l' stalls the meter console
 *              task for over a second and runs ONLY on demand
 *
 * Set/run/unlock/refresh requests from the web/modbus tasks arrive via
 * the command queue; the UART is touched from this task only.
 */
#include "ske02.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "debug.h"
#include "protocol.h"

#define SKE_UART_NUM      UART_NUM_0
#define SKE_UART_BAUD     115200
#define SKE_UART_RX_BUF   1024
#define SKE_CMD_TIMEOUT   pdMS_TO_TICKS(1200)
#define SKE_LIST_TIMEOUT  pdMS_TO_TICKS(15000)
#define SKE_WAKE_PERIOD   pdMS_TO_TICKS(1000)
#define SKE_VALUES_PERIOD pdMS_TO_TICKS(1000)
#define SKE_WAKE_MISS     3

enum
{
    ST_WAKE,
    ST_LIST,
    ST_IDLE
};

static ProtoCtx sCtx;
static SemaphoreHandle_t sLock;
static uint8_t sState;
static TickType_t sT;
static uint8_t sMisses;
static bool sReady;
static TickType_t sLastOk;

/* request plumbing: one static slot, one in-flight request at a time */
static QueueHandle_t sQueue;       /* of &sSlot */
static SemaphoreHandle_t sReqMutex;
static SemaphoreHandle_t sReqDone;
static ske_req_t sSlot;
static volatile uint32_t sSeq;

/* ------------------------------------------------------------------ */
/* uart helpers                                                        */

static void uart_flush_rx(void)
{
    uint8_t c;
    while (uart_read_bytes(SKE_UART_NUM, &c, 1, 0) > 0)
        ;
}

static void uart_send_line(const char *cmd)
{
    uart_write_bytes(SKE_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(SKE_UART_NUM, "\r", 1);
}

/*
 * Read one line (rc==2) or the prompt (rc==1); 0 on timeout. The prompt
 * matcher runs on every byte; on rc==1 the buffer holds the trailing
 * partial line, usually junk.
 */
static int read_line_until_prompt(char *line, size_t cap, TickType_t timeout)
{
    size_t pos = 0;
    TickType_t deadline = xTaskGetTickCount() + timeout;
    proto_prompt_reset(&sCtx);
    for (;;)
    {
        uint8_t c;
        int32_t remain = (int32_t)(deadline - xTaskGetTickCount());
        int n;
        if (remain <= 0)
            return 0; /* TickType_t is unsigned: a past deadline would
                        * otherwise underflow into an infinite wait */
        n = uart_read_bytes(SKE_UART_NUM, &c, 1, remain);
        if (n <= 0)
            return 0;
        if (proto_prompt_feed(&sCtx, (char)c))
            return 1;
        if (c == '\n' || c == '\r')
        {
            if (pos)
            {
                line[pos] = 0;
                return 2;
            }
        }
        else if (pos < cap - 1)
        {
            line[pos++] = (char)c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* line routing                                                        */

static void line_to_parser(char *line)
{
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.nowMs = (uint32_t)xTaskGetTickCount();
    proto_parse_line(&sCtx, line);
    xSemaphoreGive(sLock);
}

typedef void (*line_cb)(char *line, void *user);

static bool txt_command(const char *cmd, line_cb cb, void *user,
                        TickType_t timeout)
{
    char line[512];
    uart_flush_rx();
    uart_send_line(cmd);
    for (;;)
    {
        int rc = read_line_until_prompt(line, sizeof(line), timeout);
        if (rc == 0)
            return false;
        if (rc == 2 && cb)
            cb(line, user);
        if (rc == 1)
            return true;
    }
}

/* ------------------------------------------------------------------ */
/* individual console commands                                        */

static void echo_cb(char *line, void *user)
{
    char *last = (char *)user;
    if (!strncmp(line, "echo", 4))
    {
        strncpy(last, line, 31);
        last[31] = 0;
    }
}

static bool ske_echo_off(void)
{
    char last[32];
    int attempt;
    for (attempt = 0; attempt < 3; attempt++)
    {
        last[0] = 0;
        /* the leading empty line drops any half-typed text line the
         * console may be holding after boot noise */
        txt_command("", NULL, NULL, SKE_CMD_TIMEOUT);
        if (txt_command("e", echo_cb, last, SKE_CMD_TIMEOUT) &&
            !strncmp(last, "echo off", 8))
            return true;
    }
    return false;
}

static void info_cb(char *line, void *user)
{
    (void)user;
    if (!strncmp(line, "model", 5))
    {
        char model[8] = {0};
        char fw[8] = {0};
        char *m = line + 5, *w, *f, *we;
        while (*m == ' ')
            m++;
        w = strchr(m, ' ');
        if (w)
            *w = 0;
        strncpy(model, m, sizeof(model) - 1);
        f = w ? strstr(w + 1, "fw ") : NULL;
        if (f)
        {
            f += 3;
            we = strchr(f, ' ');
            if (we)
                *we = 0;
            strncpy(fw, f, sizeof(fw) - 1);
        }
        if (model[0] && fw[0])
        {
            xSemaphoreTake(sLock, portMAX_DELAY);
            snprintf(sCtx.version, sizeof(sCtx.version), "%s/%s", model, fw);
            xSemaphoreGive(sLock);
        }
    }
    else if (!strncmp(line, "serial", 6))
    {
        char *p = strstr(line, "params");
        long cnt;
        if (!p)
            return;
        cnt = strtol(p + 6, NULL, 10);
        if (cnt > 0)
        {
            xSemaphoreTake(sLock, portMAX_DELAY);
            sCtx.count =
                (cnt > SKE_MAX_PARAMS) ? SKE_MAX_PARAMS : (uint16_t)cnt;
            xSemaphoreGive(sLock);
        }
    }
}

static bool ske_info(void)
{
    return txt_command("i", info_cb, NULL, SKE_CMD_TIMEOUT);
}

static void text_start(void)
{
    uart_flush_rx();
    uart_send_line("l");
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.inListing = true;
    sCtx.curSection = 0;
    sCtx.curGroup = 0;
    memset(sCtx.hdrName, 0, sizeof(sCtx.hdrName));
    xSemaphoreGive(sLock);
}

static bool capture_listing(TickType_t timeout)
{
    char line[512];
    bool ok;
    proto_prompt_reset(&sCtx);
    for (;;)
    {
        int rc = read_line_until_prompt(line, sizeof(line), timeout);
        if (rc == 0)
        {
            ok = false;
            break;
        }
        if (rc == 2)
            line_to_parser(line);
        if (rc == 1)
        {
            ok = true;
            break;
        }
    }
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.inListing = false;
    xSemaphoreGive(sLock);
    return ok;
}

static void values_cb(char *line, void *user)
{
    (void)user;
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.nowMs = (uint32_t)xTaskGetTickCount();
    proto_values_line(&sCtx, line);
    xSemaphoreGive(sLock);
}

static bool ske_query_values(void)
{
    bool ok;
    xSemaphoreTake(sLock, portMAX_DELAY);
    proto_values_restart(&sCtx);
    xSemaphoreGive(sLock);
    if (!txt_command("m", values_cb, NULL, SKE_CMD_TIMEOUT))
        return false;
    xSemaphoreTake(sLock, portMAX_DELAY);
    ok = proto_values_ready(&sCtx);
    xSemaphoreGive(sLock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* request execution (meter task context only)                         */

typedef struct
{
    int status;   /* first ERR code seen, 0 = none */
    bool seen_ok; /* "OK " line seen */
    char value_line[128]; /* the resulting parameter line after "OK " */
} reply_ctx_t;

static void reply_cb(char *line, void *user)
{
    reply_ctx_t *rc_ = (reply_ctx_t *)user;
    if (!strncmp(line, "ERR", 3))
    {
        if (!rc_->status)
        {
            if (strstr(line, "read only"))
                rc_->status = BS_READONLY;
            else if (strstr(line, "access"))
                rc_->status = BS_ACCESS;
            else if (strstr(line, "wait"))
                rc_->status = BS_WAIT;
            else if (strstr(line, "id"))
                rc_->status = BS_BAD_ID;
            else
                rc_->status = BS_BAD_TYPE;
        }
        return;
    }
    if (!strncmp(line, "OK ", 3))
    {
        rc_->seen_ok = true;
        strncpy(rc_->value_line, line + 3, sizeof(rc_->value_line) - 1);
        rc_->value_line[sizeof(rc_->value_line) - 1] = 0;
    }
}

static void unlock_cb(char *line, void *user)
{
    reply_ctx_t *rc_ = (reply_ctx_t *)user;
    if (!strncmp(line, "access:", 7))
        rc_->seen_ok = true;
    else if (!strncmp(line, "ERR", 3) && !rc_->status)
        rc_->status = strstr(line, "wait") ? BS_WAIT : BS_ACCESS;
}

static const char *bs_message(int rc)
{
    switch (rc)
    {
    case SKE_ERR_TRANSPORT: return "прибор не отвечает";
    case BS_BAD_ID:         return "нет такого параметра";
    case BS_BAD_TYPE:       return "неверное значение";
    case BS_READONLY:       return "только для чтения";
    case BS_ACCESS:         return "нет доступа: неверный пароль меню";
    case BS_WAIT:           return "ввод пароля заблокирован, подождите";
    default:                return "";
    }
}

static void exec_request(ske_req_t *req)
{
    char cmd[64];
    reply_ctx_t rc_;
    SkeParam *p;

    memset(&rc_, 0, sizeof(rc_));
    req->err[0] = 0;
    req->result = BS_OK;

    switch (req->cmd)
    {
    case SKEQ_SET:
        xSemaphoreTake(sLock, portMAX_DELAY);
        p = proto_param(&sCtx, req->id);
        xSemaphoreGive(sLock);
        if (!p)
        {
            req->result = BS_BAD_ID;
            break;
        }
        /* the text is the packed console value already (proto_raw_to_cmd) */
        snprintf(cmd, sizeof(cmd), "s %u %s", (unsigned)req->id, req->text);
        if (!txt_command(cmd, reply_cb, &rc_, SKE_CMD_TIMEOUT))
        {
            req->result = SKE_ERR_TRANSPORT;
            break;
        }
        if (rc_.seen_ok && rc_.value_line[0])
            line_to_parser(rc_.value_line); /* sync the cache */
        req->result = rc_.seen_ok ? BS_OK
                                  : (rc_.status ? rc_.status : BS_BAD_TYPE);
        if (req->result == BS_OK)
            sLastOk = xTaskGetTickCount();
        break;

    case SKEQ_RUN:
        snprintf(cmd, sizeof(cmd), "x %u", (unsigned)req->id);
        if (!txt_command(cmd, reply_cb, &rc_, SKE_CMD_TIMEOUT))
        {
            req->result = SKE_ERR_TRANSPORT;
            break;
        }
        req->result = rc_.seen_ok ? BS_OK
                                  : (rc_.status ? rc_.status : BS_BAD_TYPE);
        if (req->result == BS_OK)
            sLastOk = xTaskGetTickCount();
        break;

    case SKEQ_UNLOCK:
        if (!req->text[0])
        {
            req->result = BS_BAD_TYPE;
            snprintf(req->err, sizeof(req->err), "пустой пароль");
            break;
        }
        snprintf(cmd, sizeof(cmd), "p %s", req->text);
        if (!txt_command(cmd, unlock_cb, &rc_, SKE_CMD_TIMEOUT))
        {
            req->result = SKE_ERR_TRANSPORT;
            break;
        }
        req->result = rc_.seen_ok ? BS_OK
                                  : (rc_.status ? rc_.status : BS_ACCESS);
        if (req->result == BS_OK)
            sLastOk = xTaskGetTickCount();
        break;

    case SKEQ_REFRESH:
        text_start();
        req->result = capture_listing(SKE_LIST_TIMEOUT) ? BS_OK
                                                        : SKE_ERR_TRANSPORT;
        break;

    case SKEQ_RESCAN:
        xSemaphoreTake(sLock, portMAX_DELAY);
        proto_reset(&sCtx);
        xSemaphoreGive(sLock);
        text_start();
        req->result = capture_listing(SKE_LIST_TIMEOUT) ? BS_OK
                                                        : SKE_ERR_TRANSPORT;
        if (req->result == BS_OK)
            sReady = true;
        break;

    case SKEQ_REBOOT:
        uart_flush_rx();
        uart_send_line("r");
        vTaskDelay(pdMS_TO_TICKS(200));
        sState = ST_WAKE;
        sT = 0;
        sReady = false;
        break;

    default:
        req->result = BS_BAD_TYPE;
        break;
    }

    if (!req->err[0] && req->result != BS_OK)
        snprintf(req->err, sizeof(req->err), "%s", bs_message(req->result));
}

/* ------------------------------------------------------------------ */
/* the task                                                            */

static void meter_task(void *arg)
{
    (void)arg;
    ske_req_t *req;
    for (;;)
    {
        while (xQueueReceive(sQueue, &req, 0) == pdTRUE)
        {
            exec_request(req);
            xSemaphoreGive(sReqDone);
        }

        switch (sState)
        {
        case ST_WAKE:
            if (xTaskGetTickCount() - sT >= SKE_WAKE_PERIOD)
            {
                sT = xTaskGetTickCount();
                /* ~1 Hz while the meter link is down; read on GPIO2 */
                DBG("ske02: wake: console silent\n");
                if (ske_echo_off() && ske_info())
                {
                    xSemaphoreTake(sLock, portMAX_DELAY);
                    DBG("ske02: %s, %u params\n", sCtx.version,
                        (unsigned)sCtx.count);
                    xSemaphoreGive(sLock);
                    text_start();
                    sState = ST_LIST;
                    sMisses = 0;
                }
            }
            else
                vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case ST_LIST:
            if (capture_listing(SKE_LIST_TIMEOUT))
            {
                sReady = true;
                sState = ST_IDLE;
                sT = xTaskGetTickCount();
                xSemaphoreTake(sLock, portMAX_DELAY);
                DBG("ske02: listing done (%u params)\n", (unsigned)sCtx.count);
                xSemaphoreGive(sLock);
            }
            else if (++sMisses >= SKE_WAKE_MISS)
            {
                sMisses = 0;
                sState = ST_WAKE;
                sT = 0;
            }
            break;

        case ST_IDLE:
            if (xTaskGetTickCount() - sT >= SKE_VALUES_PERIOD)
            {
                sT = xTaskGetTickCount();
                if (ske_query_values())
                {
                    static TickType_t lastBeat;
                    sMisses = 0;
                    if (sLastOk - lastBeat >= pdMS_TO_TICKS(10000))
                    {
                        lastBeat = sLastOk;
                        xSemaphoreTake(sLock, portMAX_DELAY);
                        DBG("ske02: m ok (rate %.2f, total %.1f)\n",
                            (double)sCtx.vals.rate, (double)sCtx.vals.total);
                        xSemaphoreGive(sLock);
                    }
                }
                else if (++sMisses >= SKE_WAKE_MISS)
                {
                    sMisses = 0;
                    sState = ST_WAKE;
                    sT = 0;
                }
            }
            else
                vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

void ske02_start(void)
{
    uart_config_t cfg = {
        .baud_rate = SKE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
    };
    uart_driver_install(SKE_UART_NUM, SKE_UART_RX_BUF, 0, 0, NULL, 0);
    uart_param_config(SKE_UART_NUM, &cfg);

    proto_init(&sCtx);
    sLock = xSemaphoreCreateMutex();
    sReqMutex = xSemaphoreCreateMutex();
    sReqDone = xSemaphoreCreateBinary();
    sQueue = xQueueCreate(2, sizeof(ske_req_t *));
    xTaskCreate(meter_task, "meter", 4096, NULL, 5, NULL);
}

bool ske02_request(const ske_req_t *req, uint32_t timeout_ms)
{
    bool ok = false;
    if (xSemaphoreTake(sReqMutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;

    sSlot = *req;
    sSlot.result = BS_OK;
    sSlot.err[0] = 0;
    sSeq++;

    {
        ske_req_t *ptr = &sSlot;
        if (xQueueSend(sQueue, &ptr, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
        {
            if (xSemaphoreTake(sReqDone, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
            {
                /* copy the results back to the caller */
                memcpy((void *)&req->result, &sSlot.result, sizeof(int));
                memcpy((void *)req->err, sSlot.err, sizeof(req->err));
                ok = (sSlot.result == BS_OK);
            }
            /* a late completion may still land in sSlot - harmless: the
             * slot is static and the seq guard rejects stale results */
        }
    }
    xSemaphoreGive(sReqMutex);
    return ok;
}

SemaphoreHandle_t ske02_lock(void) { return sLock; }
ProtoCtx *ske02_ctx(void) { return &sCtx; }

const SkeValues *ske02_values(void)
{
    return sCtx.vals.updated ? &sCtx.vals : NULL;
}

bool ske02_link_up(void)
{
    return sLastOk && (xTaskGetTickCount() - sLastOk) < pdMS_TO_TICKS(15000);
}

bool ske02_ready(void) { return sReady; }
