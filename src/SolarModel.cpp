#include "SolarModel.h"
#include <cmath>

namespace solar {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float solarDeclination(float gamma) {
    float c1 = cosf(gamma),       s1 = sinf(gamma);
    float c2 = cosf(2.0f*gamma),  s2 = sinf(2.0f*gamma);
    float c3 = cosf(3.0f*gamma),  s3 = sinf(3.0f*gamma);
    return  0.006918f
          - 0.399912f * c1 + 0.070257f * s1
          - 0.006758f * c2 + 0.000907f * s2
          - 0.002697f * c3 + 0.001480f * s3;
}

float equationOfTime(float gamma) {
    // 229.183118 = 24*60/(2*pi): fraction-of-a-day Fourier -> minutes.
    float c1 = cosf(gamma),       s1 = sinf(gamma);
    float c2 = cosf(2.0f*gamma),  s2 = sinf(2.0f*gamma);
    return 229.183118f * (
          0.000075f
        + 0.001868f * c1 - 0.032077f * s1
        - 0.014615f * c2 - 0.040849f * s2);
}

void prepareCurve(SolarCurve& c, int doy, const SolarParams& p) {
    float gamma = 2.0f * (float)M_PI * (float)(doy - 1) / 365.0f;
    float decl  = solarDeclination(gamma);
    float E     = equationOfTime(gamma);
    float latR  = p.lat * (float)(M_PI / 180.0);
    c.sinLat   = sinf(latR);
    c.cosLat   = cosf(latR);
    c.sinDecl  = sinf(decl);
    c.cosDecl  = cosf(decl);
    c.noonMin  = 720.0f - E + 4.0f * (p.tz * 15.0f - p.lon);
    while (c.noonMin < 0.0f)     c.noonMin += 1440.0f;
    while (c.noonMin >= 1440.0f) c.noonMin -= 1440.0f;
    c.atmK     = p.atmK;
}

float intensityAt(const SolarCurve& c, float tMin) {
    float hRad  = ((float)(M_PI / 720.0)) * (tMin - c.noonMin);
    float cosZ  = c.sinLat * c.sinDecl + c.cosLat * c.cosDecl * cosf(hRad);
    if (cosZ <= 0.0f) return 0.0f;
    return powf(cosZ, c.atmK);
}

float solarNoonMinutes(int doy, const SolarParams& p) {
    SolarCurve c;
    prepareCurve(c, doy, p);
    return c.noonMin;
}

}  // namespace solar
