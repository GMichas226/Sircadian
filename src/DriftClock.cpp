#include "DriftClock.h"

namespace solar {

// Day-of-year (1..366) for a count of civil days since 1970-01-01 (proleptic
// Gregorian). Arithmetic only -- no <ctime>, so it is identical on every
// toolchain and needs no gmtime_r/gmtime_s split. Date algebra follows
// Howard Hinnant's civil_from_days.
static int16_t doyFromDays(int64_t z) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                    // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t dma = doe - (365 * yoe + yoe / 4 - yoe / 100);             // day, March-based year
    int64_t mp  = (5 * dma + 2) / 153;                                 // month, March=0
    int64_t d   = dma - (153 * mp + 2) / 5 + 1;                        // day of month
    int64_t m   = mp + (mp < 10 ? 3 : -9);                             // month 1..12
    int64_t y   = yoe + era * 400 + (m <= 2 ? 1 : 0);
    static const int16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181,
                                     212, 243, 273, 304, 334 };
    bool leap = (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0));
    return (int16_t)(cum[m - 1] + d + ((leap && m > 2) ? 1 : 0));
}

void DriftClock::setEpochMs(int64_t epochMs) {
    epochAtSet_  = epochMs;
    localAtSet_  = (int64_t)now_();
    localAtSync_ = localAtSet_;
    synced_      = true;
}

int64_t DriftClock::epochMs() const {
    if (!synced_) return 0;
    int64_t localDt = (int64_t)now_() - localAtSet_;
    // local clock runs at (1 + ppm/1e6) of wall rate.
    int64_t correction = (localDt * (int64_t)driftPpm_) / 1000000LL;
    return epochAtSet_ + localDt + correction;
}

void DriftClock::slewRatePpm(int32_t ppm) {
    if (!synced_) { driftPpm_ = ppm; return; }
    // Unchanged rate: keep the current anchor. Every fold below truncates
    // the accrued correction to whole ms, so re-anchoring once a minute at
    // an unchanged rate would systematically shed up to ~1 ms per fold
    // (~1 s/day of phantom drift); folding only on actual rate changes
    // keeps that loss to a handful of ms per day.
    if (ppm == driftPpm_) return;
    // Fold everything accrued under the old rate into the anchor, then start
    // the new rate from here: piecewise-constant rate integration.
    epochAtSet_ = epochMs();
    localAtSet_ = (int64_t)now_();
    driftPpm_   = ppm;
}

int32_t DriftClock::secondsSinceSync() const {
    if (!synced_) return 0;
    int64_t dtMs = (int64_t)now_() - localAtSync_;
    if (dtMs < 0) return 0;
    return (int32_t)(dtMs / 1000);
}

int32_t DriftClock::localSecondOfDay(float tzHours) const {
    if (!synced_) return -1;
    int64_t epMs = epochMs();
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
    if (!synced_) return -1;
    int64_t localSec = epochMs() / 1000 + (int64_t)(tzHours * 3600.0f);
    int64_t days = localSec / 86400;
    if (localSec < 0 && localSec % 86400 != 0) --days;   // floor toward -inf
    return doyFromDays(days);
}

}  // namespace solar
