/*
 * Серверная история графиков: по слоту int8[1024] на виджет (максимум
 * два виджета графика). Кадр 'm' приходит 5 раз в секунду - каждое
 * значение нормируется в шкалу слота (lo..hi -> -128..127) и пишется
 * в кольцо. Фронт раз в секунду забирает НОВЫЕ сэмплы (с since),
 * декодирует и докладывает в свой график: дыры при перезагрузке
 * страницы закрываются всеми 5 Гц точками, а не 1 Гц огрызками.
 *
 * Смена шкалы (chart_cfg) сбрасывает кольцо слота: старые int8
 * нормированы по другой шкале и несовместимы.
 */
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "chartbuf.h"
#include "protocol.h"
#include "ske02.h"

static SemaphoreHandle_t sLock;

typedef struct
{
    char src[8];        /* ключ CHSRC ("" = слот свободен) */
    float lo, hi;       /* шкала нормировки */
    int8_t buf[CHART_LEN];
    uint16_t head;      /* следующая позиция записи */
    uint32_t total;     /* всего записано (индекс следующего сэмпла) */
    uint32_t last_ms;   /* время последнего сэмпла */
} chart_slot_t;

static chart_slot_t sSlots[CHART_SLOTS];

void chart_init(void)
{
    sLock = xSemaphoreCreateMutex();
}

/* значение источника из кадра (ключи = CHSRC на странице) */
static float src_value(const char *src, const SkeValues *v)
{
    if (!strcmp(src, "rate")) return v->rate;
    if (!strcmp(src, "total")) return v->total;
    if (!strcmp(src, "tplus")) return v->total_plus;
    if (!strcmp(src, "tminus")) return v->total_minus;
    if (!strcmp(src, "tsum")) return v->total_sum;
    if (!strcmp(src, "gtotal")) return v->gtotal;
    if (!strcmp(src, "batch")) return v->batch;
    if (!strcmp(src, "kf")) return v->kf_value;
    if (!strcmp(src, "price")) return v->kf_value > 0 ? 1.0f / v->kf_value : 0;
    if (!strcmp(src, "freq")) return v->frequency;
    if (!strcmp(src, "pulses")) return (float)v->pulses;
    return NAN;
}

void chart_cfg(int slot, const char *src, float lo, float hi)
{
    chart_slot_t *s;
    if (slot < 0 || slot >= CHART_SLOTS || !(hi > lo))
        return;
    s = &sSlots[slot];
    xSemaphoreTake(sLock, portMAX_DELAY);
    memset(s, 0, sizeof(*s));
    snprintf(s->src, sizeof(s->src), "%s", src);
    s->lo = lo;
    s->hi = hi;
    xSemaphoreGive(sLock);
}

void chart_push(const SkeValues *v, uint32_t t_ms)
{
    int i;
    if (!sLock)
        return;
    xSemaphoreTake(sLock, portMAX_DELAY);
    for (i = 0; i < CHART_SLOTS; i++)
    {
        chart_slot_t *s = &sSlots[i];
        float f;
        int q;
        if (!s->src[0])
            continue;
        f = src_value(s->src, v);
        if (!isfinite(f))
        {
            /* прибора-нет-кадра для источника: пишем край шкалы? нет -
             * пропускаем сэмпл, кольцо держит разрыв времени */
            continue;
        }
        q = (int)((f - s->lo) / (s->hi - s->lo) * 255.0f) - 128;
        if (q < -128)
            q = -128;
        if (q > 127)
            q = 127;
        s->buf[s->head++] = (int8_t)q;
        if (s->head >= CHART_LEN)
            s->head = 0;
        s->total++;
        s->last_ms = t_ms;
    }
    xSemaphoreGive(sLock);
}

/* Новые сэмплы с индекса since: копируются по порядку возрастания.
 * Возврат: число сэмплов (0 = ничего нового); out_total/last_ms/src/
 * lo/hi заполняются всегда (для фронтенда). since=0 и кольцо уже
 * перезаписано -> отдаём всё кольцо (фронт сам сбросится по total). */
int chart_fetch(int slot, uint32_t since, uint32_t upto, int8_t *out,
                int max, uint32_t *out_total, uint32_t *out_last, char *src,
                size_t srccap, float *lo, float *hi)
{
    chart_slot_t *s;
    uint32_t have, n = 0;

    if (slot < 0 || slot >= CHART_SLOTS)
        return 0;
    s = &sSlots[slot];
    xSemaphoreTake(sLock, portMAX_DELAY);
    if (out_total)
        *out_total = s->total;
    if (out_last)
        *out_last = s->last_ms;
    if (src && srccap)
        snprintf(src, srccap, "%s", s->src);
    if (lo)
        *lo = s->lo;
    if (hi)
        *hi = s->hi;
    /* верх диапазона: upto=0 -> до последнего (total) */
    {
        uint32_t end = s->total;
        if (upto && upto < end)
            end = upto;
        if (s->src[0] && since < end)
        {
        uint32_t oldest = s->total > CHART_LEN ? s->total - CHART_LEN : 0;
        uint32_t first = since > oldest ? since : oldest;
        have = end - first;
        /* просили больше максимума: отдаём НЕ САМЫЕ СТАРЫЕ, а окно
         * в конец (новейшие max сэмплов) - при since=0 (свежая
         * страница/сворачивание) это то, что рисуется сразу */
        if (have > (uint32_t)max)
        {
            first += have - (uint32_t)max;
            have = (uint32_t)max;
        }
        while (n < have)
        {
            uint32_t idx = (first + n) % CHART_LEN;
            out[n] = s->buf[idx];
            n++;
        }
        }
    }
    xSemaphoreGive(sLock);
    return (int)n;
}
