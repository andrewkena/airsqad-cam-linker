#pragma once
#include <Preferences.h>

// ---------------------------------------------------------------------
// Настройки моста, сохраняются в NVS (переживают перезагрузку/потерю питания).
// ---------------------------------------------------------------------

enum class RecTriggerMode : uint8_t { AIR = 0, SWITCH = 1 };

struct Settings {
    RecTriggerMode triggerMode = RecTriggerMode::AIR;
    uint8_t triggerAuxChannel = 1; // AUX1..AUX8 (1-based), используется только в режиме SWITCH
    bool stopOnDisarm = true;

    void load() {
        Preferences prefs;
        prefs.begin("cfg", true);
        triggerMode = (RecTriggerMode)prefs.getUChar("trigMode", (uint8_t)RecTriggerMode::AIR);
        triggerAuxChannel = prefs.getUChar("trigAux", 1);
        stopOnDisarm = prefs.getBool("stopDisarm", true);
        prefs.end();
    }

    void save() const {
        Preferences prefs;
        prefs.begin("cfg", false);
        prefs.putUChar("trigMode", (uint8_t)triggerMode);
        prefs.putUChar("trigAux", triggerAuxChannel);
        prefs.putBool("stopDisarm", stopOnDisarm);
        prefs.end();
    }
};
