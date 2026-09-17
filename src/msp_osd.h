#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------
// MSPv2 -> Betaflight "Custom message" OSD elements
//
// Command:   MSP2_SET_TEXT = 0x3007
// Payload:   [text_type][ascii bytes, NOT null-terminated]
// text_type для custom-сообщений: MSP2TEXT_CUSTOM_MSG_0 + index
//   MSP2TEXT_CUSTOM_MSG_0 = 7  (см. src/main/msp/msp_protocol_v2_betaflight.h
//   в исходниках Betaflight — проверено по актуальному master)
//   CUSTOM_MSG_MAX_NUM    = 4  -> т.е. доступны индексы 0..3
//   (OSD-элементы OSD_CUSTOM_MSG1..OSD_CUSTOM_MSG4 в OSD-табе Configurator)
//
// Каждое сообщение ограничено ~16 символами (как имя пилота/крафта) —
// длинные строки будут обрезаны прошивкой.
// ---------------------------------------------------------------------

namespace mspOsd {

constexpr uint16_t MSP2_SET_TEXT = 0x3007;
constexpr uint8_t MSP2TEXT_CUSTOM_MSG_0 = 7;
constexpr uint8_t CUSTOM_MSG_MAX_NUM = 4;
constexpr uint8_t CUSTOM_MSG_MAX_LEN = 16;

// CRC8 DVB-S2 — используется во всех MSPv2-кадрах
inline uint8_t crc8DvbS2(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : (crc << 1);
    }
    return crc;
}

inline uint8_t crc8DvbS2Buf(uint8_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) crc = crc8DvbS2(crc, data[i]);
    return crc;
}

// Отправляет один MSPv2-запрос (без ожидания ответа — нам достаточно "fire and forget"
// для установки текста, FC ответ на MSP2_SET_TEXT не критичен для этой задачи).
inline void sendMspV2(Stream &port, uint16_t function, const uint8_t *payload, uint16_t size) {
    uint8_t header[8];
    header[0] = '$';
    header[1] = 'X';
    header[2] = '<';
    header[3] = 0; // flag
    header[4] = function & 0xFF;
    header[5] = (function >> 8) & 0xFF;
    header[6] = size & 0xFF;
    header[7] = (size >> 8) & 0xFF;

    uint8_t crc = 0;
    crc = crc8DvbS2Buf(crc, &header[3], 5); // flag + function(2) + size(2)
    crc = crc8DvbS2Buf(crc, payload, size);

    port.write(header, sizeof(header));
    if (size > 0) port.write(payload, size);
    port.write(crc);
}

// index: 0..3 (соответствует OSD_CUSTOM_MSG1..4)
inline void setCustomMessage(Stream &port, uint8_t index, const char *text) {
    if (index >= CUSTOM_MSG_MAX_NUM) return;

    uint8_t payload[1 + CUSTOM_MSG_MAX_LEN];
    payload[0] = MSP2TEXT_CUSTOM_MSG_0 + index;

    size_t len = strnlen(text, CUSTOM_MSG_MAX_LEN);
    memcpy(&payload[1], text, len);

    sendMspV2(port, MSP2_SET_TEXT, payload, 1 + len);
}

} // namespace mspOsd
