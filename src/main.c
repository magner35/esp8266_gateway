/*
 * ESP8266 gateway for the SKE-02 flow meter - ESP8266_RTOS_SDK v3.4 port.
 * Tasks: wifi manager, meter (UART console client), httpd, modbus tcp,
 * captive dns (portal mode only).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "debug.h"
#include "dns.h"
#include "modbus_tcp.h"
#include "ske02.h"
#include "storage.h"
#include "web.h"
#include "wifi.h"

static void boot_task(void *arg)
{
    (void)arg;
    DBG("gateway rtos: boot\n");
    storage_init();
    wifi_start();
    /* the wifi task needs a moment to bring an interface up */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ske02_start();
    web_start();
    dns_start();
    modbus_start();
    vTaskDelete(NULL);
}

void app_main(void)
{
    dbg_init(); /* UART1 log + silence the SDK console on UART0 */
    xTaskCreate(boot_task, "boot", 3072, NULL, 4, NULL);
}
