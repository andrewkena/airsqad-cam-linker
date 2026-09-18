#include <Arduino.h>
#include <WiFi.h>

// ---------------------------------------------------------------------
// Временный диагностический инструмент (env "wifi-scan" в platformio.ini,
// не часть основной прошивки моста).
//
// Сканирует Wi-Fi эфир и печатает в Serial все найденные сети: SSID,
// RSSI, тип шифрования, MAC точки доступа. Нужен, чтобы точно узнать имя
// и защиту точки доступа камеры (Insta360 создаёт свою AP в режиме
// 192.168.42.1:6666 по протоколу из doc/protocol.md).
//
// Прошивка:   pio run -e wifi-scan -t upload --upload-port COMx
// Просмотр:   pio device monitor --port COMx --baud 115200
// ---------------------------------------------------------------------

const char *encTypeToStr(wifi_auth_mode_t type) {
    switch (type) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
        default: return "UNKNOWN";
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== WiFi scan diagnostic tool ===");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
}

void loop() {
    Serial.println();
    Serial.println("Scanning WiFi...");

    int n = WiFi.scanNetworks();
    Serial.print("Found ");
    Serial.print(n);
    Serial.println(" network(s):");

    for (int i = 0; i < n; i++) {
        Serial.print("--- SSID: '");
        Serial.print(WiFi.SSID(i));
        Serial.print("'  RSSI=");
        Serial.print(WiFi.RSSI(i));
        Serial.print("  enc=");
        Serial.print(encTypeToStr(WiFi.encryptionType(i)));
        Serial.print("  BSSID=");
        Serial.println(WiFi.BSSIDstr(i));
    }

    WiFi.scanDelete();
    delay(2000);
}
