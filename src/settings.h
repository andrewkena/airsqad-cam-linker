#pragma once
#include <Preferences.h>

// ---------------------------------------------------------------------
// Настройки моста, сохраняются в NVS (переживают перезагрузку/потерю питания).
// ---------------------------------------------------------------------

enum class RecTriggerMode : uint8_t { AIR = 0, SWITCH = 1 };

// Что выводить в конкретном слоте OSD_CUSTOM_MSG (1..4).
enum class OsdField : uint8_t {
    NONE = 0,
    CONNECTION = 1, // GP OK / GP ---
    RECORDING = 2,  // REC / IDLE
    BATTERY = 3,    // BAT NN%
    SD_STATUS = 4,  // SD <код>
};

struct Settings {
    static constexpr uint8_t OSD_SLOT_COUNT = 4;

    RecTriggerMode triggerMode = RecTriggerMode::AIR;
    uint8_t triggerAuxChannel = 1; // AUX1..AUX8 (1-based), используется только в режиме SWITCH
    bool stopOnDisarm = true;

    OsdField osdSlotField[OSD_SLOT_COUNT] = {
        OsdField::CONNECTION,
        OsdField::RECORDING,
        OsdField::BATTERY,
        OsdField::SD_STATUS,
    };

    void load() {
        Preferences prefs;
        prefs.begin("cfg", true);
        triggerMode = (RecTriggerMode)prefs.getUChar("trigMode", (uint8_t)RecTriggerMode::AIR);
        triggerAuxChannel = prefs.getUChar("trigAux", 1);
        stopOnDisarm = prefs.getBool("stopDisarm", true);

        for (uint8_t i = 0; i < OSD_SLOT_COUNT; i++) {
            char key[8];
            snprintf(key, sizeof(key), "osdF%u", i);
            osdSlotField[i] = (OsdField)prefs.getUChar(key, (uint8_t)osdSlotField[i]);
        }
        prefs.end();
    }

    void save() const {
        Preferences prefs;
        prefs.begin("cfg", false);
        prefs.putUChar("trigMode", (uint8_t)triggerMode);
        prefs.putUChar("trigAux", triggerAuxChannel);
        prefs.putBool("stopDisarm", stopOnDisarm);

        for (uint8_t i = 0; i < OSD_SLOT_COUNT; i++) {
            char key[8];
            snprintf(key, sizeof(key), "osdF%u", i);
            prefs.putUChar(key, (uint8_t)osdSlotField[i]);
        }
        prefs.end();
    }
};
