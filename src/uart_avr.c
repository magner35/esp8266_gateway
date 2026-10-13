/*
 * Транспорт урпрограмматора: UART0, который в обычной жизни занят
 * консолью прибора. Драйвер уже установлен ske02_uart_setup() и живёт
 * вечно (повторный uart_driver_install невозможен) - здесь только
 * переключение скорости и вход в загрузчик.
 *
 * Вход в urboot БЕЗ линии сброса: консольная команда BOOT переводит
 * приложение прибора в загрузчик (EXTRF + jmp), после этого UART
 * переключается на 1 Мбауд загрузчика.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"

#include "avr_conf.h"
#include "debug.h"
#include "ske02.h"
#include "uart_avr.h"

static const char *TAG = "uart_avr";

esp_err_t avr_uart_init(void)
{
    /* драйвер уже живёт (ske02): достаточно очистить буфер */
    avr_drain();
    return ESP_OK;
}

/* Аппаратный сброс: GPIO5 -> N-MOSFET -> /RESET ATmega128.
 * Проверено на железе: достаточно импульса 10 мс. urboot видит
 * EXTRF и остаётся в загрузчике - это штатный вход (в отличие от
 * программного BOOT, который загрузчик игнорирует). */
void avr_reset_pulse(void)
{
    static bool sGpioReady;

    if (!sGpioReady)
    {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << 5,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        sGpioReady = true;
    }
    uart_set_baudrate(AVR_UART_NUM, AVR_UART_BAUD);
    uart_flush_input(AVR_UART_NUM);
    gpio_set_level(GPIO_NUM_5, 1);
    vTaskDelay(pdMS_TO_TICKS(10));   /* длительность проверена на железе */
    gpio_set_level(GPIO_NUM_5, 0);
    vTaskDelay(pdMS_TO_TICKS(50));   /* подъём RC-линии до старта urboot */
    uart_flush_input(AVR_UART_NUM);
    DBG("%s: hardware reset pulse (GPIO5, 10 ms)\n", TAG);
}

int avr_send(const uint8_t *data, size_t len)
{
    int tx = uart_write_bytes(AVR_UART_NUM, (const char *)data, len);
    uart_wait_tx_done(AVR_UART_NUM, pdMS_TO_TICKS(200));
    return tx;
}

int avr_recv(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (got < len)
    {
        TickType_t now = xTaskGetTickCount();
        TickType_t left = deadline - now;
        TickType_t chunk = left;
        int n;
        /* TickType_t беззнаковый: сравнение через знаковый дифф */
        if ((int32_t)left <= 0)
            break;
        /* длинное ожидание (chip erase - до 20с) крошим на куски по
         * 200 мс с покормкой soft-WDT: единый блок ловил wdt reset */
        if (chunk > pdMS_TO_TICKS(200))
            chunk = pdMS_TO_TICKS(200);
        n = uart_read_bytes(AVR_UART_NUM, buf + got, len - got, chunk);
        if (n < 0)
            break;
        got += n;
        /* WDT кормится из IDLE-хука; на случай, если что-то на пути
         * chip erase душит IDLE - кормим таймер напрямую здесь */
        esp_task_wdt_reset();
        vTaskDelay(1);
    }
    return (int)got;
}

void avr_drain(void)
{
    uart_flush_input(AVR_UART_NUM);
}

/* контекст сессии прошивки: обмен с прибором на паузе, UART у нас */

void avr_flash_begin(void)
{
    ske02_suspend(true);
    uart_set_baudrate(AVR_UART_NUM, AVR_UART_BAUD);
}

void avr_flash_end(void)
{
    uart_set_baudrate(AVR_UART_NUM, 115200);
    uart_flush_input(AVR_UART_NUM);
    ske02_suspend(false);
}
