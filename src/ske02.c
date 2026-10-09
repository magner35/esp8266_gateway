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
#include <stdlib.h>   /* atoi: lockout seconds */
#include <string.h>

#include "driver/uart.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "debug.h"
#include "protocol.h"
#include "tree_store.h"

#define SKE_UART_NUM      UART_NUM_0
#define SKE_UART_BAUD     115200
#define SKE_UART_RX_BUF   4096  /* стирание flash подвешивает CPU при NVS-записях листинга */
#define SKE_CMD_TIMEOUT   pdMS_TO_TICKS(1200)
#define SKE_LIST_TIMEOUT  pdMS_TO_TICKS(15000)
#define SKE_WAKE_PERIOD   pdMS_TO_TICKS(200)
#define SKE_VALUES_PERIOD pdMS_TO_TICKS(200)
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

/* first line of the last transaction - wake diagnostics */
static char sFirstLine[40];
static bool sFirstLineValid;

static bool txt_command(const char *cmd, line_cb cb, void *user,
                        TickType_t timeout)
{
    char line[512];
    uart_flush_rx();
    uart_send_line(cmd);
    sFirstLineValid = false;
    for (;;)
    {
        int rc = read_line_until_prompt(line, sizeof(line), timeout);
        if (rc == 0)
            return false;
        if (rc == 2)
        {
            if (!sFirstLineValid)
            {
                strncpy(sFirstLine, line, sizeof(sFirstLine) - 1);
                sFirstLine[sizeof(sFirstLine) - 1] = 0;
                sFirstLineValid = true;
            }
            if (cb)
                cb(line, user);
        }
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

static void ske02_uart_setup(void);

static void uart_reinit(void)
{
    /* full driver restart: recovers from a wedged ring/FIFO state that
     * plain flushes cannot clear (e.g. a broken listing) */
    uart_driver_delete(SKE_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(50));
    ske02_uart_setup();
    DBG("ske02: uart driver reinstalled\n");
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
        DBG("ske02: wake reply: '%s'\n",
            sFirstLineValid ? sFirstLine : "<none>");
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

/*
 * Приём листинга ДВУМЯ ФАЗАМИ:
 *   фаза 1 - весь листинг (13КБ) читается в буфер КУЧИ: никакого
 *             парсинга и NVS во время приёма, UART не теряет
 *             строки на flash-подвисаниях;
 *   фаза 2 - парсинг из буфера + потоковая запись в NVS: тут
 *             подвисания flash уже ничему не мешают.
 * Прежняя однопроходная схема теряла строки (запись в NVС прямо
 * во время чтения) и с "коммитом только полного листинга"
 * навсегда застревала в ретраях без повторной команды 'l'.
 */
static bool capture_listing(TickType_t timeout)
{
    char line[512];
    bool ok = false;
    size_t len = 0;
    /*
     * Буфер в КУЧЕ, НО с деградацией: если целый 14К не выделился,
     * пробуем меньше (10К/6К) - листинг 13К влезет не весь, но
     * capture честно провалится и повторится, НО приём идёт и
     * UART не теряет данные. Кучу НЕ держим: free сразу после фазы 2.
     * Статику не используем: 14К навсегда - RAM нет.
     */
    char *buf = malloc(14 * 1024);
    size_t bufSz = 14 * 1024;
    while (!buf && bufSz > 4096)
    {
        bufSz -= 2048;
        buf = malloc(bufSz);
    }
    if (!buf)
        return false;

    proto_prompt_reset(&sCtx);

    /* фаза 1: чистый приём в буфер */
    for (;;)
    {
        int rc = read_line_until_prompt(line, sizeof(line), timeout);
        if (rc == 0)
            break;         /* таймаут */
        if (rc == 2)
        {
            size_t n = strlen(line);
            if (len + n + 2 <= bufSz)
            {
                memcpy(buf + len, line, n);
                len += n;
                buf[len++] = '\n';
            }
        }
        if (rc == 1)
        {
            ok = true;     /* промпт = листинг принят целиком */
            break;
        }
    }

    if (ok)
    {
        /* фаза 2: парсинг + NVS уже без давления на UART */
        proto_listing_begin(&sCtx);
        tree_begin();
        {
            char *p = buf;
            char *end = buf + len;
            while (p < end)
            {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
                if (n >= sizeof(line))
                    n = sizeof(line) - 1;
                memcpy(line, p, n);
                line[n] = 0;
                line_to_parser(line);
                p = nl ? nl + 1 : end;
            }
        }
        xSemaphoreTake(sLock, portMAX_DELAY);
        sCtx.inListing = false;
        xSemaphoreGive(sLock);
        /* полный ли листинг? (count из 'i'; иначе - провал/ретрай) */
        if (sCtx.liveCount &&
            sCtx.liveCount >= (sCtx.count ? sCtx.count * 9 / 10
                                          : sCtx.liveCount))
            tree_commit(&sCtx, sCtx.liveCount);
        else
        {
            tree_abort();
            ok = false;
        }
    }

    free(buf);
    if (ok)
        sLastOk = xTaskGetTickCount();
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

/*
 * Кэш единиц "Единицы измерения" (Расход/Период/Объём/Общий):
 * enum-индексы собираются ПОТОКОМ во время листинга (on_param) -
 * сканировать дерево на каждом кадре 'm' больше не нужно (его в ОЗУ
 * и нет - оно в NVS). Индекс = множитель (settings.c прибора).
 */
static float sU_mlPerRate = 1000.0f;
static float sU_tb = 60.0f;
static float sU_totDiv = 1000.0f;
static float sU_gtotDiv = 1000.0f;

static void units_note(const SkeParam *p)
{
    static const float P3[4] = {1.0f, 1000.0f, 1000000.0f, 1000000.0f};
    static const float TB[4] = {1.0f, 60.0f, 3600.0f, 3600.0f};
    uint32_t idx = p->value;
    if (p->type != SKT_ENUM)
        return;
    if (!strcmp(p->name, "Расход"))
        sU_mlPerRate = P3[idx < 4 ? idx : 1];
    else if (!strcmp(p->name, "Период"))
        sU_tb = TB[idx < 4 ? idx : 1];
    else if (!strcmp(p->name, "Объём"))
        sU_totDiv = P3[idx < 4 ? idx : 1];
    else if (!strcmp(p->name, "Общий"))
        sU_gtotDiv = P3[idx < 4 ? idx : 1];
}

/*
 * Новый кадр 'm' прибора несёт только ml-базис (rateMLPM мл/мин,
 * totalml_*, gtotalml). Производные в единицах прибора считаем тут
 * по тем же формулам, что раньше считал сам прибор (settings.c):
 *   rate = rateMLPM x [сек/периода] / (60 x [мл на ед. расхода])
 *   total_* = totalml_* / 1000^ед, gtotal = gtotalml / 1000^ед
 * Множитель K-фактора и коррекция уже встроены в rateMLPM.
 */
static void vals_derive(ProtoCtx *ctx)
{
    SkeValues *v = &ctx->vals;

    v->rate = v->rateMLPM * sU_tb / (60.0f * sU_mlPerRate);
    v->rate_raw = v->rate;
    v->rate_fast = v->rate;
    v->total_plus = v->totalml_plus / sU_totDiv;
    v->total_minus = v->totalml_minus / sU_totDiv;
    v->total = v->total_plus - v->total_minus;
    v->total_sum = v->total_plus + v->total_minus;
    v->gtotal = v->gtotalml / sU_gtotDiv;
}

/* потоковые хуки парсера: параметр листинга -> NVS (+кэш единиц) */
static void on_tree_param(ProtoCtx *ctx, uint16_t id, const SkeParam *p)
{
    units_note(p);
    tree_write(id, ctx, p);
}

static void on_tree_value(ProtoCtx *ctx, uint16_t id, uint32_t raw)
{
    (void)ctx;
    tree_set_value(id, raw);
}

/*
 * Кольцо последних снимков значений (5 Гц опрос прибора):
 * /api/values отдаёт ПАЧКУ за секунду одним запросом, страница
 * проигрывает её по 200 мс. 8 x (SkeValues + ts) ~ 700 Б статики.
 */
#define VAL_RING_MAX 8
static SkeValues sVRing[VAL_RING_MAX];
static uint32_t sVTs[VAL_RING_MAX];
static int sVHead, sVCount;
static SemaphoreHandle_t sVLock;

void ske02_valring_init(void)
{
    sVLock = xSemaphoreCreateMutex();
}

void ske02_valring_push(uint32_t t_ms, const SkeValues *v)
{
    if (!sVLock)
        return;
    xSemaphoreTake(sVLock, portMAX_DELAY);
    sVRing[sVHead] = *v;
    sVTs[sVHead] = t_ms;
    sVHead = (sVHead + 1) % VAL_RING_MAX;
    if (sVCount < VAL_RING_MAX)
        sVCount++;
    xSemaphoreGive(sVLock);
}

int ske02_valring_since(uint32_t since_ms, uint32_t *ts, SkeValues *out, int max)
{
    int n = 0;
    if (!sVLock)
        return 0;
    xSemaphoreTake(sVLock, portMAX_DELAY);
    int first = (sVHead - sVCount + VAL_RING_MAX) % VAL_RING_MAX;
    for (int k = 0; k < sVCount && n < max; k++)
    {
        int i = (first + k) % VAL_RING_MAX;
        if (sVTs[i] > since_ms)
        {
            ts[n] = sVTs[i];
            out[n] = sVRing[i];
            n++;
        }
    }
    xSemaphoreGive(sVLock);
    return n;
}

static bool ske_query_values(void)
{
    bool ok;
    SkeValues snap = {0};
    xSemaphoreTake(sLock, portMAX_DELAY);
    proto_values_restart(&sCtx);
    xSemaphoreGive(sLock);
    if (!txt_command("m", values_cb, NULL, SKE_CMD_TIMEOUT))
        return false;
    xSemaphoreTake(sLock, portMAX_DELAY);
    ok = proto_values_ready(&sCtx);
    if (ok)
    {
        vals_derive(&sCtx);
        snap = sCtx.vals;
    }
    /* БАГ-ФИКС: лок отдавался только при ok — битый ответ прибора
     * оставлял мьютекс взятым НАВСЕГДА, и все portMAX_DELAY-ловцы
     * (httpd-обработчики) намертво вешали однопоточный сервер */
    xSemaphoreGive(sLock);
    if (ok)
    {
        ske02_valring_push(
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS), &snap);
        sLastOk = xTaskGetTickCount();
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* request execution (meter task context only)                         */

typedef struct
{
    int status;   /* first ERR code seen, 0 = none */
    bool seen_ok; /* "OK " line seen */
    uint16_t wait_s; /* lockout remainder from "access: none (lockout)" */
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
    {
        /*
         * "access: menu ..." = уровни разблокированы - успех.
         * "access: none (lockout ~48s)" = пароль ВЕРНЫЙ не был принят
         * (или введён во время блокировки): честно ждём, "ok" нельзя.
         */
        if (strstr(line, " none"))
        {
            const char *lk = strstr(line, "lockout");
            if (lk)
            {
                /*
                 * Формат: "(lockout ~48s)". lk+7 указывает на
                 * " ~48s)" — atoi упирается в '~' и вернёт 0.
                 * Ищем первую цифру после "lockout".
                 */
                const char *num = lk + 7;
                while (*num && (*num < '0' || *num > '9'))
                    num++;
                rc_->status = BS_WAIT;
                rc_->wait_s = (*num) ? (uint16_t)atoi(num) : 0;
                /* защита от мусора: прибор выдаёт максимум 240 с */
                if (rc_->wait_s > 300)
                    rc_->wait_s = 300;
            }
            /* none без lockout: пароль просто не подошёл ни одному
             * уровню - это ошибка доступа */
            else
                rc_->status = BS_ACCESS;
        }
        else
            rc_->seen_ok = true;
    }
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
    case BS_WAIT:           return "ввод пароля заблокирован";
    default:                return "";
    }
}

static void exec_request(ske_req_t *req)
{
    char cmd[64];
    reply_ctx_t rc_;

    memset(&rc_, 0, sizeof(rc_));
    req->err[0] = 0;
    req->wait_s = 0;   /* caller's stack garbage must not leak to JSON */
    req->result = BS_OK;

    switch (req->cmd)
    {
    case SKEQ_SET:
        {
            TreeRec rec;
            if (!tree_get(req->id, &rec) || !rec.p.present)
            {
                req->result = BS_BAD_ID;
                break;
            }
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
        req->wait_s = rc_.wait_s;
        /*
         * "ERR wait" не несёт секунд: после него доспрашиваем статус
         * голым "p", прибор ответит "access: none (lockout ~Ns)".
         */
        if (req->result == BS_WAIT && req->wait_s == 0)
        {
            reply_ctx_t st = {0};
            if (txt_command("p", unlock_cb, &st, SKE_CMD_TIMEOUT) &&
                st.wait_s > 0)
                req->wait_s = st.wait_s;
        }
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
        /* the reset wipes count/version too: re-run the full wake
         * sequence ('e' + 'i'), not just the listing - otherwise
         * count stays 0 and every param looks gone */
        if (ske_echo_off() && ske_info())
        {
            text_start();
            req->result = capture_listing(SKE_LIST_TIMEOUT) ? BS_OK
                                                            : SKE_ERR_TRANSPORT;
        }
        else
            req->result = SKE_ERR_TRANSPORT;
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
                static uint16_t wakeFails;
                sT = xTaskGetTickCount();
                /* ~1 Hz while the meter link is down; read on GPIO2 */
                DBG("ske02: wake: console silent\n");
                if (ske_echo_off() && ske_info())
                {
                    wakeFails = 0;
                    xSemaphoreTake(sLock, portMAX_DELAY);
                    DBG("ske02: %s, %u params\n", sCtx.version,
                        (unsigned)sCtx.count);
                    xSemaphoreGive(sLock);
                    text_start();
                    sState = ST_LIST;
                    sMisses = 0;
                }
                else if (++wakeFails >= 30)
                {
                    /* 30 s of silence: restart the uart driver - a
                     * wedged ring/FIFO never clears by flushing alone */
                    wakeFails = 0;
                    uart_reinit();
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
                    static uint16_t sCfgRev;
                    static TickType_t sLastRelist;
                    sMisses = 0;
                    /* смена версии настроек в кадре 'm' => параметры
                     * меняли (консоль 'S' или меню панели) - перечиты-
                     * ваем листинг. ДЕБАУНС 10с: перечитывание на 1-2с
                     * замораживает опрос значений и грузит CPU - при
                     * частых правках это тормозило шлюз; пропущенные
                     * версии догоняются следующим изменением */
                    if (sCtx.vals.cfg_rev &&
                        sCtx.vals.cfg_rev != sCfgRev)
                    {
                        TickType_t nowT = xTaskGetTickCount();
                        sCfgRev = sCtx.vals.cfg_rev;
                        if (sLastRelist == 0 ||
                            nowT - sLastRelist >= pdMS_TO_TICKS(10000))
                        {
                            sLastRelist = nowT;
                            DBG("ske02: cfg rev %u -> relisting\n",
                                (unsigned)sCtx.vals.cfg_rev);
                            text_start();
                            sState = ST_LIST;
                            break;
                        }
                        DBG("ske02: cfg rev %u (debounced)\n",
                            (unsigned)sCtx.vals.cfg_rev);
                    }
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

void ske02_uart_setup(void)
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
}

void ske02_start(void)
{
    tree_init();
    ske02_valring_init();
    ske02_uart_setup();
    proto_init(&sCtx);
    /* хуки СТРОГО ПОСЛЕ proto_init: он делает memset всего контекста
     * и затирал on_param/on_value - парсер.emitтил в NULL, дерево
     * не писалось (wrote 0, err 0) при живом коммите */
    sCtx.on_param = on_tree_param;
    sCtx.on_value = on_tree_value;
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
                memcpy((void *)&req->wait_s, &sSlot.wait_s, sizeof(uint16_t));
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
