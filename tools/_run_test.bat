@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /Fe:tools\test_stream.exe /I src tools\test_stream.cpp src\protocol.c
if errorlevel 1 exit /b 1
tools\test_stream.exe
