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
#include "ntp.h"
#include "web.h"
#include "mb_pass.h"
#include "wifi.h"
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>   /* atoi: lockout seconds */
#include <stdarg.h>   /* va_list: wifi_send_line */
#include <string.h>
#include <time.h>

#include "driver/uart.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "tcpip_adapter.h"

#include "debug.h"
#include "protocol.h"
#include "chartbuf.h"
#include "storage.h"
#include "tree_store.h"

#define SKE_UART_NUM      UART_NUM_0
#define SKE_UART_BAUD     115200 /* единая скорость консоли прибора (см. avr_conf.h) */
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
static char *sListBuf;   /* строки LIST одного уровня */
static uint8_t sLocalSets;
static char sParseBuf[512];  /* буфер разбора ответов GET/SET: rusify
                              * расширяет in-place, локальные 512Б
                              * переполняли стек meter (canary) */  /* наши успешные SET: их cfg_rev ловим
                             * БЕЗ перечита - NVS уже обновлён точечно
                             * (tree_set_value из OK-строки SET) */
#define SKE_LIST_BYTES 2300
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
        if (txt_command("ECHO", echo_cb, last, SKE_CMD_TIMEOUT) &&
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
    return txt_command("INFO", info_cb, NULL, SKE_CMD_TIMEOUT);
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
/*
 * Листинг ПО РАЗДЕЛАМ: прибор отдаёт меню командами L/1/2/3 (l удалена).
 * Каждая секция ~3-5КБ - буфер кучи всего 6К, выделяется даже при
 * поднятом WiFi (прежние 14К для полного 'l' фрагментировались).
 * Схема на секцию: приём целиком в буфер -> парсинг + NVS (UART в
 * это время молчит: следующая команда ещё не отправлена).
 */
static bool capture_listing(TickType_t timeout)
{
    /*
     * ПОУРОВНЕВЫЙ листинг (синтаксис прибора): LIST s (0..2) - один
     * уровень вниз от секции; LIST s m - уровень от m-го подменю;
     * LIST s m n - ещё глубже. Ответ = заголовок цели + подменю
     * ЗАГОЛОВКАМИ (без рекурсии) + параметры уровня (глоб. id).
     * Шлюз идёт очередью: каждый дочерний заголовок в ответе
     * порождает команду уровнем глубже. Ответ уровня < 1.5К - буфер
     * 2.3К в куче, выделяется ОДИН РАЗ (старт, куча девственна).
     */
    char queue[40][14];
    int qt = 0;
    int chunks = 0;
    int len;
    int s;

    if (!sListBuf)
    {
        sListBuf = malloc(2300);   /* строки LIST одного уровня */
        if (!sListBuf)
        {
            DBG("ske02: listing buffer alloc failed\n");
            return false;
        }
    }

    proto_listing_begin(&sCtx);
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.inListing = true;
    sCtx.curSection = 0;
    sCtx.curGroup = 0;
    memset(sCtx.hdrName, 0, sizeof(sCtx.hdrName));
    xSemaphoreGive(sLock);
    tree_begin();

    for (s = 0; s < 3; s++)
    {
        queue[qt][0]='L';queue[qt][1]='I';queue[qt][2]='S';queue[qt][3]='T';
        queue[qt][4]=' ';queue[qt][5]=(char)('0'+s);queue[qt][6]=0;
        qt++;
    }

    /* DFS (стек): дочерние команды кладутся НА ВЕРШИНУ стека и
     * выполняются СРАЗУ за родителем - L3-подменю идут за своими
     * L2, а не все скопом в конце (BFS). Снятие = qt-- (БЕЗ qh:
     * старый qh++ догонял qt и цикл умирал после 2 кусков) */
    while (qt > 0 && chunks < 40)
    {
        char parentCmd[14];
        qt--;   /* снимаем с вершины стека */
        /* СТЁК: родитель лежит в ТОЙ ЖЕ ячейке, куда запишутся дети
         * (первый ребёнок затирал родителя, второй строил команду от
         * уже перезаписанного -> " 07 07"). Копируем до разбора. */
        snprintf(parentCmd, sizeof(parentCmd), "%s", queue[qt]);
        char *pp, *pe;
        int child = 0;
        int nArgs = 1;
        const char *q;
        bool prompt = false;
        bool overflow = false;
        size_t llen = 0;

        /* самопроверка очереди: только A-Z/0-9/пробел и разумная
         * длина; мусор = очередь побита, лучше упасть с логом */
        {
            const char *v;
            for (v = parentCmd; *v; v++)
                if (!((*v >= 'A' && *v <= 'Z') || (*v >= '0' && *v <= '9') ||
                      *v == ' '))
                    break;
            if (*v || v - parentCmd > 13 || parentCmd[0] != 'L')
            {
                DBG("ske02: queue[%d] CORRUPT: %s\n", qt, parentCmd);
                goto fail;
            }
        }

        uart_flush_rx();
        uart_send_line(parentCmd);
        proto_prompt_reset(&sCtx);
        for (;;)
        {
            char line[512];
            int rc = read_line_until_prompt(line, sizeof(line), timeout);
            if (rc == 0)
                break; /* таймаут */
            if (rc == 2)
            {
                size_t n = strlen(line);
                if (llen + n + 1 >= SKE_LIST_BYTES)
                    overflow = true;
                else
                {
                    memcpy(sListBuf + llen, line, n + 1);
                    llen += n + 1;
                }
            }
            if (rc == 1)
            {
                prompt = true;
                break;
            }
        }
        if (overflow || !prompt)
        {
            /* таймаут посередине = частичный захват: НЕ успех */
            DBG("ske02: '%s' failed (%s)\n", parentCmd,
                overflow ? "overflow" : "timeout");
            goto fail;
        }

        for (q = parentCmd; *q; q++)
            if (*q == ' ')
                nArgs++;

        pp = sListBuf;
        pe = sListBuf + llen;
        while (pp < pe)
        {
            char lbuf[512];
            size_t ln;
            char *nul = memchr(pp, 0, (size_t)(pe - pp));
            ln = nul ? (size_t)(nul - pp) : (size_t)(pe - pp);
            if (ln >= sizeof(lbuf))
                ln = sizeof(lbuf) - 1;
            memcpy(lbuf, pp, ln);
            lbuf[ln] = 0;
            /* ГЛАВНОЕ: разбор из КОПИИ. line_to_parser русифицирует
             * строку С РАСШИРЕНИЕМ (гомоглиф 1Б -> кириллица 2Б) -
             * на месте он затирал начало СЛЕДУЮЩЕЙ строки в буфере
             * захвата: заголовки детей умирали до сканирования,
             * строки слипались, дерево теряло куски */
            if (lbuf[0] == '[' && lbuf[1] == 'L')
            {
                /* первый заголовок ответа - сама цель; остальные -
                 * дети: команда на уровень глубже (макс. 3 числа) */
                /* nArgs = слова команды с 'LIST': 2='LIST s', 3='LIST s m',
                 * 4='LIST s m n' (максимум прибора). off-by-one: '< 3'
                 * блокировало ВСЕ L3-запросы - 93 параметра вместо 188 */
                if (child > 0 && nArgs < 4 && qt < 40 && child <= 100)
                {
                    /* ручная сборка: snprintf в буфер 14Б с %s%d
                     * давал -Werror=format-truncation */
                    char *w = queue[qt];
                    const char *r = parentCmd;
                    while (*r && w < queue[qt] + 11)
                        *w++ = *r++;
                    *w++ = ' ';
                    *w++ = (char)('0' + (child - 1) / 10);
                    *w++ = (char)('0' + (child - 1) % 10);
                    *w = 0;

                    qt++;
                }
                child++;
            }
            else if (!strncmp(lbuf, "ERR", 3))
            {
                /* прибор не понял команду: дальше не идём */
            DBG("ske02: '%s' -> %s\n", parentCmd, lbuf);
                goto fail;
            }
            line_to_parser(lbuf);
            pp = nul ? nul + 1 : pe;
        }
        chunks++;
    }

    DBG("ske02: listing done (%d chunks, %u params)\n",
        chunks, (unsigned)sCtx.liveCount);
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.inListing = false;
    xSemaphoreGive(sLock);
    /* полнота = каждый кусок дошёл до промпта */
    tree_commit(&sCtx, sCtx.liveCount);
    sLastOk = xTaskGetTickCount();
    return true;

fail:
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.inListing = false;
    xSemaphoreGive(sLock);
    tree_abort();
    return false;
}

/*
 * Кэш единиц "Единицы измерения" (Расход/Период/Объём/Общий):
 * enum-индексы собираются ПОТОКОМ во время листинга (on_param) -
 * сканировать дерево на каждом кадре 'm' больше не нужно. Индекс
 * = множитель (settings.c прибора).
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

/* мгновенное обновление кэша единиц: зовётся из web после SET
 * enum-а из "Единицы измерения" - иначе до следующего листинга
 * (десятки секунд) /api/values конвертировал бы по старым единицам,
 * а ярлыки на странице уже показывали новые */
void ske02_units_refresh(uint16_t id)
{
    TreeRec rec;
    if (tree_get(id, &rec))
    {
        xSemaphoreTake(sLock, portMAX_DELAY);
        units_note(&rec.p);
        xSemaphoreGive(sLock);
    }
}

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

static void values_cb(char *line, void *user)
{
    (void)user;
    xSemaphoreTake(sLock, portMAX_DELAY);
    sCtx.nowMs = (uint32_t)xTaskGetTickCount();
    proto_values_line(&sCtx, line);
    xSemaphoreGive(sLock);
}

/*
 * Раз в ~5 с проталкиваем прибору состояние WiFi раздельными командами:
 *   WIP <u32>            — IP как десятичное число (a<<24|b<<16|c<<8|d)
 *   WMAC <hi32> <lo32>   — MAC: байты 0..3 и 4..5 двумя числами
 *   WRSSI <int>          — RSSI (со знаком)
 *   WNET <STA:имя|AP:имя> — строка режима+SSID в raw UTF-8
 * Каждая команда проходит через обычный word+number парсер прибора
 * (WNET - текстовый перехватчик), ответ OK/промпт отбрасываем.
 */
#define WIFI_PUSH_EVERY 50 /* 10 с (50 циклов 'm' по 200 мс) */
static void wifi_send_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wifi_send_line(const char *fmt, ...)
{
    static char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    uart_flush_rx();
    uart_write_bytes(SKE_UART_NUM, line, strlen(line));
    uart_write_bytes(SKE_UART_NUM, "\r", 1);
    for (;;)
    {
        char resp[72];
        int rc = read_line_until_prompt(resp, sizeof(resp), pdMS_TO_TICKS(300));
        if (rc != 2)
            break; /* промпт или таймаут */
        /* прибор просит забыть все сети (меню "Forget all Net?") */
        if (!strncmp(resp, "FORGET", 6))
        {
            DBG("wifi: gateway forget signal from meter menu\n");
            storage_net_delall();
            storage_set_wifi("", "");
            storage_save(storage_get());
            esp_restart(); /* перезагрузка в режим портала */
        }
        if (!strncmp(resp, "OK", 2))
            break;
    }
}

static void wifi_push_info(void)
{
    bool sta = wifi_sta_connected();

    if (!sta && !ske02_link_up())
        return;

    tcpip_adapter_ip_info_t ip;
    uint8_t mac[6] = {0};
    int rssi = 0;
    const char *ssid = "";
    uint32_t ipval = 0;

    if (sta)
    {
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK)
            ipval = ip.ip.addr;
        esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            rssi = ap.rssi;
            ssid = (const char *)ap.ssid;
        }
    }
    else
    {
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip) == ESP_OK)
            ipval = ip.ip.addr;
        esp_wifi_get_mac(ESP_IF_WIFI_AP, mac);
        ssid = wifi_ap_ssid();
        if (!ssid)
            ssid = "AP";
    }

    wifi_send_line("WIP %02X%02X%02X%02X",
                   (unsigned)(ipval & 0xFF), (unsigned)((ipval >> 8) & 0xFF),
                   (unsigned)((ipval >> 16) & 0xFF), (unsigned)((ipval >> 24) & 0xFF));
    wifi_send_line("WMAC %02X%02X%02X%02X%02X%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    wifi_send_line("WRSSI %d", rssi);
    wifi_send_line("WNET %s:%s", sta ? "STA" : "AP", ssid);
}

static bool ske_query_values(void)
{
    bool ok;
    SkeValues snap = {0};
    xSemaphoreTake(sLock, portMAX_DELAY);
    proto_values_restart(&sCtx);
    xSemaphoreGive(sLock);
    if (!txt_command("DATA", values_cb, NULL, SKE_CMD_TIMEOUT))
        return false;
    xSemaphoreTake(sLock, portMAX_DELAY);
    ok = proto_values_ready(&sCtx);
    if (ok)
    {
        vals_derive(&sCtx);
        snap = sCtx.vals;
    }
    /* БАГ-ФИКС: лок отдавался только при ok - битый ответ прибора
     * оставлял мьютекс взятым НАВСЕГДА, и все portMAX_DELAY-ловцы
     * (httpd-обработчики) намертво вешали однопоточный сервер */
    xSemaphoreGive(sLock);
    if (ok)
    {
        ske02_valring_push(
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS), &snap);
        /* серверное кольцо графиков питается ЗДЕСЬ, из задачи Meter,
         * независимо от WiFi/httpd: обрыв связи браузера дыр не даёт */
        chart_push(&snap, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
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

/* ответ "G <id>": БЕЗ "OK", строка параметра приходит сама */
/* копия value_line в общий буфер (rusify расширяет in-place) */
static char *sParseBuf_filled(reply_ctx_t *rc_)
{
    snprintf(sParseBuf, sizeof(sParseBuf), "%s", rc_->value_line);
    return sParseBuf;
}

static void get_cb(char *line, void *user)
{
    reply_ctx_t *rc_ = (reply_ctx_t *)user;
    if (!strncmp(line, "ERR", 3))
    {
        if (!rc_->status)
            rc_->status = BS_BAD_ID;
        return;
    }
    if (!rc_->seen_ok && line[0])
    {
        rc_->seen_ok = true;
        strncpy(rc_->value_line, line, sizeof(rc_->value_line) - 1);
        rc_->value_line[sizeof(rc_->value_line) - 1] = 0;
    }
}

/*
 * ТОЧЕЧНЫЙ ОБХОД ЗНАЧЕНИЙ (fallback, когда маркер правки прыжком
 * больше 1 или его нет): G <id> по каждой известной записи дерева,
 * NVS обновляет только изменившиеся (tree_set_value). Структуру не
 * трогаем - с клавиатуры/консоли SET изменить имена/типы/опции нельзя.
 * false = транспорт лёг (rev НЕ принят, обход повторится).
 */
static void gw_features_find(void);
static void gw_feature_apply(uint16_t id);
static void gw_features_apply_all(void);

static bool sweep_values(void)
{
    static TreeRec rec;   /* ~755Б на стеке + NVS-цепь tree_set_value
                           * при разборе - канарейка meter; meter-only */
    uint16_t id;
    for (id = 0; id <= sCtx.maxId && sCtx.maxId; id++)
    {
        char cmd[16];
        reply_ctx_t rc_;
        if (!tree_get(id, &rec) || !rec.p.present)
            continue;
        if (rec.p.type == SKT_CMD)
            continue;   /* команды значения не имеют */
        snprintf(cmd, sizeof(cmd), "GET %u", (unsigned)id);
        memset(&rc_, 0, sizeof(rc_));
        if (!txt_command(cmd, get_cb, &rc_, SKE_CMD_TIMEOUT))
            return false;
        if (rc_.seen_ok && rc_.value_line[0])
            line_to_parser(sParseBuf_filled(&rc_));
    }
    /* кэш единиц (sU_*): enum-ы "Единицы измерения" могли смениться
     * с клавиатуры, листинга-обновления больше нет */
    for (id = 0; sCtx.maxId && id <= sCtx.maxId; id++)
        if (tree_get(id, &rec) && rec.p.present)
        {
            xSemaphoreTake(sLock, portMAX_DELAY);
            units_note(&rec.p);
            xSemaphoreGive(sLock);
        }
    /* id фич могли смениться (relist после ребута прибора) */
    gw_features_find();
    gw_features_apply_all();
    return true;
}

/* ---- фичи шлюза из дерева прибора --------------------------------
 * «Wi-Fi gateway» в меню прибора: WEB panel / Modbus TCP/IP / OTA.
 * Обычные параметры дерева: прибор хранит, шлюз применяет. Поиск id
 * по латинским токенам в имени (WEB/Modbus/OTA - одинаковы в обоих
 * языках прибора). Отсутствуют (старая прошивка) - всё включено. */
static uint16_t sFwWeb = 0xFFFF, sFwMb = 0xFFFF, sFwOta = 0xFFFF;

static void gw_features_find(void)
{
    static TreeRec rec;   /* ~755Б: только задача meter, как в sweep */
    uint16_t id;
    sFwWeb = sFwMb = sFwOta = 0xFFFF;
    for (id = 0; sCtx.maxId && id <= sCtx.maxId; id++)
        if (tree_get(id, &rec) && rec.p.present && rec.p.name[0])
        {
            if (strstr(rec.p.name, "WEB"))
                sFwWeb = id;
            else if (strstr(rec.p.name, "Modbus"))
                sFwMb = id;
            else if (strstr(rec.p.name, "OTA"))
                sFwOta = id;
        }
}

static void gw_feature_apply(uint16_t id)
{
    TreeRec rec;
    bool on;
    if (id == 0xFFFF || !tree_get(id, &rec) || !rec.p.present)
        return;
    on = rec.p.value != 0;
    DBG("gw: feature id=%u on=%d (web=%u mb=%u ota=%u)\n",
        (unsigned)id, (int)on,
        (unsigned)sFwWeb, (unsigned)sFwMb, (unsigned)sFwOta);
    if (id == sFwWeb)
        web_httpd_enable(on);
    else if (id == sFwMb)
        mb_pass_enable(on);
    else if (id == sFwOta)
        web_ota_enable(on);
}

static void gw_features_apply_all(void)
{
    gw_feature_apply(sFwWeb);
    gw_feature_apply(sFwMb);
    gw_feature_apply(sFwOta);
}

/* синхронизация ОДНОГО пункта: G <id> -> строка -> tree_set_value.
 * Так ловится правка с клавиатуры прибора: маркер (edit_id/cnt)
 * приезжает в кадре 'm', одна команда вместо обхода/листинга */
static void one_param_sync(uint16_t id)
{
    static TreeRec rec;   /* ~755Б: на стеке + второй такой же в
                           * ske02_units_refresh рвали канарейку meter */
    char cmd[16];
    reply_ctx_t rc_;

    if (!tree_get(id, &rec) || !rec.p.present || rec.p.type == SKT_CMD)
        return;
    snprintf(cmd, sizeof(cmd), "GET %u", (unsigned)id);
    memset(&rc_, 0, sizeof(rc_));
    if (txt_command(cmd, get_cb, &rc_, SKE_CMD_TIMEOUT) &&
        rc_.seen_ok && rc_.value_line[0])
        line_to_parser(sParseBuf_filled(&rc_));
    {
        TreeRec after;
        if (tree_get(id, &after))
            DBG("ske02: sync %u -> raw=%lu\n", (unsigned)id,
                (unsigned long)after.p.value);
    }
    /* кэш единиц: rec уже прочитан - БЕЗ повторного tree_get+NVS
     * (ske02_units_refresh заводил второй TreeRec и всю NVS-цепь) */
    xSemaphoreTake(sLock, portMAX_DELAY);
    units_note(&rec.p);
    xSemaphoreGive(sLock);
    /* «Wi-Fi gateway»: включение/выключение фич шлюза */
    gw_feature_apply(id);
}

static void unlock_cb(char *line, void *user)
{
    reply_ctx_t *rc_ = (reply_ctx_t *)user;
    if (!strncmp(line, "access:", 7))
    {        /*
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

/* собрать текст "HHMMSS DDMMYY" локального времени; false = нечего
 * слать (NTP выключен или не синхронизирован) */
static bool time_text(char *text, size_t cap)
{
    struct tm tm;
    time_t t;
    const ntp_cfg_t *nc = ntp_cfg();

    if (!nc->on || !ntp_synced())
        return false;
    t = (time_t)ntp_epoch_utc() + (time_t)ntp_tz_effective() * 3600;
    gmtime_r(&t, &tm);
    snprintf(text, cap, "%02d%02d%02d %02d%02d%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    return true;
}

static void exec_request(ske_req_t *req); /* вперёд: из push_time_if_needed */

/* однократная отправка NTP-времени в прибор (DS1307) — ДЛЯ ЧУЖИХ
 * задач (httpd): через очередь ske02_request. Из задачи Meter звать
 * НЕЛЬЗЯ - она ждёт сама себя (deadlock на 5 с на каждый тик). */
bool ske02_time_push(char *err, size_t errcap)
{
    ske_req_t r;

    memset(&r, 0, sizeof(r));
    if (!time_text(r.text, sizeof(r.text)))
    {
        snprintf(err, errcap, "%s", "время не синхронизировано");
        return false;
    }
    r.cmd = SKEQ_SETTIME;
    ske02_request(&r, 5000);
    DBG("ske02: time push %s -> %s\n", r.text, r.err);
    snprintf(err, errcap, "%s", r.err[0] ? r.err : "прибор отказал");
    return r.result == BS_OK;
}

/* автоотправка: сразу после первой синхронизации, далее не чаще
 * 2 раз в сутки (12 ч). Выполняется В ЗАДАЧЕ METER: exec_request
 * напрямую, без очереди (иначе самоожидание) */
static void push_time_if_needed(void)
{
    static TickType_t sLastPush;
    static bool sEverPushed;
    ske_req_t r;
    TickType_t now = xTaskGetTickCount();

    if (sEverPushed && now - sLastPush < pdMS_TO_TICKS(12UL * 3600 * 1000))
        return;
    /* ВТОРОЙ гейт по настенным часам в NVS: static-переменные живут
     * только до рестарта, и тихие ребуты снова и снова слали TIME */
    {
        time_t nowE = time(NULL);
        uint32_t lastE = storage_timepush_get();
        if (nowE > 1600000000UL && lastE > 1600000000UL &&
            (uint32_t)nowE - lastE < 12UL * 3600UL)
        {
            sLastPush = now;      /* синхронизируем и тиковый гейт */
            sEverPushed = true;
            return;
        }
    }
    memset(&r, 0, sizeof(r));
    if (!time_text(r.text, sizeof(r.text)))
        return;
    r.cmd = SKEQ_SETTIME;
    exec_request(&r);
    DBG("ske02: auto time push %s -> %s\n", r.text, r.err);
    if (r.result == BS_OK)
    {
        sLastPush = now;
        sEverPushed = true;
        {
            time_t nowE = time(NULL);
            if (nowE > 1600000000UL)
                storage_timepush_set((uint32_t)nowE);
        }
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
    case SKEQ_SETTIME:
        {
            /* req->text уже "HHMMSS DDMMYY" (локальное время) */
            snprintf(cmd, sizeof(cmd), "TIME %s", req->text);
            if (!txt_command(cmd, reply_cb, &rc_, SKE_CMD_TIMEOUT))
            {
                req->result = SKE_ERR_TRANSPORT;
                break;
            }
            req->result = rc_.seen_ok ? BS_OK
                              : (rc_.status ? rc_.status : BS_BAD_TYPE);
            snprintf(req->err, sizeof(req->err), "%.40s", rc_.value_line);
        }
        break;

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
        snprintf(cmd, sizeof(cmd), "SET %u %s", (unsigned)req->id, req->text);
        if (!txt_command(cmd, reply_cb, &rc_, SKE_CMD_TIMEOUT))
        {
            req->result = SKE_ERR_TRANSPORT;
            break;
        }
        if (rc_.seen_ok && rc_.value_line[0])
            line_to_parser(sParseBuf_filled(&rc_));
        /* OK-строка -> on_value -> tree_set_value: NVS уже
         * обновлён ТОЧЕЧНО, перечит всего дерева не нужен */
        req->result = rc_.seen_ok ? BS_OK
                                  : (rc_.status ? rc_.status : BS_BAD_TYPE);
        if (req->result == BS_OK)
        {
            sLocalSets++;   /* ближайший скачок cfg_rev - наш: не листить */
            sLastOk = xTaskGetTickCount();
        }
        break;

    case SKEQ_RUN:
        snprintf(cmd, sizeof(cmd), "RUN %u", (unsigned)req->id);
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

    case SKEQ_CMD:
        /* консольное слово как есть (кнопки дозатора BSTART/BPAUSE/
         * BSTOP/TRESET; белый список проверяет web-слой) */
        snprintf(cmd, sizeof(cmd), "%s", req->text);
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
        snprintf(cmd, sizeof(cmd), "PASS %s", req->text);
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
            if (txt_command("PASS", unlock_cb, &st, SKE_CMD_TIMEOUT) &&
                st.wait_s > 0)
                req->wait_s = st.wait_s;
        }
        if (req->result == BS_OK)
            sLastOk = xTaskGetTickCount();
        break;

    case SKEQ_REFRESH:
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
        uart_send_line("REBOOT");
        vTaskDelay(pdMS_TO_TICKS(200));
        sState = ST_WAKE;
        sT = 0;
        sReady = false;
        break;

    case SKEQ_MODBUS:
        {
            /* MB <hex>: прибор дописывает CRC сам, ответ "MBR <hex>".
             * Кадр до 253 байт -> 506 hex-символов: статические буферы,
             * очередь ske02 гарантирует одну транзакцию в момент. */
            static char mbout[2 * 253 + 8];
            static char mbline[2 * 255 + 8];
            uint16_t i;
            int o = snprintf(mbout, sizeof(mbout), "MB ");
            for (i = 0; i < req->mb_reqlen; i++)
                o += snprintf(mbout + o, sizeof(mbout) - o, "%02X",
                              req->mb_req[i]);
            uart_flush_rx();
            uart_write_bytes(SKE_UART_NUM, mbout, strlen(mbout));
            uart_write_bytes(SKE_UART_NUM, "\r", 1);

            *req->mb_rlen = 0;
            req->result = SKE_ERR_TRANSPORT;
            TickType_t tmo = pdMS_TO_TICKS(SKE_CMD_TIMEOUT);
            for (;;)
            {
                int rc = read_line_until_prompt(mbline, sizeof(mbline), tmo);
                if (rc == 0)
                    break; /* таймаут: прибор молчит */
                if (rc == 1)
                    break; /* приглашение без MBR - не должно случаться */
                if (!strncmp(mbline, "MBR ", 4))
                {
                    const char *h = mbline + 4;
                    uint16_t n = 0;
                    while (isxdigit((unsigned char)h[0]) &&
                           isxdigit((unsigned char)h[1]) && n < 255)
                    {
                        int hi = (h[0] <= '9') ? h[0] - '0'
                                               : (h[0] | 0x20) - 'a' + 10;
                        int lo = (h[1] <= '9') ? h[1] - '0'
                                               : (h[1] | 0x20) - 'a' + 10;
                        req->mb_resp[n++] = (uint8_t)((hi << 4) | lo);
                        h += 2;
                    }
                    *req->mb_rlen = n;
                    req->result = BS_OK;
                    break;
                }
                if (!strncmp(mbline, "MBE", 3))
                {
                    /* прибор ответил ошибкой: для мастера это "нет
                     * ответа" - пусть повторяет/разбирается сам */
                    break;
                }
                /* прочие строки игнорируем (мусор линии) */
            }
        }
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

static volatile bool sSuspended = false; /* сессия прошивки AVR: обмен на паузе */

void ske02_suspend(bool on)
{
    sSuspended = on;
    if (on)
        vTaskDelay(pdMS_TO_TICKS(300)); /* задача прибора успевает замолчать */
}

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

        if (sSuspended)
        {
            /* UART отдан программатору AVR: не слать ничего в линию */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
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
                    /* дерево УЖЕ прочитано этой загрузкой (обрыв связи
                     * не стирает NVS): перечитывать нечего - скачем
                     * в опрос значений. Реальные изменения прибора
                     * поймает cfg_rev в ST_IDLE. Без этого каждый
                     * wifi-обрыв рвёт DATA -> полный wake-обмен
                     * с двойным листингом. */
                    /* sReady (а НЕ tree_count - тот читает старое
                     * дерево из NVS и на ПЕРВОМ wake хватал ST_IDLE
                     * без листинга - sReady не ставился, wifi ждал 180с) */
                    sState = sReady ? ST_IDLE : ST_LIST;
                    sMisses = 0;
                    sT = xTaskGetTickCount();
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
                /* liveCount = реально разобрано из секций 1/2/3;
                 * count из 'i' считает ВСЕ меню (вкл. Настройки) и
                 * только путает в логе */
                DBG("ske02: listing done (%u params)\n", (unsigned)sCtx.liveCount);
                xSemaphoreGive(sLock);
                /* дерево свежее: найти и применить фичи «Wi-Fi
                 * gateway» (веб/modbus/OTA) из дерева прибора */
                gw_features_find();
                gw_features_apply_all();
            }
            else
            {
                /* пауза между попытками: без неё неудачный листинг
                 * молотил линию без остановки (веб-запросы висли) */
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (++sMisses >= SKE_WAKE_MISS)
                {
                    sMisses = 0;
                    sState = ST_WAKE;
                    sT = 0;
                }
            }
            break;

        case ST_IDLE:
            if (xTaskGetTickCount() - sT >= SKE_VALUES_PERIOD)
            {
                static uint8_t sWifiCnt;
                sT = xTaskGetTickCount();
                if (ske_query_values())
                {
                    /* WiFi-инфо для меню прибора: раз в ~5 с (25 циклов 'm') */
                    if (++sWifiCnt >= WIFI_PUSH_EVERY)
                    {
                        sWifiCnt = 0;
                        wifi_push_info();
                    }
                    static TickType_t lastBeat;
                    static uint8_t sEditCnt;   /* прошлый маркер правки */
                    static bool sEditInit;
                    static TickType_t sLastRelist;
                    sMisses = 0;
                    /* МАРКЕР ПРАВКИ (7-8 поля кадра 'm'; cfg_rev убран):
                     * правка прибора = скачок счётчика. НАЗАД = ребут
                     * прибора - структура меню могла смениться после
                     * перепрошивки, честный полный relist */
                    if (!sEditInit)
                    {
                        /* ПЕРВЫЙ кадр (а не первая правка!): принимаем
                         * маркер; если прибор уже правилили ПОСЛЕ
                         * листинга - один обход значений догоняет */
                        sEditInit = true;
                        sEditCnt = sCtx.vals.edit_cnt;
                        if (sCtx.vals.edit_cnt)
                        {
                            DBG("ske02: edits during listing, sweeping\n");
                            sweep_values();
                        }
                    }
                    else if (sCtx.vals.edit_cnt != sEditCnt)
                    {
                        /* счётчик НАЗАД = ребут прибора */
                        if (sCtx.vals.edit_cnt < sEditCnt)
                        {
                            sEditCnt = sCtx.vals.edit_cnt;
                            DBG("ske02: edit cnt back -> device reboot, "
                                "relisting\n");
                            sState = ST_LIST;
                        }
                        else
                        {
                            uint8_t delta = (uint8_t)(sCtx.vals.edit_cnt -
                                                      sEditCnt);
                            sEditCnt = sCtx.vals.edit_cnt;
                            DBG("ske02: marker cnt=%u delta=%u id=%u\n",
                                (unsigned)sCtx.vals.edit_cnt,
                                (unsigned)delta,
                                (unsigned)sCtx.vals.edit_id);
                            if (sLocalSets)
                            {
                                /* наш SET: OK-строка уже обновила NVS,
                                 * GET не нужен; фичи - применить */
                                sLocalSets = 0;
                                gw_feature_apply(sCtx.vals.edit_id);
                                DBG("ske02: own set, no resync\n");
                            }
                            else if (delta == 1 &&
                                     sCtx.vals.edit_id != 0xFFFF)
                            {
                                one_param_sync(sCtx.vals.edit_id);
                                DBG("ske02: edit %u synced (marker)\n",
                                    (unsigned)sCtx.vals.edit_id);
                            }
                            else
                            {
                                /* несколько правок между опросами */
                                TickType_t nowT = xTaskGetTickCount();
                                if (sLastRelist == 0 ||
                                    nowT - sLastRelist >=
                                        pdMS_TO_TICKS(10000))
                                {
                                    sLastRelist = nowT;
                                    if (sweep_values())
                                        DBG("ske02: values swept\n");
                                    else
                                        DBG("ske02: sweep broken, retry\n");
                                }
                            }
                        }
                    }
                    push_time_if_needed();
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
    xTaskCreate(meter_task, "meter", 3584, NULL, 5, NULL);
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

bool ske02_modbus(const uint8_t *req, uint16_t reqlen, uint8_t *resp,
                  uint16_t *rlen, uint32_t timeout_ms)
{
    ske_req_t r;
    memset(&r, 0, sizeof(r));
    r.cmd = SKEQ_MODBUS;
    r.mb_req = req;
    r.mb_reqlen = reqlen;
    r.mb_resp = resp;
    r.mb_rlen = rlen;
    *rlen = 0;
    return ske02_request(&r, timeout_ms);
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
