#pragma once
#include <NimBLEDevice.h>
#include <vector>
#include "bonded_cameras.h"

// ---------------------------------------------------------------------
// Клиент к GoPro по Open GoPro BLE spec (сервис FEA6).
// UUID-ы характеристик подтверждены по официальным демо gopro/OpenGoPro:
//   Command  Req/Rsp : b5f90072 / b5f90073
//   Settings Req/Rsp : b5f90074 / b5f90075
//   Query    Req/Rsp : b5f90076 / b5f90077
//
// Статус запрашивается через GetStatusValue (query_id 0x13), ID из
// официального списка Status IDs (gopro/OpenGoPro):
//   10  = Encoding Active (recording, bool)
//   70  = Internal Battery Percentage (0..100) — раньше здесь ошибочно
//         стоял ID 2 (Internal Battery BARS, шкала делений, не проценты)
//   2   = Internal Battery Bars (шкала делений, сырое значение)
//   33  = Primary Storage (SD Status)
//   6   = Overheating (bool)
//   8   = Busy (bool)
//   35  = Remaining Video Time, сек (uint32, big-endian)
//   54  = SD Card Remaining, КБ (uint64, big-endian)
//   117 = SD Card Capacity, КБ (uint32, big-endian)
//   112 = SD Card Errors (uint8)
//
// Ответ на GetStatusValue может не поместиться в один BLE-пакет (MTU) —
// используется пересборка по протоколу пакетизации Open GoPro BLE
// (GENERAL/EXT_13/EXT_16 заголовки + continuation-бит), см. accumulateQueryPacket().
//
// Старт/стоп записи — команда Set Shutter (ID 0x01) через Command Req.
// ---------------------------------------------------------------------

class GoProBle {
public:
    struct Status {
        bool connected = false;
        bool recording = false;
        int batteryPercent = -1;
        uint8_t batteryBars = 0;
        uint8_t sdStatus = 0;
        bool overheating = false;
        bool busy = false;
        uint32_t remainingVideoTimeSec = 0;
        uint64_t sdRemainingKB = 0;
        uint32_t sdCapacityKB = 0;
        uint8_t sdErrors = 0;
        uint32_t lastUpdateMs = 0;
    };

    Status status;

    static constexpr const char *SERVICE_UUID     = "0000fea6-0000-1000-8000-00805f9b34fb";
    static constexpr const char *COMMAND_REQ_UUID = "b5f90072-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *COMMAND_RSP_UUID = "b5f90073-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *QUERY_REQ_UUID   = "b5f90076-aa8d-11e3-9046-0002a5d5c51b";
    static constexpr const char *QUERY_RSP_UUID   = "b5f90077-aa8d-11e3-9046-0002a5d5c51b";

    static constexpr uint8_t STATUS_ENCODING_ACTIVE      = 10;
    static constexpr uint8_t STATUS_BATTERY_PERCENT      = 70;
    static constexpr uint8_t STATUS_BATTERY_BARS         = 2;
    static constexpr uint8_t STATUS_SD_STATUS            = 33;
    static constexpr uint8_t STATUS_OVERHEATING          = 6;
    static constexpr uint8_t STATUS_BUSY                 = 8;
    static constexpr uint8_t STATUS_REMAINING_VIDEO_TIME = 35;
    static constexpr uint8_t STATUS_SD_REMAINING         = 54;
    static constexpr uint8_t STATUS_SD_CAPACITY          = 117;
    static constexpr uint8_t STATUS_SD_ERRORS            = 112;

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

    // Буфер пересборки многопакетного BLE-ответа (см. accumulateQueryPacket).
    std::vector<uint8_t> _queryAccum;
    size_t _queryBytesRemaining = 0;

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
        // GetStatusValue: [len][0x13][id1][id2]...[idN]
        uint8_t ids[] = {
            STATUS_ENCODING_ACTIVE, STATUS_BATTERY_PERCENT, STATUS_BATTERY_BARS,
            STATUS_SD_STATUS, STATUS_OVERHEATING, STATUS_BUSY,
            STATUS_REMAINING_VIDEO_TIME, STATUS_SD_REMAINING,
            STATUS_SD_CAPACITY, STATUS_SD_ERRORS,
        };
        uint8_t pkt[2 + sizeof(ids)];
        pkt[0] = 1 + sizeof(ids); // query_id byte + N id bytes
        pkt[1] = 0x13;            // GetStatusValue
        memcpy(&pkt[2], ids, sizeof(ids));
        _pQueryReq->writeValue(pkt, sizeof(pkt), true);
    }

    // Пересборка BLE-пакетов по протоколу пакетизации Open GoPro BLE.
    // Заголовок первого байта пакета:
    //   бит 7       — continuation (1 = продолжение предыдущего сообщения)
    //   биты 6-5    — тип заголовка (0=GENERAL/5-bit len, 1=EXT_13, 2=EXT_16)
    // GENERAL:  длина в младших 5 битах первого байта.
    // EXT_13:   длина в младших 5 битах первого байта (старшие) + весь второй байт.
    // EXT_16:   длина — 2-й и 3-й байты целиком (первый байт только тип).
    // Копится в _queryAccum, пока не наберётся заявленное число байт —
    // тогда буфер целиком отдаётся в parseQueryPayload().
    void onQueryResponse(uint8_t *data, size_t len) {
        if (len == 0) return;

        uint8_t b0 = data[0];
        size_t offset;

        if (b0 & 0x80) {
            // continuation-пакет — данные идут сразу после заголовка (1 байт)
            if (_queryAccum.empty() && _queryBytesRemaining == 0) return; // нечего продолжать
            offset = 1;
        } else {
            _queryAccum.clear();
            uint8_t hdrType = (b0 & 0x60) >> 5;
            if (hdrType == 0) { // GENERAL
                _queryBytesRemaining = b0 & 0x1F;
                offset = 1;
            } else if (hdrType == 1) { // EXT_13
                if (len < 2) return;
                _queryBytesRemaining = ((size_t)(b0 & 0x1F) << 8) | data[1];
                offset = 2;
            } else if (hdrType == 2) { // EXT_16
                if (len < 3) return;
                _queryBytesRemaining = ((size_t)data[1] << 8) | data[2];
                offset = 3;
            } else {
                return; // RESERVED — не поддерживается
            }
        }

        if (offset > len) return;
        size_t chunkLen = len - offset;
        _queryAccum.insert(_queryAccum.end(), data + offset, data + offset + chunkLen);
        _queryBytesRemaining = (chunkLen > _queryBytesRemaining) ? 0 : (_queryBytesRemaining - chunkLen);

        if (_queryBytesRemaining == 0 && !_queryAccum.empty()) {
            parseQueryPayload(_queryAccum.data(), _queryAccum.size());
            _queryAccum.clear();
        }
    }

    // Собранный payload: [query_id_echo][id][val_len][val...][id][val_len][val...]...
    void parseQueryPayload(const uint8_t *data, size_t len) {
        if (len < 1) return;
        size_t pos = 1; // пропускаем query_id_echo
        while (pos + 2 <= len) {
            uint8_t id = data[pos];
            uint8_t vlen = data[pos + 1];
            if (pos + 2 + vlen > len) break;
            applyStatusValue(id, &data[pos + 2], vlen);
            pos += 2 + vlen;
        }
        status.lastUpdateMs = millis();
    }

    // Числовые статусы Open GoPro передаются big-endian.
    static uint64_t toUint(const uint8_t *val, uint8_t vlen) {
        uint64_t result = 0;
        for (uint8_t i = 0; i < vlen && i < 8; i++) {
            result = (result << 8) | val[i];
        }
        return result;
    }

    void applyStatusValue(uint8_t id, const uint8_t *val, uint8_t vlen) {
        if (vlen == 0) return;
        uint64_t v = toUint(val, vlen);

        switch (id) {
            case STATUS_ENCODING_ACTIVE:      status.recording = v != 0; break;
            case STATUS_BATTERY_PERCENT:      status.batteryPercent = (int)v; break;
            case STATUS_BATTERY_BARS:         status.batteryBars = (uint8_t)v; break;
            case STATUS_SD_STATUS:            status.sdStatus = (uint8_t)v; break;
            case STATUS_OVERHEATING:          status.overheating = v != 0; break;
            case STATUS_BUSY:                 status.busy = v != 0; break;
            case STATUS_REMAINING_VIDEO_TIME: status.remainingVideoTimeSec = (uint32_t)v; break;
            case STATUS_SD_REMAINING:         status.sdRemainingKB = v; break;
            case STATUS_SD_CAPACITY:          status.sdCapacityKB = (uint32_t)v; break;
            case STATUS_SD_ERRORS:            status.sdErrors = (uint8_t)v; break;
            default: break;
        }
    }
};
