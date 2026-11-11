@echo off
chcp 65001 >nul
cd /d "%~dp0"
title ESP32 Car Web Controller
echo.
echo Starting ESP32 car web controller...
echo If port 8080 is busy, stop the NetAssist TCP Server first.
echo Keep this window open while using the web page.
echo.
node server.js
echo.
echo Server stopped.
pause
