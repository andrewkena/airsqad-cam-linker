#include <Arduino.h>
#include "gopro_ble.h"
#include "msp_osd.h"
#include "msp_fc.h"
#include "settings.h"
#include "status_portal.h"
#include "status_led.h"

// ---------------------------------------------------------------------
// UART к полётному контроллеру (MSP).
// Пины подобраны так, чтобы НЕ пересекаться с USB-CDC на ESP32-C3
// SuperMini (обычно занят GPIO18/19 или GPIO20/21 в зависимости от
// партии платы) — если конфликтует, замените на любые свободные GPIO.
// ---------------------------------------------------------------------
static constexpr int MSP_RX_PIN = 4;
static constexpr int MSP_TX_PIN = 5;
static constexpr uint32_t MSP_BAUD = 115200;

static constexpr uint32_t OSD_UPDATE_INTERVAL_MS = 500;

// PWM-порог, выше которого AUX-канал считается "включён" (режим SWITCH).
static constexpr uint16_t AUX_SWITCH_THRESHOLD = 1700;

// Встроенная кнопка BOOT на ESP32-C3 SuperMini (активный уровень LOW).
// Долгое удержание запускает привязку новой камеры. Кнопку можно жать
// только после того, как плата уже загрузилась — удержание BOOT именно
// в момент включения/сброса переводит чип в режим прошивки, а не в
// пользовательский код.
static constexpr int PAIRING_BUTTON_PIN = 9;
static constexpr uint32_t PAIRING_HOLD_MS = 2000;

// Встроенный светодиод на большинстве клонов ESP32-C3 SuperMini (активный LOW).
// Если на вашей плате его нет/он на другом пине — поменяйте здесь.
static constexpr int STATUS_LED_PIN = 8;

HardwareSerial mspSerial(1);
GoProBle goPro;
StatusPortal portal;
Settings settings;
MspFc mspFc;
StatusLed statusLed;

uint32_t lastOsdUpdate = 0;
bool recordingActive = false;
uint32_t buttonPressStartMs = 0;
bool pairingTriggered = false;

// Долгое удержание кнопки BOOT -> разрыв текущей связи и форсированный
// поиск новой камеры (см. GoProBle::startPairing()).
void checkPairingButton() {
    bool pressed = (digitalRead(PAIRING_BUTTON_PIN) == LOW);
    if (pressed) {
        if (buttonPressStartMs == 0) {
            buttonPressStartMs = millis();
        } else if (!pairingTriggered && millis() - buttonPressStartMs > PAIRING_HOLD_MS) {
            pairingTriggered = true;
            goPro.startPairing();
            Serial.println("Pairing button held: forcing new camera scan");
        }
    } else {
        buttonPressStartMs = 0;
        pairingTriggered = false;
    }
}

// Формирует текст для конкретного поля данных GoPro (независимо от слота,
// в который оно в итоге попадёт — привязку поле->слот задают настройки).
void buildFieldText(OsdField field, char *out, size_t outSize) {
    switch (field) {
        case OsdField::CONNECTION:
            if (goPro.isPairing()) {
                snprintf(out, outSize, "PAIRING");
            } else {
                snprintf(out, outSize, goPro.status.connected ? "GP OK" : "GP ---");
            }
            break;
        case OsdField::RECORDING:
            snprintf(out, outSize, goPro.status.recording ? "REC" : "IDLE");
            break;
        case OsdField::BATTERY:
            if (goPro.status.batteryPercent >= 0) {
                snprintf(out, outSize, "BAT %d%%", goPro.status.batteryPercent);
            } else {
                snprintf(out, outSize, "BAT --");
            }
            break;
        case OsdField::SD_STATUS:
            // Значение сырое (код Open GoPro status ID 33), точная расшифровка
            // кодов пока не подтверждена — при необходимости уточнить и заменить
            // на текстовые статусы (OK/FULL/ERROR и т.д.).
            snprintf(out, outSize, "SD %u", goPro.status.sdStatus);
            break;
        case OsdField::OVERHEATING:
            snprintf(out, outSize, goPro.status.overheating ? "HOT" : "TEMP OK");
            break;
        case OsdField::REMAINING_VIDEO_TIME: {
            uint32_t s = goPro.status.remainingVideoTimeSec;
            snprintf(out, outSize, "TIME %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
            break;
        }
        case OsdField::SD_REMAINING:
            snprintf(out, outSize, "FREE %luMB", (unsigned long)(goPro.status.sdRemainingKB / 1024));
            break;
        case OsdField::SD_CAPACITY:
            snprintf(out, outSize, "CAP %luMB", (unsigned long)(goPro.status.sdCapacityKB / 1024));
            break;
        case OsdField::SD_ERRORS:
            snprintf(out, outSize, "SD ERR %u", goPro.status.sdErrors);
            break;
        case OsdField::BUSY:
            snprintf(out, outSize, goPro.status.busy ? "BUSY" : "READY");
            break;
        case OsdField::BATTERY_BARS:
            snprintf(out, outSize, "BAT BARS %u", goPro.status.batteryBars);
            break;
        case OsdField::NONE:
        default:
            out[0] = '\0';
            break;
    }
}

void updateOsd() {
    for (uint8_t slot = 0; slot < Settings::OSD_SLOT_COUNT; slot++) {
        char text[mspOsd::CUSTOM_MSG_MAX_LEN + 1];
        buildFieldText(settings.osdSlotField[slot], text, sizeof(text));
        if (text[0] != '\0') {
            mspOsd::setCustomMessage(mspSerial, slot, text);
        }
    }
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
    Serial.println("AIRSQAD Cam Linker starting...");

    pinMode(PAIRING_BUTTON_PIN, INPUT_PULLUP);
    statusLed.begin(STATUS_LED_PIN, true);

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
    checkPairingButton();

    if (!settings.ledEnabled) {
        statusLed.off();
    } else {
        StatusLed::Pattern ledPattern;
        if (portalActive) {
            ledPattern = StatusLed::Pattern::WIFI_ACTIVE;
        } else if (!goPro.status.connected) {
            ledPattern = StatusLed::Pattern::WAITING_CAMERA;
        } else if (goPro.status.recording) {
            ledPattern = StatusLed::Pattern::RECORDING;
        } else {
            ledPattern = StatusLed::Pattern::CONNECTED;
        }
        statusLed.update(ledPattern);
    }

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
