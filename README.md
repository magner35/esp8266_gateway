# ESP8266 Gateway for the SKE-02 flow meter — ESP8266_RTOS_SDK port

Форк проекта [esp8266_gateway](../esp8266_gateway), переписанный с Arduino (C++)
на **ESP8266_RTOS SDK v3.4** чистым C: настоящие FreeRTOS-задачи вместо
кооперативного loop(), BSD-сокеты вместо WiFiServer/ESP8266WebServer,
NVS вместо EEPROM-сектора. Протокольная логика перенесена байт в байт.

## Архитектура

| Файл | Назначение |
|---|---|
| `src/main.c` | `app_main`: старт задач |
| `src/protocol.{c,h}` | **чистое C-ядро**: парсер листинга/значений, руссификация, HHMM/DDMMYY, кэш параметров, 'm'-кадры, промпт-матчер. Без SDK-зависимостей — этот же файл компилирует хост-тест (`tools/test_parser.cpp`) |
| `src/ske02.c` | задача **meter**: uart-драйвер, стейт-машина WAKE→LIST→IDLE, очередь команд (SET/RUN/UNLOCK/REFRESH/RESCAN/REBOOT) с мьютексом и слотом, 1 Гц мониторинг 'm' |
| `src/wifi.c` | задача **wifi**: STA-подключение по кредам, фолбэк в AP-портал (10.0.0.1), события esp_event |
| `src/web.c` | **esp_http_server**: портал, мониторинговое окно, настройки, JSON API; страницы в `src/pages.h` |
| `src/pages.h` | HTML-страницы (рукоподдерживаемый источник, правится напрямую) |
| `src/modbus_tcp.c` | Modbus TCP slave (502) на BSD-сокетах, та же карта регистров |
| `src/dns.c` | captive-DNS (udp/53) для портала |
| `src/storage.c` | NVS: SSID/пароль WiFi |

## Сборка

```bash
pio run -e esp8266_modern_rtos            # сборка
pio run -e esp8266_modern_rtos -t upload  # прошивка
python tools/gen_sdkconfig.py             # перегенерация sdkconfig при смене опций
cmd /c tools\run_parser_test.bat          # хост-тест протокольного ядра
```

Платформа — [freedib/platform-espressif8266](https://github.com/freedib/platform-espressif8266)
+ SDK v3.4; `tool-genbin-esp8266` в реестре PlatformIO отсутствует и
переопределён на git-копию freedib (нужен только nonos-билдеру, но
объявлен обязательным в манифесте платформы).

## Отличия от Arduino-версии

- **Задачи вместо loop-пауз**: листинг принимается задачей meter в своём
  собственном контексте — веб/Modbus никогда не останавливаются, параметр
  `skeCapturing` не нужен.
- **Очередь команд**: веб-обработчики отправляют запросы (SET/RUN/UNLOCK)
  в задачу meter и ждут семафора — UART трогает ровно одна задача.
- **NVS** с wear-levelling вместо ручного CRC8-сектора.
- Мониторинговое окно (MeterData, 1 Гц 'm') и все прочие функции — те же.
- Хост-тест теперь компилирует **настоящий** `protocol.c` вместо ручной
  копии парсера — источник истины один.

## Прошивка на стенде

По-прежнему действуют правила стенда: COM28 один и коммутируется
перемычкой ATmega128 ↔ ESP8266, у ESP есть кнопка BOOT. **Перед прошивкой
шиза — пауза и явный запрос подтверждения**: только пользователь
коммутирует перемычку и держит BOOT.
