@echo off
rem Host test bench: compiles the REAL src/protocol.c and feeds it the
rem captured device listing. See test_parser.cpp.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo vcvarsall.bat not found - edit VCVARS in tools\run_parser_test.bat
    exit /b 1
)
call "%VCVARS%" x64 >nul
cl /nologo /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:tools\test_parser.exe /Fo:tools\ tools\test_parser.cpp src\protocol.c
if errorlevel 1 exit /b 1
tools\test_parser.exe tools\ske_l.txt
