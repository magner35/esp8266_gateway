#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "debug.h"
#include "ske02.h"
#include "mb_pass.h"

/*
 * Прозрачный Modbus TCP (порт 502). Одна транзакция в момент (RTU-движок
 * прибора одиночный); до MBP_MAX_CONN подключений, каждое обслуживается
 * своей транзакзиентной задачей. Транспорт к прибору - консольная
 * команда MB/MBR через очередь ske02 (UART трогает только meter task).
 */

#define MBP_PORT         502
#define MBP_MAX_CONN     2   /* лимит ОДНОВРЕМЕННЫХ клиентов: +1 = ~3.2К кучи */
#define MBP_RXBUF        300
#define MBP_TRANSACT_MS  800

static volatile int sClients; /* подключено прямо сейчас */
static volatile bool sEnabled = true; /* фича «Wi-Fi gateway: Modbus» */

void mb_pass_enable(bool on)
{
    sEnabled = on;
    DBG("mbp: %s\n", on ? "enabled" : "disabled (feature off)");
}

static uint16_t mb_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

/* Один MBAP-запрос -> RTU (MB) -> прибор -> MBAP-ответ. Ответ может не
 * уйти (таймаут прибора / широковещательный кадр): соединение живёт,
 * мастер повторит сам. */
static void cli_process(int fd, uint8_t *b)
{
    uint16_t mlen = (uint16_t)((b[4] << 8) | b[5]); /* uid + pdu */
    if (mlen < 2 || (uint16_t)(6 + mlen) > MBP_RXBUF)
        return;

    uint8_t resp[260];
    uint16_t rlen = 0;
    uint16_t pdu_len = mlen - 1;

    /* RTU-кадр: адрес + PDU; CRC допишет прибор */
    if (!ske02_modbus(b + 6, mlen, resp, &rlen, MBP_TRANSACT_MS))
        return; /* прибор не ответил (или широковещательный кадр) */

    /* прибор возвращает полный кадр с CRC */
    if (rlen < 4)
        return;
    uint16_t crc = mb_crc16(resp, (uint16_t)(rlen - 2));
    if (resp[rlen - 2] != (uint8_t)crc || resp[rlen - 1] != (uint8_t)(crc >> 8))
        return;

    uint8_t out[263];
    uint16_t out_len = (uint16_t)(rlen - 2); /* uid + pdu, без CRC */
    out[0] = b[0];                           /* тот же transaction id */
    out[1] = b[1];
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint8_t)(out_len >> 8);
    out[5] = (uint8_t)out_len;
    out[6] = resp[0];
    memcpy(out + 7, resp + 1, (uint16_t)(rlen - 3));

    int sent = 0;
    while (sent < 6 + out_len)
    {
        int n = send(fd, out + sent, (uint16_t)(6 + out_len - sent), 0);
        if (n <= 0)
            return; /* клиент отвалился - не страшно */
        sent += n;
    }
}

/* Задача-клиент прозрачного сервера (транзиентная, память на время сессии). */
static void mb_pass_client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint16_t got = 0;
    uint8_t buf[MBP_RXBUF];

    for (;;)
    {
        /* фича выключена прямо посреди соединения: не обслуживаем
         * дальше и рвём связь - мастер с keep-alive иначе работал бы
         * бесконечно (гейт был только на новых accept-ах) */
        if (!sEnabled)
            break;
        int n = recv(fd, buf + got, MBP_RXBUF - got, 0);
        if (n <= 0)
            break;
        got = (uint16_t)(got + n);

        for (;;)
        {
            if (got < 7)
                break;
            uint16_t mlen = (uint16_t)((buf[4] << 8) | buf[5]);
            uint16_t total = (uint16_t)(6 + mlen);
            if (mlen < 2 || total > MBP_RXBUF)
                goto out; /* битый заголовок - клиент неисправим */
            if (got < total)
                break;
            cli_process(fd, buf);
            uint16_t rest = (uint16_t)(got - total);
            memmove(buf, buf + total, rest);
            got = rest;
        }
    }
out:
    close(fd);
    sClients--;
    vTaskDelete(NULL);
}

static void mb_pass_task(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
    {
        DBG("mbp: no socket\n");
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MBP_PORT);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, MBP_MAX_CONN) < 0)
    {
        DBG("mbp: bind/listen fail\n");
        close(ls);
        vTaskDelete(NULL);
        return;
    }
    DBG("mbp: transparent modbus on tcp/502\n");

    for (;;)
    {
        if (!sEnabled)
        {
            if (sClients)
                DBG("mbp: %d client(s) alive, feature off\n",
                    (int)sClients);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        int fd = accept(ls, NULL, NULL);
        if (fd < 0)
            continue;
        /* фича могла выключиться, пока мы блокировали в accept:
         * подключившемуся сразу отказ */
        if (!sEnabled)
        {
            DBG("mbp: connect refused (feature off)\n");
            close(fd);
            continue;
        }
        /* Анти-фриз: мастер, потерявший WiFi без FIN, держал слот и стек
         * задачи навсегда - idle-соединение lwIP сам не закрывает (нет
         * данных - нет ретрансмиссий - нет таймаута). Тишина 30 с =
         * клиент мёртв: recv вернёт таймаут, задача закроет сокет.
         * Keepalive (15с зонд / 5с интервал / 3 попытки) - страховка
         * от "жив, но молчит" и от полуоткрытых соединений. */
        {
            struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
            int one = 1, idle = 15, intv = 5, cnt = 3;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intv, sizeof(intv));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        }
        /* каждое ПОДКЛЮЧЁННОЕ соединение стоит ~3.2К кучи (стек задачи),
         * поэтому лимит явный: сверх MBP_MAX_CONN - отказ подключения */
        if (sClients >= MBP_MAX_CONN)
        {
            DBG("mbp: client refused (limit %d)\n", MBP_MAX_CONN);
            close(fd);
            continue;
        }
        sClients++;
        if (xTaskCreate(mb_pass_client_task, "mbp", 3072, (void *)(intptr_t)fd,
                        3, NULL) != pdPASS)
        {
            DBG("mbp: client oom\n");
            sClients--;
            close(fd);
        }
    }
}

void mb_pass_start(void)
{
    xTaskCreate(mb_pass_task, "mb_pass", 2048, NULL, 3, NULL);
}
