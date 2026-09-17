#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include "gopro_ble.h"

// ---------------------------------------------------------------------
// Временный Wi-Fi портал состояния/настроек.
//
// На AP_DURATION_MS миллисекунд после старта поднимает открытую точку
// доступа с фиксированным IP 10.0.0.1, где отдаёт страницу с текущим
// статусом GoPro. Затем сам выключает Wi-Fi, чтобы не мешать BLE/UART
// в обычном режиме работы моста.
// ---------------------------------------------------------------------

class StatusPortal {
public:
    static constexpr uint32_t AP_DURATION_MS = 30000;

    void begin(const char *ssid, const char *password = nullptr) {
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
    WebServer _server{80};
    uint32_t _startMs = 0;
    bool _active = false;
    GoProBle::Status _lastStatus;

    void stop() {
        _server.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        _active = false;
    }

    void handleRoot() {
        char batteryStr[8];
        if (_lastStatus.batteryPercent >= 0) {
            snprintf(batteryStr, sizeof(batteryStr), "%d%%", _lastStatus.batteryPercent);
        } else {
            snprintf(batteryStr, sizeof(batteryStr), "-");
        }

        char html[900];
        snprintf(html, sizeof(html),
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width, initial-scale=1'>"
            "<title>GoPro OSD Bridge</title></head><body>"
            "<h1>GoPro &lt;-&gt; Betaflight OSD Bridge</h1>"
            "<h2>Статус</h2>"
            "<ul>"
            "<li>GoPro подключена: %s</li>"
            "<li>Идёт запись: %s</li>"
            "<li>Заряд батареи: %s</li>"
            "<li>Статус SD-карты: %u</li>"
            "</ul>"
            "<h2>Настройки</h2>"
            "<p>Пока недоступны, появятся в следующих версиях.</p>"
            "<p><small>Портал отключится через %lu с после включения платы.</small></p>"
            "</body></html>",
            _lastStatus.connected ? "да" : "нет",
            _lastStatus.recording ? "да" : "нет",
            batteryStr,
            _lastStatus.sdStatus,
            (unsigned long)(AP_DURATION_MS / 1000)
        );
        _server.send(200, "text/html; charset=utf-8", html);
    }
};
