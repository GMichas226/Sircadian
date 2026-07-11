#include "Sircadian.h"

namespace solar {

static inline int wrapDoy(int d) { return ((d - 1 + 366) % 366) + 1; }

Sircadian::Sircadian(DriftClock::MonotonicMsFn nowMs)
    : now_(nowMs), clk_(nowMs), disc_(DisciplineConfig{}, SIRCADIAN_MAX_PPM_STEP) {
    clearDay();
}

void Sircadian::begin(const SircadianConfig& cfg) {
    cfg_  = cfg;
    clk_  = DriftClock(now_);
    disc_ = Discipline(cfg_.discipline, cfg_.maxPpmStep);
    lastFit_     = FitResult{};
    lastMinute_  = -1;
    bufferDoy_   = -1;
    haveTemp_    = false;
    lastTempC_   = 0.0f;
    clearDay();
}

void Sircadian::clearDay() {
    for (int m = 0; m < SIRCADIAN_MINUTES_PER_DAY; ++m) obs_[m] = NO_DATA;
    minuteSum_ = 0; minuteCount_ = 0; minuteSlot_ = -1;
}

void Sircadian::setEpochMs(int64_t epochMs) {
    clk_.setEpochMs(epochMs);
}

void Sircadian::setTemperature(float tC) {
    lastTempC_ = tC;
    haveTemp_  = true;
}

// -- light input --------------------------------------------------------------

uint16_t Sircadian::correctLightSample(uint16_t adc, float tC) const {
    float g = 1.0f + cfg_.lightTempco * (tC - cfg_.lightTempRefC);
    if (g < 0.05f) g = 0.05f;             // same floor as a real gain can hit
    float corrected = cfg_.lightDarkAdc + ((float)adc - cfg_.lightDarkAdc) / g;
    if (corrected < 0.0f) return 0;
    if (corrected >= 65534.0f) return 65534;   // never NO_DATA (0xFFFF)
    return (uint16_t)(corrected + 0.5f);
}

void Sircadian::flushMinute() {
    if (minuteSlot_ >= 0 && minuteCount_ > 0)
        obs_[minuteSlot_] = (uint16_t)(minuteSum_ / minuteCount_);
    minuteSum_ = 0; minuteCount_ = 0; minuteSlot_ = -1;
}

void Sircadian::addLightSample(uint16_t adc) {
    int16_t m = clk_.localMinuteOfDay(cfg_.site.tz);
    if (m < 0) return;                    // unsynced: no slot to file under
    if (cfg_.lightCompEnabled && haveTemp_)
        adc = correctLightSample(adc, lastTempC_);
    if (m != minuteSlot_) flushMinute();
    minuteSlot_ = m;
    minuteSum_ += adc;
    ++minuteCount_;
    bufferDoy_ = clk_.dayOfYear(cfg_.site.tz);
}

void Sircadian::addLightSample(int minuteOfDay, uint16_t adc) {
    if (minuteOfDay < 0 || minuteOfDay >= SIRCADIAN_MINUTES_PER_DAY) return;
    if (cfg_.lightCompEnabled && haveTemp_)
        adc = correctLightSample(adc, lastTempC_);
    obs_[minuteOfDay] = adc;
}

// -- fitting ------------------------------------------------------------------

void Sircadian::applyFit(const FitResult& r, int64_t reportedMs) {
    if (!r.accepted) return;
    // The shared discipline loop: rate EMA + fractional offset re-anchor.
    disc_.onAcceptedFit(clk_, r, reportedMs);
}

FitResult Sircadian::runFitNow() {
    if (!clk_.isSynced()) { lastFit_ = FitResult{}; return lastFit_; }
    flushMinute();
    int64_t reported = clk_.epochMs();
    int doy = clk_.dayOfYear(cfg_.site.tz);
    int doys[3] = { wrapDoy(doy - 1), wrapDoy(doy), wrapDoy(doy + 1) };
    lastFit_ = fitBestOfDays(doys, 3, obs_, NO_DATA, cfg_.site, cfg_.fit);
    applyFit(lastFit_, reported);
    return lastFit_;
}

bool Sircadian::service() {
    if (!clk_.isSynced()) return false;
    int16_t m = clk_.localMinuteOfDay(cfg_.site.tz);
    if (m < 0) return false;
    bool rolled = (lastMinute_ >= 0 && m < lastMinute_);
    lastMinute_ = m;
    if (!rolled) return false;

    // The buffer holds the day that just ended; fit against ITS calendar day
    // (captured at sample time), not the new day's.
    flushMinute();
    int64_t reported = clk_.epochMs();
    int doy = (bufferDoy_ > 0) ? bufferDoy_ : clk_.dayOfYear(cfg_.site.tz);
    int doys[3] = { wrapDoy(doy - 1), wrapDoy(doy), wrapDoy(doy + 1) };
    lastFit_ = fitBestOfDays(doys, 3, obs_, NO_DATA, cfg_.site, cfg_.fit);
    applyFit(lastFit_, reported);
    clearDay();
    bufferDoy_ = -1;
    return true;
}

// -- persistence ----------------------------------------------------------------

void Sircadian::exportState(float out[STATE_FLOATS]) const {
    out[0] = (float)clk_.driftPpm();
}

void Sircadian::importState(const float in[STATE_FLOATS]) {
    clk_.setDriftPpm((int32_t)in[0]);
}

}  // namespace solar
