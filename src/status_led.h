#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------
// Индикация статуса светодиодом.
//
// На многих клонах ESP32-C3 SuperMini уже есть встроенный светодиод на
// GPIO8 (активный уровень LOW) — можно обойтись без дополнительной пайки.
// Если на вашей плате диода нет или он на другом пине — поменяйте
// STATUS_LED_PIN в main.cpp, либо подключите внешний светодиод с
// резистором на любой свободный GPIO.
//
// Паттерны:
//   WIFI_ACTIVE     — быстро мигает не переставая: работает Wi-Fi портал
//                      настроек (доступен веб-интерфейс)
//   WAITING_CAMERA  — медленно мигает не переставая: ждёт соединения с GoPro
//   CONNECTED       — мигает 2 раза, пауза, повтор: связь с камерой есть
//   RECORDING       — мигает 3 раза быстро, пауза, повтор: камера пишет
// ---------------------------------------------------------------------

class StatusLed {
public:
    enum class Pattern { WIFI_ACTIVE, WAITING_CAMERA, CONNECTED, RECORDING };

    void begin(int pin, bool activeLow = true) {
        _pin = pin;
        _activeLow = activeLow;
        pinMode(_pin, OUTPUT);
        setRaw(false);
    }

    void off() {
        setRaw(false);
    }

    // Вызывать из loop() как можно чаще — иначе мигание будет дёрганым.
    void update(Pattern pattern) {
        uint32_t now = millis();
        switch (pattern) {
            case Pattern::WIFI_ACTIVE:
                setRaw(((now / FAST_HALF_PERIOD_MS) % 2) == 0);
                break;
            case Pattern::WAITING_CAMERA:
                setRaw(((now / SLOW_HALF_PERIOD_MS) % 2) == 0);
                break;
            case Pattern::CONNECTED:
                setRaw(blinkGroup(now, 2));
                break;
            case Pattern::RECORDING:
                setRaw(blinkGroup(now, 3));
                break;
        }
    }

private:
    static constexpr uint32_t FAST_HALF_PERIOD_MS = 150;
    static constexpr uint32_t SLOW_HALF_PERIOD_MS = 500;
    static constexpr uint32_t GROUP_BLINK_ON_MS = 150;
    static constexpr uint32_t GROUP_BLINK_OFF_MS = 150;
    static constexpr uint32_t GROUP_PAUSE_MS = 1000;

    int _pin = -1;
    bool _activeLow = true;

    // count коротких вспышек подряд, затем пауза, и цикл повторяется.
    static bool blinkGroup(uint32_t now, uint8_t count) {
        uint32_t blinkPhase = count * (GROUP_BLINK_ON_MS + GROUP_BLINK_OFF_MS);
        uint32_t cycle = blinkPhase + GROUP_PAUSE_MS;
        uint32_t t = now % cycle;
        if (t >= blinkPhase) return false; // пауза между группами
        uint32_t withinBlink = t % (GROUP_BLINK_ON_MS + GROUP_BLINK_OFF_MS);
        return withinBlink < GROUP_BLINK_ON_MS;
    }

    void setRaw(bool on) {
        if (_pin < 0) return;
        bool level = _activeLow ? !on : on;
        digitalWrite(_pin, level ? HIGH : LOW);
    }
};
