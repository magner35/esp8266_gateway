@echo off
rem ПОЛНЫЙ цикл прошивки начисто:
rem   erase чипа -> прошивка (boot+app) -> таблица разделов (nvs 64K)
rem Запуск: tools\_full_flash.bat COM7
if "%1"=="" (
  echo Usage: tools\_full_flash.bat ^<COM-port^>
  exit /b 1
)
set PORT=%1

echo === 1/3 erase chip (всё сотрётся: wifi, виджеты, дерево) ===
python -m esptool --port %PORT% erase_flash
if errorlevel 1 goto fail

echo === 2/3 firmware (boot + app) ===
pio run -t upload
if errorlevel 1 goto fail

echo === 3/3 partition table (nvs 64K - PlatformIO сам это НЕ шьёт) ===
python -m esptool --port %PORT% --baud 921600 write_flash 0x8000 .pio\build\esp8266_modern_rtos\partitions.bin
if errorlevel 1 goto fail

echo.
echo === OK. Перезагрузь шлюз питанием. ===
echo WiFi сотёрт: сеть настраивается через точку SKE02-GW-... (192.168.4.1)
goto :eof

:fail
echo === FAILED на одном из шагов ===
exit /b 1
