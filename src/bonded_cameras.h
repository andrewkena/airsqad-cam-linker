#pragma once
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------
// Список привязанных (bonded) камер поверх NimBLE bond store.
//
// NimBLE сам хранит адреса и ключи привязки, но не имена устройств —
// имя дополнительно кэшируется здесь в NVS по адресу устройства, чтобы
// показать человекочитаемый список в веб-портале. Источником истины по
// тому, какие устройства реально привязаны, остаётся сам NimBLE bond
// store (NimBLEDevice::getNumBonds()/getBondedAddress()).
// ---------------------------------------------------------------------

struct BondedCamera {
    std::string address;
    std::string name;
};

class BondedCameras {
public:
    // Вызывать сразу после успешного подключения+bonding к камере.
    static void remember(const std::string &address, const std::string &name) {
        Preferences prefs;
        prefs.begin("bonds", false);
        prefs.putString(address.c_str(), name.c_str());
        prefs.end();
    }

    static std::vector<BondedCamera> list() {
        std::vector<BondedCamera> result;
        int count = NimBLEDevice::getNumBonds();

        Preferences prefs;
        prefs.begin("bonds", true);

        for (int i = 0; i < count; i++) {
            std::string addrStr = NimBLEDevice::getBondedAddress(i).toString();
            String cachedName = prefs.getString(addrStr.c_str(), "");
            BondedCamera cam;
            cam.address = addrStr;
            cam.name = cachedName.length() ? std::string(cachedName.c_str()) : addrStr;
            result.push_back(cam);
        }

        prefs.end();
        return result;
    }

    // Удаляет и сам bond в NimBLE, и закэшированное имя.
    static void forget(const std::string &address) {
        int count = NimBLEDevice::getNumBonds();
        for (int i = 0; i < count; i++) {
            NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
            if (addr.toString() == address) {
                NimBLEDevice::deleteBond(addr);
                break;
            }
        }

        Preferences prefs;
        prefs.begin("bonds", false);
        prefs.remove(address.c_str());
        prefs.end();
    }
};
