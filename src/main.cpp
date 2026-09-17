#include <Arduino.h>
#include "gopro_ble.h"
#include "msp_osd.h"
#include "msp_fc.h"
#include "settings.h"
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

// PWM-порог, выше которого AUX-канал считается "включён" (режим SWITCH).
static constexpr uint16_t AUX_SWITCH_THRESHOLD = 1700;

HardwareSerial mspSerial(1);
GoProBle goPro;
StatusPortal portal;
Settings settings;
MspFc mspFc;

uint32_t lastOsdUpdate = 0;
bool recordingActive = false;

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

// Решает, нужно ли сейчас писать видео, исходя из настроек триггера,
// и при смене состояния шлёт GoPro команду старт/стоп.
void updateRecordingTrigger() {
    if (!goPro.status.connected) return;

    bool shouldRecord;
    if (settings.triggerMode == RecTriggerMode::AIR) {
        shouldRecord = mspFc.armed;
    } else {
        shouldRecord = mspFc.getAuxChannelValue(settings.triggerAuxChannel) > AUX_SWITCH_THRESHOLD;
    }

    if (settings.stopOnDisarm && !mspFc.armed) {
        shouldRecord = false;
    }

    if (shouldRecord != recordingActive) {
        recordingActive = shouldRecord;
        goPro.setShutter(recordingActive);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("GoPro <-> Betaflight OSD bridge starting...");

    settings.load();

    mspSerial.begin(MSP_BAUD, SERIAL_8N1, MSP_RX_PIN, MSP_TX_PIN);
    mspFc.begin(mspSerial);

    // BLE-стек поднимаем сразу — он нужен порталу для показа списка
    // привязанных камер. Само сканирование включаем только после
    // закрытия портала (см. loop()), чтобы не грузить радиомодуль
    // ESP32-C3 и не мешать работе Wi-Fi точки доступа.
    goPro.begin("GoPro"); // подставьте точное имя устройства при необходимости

    // Первые 30 секунд после включения: точка доступа
    // "AIRSQAD Cam Linker" на 10.0.0.1 со страницей статуса/настроек.
    portal.begin("AIRSQAD Cam Linker", "12345678", &settings);
}

void loop() {
    bool portalActive = portal.loop(goPro.status);
    if (!portalActive) {
        goPro.enableScanning();
    }
    goPro.loop();
    mspFc.loop();

    uint32_t now = millis();
    if (now - lastOsdUpdate > OSD_UPDATE_INTERVAL_MS) {
        lastOsdUpdate = now;
        updateOsd();
        updateRecordingTrigger();

        // Отладочный вывод в USB-Serial
        Serial.printf("GoPro: connected=%d recording=%d battery=%d%% sd=%u armed=%d\n",
                       goPro.status.connected,
                       goPro.status.recording,
                       goPro.status.batteryPercent,
                       goPro.status.sdStatus,
                       mspFc.armed);
    }
}
