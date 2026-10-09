@echo off
rem Прошивка таблицы разделов (nvs 64K) - PlatformIO для RTOS-SDK
rem этого НЕ делает сам. Запуск: tools\_flash_parts.bat COM7
if "%1"=="" (
  echo Usage: tools\_flash_parts.bat ^<COM-port^>
  echo   например: tools\_flash_parts.bat COM7
  exit /b 1
)
python -m esptool --port %1 --baud 921600 write_flash 0x8000 .pio\build\esp8266_modern_rtos\partitions.bin
if errorlevel 1 (
  echo.
  echo FAILED - проверь порт и что pio run -t upload уже выполнен
  exit /b 1
)
echo.
echo OK: partition table flashed. Перезагрузи шлюз (питанием).
echo WiFi-креды сотрутся - настрой сеть через 192.168.4.1
