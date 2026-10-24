@echo off
rem Flasht das Thermometer und prueft den Kaltstart - funktioniert auch ohne
rem Internet, weil ESP-IDF lokal installiert ist.
rem
rem Der Port wird automatisch gesucht, kann aber uebergeben werden:
rem     flash-und-pruefen.cmd COM11
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash-und-pruefen.ps1" %*
echo.
pause
