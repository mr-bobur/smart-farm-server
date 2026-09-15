#!/bin/bash
echo "========================================================"
echo "  AQLLI VA XAVFSIZ FERMA - SERVER ISHGA TUSHMOQDA...    "
echo "========================================================"

# Virtualenv aktivlashtirish (mavjud bo'lsa)
if [ -d "venv" ]; then
    source venv/bin/activate
fi

python3 app.py
