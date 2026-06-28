#pragma once
#include <cstdint>
#include <cmath>

// Deterministic RNG for the simulator: splitmix64 seeding -> xorshift64*.
// Counter-based seeding makes every deployment reproducible regardless of how
// the GUI schedules the worker processes -- essential for independent
// verification and for the "same deployment, different weather roll" picture.

namespace sim {

#ifndef SIM_PI
#define SIM_PI 3.14159265358979323846
#endif

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) {
        uint64_t z = seed + 0x9E3779B97F4A7C15ULL;     // splitmix64
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        s = z ^ (z >> 31);
        if (s == 0) s = 0x1234567890ABCDEFULL;
    }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * uniform(); }
    // Standard-normal (Box-Muller). Consumes two uniforms.
    double gaussian() {
        double u1 = uniform(); if (u1 < 1e-300) u1 = 1e-300;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * SIM_PI * uniform());
    }
};

// Distinct, well-mixed seed per deployment from a base seed + index.
inline uint64_t makeSeed(uint64_t seedBase, uint32_t deployment) {
    return (seedBase * 0x9E3779B97F4A7C15ULL)
         ^ ((uint64_t)deployment << 32)
         ^ ((uint64_t)deployment * 0xD1B54A32D192ED03ULL)
         ^ 0xA5A5A5A5DEADBEEFULL;
}

}  // namespace sim
