#ifndef GW_CHARTBUF_H
#define GW_CHARTBUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* серверная история графика: 1 слот x 1024 int8, 5 Гц кадры.
 * Память кольца - ОТДЕЛЬНЫЙ malloc при первом появлении графика
 * на панели (арену листинга больше не делим, меню график не стирает) */
#define CHART_SLOTS 1
#define CHART_LEN   1024
#define CHART_WARMUP 5      /* шкала фиксируется по первым 5 значениям */
#define CHART_DT_MS 200     /* период сэмплов (5 Гц опрос прибора) */

#include "protocol.h"

void chart_init(void);
/* назначить слот: при hi>lo шкала ФИКСИРОВАНА (уставки параметра,
 * значения вне шкалы клиппятся); иначе - прогрев по 5 значениям */
void chart_cfg(int slot, const char *src, float lo, float hi);
bool chart_ready(int slot);                  /* шкала готова (5 значений) */
void chart_push(const SkeValues *v, uint32_t t_ms);
int chart_take(int slot, uint32_t since, int8_t *out, int max,
               uint32_t *out_total, uint32_t *out_first, uint32_t *out_t0,
               float *lo, float *hi);

#endif
