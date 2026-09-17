# gopro-osd-bridge

Мост между экшн-камерой **GoPro 11** и OSD полётного контроллера на **Betaflight**.

Устройство на базе **ESP32-C3 SuperMini**:
- по **Bluetooth LE** подключается к GoPro и опрашивает её статус (запись, заряд батареи, состояние SD-карты);
- по **UART** (протокол **MSP v2**) передаёт этот статус на полётный контроллер, который выводит его в OSD в одном из слотов **Custom Message**.

В результате пилот видит на экране FPV, идёт ли запись на GoPro, заряд её батареи и статус SD-карты — прямо во время полёта.

## Как это работает

```
GoPro 11  <--BLE-->  ESP32-C3 SuperMini  <--UART/MSPv2-->  Полётный контроллер (Betaflight)
                                                                    |
                                                                    v
                                                          OSD: OSD_CUSTOM_MSG1..4
```

1. `GoProBle` ([src/gopro_ble.h](src/gopro_ble.h)) сканирует эфир, находит GoPro по имени (`GoPro...`) или по рекламируемому сервису `FEA6` (Open GoPro BLE spec), подключается и раз в секунду запрашивает статус (`GetStatusValue`, query ID `0x13`) по трём параметрам:
   - **10** — Encoding Active (идёт запись или нет);
   - **2** — Internal Battery Level (заряд, %);
   - **33** — SD Card Status.

   При разрыве связи переподключается автоматически раз в 5 секунд. Так как GoPro требует bonding, при первом подключении камера должна разрешить сопряжение.

2. `main.cpp` ([src/main.cpp](src/main.cpp)) раз в 500 мс формирует короткую строку по текущему статусу:
   - `GP ---` — GoPro не подключена;
   - `REC BAT<N>%` / `REC` — идёт запись (с зарядом батареи или без);
   - `GP IDLE` — подключена, но не пишет.

3. `mspOsd::setCustomMessage` ([src/msp_osd.h](src/msp_osd.h)) отправляет эту строку на FC по MSPv2 (`MSP2_SET_TEXT`, CRC8 DVB-S2) в слот `OSD_CUSTOM_MSG1` (индекс 0).

## Железо и подключение

- Плата: **ESP32-C3 SuperMini**
- UART к полётному контроллеру:
  - `MSP_RX_PIN = GPIO4`
  - `MSP_TX_PIN = GPIO5`
  - Скорость: **115200 бод**, MSP v2

  Подключение к FC: TX ESP32 → RX свободного UART-порта FC, RX ESP32 → TX того же порта. В Betaflight Configurator на выбранном UART нужно включить периферию **MSP**.

- USB на ESP32-C3 используется одновременно для прошивки и для отладочного вывода через `Serial` (USB-CDC) — см. `build_flags` в [platformio.ini](platformio.ini).

> Если пины UART конфликтуют с распиновкой вашей конкретной партии платы — поменяйте `MSP_RX_PIN`/`MSP_TX_PIN` в [src/main.cpp](src/main.cpp) на любые свободные GPIO.

## Настройка в Betaflight

1. В Configurator → **Ports**: включите **MSP** на UART, к которому подключён ESP32.
2. В Configurator → **OSD**: включите элементы **Custom message 1** (и при необходимости 2–4) и разместите их на экране.

## Сборка и прошивка

Проект собирается через **PlatformIO**.

```
pio run                 ; сборка
pio run -t upload       ; прошивка
pio device monitor      ; отладочный вывод (115200 бод)
```

Конфигурация окружения — [platformio.ini](platformio.ini):
- `platform = espressif32`, `framework = arduino`
- `board = esp32-c3-devkitm-1` (ближайший аналог SuperMini в реестре плат PlatformIO)
- Зависимость: `h2zero/NimBLE-Arduino`

## Настройка под свою камеру

- Имя BLE-устройства по умолчанию ищется по префиксу `"GoPro"` — при необходимости укажите точное имя своей камеры в `goPro.begin(...)` в [src/main.cpp](src/main.cpp).
- Индекс используемого слота OSD задаётся константой `OSD_SLOT` в [src/main.cpp](src/main.cpp) (0 → `OSD_CUSTOM_MSG1`).

## Статус проекта

Ранний рабочий прототип: базовый сценарий (подключение → опрос статуса → вывод в OSD) реализован и компилируется, но не протестирован на полном стенде (ESP32 + реальная GoPro 11 + полётный контроллер). Обработка ошибок минимальна, README и код рассчитаны на подстройку под конкретную сборку дрона.
