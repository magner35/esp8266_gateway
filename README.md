# ESP8266 Gateway for the SKE-02 flow meter — ESP8266_RTOS_SDK port

Форк проекта [esp8266_gateway](../esp8266_gateway), переписанный с Arduino (C++)
на **ESP8266_RTOS SDK v3.4** чистым C: настоящие FreeRTOS-задачи вместо
кооперативного loop(), BSD-сокеты вместо WiFiServer/ESP8266WebServer,
NVS вместо EEPROM-сектора. Протокольная логика перенесена байт в байт.

Включает **прозрачный мост Modbus TCP (порт 502)** к родной карте регистров
прибора: SCADA работает с расходомером по сети ровно как по RS-485,
через ту же командную строку сервисной консоли (`MB`/`MBR`).

## Архитектура

| Файл | Назначение |
|---|---|
| `src/main.c` | `app_main`: boot_task (дерево → wifi → веб → modbus) |
| `src/protocol.{c,h}` | **чистое C-ядро**: парсер листинга/значений (все типы F/U32/H32/TIME/DATE/U8/I8/H8/U16/H16/I16/B/E/S/CMD/MAC/IP), руссификация, 'm'-кадры. Без SDK-зависимостей — хост-тест компилирует этот же файл |
| `src/ske02.c` | задача **meter** (стек 3584): uart 115200, WAKE→LIST→IDLE, очередь команд, опрос 'm' 5 Гц; **маркер правки** (edit_id/edit_cnt в кадре) → точечная синхронизация `GET <id>` / обход значений / relist при ребуте прибора; **фичи** Wi-Fi gateway (WEB/Modbus/OTA) из дерева прибора |
| `src/chartbuf.{c,h}` | серверное кольцо графика: 1 слот × int8[1024] в куче (malloc при первом появлении графика), шкала авто-прогрев по 5 значениям или уставками; отдача в `/api/values` (s0/c0/l0/h0) |
| `src/espcons.c` | **консоль шлюза** + OTA-механизм (download/update_gateway/update_meter) |
| `src/wifi.c` | задача **wifi**: перебор сетей, фолбэк в AP-портал |
| `src/web.c` | **esp_http_server** (7 сокетов, lru-purge, стек 8192): SPA + JSON API; графики в `/api/values`; `/api/params` (12КБ, чанками, heap-guard 14КБ) |
| `src/mb_pass.c` | **Modbus TCP (502)**: MBAP → `MB <hex>` → RTU; фича «Modbus TCP/IP» гейтит и новые, и живущие соединения |
| `src/ntp.c` | SNTP: окно 12ч (NVS), время из DS1307 при потере питания, HTTP-only геолокация |
| `src/urclock.c` | программатор urboot: прошивка ATmega128 по GPIO5-сбросу |
| `src/dns.c` | captive-DNS для портала |
| `src/storage.c` | NVS: WiFi, NTP-конфиг, метки OTA, epoch последнего пуша TIME |
| `src/tree_store.c` | дерево параметров в NVS: поточная запись по записи, seenMap-стирание хвостов, count = maxId+1 |

## Синхронизация правок настроек (маркер вместо cfg_rev)

Прибор пишет `edit_id`/`edit_cnt` в каждый кадр DATA (в `storePropertyValue`).
Шлюз обрабатывает:

| Сценарий | Действие |
|---|---|
| Наш SET из веба | Молча: OK-строка уже обновила NVS |
| Правка с клавиатуры (delta=1) | `GET <id>` — одна команда, ~30 мс |
| Несколько правок (delta>1) | Обход значений: GET по всем, NVS только изменившиеся |
| Ребут прибора (cnt назад) | Полный relist (структура меню могла смениться) |
| Включение/выключение фич | WEB/Modbus/OTA из подменю «Wi-Fi gateway» прибора |

Полный relist — только на старте шлюза, при ребуте прибора, или по кнопке «Обновить».

## График

Один виджет-график. Кольцо int8[1024] в куче (только когда график есть на панели).
Заполнение из задачи meter (5 Гц, независимо от WiFi). Отдача в `/api/values`
тем же запросом (ноль дополнительного трафика). Шкала: авто (первые 5 значений)
или фиксированная уставками (при auto=off, «Расход (Qmax)» = 0..Qmax в л/мин).
Смена шкалы = перемасштабирование на месте (история сохраняется).

## Сборка

```bash
pio run                                  # сборка
pio run -t upload                        # прошивка
pio run -t ota                           # OTA-образ -> gw_update.bin
python tools/gen_sdkconfig.py            # перегенерация sdkconfig
python tools/mock_server.py              # мок :8099
python tools/ram_report.py               # сводка RAM по .map
```

## Инструменты

| Скрипт | Назначение |
|---|---|
| `tools/mock_server.py` | полный HTTP-мок шлюза (:8099) — отладка фронтенда без железа |
| `tools/ram_report.py` | статика/куча по .map после сборки |
| `tools/capture_list.py` | свежий листинг с прибора (COM-порт) |
| `tools/pages.py` | сборка www/* → src/pages.h + app.html |
| `tools/ske02_sim.py` | симулятор консоли прибора на виртуальном COM |
| `tools/uart_bridge.py` | мост ESP8266 ↔ консоль СК-02 |
| `tools/flash_parts.py` | проводная прошивка (partitions + otadata) |
| `tools/ota_target.py` | сборка OTA-образа (`pio run -t ota`) |

## Прошивка на стенде

COM28 один и коммутируется перемычкой ATmega128 ↔ ESP8266.
**Перед прошивкой — пауза и явный запрос подтверждения**.

## Защищённые OTA-пакеты (.skpak)

Обновления принимаются **только** в формате `.skpak` (Ed25519+AES-CTR).
Единый артефакт — бандл `dist/system_full.skpak` (образ ESP + hex прибора).
Сервер обновлений: `dist/versions.txt`.

## Известные платформенные грабли

- **newlib `%f` в двух задачах = IllegalInstruction** (per-task reent сломан). `%f` только в httpd.
- **Логи wifi-блоба в UART0** — сильный `ets_putc` в debug.c → UART1.
- **line_to_parser русифицирует с расширением** — всегда разбирать копию, не исходный буфер.
- **TreeRec (~755Б) на стеке meter** — рвал канарейку; все в static.
- **Отрицательный float → (unsigned long) на AVR** — wrap к 2^32; кламп перед кастом в DATA-кадре.
- **type_by_token обязан знать ВСЕ типы прибора** — H32/H8/H16/MAC/IP были пропущены, параметры терялись молча.
- **tree_commit: count = maxId+1, не liveCount** — при дырах в нумерации хвост id не отдавался.
