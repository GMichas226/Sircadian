#pragma once

// Sircadian build-time configuration. One edit point for deployment values
// and the few internal constants that used to be literals in the source.
//
// Every macro is #ifndef-guarded, so any value can be overridden from the
// build without touching this file, e.g. -DSIRCADIAN_LAT=51.5f.

// ---------------------------------------------------------------------------
// Section A — Site / deployment (edit these for your install)
// ---------------------------------------------------------------------------

// Observer location and sensor response (see SolarParams in SolarModel.h).
#ifndef SIRCADIAN_LAT
#define SIRCADIAN_LAT  38.0f    // latitude, deg N
#endif
#ifndef SIRCADIAN_LON
#define SIRCADIAN_LON  23.7f    // longitude, deg E
#endif
#ifndef SIRCADIAN_TZ
#define SIRCADIAN_TZ   2.0f     // UTC offset, hours
#endif
#ifndef SIRCADIAN_ATMK
#define SIRCADIAN_ATMK 1.6f     // attenuation exponent; calibrate to the sensor
#endif

// Drop-in SolarParams initializer for the configured site.
#ifndef SIRCADIAN_SITE_PARAMS
#define SIRCADIAN_SITE_PARAMS \
    (solar::SolarParams{ SIRCADIAN_LAT, SIRCADIAN_LON, SIRCADIAN_TZ, SIRCADIAN_ATMK })
#endif

// Fit accept thresholds and search bounds (defaults for FitConfig; see the
// field comments in SolarFit.h for what each one gates).
#ifndef SIRCADIAN_COST_HALF_WINDOW_MIN
#define SIRCADIAN_COST_HALF_WINDOW_MIN  300
#endif
// The tau search runs +/-720 min, but effective shift observability is bounded
// by the cost window (SIRCADIAN_COST_HALF_WINDOW_MIN, +/-300 min around
// predicted noon): once the true peak drifts outside the window, support
// degrades. The wide search range is a bound, not an accuracy guarantee --
// it suits the design's minute-scale drift, not half-day offsets.
#ifndef SIRCADIAN_SHIFT_RANGE_MIN
#define SIRCADIAN_SHIFT_RANGE_MIN       720
#endif
#ifndef SIRCADIAN_MIN_USED_POINTS
#define SIRCADIAN_MIN_USED_POINTS       360
#endif
#ifndef SIRCADIAN_FULL_CURVE_MIN_POINTS
#define SIRCADIAN_FULL_CURVE_MIN_POINTS 540
#endif
#ifndef SIRCADIAN_MAX_RMS_OVER_ALPHA
#define SIRCADIAN_MAX_RMS_OVER_ALPHA    0.20f
#endif
#ifndef SIRCADIAN_UNAMBIGUITY_FACTOR
#define SIRCADIAN_UNAMBIGUITY_FACTOR    1.30f
#endif
#ifndef SIRCADIAN_MIN_ALPHA
#define SIRCADIAN_MIN_ALPHA             50.0f
#endif
#ifndef SIRCADIAN_MAX_ALPHA
#define SIRCADIAN_MAX_ALPHA             4096.0f
#endif

// Per-step clamp on the learned drift correction (driftPpmFromShift).
#ifndef SIRCADIAN_MAX_PPM_STEP
#define SIRCADIAN_MAX_PPM_STEP          200
#endif

// ---------------------------------------------------------------------------
// Section B — Internal constants (rarely changed; here to avoid magic numbers)
// ---------------------------------------------------------------------------

// Length of the per-minute observation buffer / theoretical curve.
#ifndef SIRCADIAN_MINUTES_PER_DAY
#define SIRCADIAN_MINUTES_PER_DAY       1440
#endif

// Floor on contributing samples for a shift to score at all (below this a
// window is too sparse to trust; sseAtTau returns DBL_MAX).
#ifndef SIRCADIAN_MIN_FIT_POINTS
#define SIRCADIAN_MIN_FIT_POINTS        60
#endif
