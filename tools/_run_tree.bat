@echo off
rem Roundtrip host test: GCC (same attribute semantics as xtensa firmware!)
set PATH=C:\msys64\ucrt64\bin;%PATH%
cd /d C:\Users\user\Documents\GitHub\esp8266_gateway_fork
g++.exe -O1 -std=c++14 -Itools/nvs_stub -Isrc -o tools/test_tree_gcc.exe tools/test_tree.cpp src/protocol.c
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
tools\test_tree_gcc.exe tools\ske_l.txt tools\tree_out.json
