@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist offline_layout_build mkdir offline_layout_build
cl /nologo /W4 /WX /O2 /Isrc\vendor offline_layout.c src\vendor\layout_610.c /Feoffline_layout_build\offline_layout.exe /Fooffline_layout_build\
if errorlevel 1 exit /b 1
