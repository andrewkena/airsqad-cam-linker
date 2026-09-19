# AIRSQAD Cam Linker — Flasher

Windows-приложение для прошивки платы ESP32-C3. Всегда берёт **последнюю
собранную прошивку** из GitHub Release репозитория (тег `latest`) — ничего
собирать локально не нужно, PlatformIO на компьютере пользователя не требуется.

Прошивка публикуется автоматически: при каждом пуше в `master` GitHub Actions
([.github/workflows/build-firmware.yml](../.github/workflows/build-firmware.yml))
собирает проект и обновляет Release `latest`.

## Использование (готовый .exe)

1. Скачать `AirsqadFlasher.exe` (см. [сборку](#сборка-exe) ниже, если готового
   файла ещё нет).
2. Подключить плату к компьютеру по USB.
3. Запустить `AirsqadFlasher.exe`.
4. Выбрать COM-порт платы, дождаться проверки версии прошивки.
5. Нажать **"Прошить плату"**.

Прогресс и возможные ошибки отображаются в логе внизу окна.

## Сборка exe

Нужен Python 3.10+ на Windows.

```
cd flasher
build_exe.bat
```

Результат — `flasher\dist\AirsqadFlasher.exe`, один файл, ничего дополнительно
устанавливать не нужно (кроме драйвера USB-CDC для ESP32-C3, если Windows его
ещё не подхватила автоматически).

## Запуск без сборки (из исходников)

```
cd flasher
pip install -r requirements.txt
python flash_app.py
```

## Как это работает

1. Приложение запрашивает `GET /repos/andrewkena/airsqad-cam-linker/releases/tags/latest`
   через GitHub API, оттуда — `manifest.json` (список файлов прошивки и адресов
   для записи во flash).
2. Скачивает `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`
   в локальный кэш (`%LOCALAPPDATA%\AirsqadFlasher\firmware\<commit>`), повторно
   не скачивает, если версия уже была загружена.
3. Вызывает `esptool` (Python-библиотека, входит в зависимости) с теми же
   параметрами и адресами, что использует `pio run -t upload` — прошивка
   идентична той, что получилась бы при обычной сборке через PlatformIO.
