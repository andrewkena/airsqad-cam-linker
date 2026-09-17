#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "gopro_ble.h"
#include "settings.h"
#include "bonded_cameras.h"
#include "logo.h"

// ---------------------------------------------------------------------
// Временный Wi-Fi портал состояния/настроек.
//
// На AP_DURATION_MS миллисекунд после старта поднимает открытую точку
// доступа с фиксированным IP 10.0.0.1, где отдаёт страницу с текущим
// статусом GoPro, настройками триггера записи и списком привязанных
// камер. Затем сам выключает Wi-Fi, чтобы не мешать BLE/UART в обычном
// режиме работы моста.
//
// Captive portal: DNS-сервер отвечает адресом платы на ЛЮБОЕ доменное
// имя, поэтому запросы ОС на проверку интернета (Android/iOS/Windows)
// попадают на наш веб-сервер и получают не то, что ожидали — система
// сама детектирует "портал с авторизацией" и открывает браузер на
// 10.0.0.1 автоматически, без ручного ввода адреса.
// ---------------------------------------------------------------------

class StatusPortal {
public:
    static constexpr uint32_t AP_DURATION_MS = 30000;

    void begin(const char *ssid, const char *password, Settings *settings) {
        _settings = settings;
        _startMs = millis();
        _active = true;

        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(IPAddress(10, 0, 0, 1), IPAddress(10, 0, 0, 1), IPAddress(255, 255, 255, 0));
        if (password && strlen(password) >= 8) {
            WiFi.softAP(ssid, password);
        } else {
            WiFi.softAP(ssid);
        }

        _server.on("/", HTTP_GET, [this]() { handleRoot(); });
        _server.on("/logo.svg", HTTP_GET, [this]() { handleLogo(); });
        _server.on("/save", HTTP_POST, [this]() { handleSaveSettings(); });
        _server.on("/forget-camera", HTTP_POST, [this]() { handleForgetCamera(); });
        _server.onNotFound([this]() { handleRoot(); });
        _server.begin();

        _dnsServer.start(53, "*", IPAddress(10, 0, 0, 1));
    }

    // Вызывать из loop(). Возвращает true, пока портал активен.
    bool loop(const GoProBle::Status &goProStatus) {
        if (!_active) return false;

        _lastStatus = goProStatus;
        _dnsServer.processNextRequest();
        _server.handleClient();

        if (millis() - _startMs > AP_DURATION_MS) {
            stop();
        }
        return _active;
    }

private:
    static constexpr const char *FIRMWARE_VERSION = "0.1";

    WebServer _server{80};
    DNSServer _dnsServer;
    uint32_t _startMs = 0;
    bool _active = false;
    GoProBle::Status _lastStatus;
    Settings *_settings = nullptr;

    void stop() {
        _dnsServer.stop();
        _server.stop();
        // Любая попытка остановить/выключить Wi-Fi (softAPdisconnect,
        // WiFi.mode(WIFI_OFF)) виснет на ESP32-C3, если BLE-стек уже
        // активен — общий радиомодуль, конфликт при переключении режима
        // из-под NimBLE. Поэтому просто останавливаем веб-сервер и DNS,
        // саму точку доступа оставляем висеть в фоне (ESP32-C3 умеет
        // работать с Wi-Fi и BLE одновременно).
        _active = false;
        Serial.println("Portal stopped, BLE scanning will start now");
    }

    void handleSaveSettings() {
        if (_settings) {
            int camType = _server.arg("cameraType").toInt();
            if (camType < 0 || camType > (int)CameraType::INSTA360) camType = 0;
            _settings->cameraType = (CameraType)camType;
            _settings->autoPowerOnCamera = _server.hasArg("autoPowerOnCamera");

            _settings->language = (_server.arg("uiLang") == "ru") ? UiLanguage::RU : UiLanguage::EN;

            String mode = _server.arg("mode");
            _settings->triggerMode = (mode == "SWITCH") ? RecTriggerMode::SWITCH : RecTriggerMode::AIR;

            int aux = _server.arg("aux").toInt();
            if (aux < 1) aux = 1;
            if (aux > 8) aux = 8;
            _settings->triggerAuxChannel = (uint8_t)aux;

            _settings->stopOnDisarm = _server.hasArg("stopOnDisarm");
            _settings->ledEnabled = _server.hasArg("ledEnabled");

            for (uint8_t i = 0; i < Settings::OSD_SLOT_COUNT; i++) {
                char argName[10];
                snprintf(argName, sizeof(argName), "osdField%u", i);
                int field = _server.arg(argName).toInt();
                if (field < 0 || field > (int)OSD_FIELD_MAX) field = 0;
                _settings->osdSlotField[i] = (OsdField)field;
            }

            _settings->save();
        }
        _server.sendHeader("Location", "/");
        _server.send(303);
    }

    static const char *fieldLabel(OsdField field, bool ru) {
        switch (field) {
            case OsdField::CONNECTION:           return ru ? "Статус подключения"        : "Connection status";
            case OsdField::RECORDING:            return ru ? "Идёт запись"               : "Recording";
            case OsdField::BATTERY:              return ru ? "Заряд батареи, %"          : "Battery, %";
            case OsdField::SD_STATUS:            return ru ? "Статус SD-карты"           : "SD card status";
            case OsdField::OVERHEATING:          return ru ? "Перегрев"                  : "Overheating";
            case OsdField::REMAINING_VIDEO_TIME: return ru ? "Осталось времени записи"   : "Remaining record time";
            case OsdField::SD_REMAINING:         return ru ? "Свободно на SD"            : "SD free space";
            case OsdField::SD_CAPACITY:          return ru ? "Ёмкость SD"                : "SD capacity";
            case OsdField::SD_ERRORS:            return ru ? "Ошибки SD-карты"           : "SD card errors";
            case OsdField::BUSY:                 return ru ? "Занята (busy/ready)"       : "Busy (busy/ready)";
            case OsdField::BATTERY_BARS:         return ru ? "Заряд батареи, делений"    : "Battery, bars";
            case OsdField::NONE:
            default:                             return ru ? "Не используется"           : "Not used";
        }
    }

    String osdFieldSelect(uint8_t slotIndex, OsdField current, bool ru) const {
        String out = "<select name='osdField" + String(slotIndex) + "'>";
        for (uint8_t f = (uint8_t)OsdField::NONE; f <= OSD_FIELD_MAX; f++) {
            out += "<option value='" + String(f) + "'" + (f == (uint8_t)current ? " selected" : "") +
                   ">" + fieldLabel((OsdField)f, ru) + "</option>";
        }
        out += "</select>";
        return out;
    }

    void handleLogo() {
        _server.send_P(200, "image/svg+xml", LOGO_SVG);
    }

    void handleForgetCamera() {
        String address = _server.arg("address");
        if (address.length() > 0) {
            BondedCameras::forget(address.c_str());
        }
        _server.sendHeader("Location", "/");
        _server.send(303);
    }

    void handleRoot() {
        String html;
        html.reserve(6144);

        html += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>AIRSQAD Cam Linker</title><style>"
                "body{background:#2b2b2b;color:#e0e0e0;font-family:-apple-system,'Segoe UI',Roboto,sans-serif;"
                "text-align:center;margin:0;padding:20px 20px;}"
                ".logo{width:180px;height:auto;margin-bottom:6px;filter:drop-shadow(0 6px 8px rgba(0,0,0,0.5));}"
                "h1{font-size:1.6em;font-weight:600;margin:0 0 8px 0;}"
                "hr{border:none;border-top:1px solid #555;width:200px;margin:0 auto 10px auto;}"
                ".version{color:#999;font-size:0.9em;margin-bottom:14px;}"
                "h2{font-size:1.1em;color:#ccc;margin:16px 0 6px 0;}"
                ".status-grid{display:flex;gap:8px;margin:8px auto 0 auto;width:100%;max-width:400px;"
                "box-sizing:border-box;}"
                ".status-box{flex:1;aspect-ratio:1/1;border:1px solid #666;border-radius:6px;background:#3a3a3a;"
                "display:flex;flex-direction:column;align-items:center;justify-content:center;padding:4px;"
                "box-sizing:border-box;}"
                ".status-box .label{font-size:0.7em;color:#999;text-transform:uppercase;text-align:center;}"
                ".status-box .value{font-size:0.95em;font-weight:600;margin-top:4px;text-align:center;}"
                ".status-box.green{background:#2e7d32;border-color:#43a047;}"
                ".status-box.green .label{color:#c8e6c9;}"
                ".status-box.red{background:#c62828;border-color:#e53935;}"
                ".status-box.red .label{color:#ffcdd2;}"
                "form.settings{display:block;text-align:left;margin:4px auto 0 auto;width:100%;max-width:400px;"
                "box-sizing:border-box;}"
                "form.settings label{display:block;margin:4px 0;}"
                ".mode-toggle{display:flex;gap:8px;margin:4px 0;}"
                ".mode-toggle input{display:none;}"
                ".mode-toggle label{flex:1;text-align:center;padding:10px 8px;border:1px solid #666;"
                "border-radius:4px;cursor:pointer;background:#3a3a3a;margin:0;}"
                ".mode-toggle label small{display:block;color:#999;font-weight:400;font-size:0.75em;margin-top:2px;}"
                ".mode-toggle input:checked+label{background:#4a90d9;border-color:#4a90d9;color:#fff;}"
                ".mode-toggle input:checked+label small{color:#dceaf9;}"
                ".mode-toggle input:disabled+label{background:#333;color:#666;border-color:#444;"
                "cursor:not-allowed;}"
                ".aux-grid{display:flex;flex-wrap:wrap;gap:8px;margin:4px 0;}"
                ".aux-grid input{display:none;}"
                ".aux-grid label{flex:0 0 calc(25% - 6px);text-align:center;padding:8px 4px;border:1px solid #666;"
                "border-radius:4px;cursor:pointer;background:#3a3a3a;margin:0;box-sizing:border-box;}"
                ".aux-grid input:checked+label{background:#4a90d9;border-color:#4a90d9;color:#fff;}"
                ".osd-grid{display:grid;grid-template-columns:1fr 1fr;gap:4px 8px;}"
                ".osd-grid label{margin:4px 0;}"
                ".osd-grid select{width:100%;box-sizing:border-box;}"
                "select,button{font-size:1em;padding:4px 8px;}"
                "button{background:#4a4a4a;color:#fff;border:1px solid #666;border-radius:4px;"
                "cursor:pointer;margin-top:8px;}"
                "button:hover{background:#5a5a5a;}"
                "form.settings>button{display:block;width:100%;box-sizing:border-box;padding:10px 8px;}"
                ".camera-row{display:flex;justify-content:space-between;align-items:center;gap:12px;"
                "border:1px solid #444;border-radius:4px;padding:6px 10px;margin:4px 0;min-width:260px;}"
                ".camera-row button{margin:0;padding:2px 10px;background:#6b2b2b;}"
                ".camera-row button:hover{background:#853636;}"
                ".footer{color:#777;font-size:0.8em;margin-top:16px;}"
                "</style></head><body>";

        bool ru = _settings && _settings->language == UiLanguage::RU;
        auto T = [ru](const char *en, const char *rus) { return String(ru ? rus : en); };

        html += "<img class='logo' src='/logo.svg' alt='AIR SQAD'>";
        html += "<h1>AIRSQAD Cam Linker</h1><hr>";
        html += "<div class='version'>Version: " + String(FIRMWARE_VERSION) + "</div>";

        char batteryStr[8];
        if (_lastStatus.batteryPercent >= 0) {
            snprintf(batteryStr, sizeof(batteryStr), "%d%%", _lastStatus.batteryPercent);
        } else {
            snprintf(batteryStr, sizeof(batteryStr), "-");
        }

        bool sdOk = String(_lastStatus.sdStatus) == "OK";

        int battPct = _lastStatus.batteryPercent;
        if (battPct < 0) battPct = 0;
        if (battPct > 100) battPct = 100;
        int battHue = (battPct * 120) / 100;
        String battStyle = (_lastStatus.batteryPercent >= 0)
            ? " style='background:hsl(" + String(battHue) + ",70%,32%);border-color:hsl(" +
              String(battHue) + ",70%,45%)'"
            : "";

        html += "<h2>" + T("Status", "Статус") + "</h2>";
        html += "<div class='status-grid'>";
        html += "<div class='status-box" + String(_lastStatus.connected ? " green" : "") + "'><div class='label'>" +
                T("Connection", "Подключение") + "</div><div class='value'>" +
                (_lastStatus.connected ? T("Connected", "Есть связь") : T("No signal", "Нет связи")) +
                "</div></div>";
        html += "<div class='status-box" + String(_lastStatus.recording ? " red" : "") + "'><div class='label'>" +
                T("Recording", "Запись") + "</div><div class='value'>" +
                (_lastStatus.recording ? T("REC", "ИДЁТ") : T("Idle", "Нет")) + "</div></div>";
        html += "<div class='status-box'" + battStyle + "><div class='label'>" + T("Battery", "Батарея") +
                "</div><div class='value'>" + String(batteryStr) + "</div></div>";
        html += "<div class='status-box" + String(sdOk ? " green" : " red") + "'><div class='label'>" +
                T("SD card", "SD-карта") + "</div><div class='value'>" + String(_lastStatus.sdStatus) +
                "</div></div>";
        html += "</div>";

        bool isAir = (!_settings || _settings->triggerMode == RecTriggerMode::AIR);
        uint8_t currentAux = _settings ? _settings->triggerAuxChannel : 1;
        bool stopOnDisarm = !_settings || _settings->stopOnDisarm;
        CameraType currentCamType = _settings ? _settings->cameraType : CameraType::GOPRO;
        bool autoPowerOn = !_settings || _settings->autoPowerOnCamera;

        html += "<form class='settings' method='POST' action='/save'>";

        html += "<h2>" + T("Camera", "Камера") + "</h2>";
        html += "<div class='mode-toggle'>";
        html += "<input type='radio' id='camGoPro' name='cameraType' value='0'" +
                String(currentCamType == CameraType::GOPRO ? " checked" : "") + "><label for='camGoPro'>GoPro</label>";
        html += "<input type='radio' id='camDji' name='cameraType' value='1' disabled>"
                "<label for='camDji'>DJI</label>";
        html += "<input type='radio' id='camInsta' name='cameraType' value='2' disabled>"
                "<label for='camInsta'>Insta360</label>";
        html += "</div>";
        html += "<label><input type='checkbox' name='autoPowerOnCamera'" + String(autoPowerOn ? " checked" : "") +
                "> " + T("Auto power-on camera on boot", "Автовключение камеры при старте") + "</label>";

        html += "<h2>" + T("Recording trigger", "Запуск записи") + "</h2>";
        html += "<div class='mode-toggle'>";
        html += "<input type='radio' id='modeAir' name='mode' value='AIR'" +
                String(isAir ? " checked" : "") + "><label for='modeAir'>AIR<br><small>" +
                T("record on arm", "запись при arm") + "</small></label>";
        html += "<input type='radio' id='modeSwitch' name='mode' value='SWITCH'" +
                String(!isAir ? " checked" : "") + "><label for='modeSwitch'>SWITCH<br><small>" +
                T("via AUX channel", "по каналу AUX") + "</small></label>";
        html += "</div>";
        html += "<div>" + T("Recording trigger channel:", "Канал запуска записи:") + "</div>";
        html += "<div class='aux-grid'>";
        for (uint8_t i = 1; i <= 8; i++) {
            String id = "aux" + String(i);
            html += "<input type='radio' id='" + id + "' name='aux' value='" + String(i) + "'" +
                    (i == currentAux ? " checked" : "") + "><label for='" + id + "'>AUX" + String(i) + "</label>";
        }
        html += "</div>";
        html += "<label><input type='checkbox' name='stopOnDisarm'" + String(stopOnDisarm ? " checked" : "") +
                "> " + T("Stop recording on disarm", "Остановить запись при disarm") + "</label>";

        html += "<h2>" + T("OSD fields (Custom Message 1-4)", "Поля OSD (Custom Message 1-4)") + "</h2>";
        html += "<div class='osd-grid'>";
        for (uint8_t slot = 0; slot < Settings::OSD_SLOT_COUNT; slot++) {
            OsdField current = _settings ? _settings->osdSlotField[slot] : OsdField::NONE;
            html += "<label>Custom Message " + String(slot + 1) + ":<br>" +
                    osdFieldSelect(slot, current, ru) + "</label>";
        }
        html += "</div>";

        bool ledEnabled = !_settings || _settings->ledEnabled;
        html += "<h2>" + T("LED indicator", "Индикация") + "</h2>";
        html += "<label><input type='checkbox' name='ledEnabled'" + String(ledEnabled ? " checked" : "") +
                "> " + T("LED indicator enabled", "Индикация светодиодом включена") + "</label>";

        html += "<h2>" + T("Language", "Язык") + "</h2>";
        html += "<div class='mode-toggle'>";
        html += "<input type='radio' id='langEn' name='uiLang' value='en'" + String(!ru ? " checked" : "") +
                "><label for='langEn'>English</label>";
        html += "<input type='radio' id='langRu' name='uiLang' value='ru'" + String(ru ? " checked" : "") +
                "><label for='langRu'>Русский</label>";
        html += "</div>";

        html += "<button type='submit'>" + T("Save", "Сохранить") + "</button>";
        html += "</form>";

        html += "<h2>" + T("Paired cameras", "Привязанные камеры") + "</h2>";
        std::vector<BondedCamera> cameras = BondedCameras::list();
        if (cameras.empty()) {
            html += "<p>" + T("No paired cameras.", "Нет привязанных камер.") + "</p>";
        } else {
            for (const auto &cam : cameras) {
                html += "<div class='camera-row'><span>" + String(cam.name.c_str()) + "</span>";
                html += "<form method='POST' action='/forget-camera' style='margin:0;'>";
                html += "<input type='hidden' name='address' value='" + String(cam.address.c_str()) + "'>";
                html += "<button type='submit'>" + T("Remove", "Удалить") + "</button>";
                html += "</form></div>";
            }
        }

        html += "<div class='footer'>" +
                (ru ? "Портал отключится через " + String(AP_DURATION_MS / 1000) + " с после включения платы."
                    : "Portal will turn off " + String(AP_DURATION_MS / 1000) + "s after power-on.") +
                "</div>";
        html += "</body></html>";

        _server.send(200, "text/html; charset=utf-8", html);
    }
};
