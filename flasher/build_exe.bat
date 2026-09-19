@echo off
REM Собирает flash_app.py в один самостоятельный AirsqadFlasher.exe (Windows).
REM Результат: flasher\dist\AirsqadFlasher.exe

cd /d "%~dp0"

pip install -r requirements.txt
if errorlevel 1 goto :error

pyinstaller --onefile --windowed --name AirsqadFlasher flash_app.py
if errorlevel 1 goto :error

echo.
echo Готово: dist\AirsqadFlasher.exe
goto :eof

:error
echo Сборка не удалась.
exit /b 1
