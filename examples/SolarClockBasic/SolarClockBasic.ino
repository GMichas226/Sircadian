// SolarClockBasic -- an RTC-less, network-less wall clock.
//
// Wiring: a photoresistor divider on LDR_PIN, placed where it sees daylight
// (a windowsill is fine). Nothing else.
//
// Seed the clock once by sending the current Unix time in milliseconds over
// the serial monitor (one line, digits only, e.g. 1783468800000). From then
// on the clock free-runs: it logs light once a minute, fits each finished
// day against the astronomical clear-sky curve for your site, and corrects
// its own drift from the result. Clear days teach it; cloudy days are
// rejected automatically.
//
// RAM note: the day buffer is ~2.9 KB, so this needs an ESP8266/ESP32-class
// board (or any MCU with >= 8 KB RAM). A classic Uno is too small.

#include <Sircadian.h>

using namespace solar;

const int LDR_PIN = A0;

Sircadian sirc(sircadianMillis64);

void setup() {
    Serial.begin(115200);

    SircadianConfig cfg;
    // Your site. lat/lon in degrees (+N/+E), tz = UTC offset in hours,
    // atmK = atmospheric sharpness exponent (1.6 is a good default;
    // calibrate to your sensor if fits are rejected on clear days).
    cfg.site = SolarParams{ 38.0f, 23.7f, 2.0f, 1.6f };
    sirc.begin(cfg);

    Serial.println(F("send Unix epoch in ms to seed the clock"));
}

void loop() {
    // One-time (or occasional) absolute seed over serial.
    if (Serial.available()) {
        long long epoch = Serial.parseInt();
        if (epoch > 1000000000000LL) {
            sirc.setEpochMs((int64_t)epoch);
            Serial.println(F("seeded"));
        }
    }

    // One light sample per minute; the facade files it under the clock's
    // own minute-of-day and averages duplicates within a minute.
    static uint32_t lastSample = 0;
    if (millis() - lastSample >= 60000UL) {
        lastSample += 60000UL;
        sirc.addLightSample((uint16_t)analogRead(LDR_PIN));
    }

    // At local midnight this fits the finished day and disciplines the
    // clock; returns true when a fit was attempted.
    if (sirc.service()) {
        Serial.print(F("fit "));
        Serial.print(sirc.lastFit().accepted ? F("accepted, shift ")
                                             : F("rejected"));
        if (sirc.lastFit().accepted) Serial.print(sirc.lastFit().shiftMin);
        Serial.print(F("  applied ppm "));
        Serial.println(sirc.appliedPpm());
    }

    // Use the time.
    static uint32_t lastPrint = 0;
    if (sirc.isSynced() && millis() - lastPrint >= 10000UL) {
        lastPrint = millis();
        int64_t ms = sirc.epochMs();
        int32_t sod = (int32_t)((ms / 1000 + 2 * 3600) % 86400);  // UTC+2
        char buf[16];
        snprintf(buf, sizeof buf, "%02d:%02d:%02d",
                 (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
        Serial.println(buf);
    }
}
