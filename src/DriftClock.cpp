#include "DriftClock.h"
#include <ctime>

namespace solar {

void DriftClock::setEpochMs(int64_t epochMs) {
    epochAtSet_ = epochMs;
    localAtSet_ = (int64_t)now_();
    synced_     = true;
}

int64_t DriftClock::epochMs() const {
    if (!synced_) return 0;
    int64_t localDt = (int64_t)now_() - localAtSet_;
    // local clock runs at (1 + ppm/1e6) of wall rate.
    int64_t correction = (localDt * (int64_t)driftPpm_) / 1000000LL;
    return epochAtSet_ + localDt + correction;
}

int32_t DriftClock::secondsSinceSync() const {
    if (!synced_) return 0;
    int64_t dtMs = (int64_t)now_() - localAtSet_;
    if (dtMs < 0) return 0;
    return (int32_t)(dtMs / 1000);
}

int32_t DriftClock::localSecondOfDay(float tzHours) const {
    int64_t epMs = epochMs();
    if (epMs == 0) return -1;
    int64_t tzSec    = (int64_t)(tzHours * 3600.0f);
    int64_t localSec = epMs / 1000 + tzSec;
    int32_t sod = (int32_t)(localSec % 86400LL);
    if (sod < 0) sod += 86400;
    return sod;
}

int16_t DriftClock::localMinuteOfDay(float tzHours) const {
    int32_t s = localSecondOfDay(tzHours);
    if (s < 0) return -1;
    return (int16_t)(s / 60);
}

int16_t DriftClock::dayOfYear(float tzHours) const {
    int64_t epMs = epochMs();
    if (epMs == 0) return -1;
    time_t local = (time_t)(epMs / 1000 + (int64_t)(tzHours * 3600.0f));
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &local);
#else
    gmtime_r(&local, &tmv);
#endif
    return (int16_t)(tmv.tm_yday + 1);
}

}  // namespace solar
