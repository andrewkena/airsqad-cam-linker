#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include "gopro_ble.h"
#include "settings.h"
#include "bonded_cameras.h"

// ---------------------------------------------------------------------
// Временный Wi-Fi портал состояния/настроек.
//
// На AP_DURATION_MS миллисекунд после старта поднимает открытую точку
// доступа с фиксированным IP 10.0.0.1, где отдаёт страницу с текущим
// статусом GoPro, настройками триггера записи и списком привязанных
// камер. Затем сам выключает Wi-Fi, чтобы не мешать BLE/UART в обычном
// режиме работы моста.
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
        _server.on("/save", HTTP_POST, [this]() { handleSaveSettings(); });
        _server.on("/forget-camera", HTTP_POST, [this]() { handleForgetCamera(); });
        _server.onNotFound([this]() { handleRoot(); });
        _server.begin();
    }

    // Вызывать из loop(). Возвращает true, пока портал активен.
    bool loop(const GoProBle::Status &goProStatus) {
        if (!_active) return false;

        _lastStatus = goProStatus;
        _server.handleClient();

        if (millis() - _startMs > AP_DURATION_MS) {
            stop();
        }
        return _active;
    }

private:
    static constexpr const char *FIRMWARE_VERSION = "0.1";

    WebServer _server{80};
    uint32_t _startMs = 0;
    bool _active = false;
    GoProBle::Status _lastStatus;
    Settings *_settings = nullptr;

    void stop() {
        _server.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        _active = false;
    }

    void handleSaveSettings() {
        if (_settings) {
            String mode = _server.arg("mode");
            _settings->triggerMode = (mode == "SWITCH") ? RecTriggerMode::SWITCH : RecTriggerMode::AIR;

            int aux = _server.arg("aux").toInt();
            if (aux < 1) aux = 1;
            if (aux > 8) aux = 8;
            _settings->triggerAuxChannel = (uint8_t)aux;

            _settings->stopOnDisarm = _server.hasArg("stopOnDisarm");
            _settings->save();
        }
        _server.sendHeader("Location", "/");
        _server.send(303);
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
        html.reserve(4096);

        html += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>AIR SQAD Cam Linker</title><style>"
                "body{background:#2b2b2b;color:#e0e0e0;font-family:-apple-system,'Segoe UI',Roboto,sans-serif;"
                "text-align:center;margin:0;padding:40px 20px;}"
                "h1{font-size:1.6em;font-weight:600;margin:0 0 16px 0;}"
                "hr{border:none;border-top:1px solid #555;width:200px;margin:0 auto 16px auto;}"
                ".version{color:#999;font-size:0.9em;margin-bottom:32px;}"
                "h2{font-size:1.1em;color:#ccc;margin-top:32px;}"
                "ul{list-style:none;padding:0;display:inline-block;text-align:left;}"
                "li{margin:6px 0;}"
                "form.settings{display:inline-block;text-align:left;margin-top:8px;}"
                "form.settings label{display:block;margin:8px 0;}"
                "select,button{font-size:1em;padding:4px 8px;}"
                "button{background:#4a4a4a;color:#fff;border:1px solid #666;border-radius:4px;"
                "cursor:pointer;margin-top:14px;}"
                "button:hover{background:#5a5a5a;}"
                ".camera-row{display:flex;justify-content:space-between;align-items:center;gap:12px;"
                "border:1px solid #444;border-radius:4px;padding:6px 10px;margin:6px 0;min-width:260px;}"
                ".camera-row button{margin:0;padding:2px 10px;background:#6b2b2b;}"
                ".camera-row button:hover{background:#853636;}"
                ".footer{color:#777;font-size:0.8em;margin-top:32px;}"
                "</style></head><body>";

        html += "<h1>AIR SQAD Cam Linker</h1><hr>";
        html += "<div class='version'>Version: " + String(FIRMWARE_VERSION) + "</div>";

        char batteryStr[8];
        if (_lastStatus.batteryPercent >= 0) {
            snprintf(batteryStr, sizeof(batteryStr), "%d%%", _lastStatus.batteryPercent);
        } else {
            snprintf(batteryStr, sizeof(batteryStr), "-");
        }

        html += "<h2>Статус</h2><ul>";
        html += "<li>GoPro подключена: " + String(_lastStatus.connected ? "да" : "нет") + "</li>";
        html += "<li>Идёт запись: " + String(_lastStatus.recording ? "да" : "нет") + "</li>";
        html += "<li>Заряд батареи: " + String(batteryStr) + "</li>";
        html += "<li>Статус SD-карты: " + String(_lastStatus.sdStatus) + "</li>";
        html += "</ul>";

        bool isAir = (!_settings || _settings->triggerMode == RecTriggerMode::AIR);
        uint8_t currentAux = _settings ? _settings->triggerAuxChannel : 1;
        bool stopOnDisarm = !_settings || _settings->stopOnDisarm;

        html += "<h2>Запуск записи</h2>";
        html += "<form class='settings' method='POST' action='/save'>";
        html += "<label><input type='radio' name='mode' value='AIR'" + String(isAir ? " checked" : "") +
                "> AIR (запись при arm)</label>";
        html += "<label><input type='radio' name='mode' value='SWITCH'" + String(!isAir ? " checked" : "") +
                "> SWITCH (по каналу AUX)</label>";
        html += "<label>Канал запуска записи:<br><select name='aux'>";
        for (uint8_t i = 1; i <= 8; i++) {
            html += "<option value='" + String(i) + "'" + (i == currentAux ? " selected" : "") +
                    ">AUX" + String(i) + "</option>";
        }
        html += "</select></label>";
        html += "<label><input type='checkbox' name='stopOnDisarm'" + String(stopOnDisarm ? " checked" : "") +
                "> Stop Video on Disarm</label>";
        html += "<button type='submit'>Сохранить</button>";
        html += "</form>";

        html += "<h2>Привязанные камеры</h2>";
        std::vector<BondedCamera> cameras = BondedCameras::list();
        if (cameras.empty()) {
            html += "<p>Нет привязанных камер.</p>";
        } else {
            for (const auto &cam : cameras) {
                html += "<div class='camera-row'><span>" + String(cam.name.c_str()) + "</span>";
                html += "<form method='POST' action='/forget-camera' style='margin:0;'>";
                html += "<input type='hidden' name='address' value='" + String(cam.address.c_str()) + "'>";
                html += "<button type='submit'>Удалить</button>";
                html += "</form></div>";
            }
        }

        html += "<h2>Настройки</h2>";
        html += "<p>Остальные настройки появятся в следующих версиях.</p>";
        html += "<div class='footer'>Портал отключится через " + String(AP_DURATION_MS / 1000) +
                " с после включения платы.</div>";
        html += "</body></html>";

        _server.send(200, "text/html; charset=utf-8", html);
    }
};
