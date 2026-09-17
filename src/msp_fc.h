#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------
// Чтение состояния ARM и RC-каналов с полётного контроллера по MSPv1.
//
// Периодически запрашивает MSP_STATUS (101) и MSP_RC (105), разбирает
// ответы прямо из потока UART. Работает поверх того же порта, что и
// mspOsd (MSPv2 SET_TEXT) — направление кадра различается заголовком
// ('M' у v1 и 'X' у v2), поэтому оба протокола спокойно живут на одной
// линии.
//
// ВНИМАНИЕ: бит 0 flightModeFlags в MSP_STATUS трактуется как ARM —
// это верно для типичной конфигурации Betaflight, где ARM зарегистрирован
// первым активным режимом, но не гарантировано для экзотических настроек.
// ---------------------------------------------------------------------

class MspFc {
public:
    static constexpr uint8_t MSP_STATUS = 101;
    static constexpr uint8_t MSP_RC = 105;
    static constexpr uint8_t MAX_CHANNELS = 18;

    bool armed = false;
    uint16_t channels[MAX_CHANNELS] = {0};
    uint8_t channelCount = 0;

    void begin(Stream &port) {
        _port = &port;
    }

    // Вызывать из loop() как можно чаще.
    void loop() {
        readIncoming();

        uint32_t now = millis();
        if (now - _lastRequest > REQUEST_INTERVAL_MS) {
            _lastRequest = now;
            sendRequest(_requestToggle ? MSP_STATUS : MSP_RC);
            _requestToggle = !_requestToggle;
        }
    }

    // auxChannelNumber: 1-based номер AUX-канала (AUX1 = 1).
    uint16_t getAuxChannelValue(uint8_t auxChannelNumber) const {
        int idx = 4 + (auxChannelNumber - 1); // 0..3 = roll,pitch,yaw,throttle
        if (idx < 0 || idx >= channelCount) return 0;
        return channels[idx];
    }

private:
    static constexpr uint32_t REQUEST_INTERVAL_MS = 100;

    Stream *_port = nullptr;
    uint32_t _lastRequest = 0;
    bool _requestToggle = false;

    enum class ParseState { IDLE, HEADER_M, HEADER_DIR, SIZE, CMD, PAYLOAD, CHECKSUM };
    ParseState _state = ParseState::IDLE;
    uint8_t _expectedSize = 0;
    uint8_t _cmd = 0;
    uint8_t _payload[64];
    uint8_t _payloadIdx = 0;
    uint8_t _checksum = 0;

    void sendRequest(uint8_t cmd) {
        if (!_port) return;
        uint8_t size = 0;
        uint8_t checksum = size ^ cmd;

        _port->write('$');
        _port->write('M');
        _port->write('<');
        _port->write(size);
        _port->write(cmd);
        _port->write(checksum);
    }

    void readIncoming() {
        if (!_port) return;
        while (_port->available()) {
            uint8_t b = _port->read();
            switch (_state) {
                case ParseState::IDLE:
                    if (b == '$') _state = ParseState::HEADER_M;
                    break;
                case ParseState::HEADER_M:
                    _state = (b == 'M') ? ParseState::HEADER_DIR : ParseState::IDLE;
                    break;
                case ParseState::HEADER_DIR:
                    // '>' = ответ от FC, остальное (ошибка/чужой запрос) игнорируем
                    _state = (b == '>') ? ParseState::SIZE : ParseState::IDLE;
                    break;
                case ParseState::SIZE:
                    _expectedSize = b;
                    _payloadIdx = 0;
                    _checksum = b;
                    _state = ParseState::CMD;
                    break;
                case ParseState::CMD:
                    _cmd = b;
                    _checksum ^= b;
                    _state = (_expectedSize > 0) ? ParseState::PAYLOAD : ParseState::CHECKSUM;
                    break;
                case ParseState::PAYLOAD:
                    if (_payloadIdx < sizeof(_payload)) {
                        _payload[_payloadIdx] = b;
                    }
                    _checksum ^= b;
                    _payloadIdx++;
                    if (_payloadIdx >= _expectedSize) {
                        _state = ParseState::CHECKSUM;
                    }
                    break;
                case ParseState::CHECKSUM:
                    if (b == _checksum) {
                        handleFrame();
                    }
                    _state = ParseState::IDLE;
                    break;
            }
        }
    }

    void handleFrame() {
        if (_cmd == MSP_STATUS && _expectedSize >= 10) {
            uint32_t flightModeFlags = (uint32_t)_payload[6] |
                                        ((uint32_t)_payload[7] << 8) |
                                        ((uint32_t)_payload[8] << 16) |
                                        ((uint32_t)_payload[9] << 24);
            armed = (flightModeFlags & 0x01) != 0;
        } else if (_cmd == MSP_RC) {
            uint8_t rawCount = _expectedSize / 2;
            channelCount = (rawCount > MAX_CHANNELS) ? MAX_CHANNELS : rawCount;
            for (uint8_t i = 0; i < channelCount; i++) {
                channels[i] = (uint16_t)_payload[i * 2] | ((uint16_t)_payload[i * 2 + 1] << 8);
            }
        }
    }
};
