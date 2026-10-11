/*
 * SNTP-клиент шлюза. Конфиг в NVS (storage): два сервера (второй -
 * подстраховка первого), смещение UTC для локального времени.
 * Синхронизированное время в дальнейшем уйдёт в прибор для коррекции
 * его таймера DS1307 (когда включена опция "Сервер NTP").
 *
 * Статус храним сами: колбэк sntp_set_time_sync_notification_cb
 * срабатывает на каждый успешный ответ сервера.
 */
#include <string.h>
#include <sys/time.h>

#include "lwip/apps/sntp.h"

#include "debug.h"
#include "ntp.h"
#include "storage.h"

static ntp_cfg_t sCfg;
static bool sSynced = false;
static uint32_t sSyncAt = 0; /* тик последней синхронизации */

static void on_sync(struct timeval *tv)
{
    (void)tv;
    sSynced = true;
    sSyncAt = (uint32_t)xTaskGetTickCount();
    DBG("ntp: time synced\n");
}

void ntp_init(void)
{
    storage_get_ntp(&sCfg);
    sntp_set_time_sync_notification_cb(on_sync);
}

void ntp_apply(void)
{
    sntp_stop();
    sSynced = false;
    if (!sCfg.on || !sCfg.s1[0])
        return;
    /* второй сервер не обязателен: один тоже валиден */
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, sCfg.s1);
    if (sCfg.s2[0])
        sntp_setservername(1, sCfg.s2);
    else
        sntp_setservername(1, NULL);
    sntp_init();
    DBG("ntp: poll %s%s%s\n", sCfg.s1, sCfg.s2[0] ? " + " : "",
        sCfg.s2[0] ? sCfg.s2 : "");
}

const ntp_cfg_t *ntp_cfg(void)
{
    return &sCfg;
}

void ntp_set(const ntp_cfg_t *c)
{
    sCfg = *c;
    storage_set_ntp(&sCfg);
    ntp_apply();
}

bool ntp_synced(void)
{
    return sSynced;
}

/* UTC-секунды с эпохи 1970; имеет смысл только после синхронизации */
uint32_t ntp_epoch_utc(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}
