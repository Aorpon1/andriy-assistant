@echo off
rem Запуск програми руху мишки. Зупинка: Ctrl+C або закрити вікно.
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py mouse_mover.py
) else (
    where python >nul 2>nul
    if %errorlevel%==0 (
        python mouse_mover.py
    ) else (
        echo Python не встановлений. Завантаж його з https://python.org
        echo Під час установки постав галочку "Add Python to PATH".
    )
)
pause
