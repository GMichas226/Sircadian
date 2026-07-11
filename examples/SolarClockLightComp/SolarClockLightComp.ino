// SolarClockLightComp -- the basic solar clock plus light-sensor temperature
// correction.
//
// A photoresistor's sensitivity drifts with temperature: its gain is roughly
//     gain(T) ~= 1 + k * (T - Tref).
// Because a room warms through the afternoon, that drift stretches the logged
// light curve asymmetrically and biases the fitted solar noon -- and thus the
// clock. If you know k for your sensor (a one-time bench calibration: log the
// same steady light at two temperatures and solve for k), Sircadian can undo
// it, dividing each sample back to its reference-temperature value before the
// fit sees it. In simulation across a 64-unit, 5-year fleet this cut steady
// clock error from ~137 s to ~114 s -- the dominant win over compensating the
// crystal, at one divide per sample and no extra RAM.
//
// Wiring: photoresistor divider on LDR_PIN + any temperature sensor near the
// sensor. A DS18B20 is shown; substitute your own readTempC().
//
// Seed once over serial with Unix ms, as in SolarClockBasic.

#include <Sircadian.h>
#include <OneWire.h>
#include <DallasTemperature.h>

using namespace solar;

const int LDR_PIN = A0;
const int ONEWIRE_PIN = 4;

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature tempSensor(&oneWire);

Sircadian sirc(sircadianMillis64);

void setup() {
    Serial.begin(115200);
    tempSensor.begin();

    SircadianConfig cfg;
    cfg.site = SolarParams{ 38.0f, 23.7f, 2.0f, 1.6f };

    cfg.lightCompEnabled = true;
    cfg.lightTempco   = 0.002f;   // frac sensitivity change per degC (bench-calibrated)
    cfg.lightTempRefC = 20.0f;    // temperature the calibration was taken at
    cfg.lightDarkAdc  = 0.0f;     // dark/offset ADC counts (measure at night)
    sirc.begin(cfg);

    Serial.println(F("send Unix epoch in ms to seed the clock"));
}

void loop() {
    if (Serial.available()) {
        long long epoch = Serial.parseInt();
        if (epoch > 1000000000000LL) {
            sirc.setEpochMs((int64_t)epoch);
            Serial.println(F("seeded"));
        }
    }

    static uint32_t lastSample = 0;
    if (millis() - lastSample >= 60000UL) {
        lastSample += 60000UL;

        // Feed temperature FIRST so the light sample is corrected on the way in.
        tempSensor.requestTemperatures();
        float tC = tempSensor.getTempCByIndex(0);
        if (tC > -100.0f) sirc.setTemperature(tC);   // skip read errors

        sirc.addLightSample((uint16_t)analogRead(LDR_PIN));
    }

    if (sirc.service()) {
        Serial.print(F("fit "));
        Serial.print(sirc.lastFit().accepted ? F("accepted") : F("rejected"));
        Serial.print(F("  applied ppm "));
        Serial.println(sirc.appliedPpm());
    }

    // Persisting the learned drift across reboots (optional): call
    // sirc.exportState(float[Sircadian::STATE_FLOATS]) now and then, store it
    // in EEPROM/NVS, and importState() after begin() on boot. The epoch itself
    // still needs one absolute seed per boot, as with DriftClock.
}
