"""Editing schema for the RunConfig (mirrors sim/RunConfig.h & sim/Deployment.h).

Drives the generic config editor. Scalar fields are dotted JSON paths; range
fields edit ranges.<name>.lo / .hi (the per-deployment rolled parameters).
kind: "f" float, "i" int (spin box), "i64" big int (line edit).
"""

SCALAR_SECTIONS = [
    ("Timeline", [
        ("horizonDays", "i"), ("cadenceDays", "i"),
        ("initialOffsetMs", "i"), ("startEpochMs", "i64")]),
    ("Discipline (fit gates)", [
        ("maxPpmStep", "i"),
        ("fit.costHalfWindowMin", "i"), ("fit.shiftRangeMin", "i"),
        ("fit.minUsedPoints", "i"), ("fit.fullCurveMinPoints", "i"),
        ("fit.maxRmsOverAlpha", "f"), ("fit.unambiguityFactor", "f"),
        ("fit.minAlpha", "f"), ("fit.maxAlpha", "f")]),
    ("Discipline (loop)", [
        ("discipline.offsetGain", "f"), ("discipline.rateMode", "i"),
        ("discipline.rateGain", "f"), ("discipline.rateWindowFits", "i"),
        ("discipline.acqGain", "f"), ("discipline.acqFits", "i")]),
    ("Cloud physics (universal)", [
        ("cloud.synopticTauDays", "f"), ("cloud.synopticStd", "f"),
        ("cloud.diffuseFrac", "f"),
        ("cloud.clearSky.kbar", "f"), ("cloud.clearSky.sigma", "f"),
        ("cloud.clearSky.tauMin", "f"),
        ("cloud.broken.kbar", "f"), ("cloud.broken.sigma", "f"),
        ("cloud.broken.tauMin", "f"),
        ("cloud.overcast.kbar", "f"), ("cloud.overcast.sigma", "f"),
        ("cloud.overcast.tauMin", "f")]),
    ("Temperature physics (universal)", [
        ("temp.summerPeakDoy", "i"), ("temp.peakHour", "f"),
        ("temp.diurnalCloudDamp", "f"), ("temp.anomalyStdC", "f"),
        ("temp.anomalyTauDays", "f")]),
    ("Sensor physics (universal)", [
        ("sensor.peakTargetAdc", "f"), ("sensor.saturationAdc", "f"),
        ("sensor.sensorTempRefC", "f")]),
    ("Run / sweep", [
        ("numDeployments", "i"), ("seedBase", "i64"),
        ("detailedDeployment", "i"), ("curveStrideDays", "i")]),
]

# Per-deployment rolled ranges: ranges.<name>.{lo,hi}
RANGE_SECTIONS = [
    ("Location (rolled per deployment; device == truth)",
     ["latDeg", "lonDeg", "atmK"]),
    ("Climate (rolled per deployment)",
     ["baseCloud", "seasonAmp", "winterPeakDoy", "tMeanC",
      "tSeasonAmpC", "tDiurnalAmpC"]),
    ("Room (rolled per deployment)",
     ["roomLagHours", "greenhouseGainC"]),
    ("Sensor unit (rolled per deployment)",
     ["sensorGamma", "sensorGain", "sensorDarkAdc", "sensorTempco",
      "sensorNoiseFrac", "sensorDropFrac"]),
    ("Oscillator (rolled per deployment)",
     ["oscPpm0", "oscTempcoA", "oscTurnoverC", "oscAgingPpmPerYr"]),
]


def get_path(d, path):
    cur = d
    for k in path.split("."):
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur


def set_path(d, path, value):
    keys = path.split(".")
    cur = d
    for k in keys[:-1]:
        cur = cur.setdefault(k, {})
    cur[keys[-1]] = value
