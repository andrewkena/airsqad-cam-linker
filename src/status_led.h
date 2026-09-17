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
//   выключен       — GoPro не подключена
//   горит ровно    — подключена, не пишет
//   быстро мигает  — идёт запись
//   медленно мигает — идёт привязка новой камеры (см. GoProBle::startPairing())
// ---------------------------------------------------------------------

class StatusLed {
public:
    enum class Pattern { OFF, SOLID, BLINK_SLOW, BLINK_FAST };

    void begin(int pin, bool activeLow = true) {
        _pin = pin;
        _activeLow = activeLow;
        pinMode(_pin, OUTPUT);
        setRaw(false);
    }

    // Вызывать из loop() как можно чаще — иначе мигание будет дёрганым.
    void update(Pattern pattern) {
        uint32_t now = millis();
        switch (pattern) {
            case Pattern::OFF:
                setRaw(false);
                break;
            case Pattern::SOLID:
                setRaw(true);
                break;
            case Pattern::BLINK_SLOW:
                setRaw(((now / 500) % 2) == 0);
                break;
            case Pattern::BLINK_FAST:
                setRaw(((now / 150) % 2) == 0);
                break;
        }
    }

private:
    int _pin = -1;
    bool _activeLow = true;

    void setRaw(bool on) {
        if (_pin < 0) return;
        bool level = _activeLow ? !on : on;
        digitalWrite(_pin, level ? HIGH : LOW);
    }
};
