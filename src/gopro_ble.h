#pragma once
#include <NimBLEDevice.h>
#include "bonded_cameras.h"

// ---------------------------------------------------------------------
// Клиент к GoPro по Open GoPro BLE spec (сервис FEA6).
// UUID-ы характеристик подтверждены по официальным демо gopro/OpenGoPro:
//   Command  Req/Rsp : b5f90072 / b5f90073
//   Settings Req/Rsp : b5f90074 / b5f90075
//   Query    Req/Rsp : b5f90076 / b5f90077
//
// Статус записи опрашивается через GetStatusValue (query_id 0x13):
//   Status ID 10 = Encoding Active (recording, bool)
//   Status ID  2 = Internal Battery Level (0..100 либо 0..3 в зависимости
//                  от модели — на GoPro 11 это проценты 0..100)
//   Status ID 33 = SD Card Status
//
// Старт/стоп записи — команда Set Shutter (ID 0x01) через Command Req.
// ---------------------------------------------------------------------

class GoProBle {
public:
    struct Status {
        bool connected = false;
        bool recording = false;
        int batteryPercent = -1;
        uint8_t sdStatus = 0;
        uint32_t lastUpdateMs = 0;
    };

    Status status;

    static constexpr const char *SERVICE_UUID     = "0000fea6-0000-1000-8000-00805f9b34fb";
    static constexpr const char *COMMAND_REQ_UUID = "b5f90072-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *COMMAND_RSP_UUID = "b5f90073-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *QUERY_REQ_UUID   = "b5f90076-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *QUERY_RSP_UUID   = "b5f90077-aa8d-11e3-9046-0002a5d5c51b";

    static constexpr uint8_t STATUS_ENCODING_ACTIVE = 10;
    static constexpr uint8_t STATUS_BATTERY_PERCENT = 2;
    static constexpr uint8_t STATUS_SD_STATUS       = 33;

    // Инициализирует BLE-стек (нужно для сканирования и для чтения списка
    // привязанных устройств в веб-портале). Само сканирование/подключение
    // остаётся выключенным, пока не позвать enableScanning() — это позволяет
    // держать портал настроек рабочим, не грузя радиомодуль ESP32-C3
    // активным сканированием (иначе Wi-Fi точка доступа перестаёт отвечать).
    void begin(const std::string &wantedNamePrefix = "GoPro") {
        _namePrefix = wantedNamePrefix;
        NimBLEDevice::init("ESP32-OSD-Bridge");
        // GoPro требует bonding для части характеристик — включаем заранее.
        NimBLEDevice::setSecurityAuth(true, true, true);
    }

    void enableScanning() {
        _scanningEnabled = true;
    }

    // Запуск/остановка записи (Open GoPro "Set Shutter", command ID 0x01).
    void setShutter(bool start) {
        if (!_pCommandReq || !status.connected) return;
        uint8_t pkt[4] = {0x03, 0x01, 0x01, (uint8_t)(start ? 0x01 : 0x00)};
        _pCommandReq->writeValue(pkt, sizeof(pkt), true);
    }

    // Вызывать часто (из loop() или отдельной FreeRTOS-задачи).
    // Сам разруливает: не подключены -> ищем и коннектимся;
    // подключены -> раз в интервал шлём GetStatusValue.
    void loop() {
        if (!_scanningEnabled) return;

        uint32_t now = millis();

        if (!status.connected) {
            if (now - _lastConnectAttempt > RECONNECT_INTERVAL_MS) {
                _lastConnectAttempt = now;
                tryConnect();
            }
            return;
        }

        if (_pClient && !_pClient->isConnected()) {
            status.connected = false;
            return;
        }

        if (now - _lastPoll > POLL_INTERVAL_MS) {
            _lastPoll = now;
            requestStatus();
        }
    }

private:
    static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;
    static constexpr uint32_t POLL_INTERVAL_MS = 1000;
    static constexpr uint32_t SCAN_TIME_MS = 4000;

    std::string _namePrefix;
    bool _scanningEnabled = false;
    NimBLEClient *_pClient = nullptr;
    NimBLERemoteCharacteristic *_pQueryReq = nullptr;
    NimBLERemoteCharacteristic *_pQueryRsp = nullptr;
    NimBLERemoteCharacteristic *_pCommandReq = nullptr;
    NimBLERemoteCharacteristic *_pCommandRsp = nullptr;
    uint32_t _lastConnectAttempt = 0;
    uint32_t _lastPoll = 0;

    void tryConnect() {
        NimBLEScan *pScan = NimBLEDevice::getScan();
        pScan->setActiveScan(true);
        NimBLEScanResults results = pScan->start(SCAN_TIME_MS, false);

        NimBLEAdvertisedDevice target;
        bool found = false;
        for (int i = 0; i < results.getCount(); i++) {
            NimBLEAdvertisedDevice dev = results.getDevice(i);
            if (dev.haveName() && dev.getName().rfind(_namePrefix, 0) == 0) {
                target = dev;
                found = true;
                break;
            }
            if (dev.isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
                target = dev;
                found = true;
                break;
            }
        }
        pScan->clearResults();
        if (!found) return;

        if (!_pClient) {
            _pClient = NimBLEDevice::createClient();
        }
        if (!_pClient->connect(&target)) {
            return;
        }

        NimBLERemoteService *pSvc = _pClient->getService(SERVICE_UUID);
        if (!pSvc) {
            _pClient->disconnect();
            return;
        }
        _pQueryReq = pSvc->getCharacteristic(QUERY_REQ_UUID);
        _pQueryRsp = pSvc->getCharacteristic(QUERY_RSP_UUID);
        _pCommandReq = pSvc->getCharacteristic(COMMAND_REQ_UUID);
        _pCommandRsp = pSvc->getCharacteristic(COMMAND_RSP_UUID);
        if (!_pQueryReq || !_pQueryRsp || !_pCommandReq || !_pCommandRsp) {
            _pClient->disconnect();
            return;
        }

        _pQueryRsp->subscribe(true,
            [this](NimBLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify) {
                this->onQueryResponse(data, len);
            });

        status.connected = true;
        BondedCameras::remember(target.getAddress().toString(), target.getName());
    }

    void requestStatus() {
        if (!_pQueryReq) return;
        // GetStatusValue: [len][0x13][id1][id2][id3]
        uint8_t ids[] = {STATUS_ENCODING_ACTIVE, STATUS_BATTERY_PERCENT, STATUS_SD_STATUS};
        uint8_t pkt[2 + sizeof(ids)];
        pkt[0] = 1 + sizeof(ids); // query_id byte + N id bytes
        pkt[1] = 0x13;            // GetStatusValue
        memcpy(&pkt[2], ids, sizeof(ids));
        _pQueryReq->writeValue(pkt, sizeof(pkt), true);
    }

    // Разбор ответа: [len][query_id_echo][id][val_len][val...] [id][val_len][val...] ...
    void onQueryResponse(uint8_t *data, size_t len) {
        if (len < 2) return;
        size_t pos = 2; // пропускаем [len][query_id_echo]
        while (pos + 2 <= len) {
            uint8_t id = data[pos];
            uint8_t vlen = data[pos + 1];
            if (pos + 2 + vlen > len) break;
            const uint8_t *val = &data[pos + 2];

            if (id == STATUS_ENCODING_ACTIVE && vlen >= 1) {
                status.recording = val[0] != 0;
            } else if (id == STATUS_BATTERY_PERCENT && vlen >= 1) {
                status.batteryPercent = val[0];
            } else if (id == STATUS_SD_STATUS && vlen >= 1) {
                status.sdStatus = val[0];
            }
            pos += 2 + vlen;
        }
        status.lastUpdateMs = millis();
    }
};
