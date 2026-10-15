#ifndef GW_CHARTBUF_H
#define GW_CHARTBUF_H

#include <stdint.h>
#include <stddef.h>

/* серверная история графиков: 2 слота x 1024 int8 (5 Гц кадры) */
#define CHART_SLOTS 2
#define CHART_LEN   1024

#include "protocol.h"

void chart_init(void);
void chart_cfg(int slot, const char *src, float lo, float hi);
void chart_push(const SkeValues *v, uint32_t t_ms);
int chart_fetch(int slot, uint32_t since, uint32_t upto, int8_t *out,
                int max, uint32_t *out_total, uint32_t *out_last, char *src,
                size_t srccap, float *lo, float *hi);

#endif
