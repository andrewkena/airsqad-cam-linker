#pragma once
#include <NimBLEDevice.h>

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

    static constexpr const char *SERVICE_UUID    = "0000fea6-0000-1000-8000-00805f9b34fb";
    static constexpr const char *QUERY_REQ_UUID  = "b5f90076-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *QUERY_RSP_UUID  = "b5f90077-aa8d-11e3-9046-0002a5d5c51b";

    static constexpr uint8_t STATUS_ENCODING_ACTIVE = 10;
    static constexpr uint8_t STATUS_BATTERY_PERCENT = 2;
    static constexpr uint8_t STATUS_SD_STATUS       = 33;

    void begin(const std::string &wantedNamePrefix = "GoPro") {
        _namePrefix = wantedNamePrefix;
        NimBLEDevice::init("ESP32-OSD-Bridge");
        // GoPro требует bonding для части характеристик — включаем заранее.
        NimBLEDevice::setSecurityAuth(true, true, true);
    }

    // Вызывать часто (из loop() или отдельной FreeRTOS-задачи).
    // Сам разруливает: не подключены -> ищем и коннектимся;
    // подключены -> раз в интервал шлём GetStatusValue.
    void loop() {
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
    NimBLEClient *_pClient = nullptr;
    NimBLERemoteCharacteristic *_pQueryReq = nullptr;
    NimBLERemoteCharacteristic *_pQueryRsp = nullptr;
    uint32_t _lastConnectAttempt = 0;
    uint32_t _lastPoll = 0;

    void tryConnect() {
        NimBLEScan *pScan = NimBLEDevice::getScan();
        pScan->setActiveScan(true);
        NimBLEScanResults results = pScan->getResults(SCAN_TIME_MS, false);

        const NimBLEAdvertisedDevice *target = nullptr;
        for (int i = 0; i < results.getCount(); i++) {
            const NimBLEAdvertisedDevice *dev = results.getDevice(i);
            if (dev->haveName() && dev->getName().rfind(_namePrefix, 0) == 0) {
                target = dev;
                break;
            }
            if (dev->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
                target = dev;
                break;
            }
        }
        pScan->clearResults();
        if (!target) return;

        if (!_pClient) {
            _pClient = NimBLEDevice::createClient();
        }
        if (!_pClient->connect(target)) {
            return;
        }

        NimBLERemoteService *pSvc = _pClient->getService(SERVICE_UUID);
        if (!pSvc) {
            _pClient->disconnect();
            return;
        }
        _pQueryReq = pSvc->getCharacteristic(QUERY_REQ_UUID);
        _pQueryRsp = pSvc->getCharacteristic(QUERY_RSP_UUID);
        if (!_pQueryReq || !_pQueryRsp) {
            _pClient->disconnect();
            return;
        }

        _pQueryRsp->subscribe(true,
            [this](NimBLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify) {
                this->onQueryResponse(data, len);
            });

        status.connected = true;
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
