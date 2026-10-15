/*
 * Серверная история графиков: по слоту int8[1024] на виджет (максимум
 * два виджета графика). Кадр 'm' приходит 5 раз в секунду - каждое
 * значение нормируется в шкалу слота (lo..hi -> -128..127) и пишется
 * в кольцо. Шкала НЕ приходит от клиента: она фиксируется по ПЕРВЫМ
 * ПЯТИ значениям источника (min/max + 15% запаса) - так кривая
 * всегда занимает весь динамический диапазон int8, а клиенту шкала
 * отдаётся вместе с сэмплами.
 *
 * Сэмплы не запрашиваются отдельно: /api/values возвращает их тем же
 * ответом (параметры s0/s1 = источники слотов, c0/c1 = курсоры),
 * последовательными страницами от курсора - backlog после обрыва
 * связи вычитывается за несколько опросов без лишних запросов.
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
    float lo, hi;       /* шкала: фиксируется по первым 5 значениям */
    int8_t *buf;        /* данные - ОТДЕЛЬНЫЙ malloc(CHART_LEN) при
                         * первом появлении графика на панели */
    uint16_t head;      /* следующая позиция записи */
    uint32_t total;     /* всего записано (индекс следующего сэмпла) */
    uint32_t last_ms;   /* время последнего сэмпла */
    bool fixed;          /* шкала задана уставками параметра */
    float first5[CHART_WARMUP];
    uint32_t first5_ms[CHART_WARMUP];
    uint8_t n5;         /* сколько из первых значений накоплено */
} chart_slot_t;

static chart_slot_t sSlots[CHART_SLOTS];

/* Кольцо живёт в СВОЕЙ памяти (malloc при первом назначении слота):
 * листинг меню его не затирает, при смене настроек график живёт дальше */
static int8_t *sRingAlloc(void)
{
    static int8_t *ring;
    if (!ring)
        ring = malloc(CHART_LEN);   /* 1К, только когда график есть */
    return ring;
}

void chart_init(void)
{
    sLock = xSemaphoreCreateMutex();
}

/* значение источника из кадра (ключи = CHSRC на странице) */
static float src_value(const char *src, const SkeValues *v)
{
    if (!strcmp(src, "rate")) return v->rate;
    /* "Расход (Qmax)" - всегда л/мин, как у барграфа: уставка Qmax
     * задана в л/мин, а кадр 'm' приходит в единицах прибора */
    if (!strcmp(src, "qmax")) return v->rateMLPM / 1000.0f;
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

static int8_t quantize(float f, float lo, float hi)
{
    int q = (int)((f - lo) / (hi - lo) * 255.0f) - 128;
    if (q < -128)
        q = -128;
    if (q > 127)
        q = 127;
    return (int8_t)q;
}

/* значение вышло за шкалу (первые 5 значений не могли знать размах
 * медленно гуляющего сигнала): расширияем границы и ПЕРЕРАЗМЕТКА
 * кольца под новую шкалу - старые int8 читаются по старым lo/hi и
 * пишутся по новым. Расширение с запасом - редкое событие, не дрожит */
static void slot_rescale(chart_slot_t *s, float f)
{
    float lo = s->lo, hi = s->hi;
    float pad;
    int i;
    if (f > hi) hi = f;
    if (f < lo) lo = f;
    pad = (hi - lo) * 0.05f;
    lo -= pad;
    hi += pad;
    if (lo < 0)   /* источники неотрицательные: низ всегда 0 */
        lo = 0;
    for (i = 0; i < CHART_LEN; i++)
    {
        float v = s->lo + ((float)s->buf[i] + 128.0f) / 255.0f * (s->hi - s->lo);
        s->buf[i] = quantize(v, lo, hi);
    }
    s->lo = lo;
    s->hi = hi;
}

static void slot_write(chart_slot_t *s, float f, uint32_t t_ms)
{
    /* фиксированная шкала - ТУПО клиппим (уставки параметра);
     * авто-шкала - расширяем при выходе за границы */
    if (!s->fixed && (f > s->hi || f < s->lo))
        slot_rescale(s, f);
    s->buf[s->head] = quantize(f, s->lo, s->hi);
    if (++s->head >= CHART_LEN)
        s->head = 0;
    s->total++;
    s->last_ms = t_ms;
}

/* смена шкалы ПРИ ЖИВОМ кольце (авто->фикс после загрузки уставок,
 * правка уставки): НЕ сбрасываем - переразмечаем int8 по новой
 * шкале, история сохраняется (вне диапазона - клиппится) */
static void slot_remap(chart_slot_t *s, float lo, float hi)
{
    int i;
    if (!(hi > lo))
        return;
    if (s->buf && s->hi > s->lo && s->total)
        for (i = 0; i < CHART_LEN; i++)
        {
            float v = s->lo +
                ((float)s->buf[i] + 128.0f) / 255.0f * (s->hi - s->lo);
            s->buf[i] = quantize(v, lo, hi);
        }
    s->lo = lo;
    s->hi = hi;
    s->fixed = true;
}

/* назначение слота: src совпадает = no-op (каждый /api/values шлёт
 * s0 повторно); смена ИСТОЧНИКА сбрасывает счётчики и шкалу */
void chart_cfg(int slot, const char *src, float lo, float hi)
{
    chart_slot_t *s;
    if (slot < 0 || slot >= CHART_SLOTS || !src || !src[0])
        return;
    s = &sSlots[slot];
    xSemaphoreTake(sLock, portMAX_DELAY);
    if (strcmp(s->src, src) != 0)
    {
        memset(s, 0, sizeof(*s));
        if (!sRingAlloc())
        {
            xSemaphoreGive(sLock);
            return;   /* кучи нет - графика нет */
        }
        s->buf = sRingAlloc();
        snprintf(s->src, sizeof(s->src), "%s", src);
    }
    /* авто-запрос (hi<=lo) шкалу НЕ трогает: прогретая шкала слота
     * не равна (0,0) и сравнение сбрасывало кольцо КАЖДЫЙ опрос.
     * Фиксированная шкала: ПЕРЕМАСШТАБИРУЕМ кольцо, не сбрасываем */
    if (hi > lo && (!s->fixed || s->lo != lo || s->hi != hi))
        slot_remap(s, lo, hi);
    else if (hi <= lo && s->fixed)
        s->fixed = false;   /* авто: значения остаются в текущей шкале */
    xSemaphoreGive(sLock);
}

bool chart_ready(int slot)
{
    return slot >= 0 && slot < CHART_SLOTS && sSlots[slot].hi > sSlots[slot].lo;
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
        if (!s->src[0] || !s->buf)
            continue;
        f = src_value(s->src, v);
        if (!isfinite(f))
            continue;   /* нет поля в кадре: кольцо держит разрыв */
        if (!s->fixed && !(s->hi > s->lo))
        {
            /* прогрев: копим первые значения для выбора шкалы */
            if (s->n5 < CHART_WARMUP)
            {
                s->first5[s->n5] = f;
                s->first5_ms[s->n5] = t_ms;
                s->n5++;
            }
            if (s->n5 < CHART_WARMUP)
                continue;
            {
                float lo = s->first5[0], hi = s->first5[0];
                int k;
                float pad;
                for (k = 1; k < CHART_WARMUP; k++)
                {
                    if (s->first5[k] < lo) lo = s->first5[k];
                    if (s->first5[k] > hi) hi = s->first5[k];
                }
                if (hi - lo < 1e-9f)
                {
                    lo -= 0.5f;
                    hi += 0.5f;
                }
                pad = (hi - lo) * 0.05f;
                lo -= pad;
                hi += pad;
                /* все источники неотрицательные: низ шкалы всегда 0
                 * (минусовой запас рисовал ось ниже нуля при нуле
                 * расхода) */
                if (lo < 0)
                    lo = 0;
                s->lo = lo;
                s->hi = hi;
                /* накопленные значения вписываем в кольцо задним числом */
                for (k = 0; k < CHART_WARMUP; k++)
                    slot_write(s, s->first5[k], s->first5_ms[k]);
            }
            continue;
        }
        slot_write(s, f, t_ms);
    }
    xSemaphoreGive(sLock);
}

/* Страница сэмплов ПО ПОРЯДКУ от курсора since (последовательное
 * вычитывание: несколько опросов разбирают любой backlog). Дыры нет:
 * если since уже перезаписан кольцом, стартуем с самого старого
 * живого сэмпла. t0 = время первого отданного сэмпла (мс от старта),
 * шаг сэмплов постоянен CHART_DT_MS. */
int chart_take(int slot, uint32_t since, int8_t *out, int max,
               uint32_t *out_total, uint32_t *out_first, uint32_t *out_t0,
               float *lo, float *hi)
{
    chart_slot_t *s;
    uint32_t first = 0, have, n = 0;

    if (slot < 0 || slot >= CHART_SLOTS)
        return 0;
    s = &sSlots[slot];
    xSemaphoreTake(sLock, portMAX_DELAY);
    if (out_total)
        *out_total = s->total;
    if (lo)
        *lo = s->lo;
    if (hi)
        *hi = s->hi;
    if (s->src[0] && s->buf && s->hi > s->lo && since < s->total)
    {
        uint32_t oldest = s->total > CHART_LEN ? s->total - CHART_LEN : 0;
        first = since > oldest ? since : oldest;
        have = s->total - first;
        if (have > (uint32_t)max)
            have = (uint32_t)max;
        while (n < have)
        {
            out[n] = s->buf[(first + n) % CHART_LEN];
            n++;
        }
        if (out_t0)
            *out_t0 = s->last_ms -
                      (s->total - 1 - first) * CHART_DT_MS;
        if (out_first)
            *out_first = first;
    }
    xSemaphoreGive(sLock);
    return (int)n;
}
