@echo off
title Aqlli va Xavfsiz Ferma - Server & AI Video Tahlil
color 0A
cd /d "%~dp0"
echo ========================================================
echo   AQLLI VA XAVFSIZ FERMA - SERVER ISHGA TUSHMOQDA...
echo ========================================================
echo Server manzili: http://localhost:8000
echo Tarmoqdagi IP : http://192.168.88.108:8000
echo.
python app.py
pause
