#ifndef GW_SKE02_H
#define GW_SKE02_H

/*
 * Meter task: owns the UART0 link to the SKE-02 service console and the
 * protocol cache (ProtoCtx, see protocol.h). Other tasks talk to it
 * through the command queue below and read snapshots under the mutex.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "protocol.h"

/* commands the meter task executes on the console */
typedef enum
{
    SKEQ_SET,      /* s <id> <packed value> */
    SKEQ_RUN,      /* x <id> */
    SKEQ_UNLOCK,   /* p <pass> */
    SKEQ_REFRESH,  /* full 'l' listing (the "Обновить" button) */
    SKEQ_RESCAN,   /* wipe the cache + full rediscovery */
    SKEQ_REBOOT    /* r */
} ske_cmd_t;

typedef struct
{
    ske_cmd_t cmd;
    uint16_t id;
    char text[40];   /* value text / password */
    int result;      /* BS_* / SKE_ERR_TRANSPORT */
    char err[64];    /* human message, empty on success */
    SemaphoreHandle_t done; /* posted when result is ready */
} ske_req_t;

void ske02_start(void);            /* create the task (from app_main) */

/* async command API: queues the request and waits (timeout ms) for it */
bool ske02_request(const ske_req_t *req, uint32_t timeout_ms);

/* cache access - take the lock, copy what you need, release */
SemaphoreHandle_t ske02_lock(void);
ProtoCtx *ske02_ctx(void);         /* valid ONLY while holding the lock */
const SkeValues *ske02_values(void); /* NULL until the first 'm' frame */

bool ske02_link_up(void);
bool ske02_ready(void);

#endif /* GW_SKE02_H */
