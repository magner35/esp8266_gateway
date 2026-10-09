@echo off
cd /d C:\Users\user\Documents\GitHub\esp8266_gateway_fork
C:\msys64\ucrt64\bin\g++.exe -O1 -std=c++14 -Itools/nvs_stub -Isrc -o tools/test_tree_gcc.exe tools/test_tree.cpp src/protocol.c 2> tools\_gcc_err.txt
if errorlevel 1 (
  echo BUILD FAILED
  type tools\_gcc_err.txt
  exit /b 1
)
tools\test_tree_gcc.exe tools\ske_l.txt tools\tree_out.json
