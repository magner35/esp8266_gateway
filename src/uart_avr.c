/*
 * Транспорт урпрограмматора: UART0, который в обычной жизни занят
 * консолью прибора. Драйвер уже установлен ske02_uart_setup() и живёт
 * вечно (повторный uart_driver_install невозможен) - здесь только
 * переключение скорости и вход в загрузчик.
 *
 * Вход в urboot - аппаратный сброс (GPIO5 -> N-MOSFET -> /RESET).
 * Перед сбросом прибору шлётся PRELOAD: он гасит задачи и показывает
 * на LCD экран "Обновление ПО".
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"

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
 * EXTRF и остаётся в загрузчике. Скорость НЕ трогаем - её задаёт
 * вызывающий. */
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
         * 200 мс с прямой покормкой WDT: единый блок ловил wdt reset */
        if (chunk > pdMS_TO_TICKS(200))
            chunk = pdMS_TO_TICKS(200);
        n = uart_read_bytes(AVR_UART_NUM, buf + got, len - got, chunk);
        if (n < 0)
            break;
        got += n;
        esp_task_wdt_reset();
    }
    return (int)got;
}

void avr_drain(void)
{
    uart_flush_input(AVR_UART_NUM);
}

void avr_uart_set_baud(uint32_t baud)
{
    uart_set_baudrate(AVR_UART_NUM, baud);
    uart_flush_input(AVR_UART_NUM);
}

/* контекст сессии прошивки: PRELOAD -> пауза обмена -> скорость bl */

void avr_flash_begin(void)
{
    char echo[24];
    int n;

    ske02_suspend(true);
    /*
     * WiFi ГАСИМ на время сессии (решение без экспериментов): всплески
     * WiFi-прерываний при слабом сигнале рвали TX-заливку FIFO посреди
     * 262-байтного кадра страницы. Веб на минуту отваливается - полоса
     * прошивки в браузере идёт по часам (service.js), реальный прогресс
     * недоступен по определению. STA после esp_wifi_start()
     * реконнектится сама (STA_START -> connect).
     */
    DBG("%s: wifi off for avr session\n", TAG);
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    /* PRELOAD на скорости консольного канала (115200): прибор гасит
     * задачи и показывает экран "Обновление ПО" ДО сброса в загрузчик;
     * затем скорость загрузчика (AVR_UART_BAUD) для urclock */
    uart_set_baudrate(AVR_UART_NUM, AVR_CONSOLE_BAUD);
    uart_flush_input(AVR_UART_NUM);
    uart_write_bytes(AVR_UART_NUM, "PRELOAD\r", 8);
    uart_wait_tx_done(AVR_UART_NUM, pdMS_TO_TICKS(200));
    n = uart_read_bytes(AVR_UART_NUM, (uint8_t *)echo, sizeof(echo) - 1,
                        pdMS_TO_TICKS(400));
    if (n > 0)
    {
        echo[n] = 0;
        DBG("%s: preload echo \"%s\"\n", TAG, echo);
    }
    else
        DBG("%s: preload no echo (old meter fw?)\n", TAG);
    uart_set_baudrate(AVR_UART_NUM, AVR_UART_BAUD);
}

void avr_flash_end(void)
{
    uart_set_baudrate(AVR_UART_NUM, AVR_CONSOLE_BAUD);
    uart_flush_input(AVR_UART_NUM);
    ske02_suspend(false);
    DBG("%s: wifi back on\n", TAG);
    esp_wifi_start();
    dbg_rom_redirect(); /* wifi-драйвер мог вернуть свой ets-putc */
}
