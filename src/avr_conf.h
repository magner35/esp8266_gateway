#ifndef AVR_CONF_H
#define AVR_CONF_H

/*
 * Конфигурация цели программирования и платы ESP8266.
 *
 * В оригинале (OTA_update_AVR_using_ESP32) всё это было рассовано по
 * компонентам (TXD_PIN/RXD_PIN/RESET_PIN в avr_pro_mode.h, BLOCK_SIZE и
 * таймауты там же). Здесь собрано в одном месте.
 */

/* ---------------- линия связи с AVR ---------------- */

/* UART0 ESP8266 (GPIO1 TX -> RXD0, GPIO3 RX <- TXD0). На ESP32 AVR сидел
 * на UART1 (GPIO4/GPIO5), но у ESP8266 UART1 не умеет принимать - обмен
 * с AVR переводится на UART0, а журнал уходит на UART1. */
#define AVR_UART_NUM        0
/* Скорость консольного канала прибора (обычный обмен, PRELOAD/BOOT):
 * 250 кбит - выше (500к/1М) рвало приём длинных кадров (FIFO ESP =
 * 128 Б + всплески WiFi-прерываний). USB-терминал при вставленном
 * кабеле ходит на 115200 - прибор сам переключает USART0 (gatewayPoll). */
#define AVR_CONSOLE_BAUD    250000
/* Запись - на 1 Мбауд. Верификация - на пониженной: приём длинных
 * кадров на 1М рвался всплесками WiFi-прерываний (FIFO ESP = 128 Б);
 * urboot с AUTOBAUD перетренируется после переподключения */
#define AVR_UART_BAUD        1000000
#define AVR_UART_BAUD_VERIFY 1000000 /* не используется: одна сессия */
#define AVR_UART_RX_BUF     1024

/* GPIO, подключённый к /RESET ATmega128 (активный низкий).
 * urboot после сброса остаётся в загрузчике ТОЛЬКО при внешнем сбросе
 * (флаг EXTRF в MCUSR), поэтому линия сброса обязательна. -1 = линии нет
 * (тогда синхронизация рассчитана на удачное попадание в окно загрузчика). */
#define AVR_RESET_GPIO      5

/* На входе сброса стоит конденсатор 1 мкФ: линия опускается/поднимается
 * через RC (при подтяжке 10 кОм тау = ~10 мс), поэтому импульс LOW и пауза
 * после отпускания взяты с запасом в несколько тау. */
#define AVR_RESET_LOW_MS    50      /* длительность импульса сброса */
#define AVR_RESET_SETTLE_MS 50      /* подъём линии до старта urboot */

/* ---------------- цель: ATmega128 @ 16 MHz, urboot на UART0 ---------------- */

/* Идентификатор МК из таблицы urboot (src/urboot_mcuid.h: id_atmega128).
 * Передаётся загрузчиком в ответах UR_INSYNC/UR_OK - проверяем при синхронизации. */
#define AVR_MCUID           85
#define AVR_MCUID_ALT      86    /* atmega128A: наш прибор - именно она */
#define AVR_MCUID_NAME      "atmega128"
#define AVR_TARGET          "ATmega128 @16MHz urboot/uart0"

/* Сигнатура для STK500v1-совместимого режима (avrdude -c arduino) */
#define AVR_SIGNATURE       {0x1E, 0x97, 0x02}

/* Flash 128 КБ: адреса 24-битные (байтные), страница 256 Б, 512 страниц.
 * Векторная таблица ATmega128 - 4-байтовые команды jmp (flash > 8 КБ). */
#define AVR_FLASH_SIZE      (128 * 1024)
#define AVR_PAGE_SIZE       256
#define AVR_VECTOR_SIZE     4

/* ---------------- тайминги протокола urclock ---------------- */

/* Ответ на синхронизацию читаем <100 мс (urprotocol.md), чтобы не кормить
 * WDT загрузчика во время неудачных попыток */
#define UR_SYNC_REPLY_MS    80
/* Окно ожидания ответа после одного сброса (WDTO urboot по умолчанию ~1 с) */
#define UR_SYNC_WINDOW_MS   800
/* Число сбросов AVR перед сдачей */
#define UR_RESET_ATTEMPTS   5

#define UR_CMD_TIMEOUT_MS   500     /* ответ на запись/чтение страницы */
#define UR_ERASE_TIMEOUT_MS 20000   /* chip erase может идти много секунд */

#endif /* AVR_CONF_H */
