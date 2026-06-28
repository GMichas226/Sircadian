// Sircadian deployment simulator (CLI).
//
// A "run" is a fleet of independently-rolled deployments of one configuration;
// this binary simulates ONE deployment per invocation (1 core per run) and the
// desktop GUI launches one process per deployment across the machine. It can
// also run a serial range or the whole fleet standalone.
//
// Reuses src/ (SolarModel, SolarFit, DriftClock) unchanged, so the sim doubles
// as validation of the real firmware.
//
// Build (from repo root):
//   g++ -std=c++17 -O2 -I src -I sim
//       src/SolarModel.cpp src/SolarFit.cpp src/DriftClock.cpp
//       sim/World.cpp sim/Deployment.cpp sim/Engine.cpp sim/sim.cpp
//       -o sim_run -Wall -Wextra -Werror
// Usage:
//   sim_run --dump-defaults run.json          # write an editable default config
//   sim_run --config run.json --deployment 3  # one deployment (GUI launches N)
//   sim_run --config run.json --deployments 0..7 --out runs/x   # serial range
//   sim_run --config run.json                 # whole fleet, serial, + summary.csv

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#define MKDIR(d) mkdir(d, 0777)
#endif

#include "config_io.h"
#include "Engine.h"

using namespace sim;

static const char* SUMMARY_HEADER =
    "index,steady_err_ms,max_err_ms,accept_frac,convergence_day,longest_gap_days,"
    "fits,accepts,lat,lon,atmK,base_cloud,season_amp,winter_peak_doy,t_mean_c,t_season_amp_c,"
    "t_diurnal_amp_c,room_lag_hours,greenhouse_gain_c,sensor_gamma,"
    "sensor_gain,sensor_dark_adc,sensor_tempco,sensor_noise_frac,sensor_drop_frac,"
    "osc_ppm0,osc_tempco_a,osc_turnover_c,osc_aging_ppm_per_yr\n";

static void putd(FILE* f, double v) { if (!std::isnan(v)) fprintf(f, "%.6g", v); }

static void writeSummaryRow(FILE* f, const DeploymentResult& r) {
    const DeploymentParams& p = r.params;
    fprintf(f, "%d,%.6g,%.6g,%.6g,%d,%d,%d,%d,",
            r.index, r.steadyErrMs, r.maxErrMs, r.acceptFrac,
            r.convergenceDay, r.longestGapDays, r.fits, r.accepts);
    fprintf(f, "%.5f,%.5f,%.4g,", p.site.lat, p.site.lon, p.site.atmK);
    fprintf(f, "%.6g,%.6g,%d,%.6g,%.6g,%.6g,%.6g,%.6g,",
            p.baseCloud, p.seasonAmp, p.winterPeakDoy, p.tMeanC, p.tSeasonAmpC,
            p.tDiurnalAmpC, p.roomLagHours, p.greenhouseGainC);
    fprintf(f, "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,",
            p.sensorGamma, p.sensorGain, p.sensorDarkAdc,
            p.sensorTempco, p.sensorNoiseFrac, p.sensorDropFrac);
    fprintf(f, "%.6g,%.6g,%.6g,%.6g\n",
            p.oscPpm0, p.oscTempcoA, p.oscTurnoverC, p.oscAgingPpmPerYr);
}

static void writeDeployment(const std::string& outDir, const DeploymentResult& r) {
    MKDIR(outDir.c_str());
    MKDIR((outDir + "/daily").c_str());

    char name[64];
    snprintf(name, sizeof name, "/daily/%d.csv", r.index);
    FILE* d = fopen((outDir + name).c_str(), "w");
    if (d) {
        fprintf(d, "day,true_ms,reported_ms,err_ms,is_fit,accepted,true_ppm,"
                   "learned_ppm,indoor_temp_c,cloudiness,regime,shift_min,"
                   "n_used,rms_over_alpha\n");
        for (const auto& row : r.daily) {
            fprintf(d, "%d,%lld,%lld,%lld,%d,%d,%.6g,%.6g,%.6g,%.6g,%d,",
                    row.day, (long long)row.trueMs, (long long)row.reportedMs,
                    (long long)row.errMs, row.isFit ? 1 : 0, row.accepted ? 1 : 0,
                    row.truePpm, row.learnedPpm, row.indoorTempC, row.cloudiness,
                    row.regime);
            putd(d, row.shiftMin); fprintf(d, ",%d,", row.nUsed);
            putd(d, row.rmsOverAlpha); fprintf(d, "\n");
        }
        fclose(d);
    }

    snprintf(name, sizeof name, "/summary_%d.csv", r.index);
    FILE* s = fopen((outDir + name).c_str(), "w");
    if (s) { fprintf(s, "%s", SUMMARY_HEADER); writeSummaryRow(s, r); fclose(s); }

    if (!r.curves.empty()) {
        MKDIR((outDir + "/curves").c_str());
        snprintf(name, sizeof name, "/curves/%d.csv", r.index);
        FILE* c = fopen((outDir + name).c_str(), "w");
        if (c) {
            fprintf(c, "day,minute,observed,theoretical\n");
            for (const auto& cp : r.curves) {
                fprintf(c, "%d,%d,", cp.day, cp.minute);
                putd(c, cp.observed); fprintf(c, ",%.6g\n", cp.theoretical);
            }
            fclose(c);
        }
    }
}

static bool loadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// Parse "A..B" (inclusive). Returns false if not in that form.
static bool parseRange(const std::string& s, int& a, int& b) {
    size_t p = s.find("..");
    if (p == std::string::npos) return false;
    a = atoi(s.substr(0, p).c_str());
    b = atoi(s.substr(p + 2).c_str());
    return true;
}

int main(int argc, char** argv) {
    std::string configPath, dumpDefaults, outOverride;
    int single = -1, rangeA = -1, rangeB = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string { return (i + 1 < argc) ? argv[++i] : def; };
        if      (a == "--config")         configPath   = next("");
        else if (a == "--dump-defaults")  dumpDefaults = next("");
        else if (a == "--out")            outOverride  = next("");
        else if (a == "--deployment")     single = atoi(next("0").c_str());
        else if (a == "--deployments")    { if (!parseRange(next(""), rangeA, rangeB))
                                                { fprintf(stderr, "bad --deployments (use A..B)\n"); return 2; } }
        else { fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
    }

    if (!dumpDefaults.empty()) {
        nlohmann::json j = RunConfig{};
        std::ofstream out(dumpDefaults);
        if (!out) { fprintf(stderr, "cannot write %s\n", dumpDefaults.c_str()); return 1; }
        out << j.dump(2) << "\n";
        printf("wrote default config to %s\n", dumpDefaults.c_str());
        return 0;
    }

    RunConfig cfg;
    if (!configPath.empty()) {
        std::string text;
        if (!loadFile(configPath, text)) { fprintf(stderr, "cannot read %s\n", configPath.c_str()); return 1; }
        try { cfg = nlohmann::json::parse(text).get<RunConfig>(); }
        catch (const std::exception& e) { fprintf(stderr, "bad config: %s\n", e.what()); return 1; }
    }
    if (!outOverride.empty()) cfg.outDir = outOverride;
    if (cfg.numDeployments < 1) cfg.numDeployments = 1;

    // Which deployments does this invocation own?
    int a = 0, b = cfg.numDeployments - 1;
    bool wholeFleet = true;
    if (single >= 0)            { a = b = single; wholeFleet = false; }
    else if (rangeA >= 0)       { a = rangeA; b = rangeB; wholeFleet = false; }
    if (a < 0) a = 0;
    if (b > cfg.numDeployments - 1) b = cfg.numDeployments - 1;
    if (b < a) { fprintf(stderr, "empty deployment selection\n"); return 1; }

    int total = b - a + 1, done = 0;
    std::vector<DeploymentResult> all;
    if (wholeFleet) all.reserve(total);

    for (int idx = a; idx <= b; ++idx) {
        bool detail = (idx == cfg.detailedDeployment);
        DeploymentResult r = simulateDeployment(cfg, (uint32_t)idx, detail);
        writeDeployment(cfg.outDir, r);
        if (wholeFleet) all.push_back(std::move(r));
        ++done;
        printf("PROGRESS %d %d\n", done, total); fflush(stdout);
    }

    // Standalone whole-fleet convenience: a merged summary.csv.
    if (wholeFleet) {
        FILE* s = fopen((cfg.outDir + "/summary.csv").c_str(), "w");
        if (s) {
            fprintf(s, "%s", SUMMARY_HEADER);
            for (const auto& r : all) writeSummaryRow(s, r);
            fclose(s);
        }
    }
    printf("DONE %d..%d -> %s/\n", a, b, cfg.outDir.c_str());
    return 0;
}
