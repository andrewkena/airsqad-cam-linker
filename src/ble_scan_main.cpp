#include <Arduino.h>
#include <NimBLEDevice.h>

// ---------------------------------------------------------------------
// Временный диагностический инструмент (env "ble-scan" в platformio.ini,
// не часть основной прошивки моста).
//
// Постоянно сканирует эфир и печатает в Serial все найденные BLE-устройства:
// имя, MAC-адрес, RSSI и рекламируемые сервисы/manufacturer data. Нужен,
// чтобы точно узнать, что рекламирует конкретная камера (Insta360, DJI и
// т.д.), вместо того чтобы полагаться на UUID из чужого реверс-инжиниринга.
//
// Прошивка:   pio run -e ble-scan -t upload --upload-port COMx
// Просмотр:   pio device monitor --port COMx --baud 115200
// ---------------------------------------------------------------------

static constexpr uint32_t SCAN_TIME_SEC = 5;

void printDevice(NimBLEAdvertisedDevice dev) {
    Serial.print("--- ");
    Serial.print(dev.getAddress().toString().c_str());
    Serial.print("  RSSI=");
    Serial.print(dev.getRSSI());
    Serial.println();

    if (dev.haveName()) {
        Serial.print("    name: ");
        Serial.println(dev.getName().c_str());
    }

    if (dev.haveServiceUUID()) {
        uint8_t count = dev.getServiceUUIDCount();
        for (uint8_t i = 0; i < count; i++) {
            Serial.print("    service: ");
            Serial.println(dev.getServiceUUID(i).toString().c_str());
        }
    }

    if (dev.haveManufacturerData()) {
        std::string data = dev.getManufacturerData();
        Serial.print("    manufacturerData (");
        Serial.print(data.length());
        Serial.print(" bytes): ");
        for (size_t i = 0; i < data.length(); i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", (uint8_t)data[i]);
            Serial.print(buf);
        }
        Serial.println();
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== BLE scan diagnostic tool ===");

    NimBLEDevice::init("BLE-Scanner");
}

void loop() {
    Serial.println();
    Serial.println("Scanning...");

    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    NimBLEScanResults results = pScan->start(SCAN_TIME_SEC, false);

    Serial.print("Found ");
    Serial.print(results.getCount());
    Serial.println(" device(s):");

    for (int i = 0; i < results.getCount(); i++) {
        printDevice(results.getDevice(i));
    }

    pScan->clearResults();
    delay(1000);
}
