// RuntimeConfig -- everything SircadianConfig lets you set at runtime.
//
// Every field defaults to the compile-time macro in src/config.h (all
// #ifndef-guarded, so -DSIRCADIAN_LAT=51.5f etc. still works for fully
// static builds); this sketch shows the runtime path with the defaults
// spelled out. Only override what you need.

#include <Sircadian.h>

using namespace solar;

Sircadian sirc(sircadianMillis64);

void setup() {
    Serial.begin(115200);

    SircadianConfig cfg;

    // --- site (required in practice) ------------------------------------
    cfg.site.lat  = 38.0f;    // degrees, +N
    cfg.site.lon  = 23.7f;    // degrees, +E
    cfg.site.tz   = 2.0f;     // UTC offset, hours
    cfg.site.atmK = 1.6f;     // clear-sky sharpness; calibrate to sensor

    // --- fit accept gates (defaults are sensible) ------------------------
    cfg.fit.minUsedPoints   = 360;     // >= 6 h of daylit samples
    cfg.fit.maxRmsOverAlpha = 0.20f;   // normalized residual ceiling
    cfg.fit.minAlpha        = 50.0f;   // reject flat/dark days

    // --- discipline loop gains -------------------------------------------
    cfg.discipline.offsetGain = 0.1f;  // Kp: fraction of shift applied
    cfg.discipline.rateGain   = 0.1f;  // Kr: rate EMA gain
    cfg.maxPpmStep            = 200;   // per-fit rate step clamp

    // --- light-sensor temperature correction (off unless enabled) --------
    cfg.lightCompEnabled = false;      // needs setTemperature() before samples
    cfg.lightTempco      = 0.0f;       // frac/degC, bench-calibrated per unit
    cfg.lightTempRefC    = 20.0f;      // temperature the calibration was taken at
    cfg.lightDarkAdc     = 0.0f;       // dark/offset ADC counts

    sirc.begin(cfg);
    Serial.println(F("configured; seed with Unix ms over serial"));
}

void loop() {
    if (Serial.available()) {
        long long epoch = Serial.parseInt();
        if (epoch > 1000000000000LL) sirc.setEpochMs((int64_t)epoch);
    }
    static uint32_t lastSample = 0;
    if (millis() - lastSample >= 60000UL) {
        lastSample += 60000UL;
        sirc.addLightSample((uint16_t)analogRead(A0));
    }
    sirc.service();
}
