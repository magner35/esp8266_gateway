/*
 * Captive-portal DNS (udp/53): every A query resolves to 10.0.0.1 so
 * that any http attempt of a freshly joined phone lands on the setup
 * page. Runs only while the AP portal is up.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "debug.h"
#include "wifi.h"

#define DNS_PORT 53

static void dns_task(void *arg)
{
    int fd;
    struct sockaddr_in bindAddr;
    uint8_t buf[128];
    (void)arg;

    /* the wifi task may still be trying STA: serve only in the portal */
    while (!wifi_ap_ssid())
        vTaskDelay(pdMS_TO_TICKS(250));

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0)
    {
        DBG("dns: socket failed\n");
        vTaskDelete(NULL);
        return;
    }
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(DNS_PORT);
    if (bind(fd, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) < 0)
    {
        DBG("dns: bind failed\n");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    DBG("dns: captive wildcard on udp/53\n");

    for (;;)
    {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int n = recvfrom(fd, buf, sizeof(buf) - 32, 0,
                         (struct sockaddr *)&cli, &clen);
        if (n < 12 || !(buf[2] & 0x80) == 0)
        {
            /* need a query with at least the header */
        }
        if (n < 12)
            continue;
        if (buf[2] & 0x80)
            continue; /* a response, not a query */

        {
            /* echo the question, append a 4-byte A record (10.0.0.1) */
            uint8_t *w = buf + n;
            uint16_t qdcnt = (buf[4] << 8) | buf[5];
            uint16_t ancnt = 1;

            /* header: same id, response+rd+ra, answers=1 */
            buf[2] = 0x81;
            buf[3] = 0x80;
            buf[6] = ancnt >> 8;
            buf[7] = ancnt & 0xFF;
            buf[8] = buf[9] = 0; /* nscount */
            buf[10] = buf[11] = 0; /* arcount */

            if (qdcnt != 1)
                continue;
            /* assume the only question is a type A/class IN; append the
             * answer: pointer to the name (0xC0 0x0C), type A, class IN,
             * ttl 60, rdlen 4, rdata 10.0.0.1 */
            if (n + 16 > (int)sizeof(buf) - 32)
                continue;
            *w++ = 0xC0;
            *w++ = 0x0C;
            *w++ = 0;
            *w++ = 1; /* type A */
            *w++ = 0;
            *w++ = 1; /* class IN */
            *w++ = 0;
            *w++ = 0;
            *w++ = 0;
            *w++ = 60; /* ttl */
            *w++ = 0;
            *w++ = 4; /* rdlen */
            *w++ = 10;
            *w++ = 0;
            *w++ = 0;
            *w++ = 1;
            sendto(fd, buf, w - buf, 0, (struct sockaddr *)&cli, clen);
        }
    }
}

void dns_start(void)
{
    /* the task itself waits for the portal mode, so it is safe to
     * start unconditionally at boot */
    xTaskCreate(dns_task, "dns", 2048, NULL, 3, NULL);
}
