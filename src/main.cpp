#include <Arduino.h>
#include "gopro_ble.h"
#include "msp_osd.h"
#include "status_portal.h"

// ---------------------------------------------------------------------
// UART к полётному контроллеру (MSP).
// Пины подобраны так, чтобы НЕ пересекаться с USB-CDC на ESP32-C3
// SuperMini (обычно занят GPIO18/19 или GPIO20/21 в зависимости от
// партии платы) — если конфликтует, замените на любые свободные GPIO.
// ---------------------------------------------------------------------
static constexpr int MSP_RX_PIN = 4;
static constexpr int MSP_TX_PIN = 5;
static constexpr uint32_t MSP_BAUD = 115200;

// Индекс OSD_CUSTOM_MSG-слота (0 -> OSD_CUSTOM_MSG1)
static constexpr uint8_t OSD_SLOT = 0;
static constexpr uint32_t OSD_UPDATE_INTERVAL_MS = 500;

HardwareSerial mspSerial(1);
GoProBle goPro;
StatusPortal portal;

uint32_t lastOsdUpdate = 0;

void updateOsd() {
    char text[mspOsd::CUSTOM_MSG_MAX_LEN + 1];

    if (!goPro.status.connected) {
        snprintf(text, sizeof(text), "GP ---");
    } else if (goPro.status.recording) {
        if (goPro.status.batteryPercent >= 0) {
            snprintf(text, sizeof(text), "REC BAT%d%%", goPro.status.batteryPercent);
        } else {
            snprintf(text, sizeof(text), "REC");
        }
    } else {
        snprintf(text, sizeof(text), "GP IDLE");
    }

    mspOsd::setCustomMessage(mspSerial, OSD_SLOT, text);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("GoPro <-> Betaflight OSD bridge starting...");

    mspSerial.begin(MSP_BAUD, SERIAL_8N1, MSP_RX_PIN, MSP_TX_PIN);

    goPro.begin("GoPro"); // подставьте точное имя устройства при необходимости

    // Первые 30 секунд после включения: точка доступа
    // "AIRSQAD Cam Linker" на 10.0.0.1 со страницей статуса/настроек.
    portal.begin("AIRSQAD Cam Linker", "12345678");
}

void loop() {
    goPro.loop();
    portal.loop(goPro.status);

    uint32_t now = millis();
    if (now - lastOsdUpdate > OSD_UPDATE_INTERVAL_MS) {
        lastOsdUpdate = now;
        updateOsd();

        // Отладочный вывод в USB-Serial
        Serial.printf("GoPro: connected=%d recording=%d battery=%d%% sd=%u\n",
                       goPro.status.connected,
                       goPro.status.recording,
                       goPro.status.batteryPercent,
                       goPro.status.sdStatus);
    }
}
