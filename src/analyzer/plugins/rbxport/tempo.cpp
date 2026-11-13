#include "analyzer/plugins/rbxport/tempo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include "analyzer/plugins/rbxport/attack.h"

namespace mixxx::rbxport {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// A segment's last beat is dropped when the next segment's first beat is
/// within this fraction of a period after it.
constexpr double kSameHit = 0.5;

/// Seconds per window of the Fourier magnitude.
constexpr double kFourierWindowSecs = 20.0;

/// A steady tempo measured within this of a whole number of BPM may be that
/// whole number.
constexpr double kWholeBpmTolerance = 0.1;
/// The share of the measured line's hits the whole-number line has to sit on.
constexpr double kWholeBpmSupport = 0.85;
/// How far from a line a hit may be and still count towards it, in seconds.
constexpr double kOnLineSecs = 0.010;

/// Windows in a row that must agree on a new tempo before it is believed.
constexpr size_t kSegmentMinWindows = 3;

/// Phases tried when measuring how much of a stretch's kick a period collects.
constexpr size_t kKickPhases = 32;

/// A bar of the new grid reads the kick when the kick band averages this
/// over its four beats.
constexpr double kKickPresent = 0.15;
/// A run of the kick is the beat only when its level is this share of the
/// strongest run's.
constexpr double kRunStrength = 0.55;
/// A bar whose next bar reads more than this many times as much kick is a
/// fill into it.
constexpr double kFillRatio = 2.0;
/// How much of the full-band flux the cut's bar must carry.
constexpr double kBoundaryStrength = 0.7;
/// Bars a run of the kick must last to be the beat rather than a fill.
constexpr size_t kBoundaryBars = 2;
/// The beat the cut goes on, within the first reliable bar.
constexpr double kCutOnAKick = 0.5;
/// A source whose settled level is below this carries nothing and does not vote.
constexpr double kLiveSource = 0.1;

/// A walked bar counts as settled at the new tempo when its period is
/// within this fraction of the new fit's.
constexpr double kSettledTolerance = 0.01;
/// From one beat to the next the period may move by this fraction and
/// still be a change of pace.
constexpr double kRampStep = 0.05;
/// How far either side of the predicted beat the next hit is looked for.
constexpr double kRampReach = 0.1;
/// Beats the walk gives up after.
constexpr size_t kMaxWalkedBeats = 1024;

/// Emphasise quiet percussion only while resolving a tempo transition.
constexpr double kTransientGain = 4.0;
constexpr double kTransientReleaseSecs = 0.020;

/// Beats in a row a line must go without a hit before the stretch is a gap.
constexpr size_t kGapBeats = 8;
/// Beats in a row with a hit before a gap that the walk into it may start from.
constexpr size_t kSupportedRun = 4;
/// The hits after a gap may sit this far from the old line, as a fraction
/// of a beat, and still be its grid.
constexpr double kPhaseTolerance = 0.1;
/// Below this share of the envelope's peak a hit is nothing.
constexpr double kHitFloor = 0.02;
/// A line resumes on hits at least this share of its own floor.
constexpr double kResumeFraction = 0.4;
/// A hit through a ramp must be at least this share of the mean of the hits
/// before it.
constexpr double kRampFade = 0.25;
/// Hits the ramp keeps in its running mean.
constexpr size_t kRampMemory = 4;
/// From one beat to the next a ramp's period may change by this much.
constexpr double kRampRatioMin = 0.8;
constexpr double kRampRatioMax = 1.3;
/// The next beat is looked for from three quarters of the last period to
/// five quarters of where the last two periods put it.
constexpr double kRampWindowMin = 0.75;
constexpr double kRampWindowMax = 1.25;
/// A ramp is believed at this many beats, and when its period has moved
/// from the line's by at least kRampDeparture.
constexpr size_t kRampMinBeats = 8;
constexpr double kRampDeparture = 0.05;

/// A line stands on the kicks of a stretch when it collects this many
/// times the kick of the same line moved half a beat.
constexpr double kKickMargin = 1.5;
/// A stretch this many beats long is fitted for its own period.
constexpr size_t kMinStretchBeats = 64;
/// A ramp's period may step back against its direction by this fraction.
constexpr double kRampWobble = 0.01;

struct Fit {
    /// Envelope sample of the first beat, which may be negative.
    double phase = 0.0;
    double period = 0.0;
};

struct Candidate {
    double bpm = 0.0;
    double acf = 0.0;
    double fourier = 0.0;
    double prior = 0.0;
    double score = 0.0;
};

/// One beat's evidence, from the three sources a beat can be heard in.
struct Evidence {
    static constexpr size_t kSources = 3;
    /// The kick band's peak near the beat: the thump.
    double kick = 0.0;
    /// The click band's spike at the beat: the kick's attack.
    double attack = 0.0;
    /// The full band's peak near the beat: anything percussive.
    double flux = 0.0;

    double source(size_t i) const {
        switch (i) {
        case 0:
            return kick;
        case 1:
            return attack;
        default:
            return flux;
        }
    }
    double sum() const {
        return kick + attack + flux;
    }
};

/// Where the fit reads its beats from.
struct Reader {
    const std::vector<double>* values = nullptr;
    double rate = 0.0;
    double originSecs = 0.0;
    const AttackMap* attacks = nullptr;
    /// The onset envelope of the kick band alone, on its own clock.
    const OnsetEnvelope* kicks = nullptr;

    Evidence evidence(double x, double period) const;
    std::optional<std::pair<double, double>> snap(double x, double reach) const;
};

struct Run {
    size_t from = 0;
    size_t to = 0;
    double bpm = 0.0;
    size_t changeFrom = 0;
};

struct Gap {
    size_t lastSupported = 0;
    std::optional<size_t> resumes;
};

using Point = std::tuple<double, double, double>; // index, time, weight

bool related(double a, double b);
std::vector<Candidate> candidates(const std::vector<double>& values,
        double rate,
        const TempoOptions& options);
std::vector<double> autocorrelation(std::span<const double> centred,
        double variance,
        size_t minLag,
        size_t maxLag);
double interpolate(const std::vector<double>& table, double x);
double fourierMagnitude(const std::vector<double>& centred, double rate, double bpm);
double tempoPrior(double bpm, const TempoOptions& options);
std::optional<Fit> fit(const Reader& reader,
        double bpm,
        size_t from,
        size_t to,
        const TempoOptions& options);
Fit wholeBpm(const Reader& reader,
        size_t from,
        size_t to,
        const Fit& f);
double onLine(const Reader& reader, size_t from, size_t to, const Fit& f);
double comb(std::span<const double> values, double period, double phase);
double sampleAt(std::span<const double> values, double x);
double refinePeriod(std::span<const double> values, double coarse, size_t phases);
double bestPhase(std::span<const double> values, double period, size_t phases);
std::optional<std::pair<Fit, double>> snapAndFit(const Reader& reader,
        size_t from,
        size_t to,
        const Fit& current,
        std::optional<double> fixedPeriod);
std::optional<std::pair<double, double>> localPeak(std::span<const double> values,
        double around,
        double reach);
double kickPeak(const OnsetEnvelope& kicks, double around, double reach);
std::vector<Segment> segment(const Reader& reader,
        double bpm,
        const TempoOptions& options);
std::optional<double> localTempo(std::span<const double> window,
        double rate,
        double bpm,
        const TempoOptions& options);
std::vector<Run> labelRuns(const std::vector<std::optional<double>>& local,
        const std::vector<size_t>& starts,
        size_t window,
        size_t n,
        double bpm,
        const TempoOptions& options);
std::vector<std::pair<size_t, size_t>> runBounds(const std::vector<double>& labels);
double referenceBpm(const Reader& reader, double bpm, const TempoOptions& options);
std::pair<double, size_t> kickCollected(const Reader& reader,
        double period,
        size_t from,
        size_t to);
double kickMargin(const Reader& reader,
        double own,
        double other,
        size_t from,
        size_t to);
std::vector<Run> kickedRuns(const Reader& reader,
        std::vector<Run> runs,
        double bpm,
        const TempoOptions& options);
std::vector<std::pair<double, double>> walkBeats(const Reader& reader,
        const Fit& a,
        const Fit& b,
        size_t from,
        size_t to);
double boundary(const Reader& reader,
        size_t from,
        size_t to,
        size_t settledFrom,
        size_t settledTo,
        const Fit& a,
        const Fit& b);
std::array<double, Evidence::kSources> settledLevels(
        const std::vector<std::pair<double, Evidence>>& beats,
        double settledFrom,
        double settledTo);
std::vector<std::tuple<size_t, size_t, double>> kickRuns(
        const std::vector<std::pair<double, Evidence>>& beats, size_t ks);
std::vector<Segment> splitGaps(const Reader& reader,
        const std::vector<Segment>& segments,
        const TempoOptions& options);
std::vector<Point> beatHits(std::span<const double> values,
        const Fit& f,
        size_t from,
        size_t to);
double kickSupport(const Reader& reader, const Fit& f, double from, double to);
std::optional<Fit> onTheKicks(const Reader& reader,
        const Fit& f,
        double from,
        double to);
std::optional<std::tuple<Fit, Fit, double>> judgeGap(const Reader& reader,
        const Fit& f,
        double from,
        double after,
        double to,
        double probe,
        const TempoOptions& options);
std::optional<Gap> firstGap(const std::vector<bool>& supported, size_t from);
std::vector<std::pair<double, double>> peaksIn(std::span<const double> values,
        double lo,
        double hi,
        double floor);
std::pair<std::vector<double>, double> walkRamp(std::span<const double> values,
        double start,
        double period,
        double until);
bool oneWay(const std::vector<double>& beats);
std::vector<Segment> rampSegments(const std::vector<double>& walked,
        double growth,
        double cut,
        double rate,
        double originSecs);
std::optional<double> lastSupportedHit(std::span<const double> values,
        const Fit& f,
        double from,
        double until,
        double floor);
std::optional<double> resumeOf(std::span<const double> values,
        const Fit& b,
        double from,
        double to,
        double floor);
std::optional<Fit> refitStart(const Reader& reader,
        const Fit& f,
        double from,
        double to,
        const std::vector<Point>& hits,
        const std::vector<bool>& supported,
        const TempoOptions& options);
double toSecsOf(const Reader& reader, double x);

size_t toSize(double value) {
    return value <= 0.0 ? 0 : static_cast<size_t>(value);
}

uint32_t toTimeMs(double secs) {
    const double value = std::round(secs * 1000.0);
    if (!(value >= 0.0) || value > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

uint16_t toTempoX100(double bpm) {
    const double value = std::round(bpm * 100.0);
    if (!(value >= 0.0) || value > static_cast<double>(std::numeric_limits<uint16_t>::max())) {
        return 0;
    }
    return static_cast<uint16_t>(value);
}

double midpoint(double a, double b) {
    return (a + b) * 0.5;
}

double clampOffset(double offset) {
    return std::clamp(offset, -0.5, 0.5);
}

/// Seconds into the file of envelope sample `x`.
double toSecsOf(const Reader& reader, double x) {
    return reader.originSecs + x / reader.rate;
}

Evidence Reader::evidence(double x, double period) const {
    const double reach = std::max(period * 0.1, 1.0);
    const double secs = originSecs + x / rate;
    const double kick = kicks
            ? kickPeak(*kicks, (secs - kicks->originSecs) * kicks->rate, reach)
            : 0.0;
    double attack = 0.0;
    if (attacks) {
        if (const auto found = attacks->attackNear(secs)) {
            attack = found->height;
        }
    }
    const auto fluxPeak = localPeak(*values, x, reach);
    const double flux = fluxPeak ? fluxPeak->second : 0.0;
    return Evidence{kick, attack, flux};
}

std::optional<std::pair<double, double>> Reader::snap(double x, double reach) const {
    if (attacks) {
        const double secs = originSecs + x / rate;
        const double reachSecs = std::min(reach / rate, attacks->options().reachSecs);
        if (const auto attack = attacks->attackWithin(secs, reachSecs)) {
            return std::make_pair(
                    (attack->secs - originSecs) * rate, attack->height);
        }
        return std::nullopt;
    }
    return localPeak(*values, x, reach);
}

bool related(double a, double b) {
    if (a <= 0.0 || b <= 0.0) {
        return false;
    }
    const double ratio = a > b ? a / b : b / a;
    constexpr std::array<double, 6> kRatios{
            1.0, 2.0, 3.0, 4.0, 1.5, 4.0 / 3.0};
    for (const double r : kRatios) {
        if (std::abs(ratio - r) < 0.02 * r) {
            return true;
        }
    }
    return false;
}

std::vector<Candidate> candidates(const std::vector<double>& values,
        double rate,
        const TempoOptions& options) {
    const size_t n = values.size();
    double mean = 0.0;
    for (const double value : values) {
        mean += value;
    }
    mean /= static_cast<double>(n);
    std::vector<double> centred(n);
    for (size_t i = 0; i < n; ++i) {
        centred[i] = values[i] - mean;
    }
    double variance = 0.0;
    for (const double value : centred) {
        variance += value * value;
    }
    variance /= static_cast<double>(n);
    if (variance <= 0.0) {
        return {};
    }

    const auto lagOf = [rate](double bpm) {
        return rate * 60.0 / bpm;
    };
    const size_t minLag = std::max(
            toSize(std::floor(lagOf(options.maxBpm * 2.0))), size_t(2));
    const size_t maxLag = std::min(
            toSize(std::ceil(lagOf(options.minBpm / 2.0))), n / 2);
    if (maxLag <= minLag + 2) {
        return {};
    }
    const std::vector<double> acf = autocorrelation(
            centred, variance, minLag, maxLag);
    const auto acfAt = [&acf, minLag](double lag) {
        return interpolate(acf, lag - static_cast<double>(minLag));
    };

    const auto inRange = [&options](double bpm) {
        return bpm >= options.minBpm && bpm <= options.maxBpm;
    };
    std::vector<double> bpms;
    const size_t lo = toSize(std::floor(lagOf(options.maxBpm)));
    const size_t hi = toSize(std::ceil(lagOf(options.minBpm)));
    const size_t firstLag = std::max(lo, minLag + 1);
    const size_t lastLag = std::min(hi, maxLag - 1);
    for (size_t lag = firstLag; lag < lastLag; ++lag) {
        const double here = acfAt(static_cast<double>(lag));
        if (here <= acfAt(static_cast<double>(lag) - 1.0) ||
                here < acfAt(static_cast<double>(lag) + 1.0) ||
                here <= 0.0) {
            continue;
        }
        const double a = acfAt(static_cast<double>(lag) - 1.0);
        const double c = acfAt(static_cast<double>(lag) + 1.0);
        const double denom = a - 2.0 * here + c;
        const double offset = std::abs(denom) > std::numeric_limits<double>::epsilon()
                ? 0.5 * (a - c) / denom
                : 0.0;
        const double bpm = rate * 60.0 /
                (static_cast<double>(lag) + clampOffset(offset));
        constexpr std::array<double, 9> kRatios{
                1.0, 2.0, 0.5, 1.5, 2.0 / 3.0, 3.0, 1.0 / 3.0, 4.0 / 3.0, 0.75};
        for (const double ratio : kRatios) {
            const double relative = bpm * ratio;
            if (!inRange(relative)) {
                continue;
            }
            bool exists = false;
            for (const double existing : bpms) {
                if (std::abs(existing - relative) < relative * 0.01) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                bpms.push_back(relative);
            }
        }
    }
    if (bpms.empty()) {
        return {};
    }

    std::vector<double> fourierRaw;
    fourierRaw.reserve(bpms.size());
    for (const double bpm : bpms) {
        fourierRaw.push_back(fourierMagnitude(centred, rate, bpm));
    }
    double fourierPeak = 0.0;
    for (const double value : fourierRaw) {
        fourierPeak = std::max(fourierPeak, value);
    }
    double acfPeak = 0.0;
    for (const double bpm : bpms) {
        acfPeak = std::max(acfPeak, acfAt(lagOf(bpm)));
    }

    std::vector<Candidate> out;
    out.reserve(bpms.size());
    for (size_t i = 0; i < bpms.size(); ++i) {
        const double bpm = bpms[i];
        const double ft = fourierRaw[i];
        const double acfValue = std::max(
                acfAt(lagOf(bpm)) / std::max(acfPeak, std::numeric_limits<double>::epsilon()),
                0.0);
        const double fourier = fourierPeak > 0.0 ? ft / fourierPeak : 0.0;
        const double prior = tempoPrior(bpm, options);
        out.push_back(Candidate{bpm,
                acfValue,
                fourier,
                prior,
                acfValue * std::sqrt(fourier) * prior});
    }
    std::stable_sort(out.begin(),
            out.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    return out;
}

std::vector<double> autocorrelation(std::span<const double> centred,
        double variance,
        size_t minLag,
        size_t maxLag) {
    const size_t n = centred.size();
    std::vector<double> out;
    out.reserve(maxLag - minLag + 1);
    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        const size_t count = n > lag ? n - lag : 0;
        if (count == 0) {
            out.push_back(0.0);
            continue;
        }
        double sum = 0.0;
        for (size_t i = 0; i < count; ++i) {
            sum += centred[i] * centred[i + lag];
        }
        out.push_back(sum / (static_cast<double>(count) * variance));
    }
    return out;
}

double interpolate(const std::vector<double>& table, double x) {
    if (table.empty()) {
        return 0.0;
    }
    x = std::clamp(x, 0.0, static_cast<double>(table.size() - 1));
    const size_t i = toSize(std::floor(x));
    const double frac = x - static_cast<double>(i);
    const double a = i < table.size() ? table[i] : 0.0;
    const double b = i + 1 < table.size() ? table[i + 1] : a;
    return a + (b - a) * frac;
}

double fourierMagnitude(const std::vector<double>& centred, double rate, double bpm) {
    const double omega = 2.0 * kPi * bpm / 60.0 / rate;
    const size_t window = std::clamp(
            toSize(kFourierWindowSecs * rate),
            size_t(64),
            std::max(centred.size(), size_t(64)));
    const size_t hop = window / 2;
    const double stepRe = std::cos(omega);
    const double stepIm = std::sin(omega);
    double total = 0.0;
    double windows = 0.0;
    size_t start = 0;
    while (start + window <= centred.size()) {
        double re = 1.0;
        double im = 0.0;
        double sumRe = 0.0;
        double sumIm = 0.0;
        for (size_t i = 0; i < window; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(window));
            const double w = centred[start + i] * hann;
            sumRe += w * re;
            sumIm += w * im;
            const double nextRe = re * stepRe - im * stepIm;
            im = re * stepIm + im * stepRe;
            re = nextRe;
        }
        total += std::sqrt(sumRe * sumRe + sumIm * sumIm) /
                static_cast<double>(window);
        windows += 1.0;
        start += hop;
        if (start + window > centred.size() && windows == 0.0) {
            break;
        }
    }
    return windows > 0.0 ? total / windows : 0.0;
}

double tempoPrior(double bpm, const TempoOptions& options) {
    if (bpm <= 0.0 || options.priorWidth <= 0.0 || options.priorCentre <= 0.0) {
        return 0.0;
    }
    const double x = std::log(bpm / options.priorCentre) / options.priorWidth;
    return std::exp(-0.5 * x * x);
}

std::optional<Fit> fit(const Reader& reader,
        double bpm,
        size_t from,
        size_t to,
        const TempoOptions& options) {
    const std::vector<double>& values = *reader.values;
    if (from > to || to > values.size()) {
        return std::nullopt;
    }
    const std::span<const double> slice(values.data() + from, to - from);
    if (slice.size() < 8) {
        return std::nullopt;
    }
    const double coarse = reader.rate * 60.0 / bpm;
    const double period = refinePeriod(slice, coarse, options.refinePhases);
    const double phase = bestPhase(slice, period, 64);
    const std::array<double, 2> starts =
            reader.attacks ? std::array<double, 2>{0.0, 0.5}
                           : std::array<double, 2>{0.0, 0.0};
    const size_t startCount = reader.attacks ? 2 : 1;
    std::optional<std::pair<Fit, double>> best;
    for (size_t s = 0; s < startCount; ++s) {
        Fit current{phase + starts[s] * period + static_cast<double>(from), period};
        double support = 0.0;
        for (int pass = 0; pass < 3; ++pass) {
            const auto next = snapAndFit(reader, from, to, current, std::nullopt);
            if (!next) {
                break;
            }
            current = next->first;
            support = next->second;
        }
        if (!best || support > best->second) {
            best = std::make_pair(current, support);
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return wholeBpm(reader, from, to, best->first);
}

Fit wholeBpm(const Reader& reader, size_t from, size_t to, const Fit& f) {
    if (f.period <= 0.0) {
        return f;
    }
    const double bpm = reader.rate * 60.0 / f.period;
    const double whole = std::round(bpm);
    if (whole <= 0.0 || std::abs(bpm - whole) > kWholeBpmTolerance) {
        return f;
    }
    const double period = reader.rate * 60.0 / whole;
    const Fit start{f.phase, period};
    Fit snapped = start;
    if (const auto result = snapAndFit(reader, from, to, start, period)) {
        snapped = result->first;
    }
    if (onLine(reader, from, to, snapped) <
            onLine(reader, from, to, f) * kWholeBpmSupport) {
        return f;
    }
    return snapped;
}

double onLine(const Reader& reader, size_t from, size_t to, const Fit& f) {
    if (f.period < 2.0) {
        return 0.0;
    }
    const double reach = std::max(f.period * 0.2, 1.0);
    const double tolerance = kOnLineSecs * reader.rate;
    const int64_t first = static_cast<int64_t>(std::ceil(
            (static_cast<double>(from) - f.phase) / f.period));
    const int64_t last = static_cast<int64_t>(std::floor(
            (static_cast<double>(to) - 1.0 - f.phase) / f.period));
    double total = 0.0;
    for (int64_t k = first; k <= last; ++k) {
        const double predicted = f.phase + static_cast<double>(k) * f.period;
        const auto hit = reader.snap(predicted, reach);
        if (!hit) {
            continue;
        }
        total += hit->second *
                std::max(1.0 - std::abs(hit->first - predicted) / tolerance, 0.0);
    }
    return total;
}

double comb(std::span<const double> values, double period, double phase) {
    if (period < 2.0 || values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    double count = 0.0;
    double x = phase;
    while (x < static_cast<double>(values.size())) {
        sum += sampleAt(values, x);
        count += 1.0;
        x += period;
    }
    return count > 0.0 ? sum / count : 0.0;
}

double sampleAt(std::span<const double> values, double x) {
    if (x < 0.0) {
        return 0.0;
    }
    const size_t i = toSize(std::floor(x));
    const double frac = x - static_cast<double>(i);
    const double a = i < values.size() ? values[i] : 0.0;
    const double b = i + 1 < values.size() ? values[i + 1] : 0.0;
    return a + (b - a) * frac;
}

double refinePeriod(std::span<const double> values, double coarse, size_t phases) {
    const auto score = [values, phases](double period) {
        const size_t steps = std::max(phases, size_t(1));
        double best = 0.0;
        for (size_t step = 0; step < steps; ++step) {
            best = std::max(best,
                    comb(values,
                            period,
                            period * static_cast<double>(step) /
                                    static_cast<double>(steps)));
        }
        return best;
    };
    std::pair<double, double> best{score(coarse), coarse};
    constexpr std::array<std::pair<double, double>, 2> kSweeps{
            std::pair{1.0, 0.02}, std::pair{0.03, 0.002}};
    for (const auto& [span, step] : kSweeps) {
        const double centre = best.second;
        double candidate = centre - span;
        while (candidate <= centre + span) {
            const double s = score(candidate);
            if (s > best.first) {
                best = {s, candidate};
            }
            candidate += step;
        }
    }
    return best.second;
}

double bestPhase(std::span<const double> values, double period, size_t phases) {
    const size_t steps = std::max(phases, size_t(1));
    std::pair<double, double> best{-std::numeric_limits<double>::infinity(), 0.0};
    for (size_t step = 0; step < steps; ++step) {
        const double phase = period * static_cast<double>(step) /
                static_cast<double>(steps);
        const double s = comb(values, period, phase);
        if (s > best.first) {
            best = {s, phase};
        }
    }
    const double step = period / static_cast<double>(steps);
    const double a = comb(values, period, best.second - step);
    const double b = best.first;
    const double c = comb(values, period, best.second + step);
    const double denom = a - 2.0 * b + c;
    const double offset = std::abs(denom) > std::numeric_limits<double>::epsilon()
            ? 0.5 * (a - c) / denom
            : 0.0;
    return best.second + std::clamp(offset, -1.0, 1.0) * step;
}

std::optional<std::pair<Fit, double>> snapAndFit(const Reader& reader,
        size_t from,
        size_t to,
        const Fit& current,
        std::optional<double> fixedPeriod) {
    const double period = current.period;
    if (period < 2.0) {
        return std::nullopt;
    }
    const double reach = std::max(period * 0.2, 1.0);
    const int64_t first = static_cast<int64_t>(std::ceil(
            (static_cast<double>(from) - current.phase) / period));
    const int64_t last = static_cast<int64_t>(std::floor(
            (static_cast<double>(to) - 1.0 - current.phase) / period));
    if (last < first + 4) {
        return std::nullopt;
    }

    std::vector<Point> snapped;
    for (int64_t k = first; k <= last; ++k) {
        const double predicted = current.phase + static_cast<double>(k) * period;
        if (const auto hit = reader.snap(predicted, reach)) {
            snapped.emplace_back(static_cast<double>(k), hit->first, hit->second);
        }
    }
    if (snapped.size() < 4) {
        return std::nullopt;
    }
    std::vector<double> heights;
    heights.reserve(snapped.size());
    for (const auto& [x, y, w] : snapped) {
        heights.push_back(w);
    }
    std::stable_sort(heights.begin(), heights.end());
    const double floorLevel = heights[heights.size() / 2] * 0.2;
    std::vector<Point> strong;
    strong.reserve(snapped.size());
    for (const auto& point : snapped) {
        if (std::get<2>(point) >= floorLevel) {
            strong.push_back(point);
        }
    }
    if (strong.size() >= 4) {
        snapped = std::move(strong);
    }

    const auto line = [fixedPeriod](const std::vector<Point>& points)
            -> std::optional<std::pair<double, double>> {
        double sw = 0.0;
        double sx = 0.0;
        double sy = 0.0;
        double sxx = 0.0;
        double sxy = 0.0;
        for (const auto& [x, y, w] : points) {
            sw += w;
            sx += w * x;
            sy += w * y;
            sxx += w * x * x;
            sxy += w * x * y;
        }
        if (sw <= 0.0) {
            return std::nullopt;
        }
        if (fixedPeriod) {
            const double slope = *fixedPeriod;
            return std::make_pair(slope, (sy - slope * sx) / sw);
        }
        const double denom = sw * sxx - sx * sx;
        if (std::abs(denom) < std::numeric_limits<double>::epsilon()) {
            return std::nullopt;
        }
        const double slope = (sw * sxy - sx * sy) / denom;
        return std::make_pair(slope, (sy - slope * sx) / sw);
    };
    const auto firstLine = line(snapped);
    if (!firstLine) {
        return std::nullopt;
    }
    double slope = firstLine->first;
    double intercept = firstLine->second;
    std::vector<double> residuals;
    residuals.reserve(snapped.size());
    for (const auto& [x, y, w] : snapped) {
        residuals.push_back(std::abs(y - intercept - slope * x));
    }
    std::stable_sort(residuals.begin(), residuals.end());
    const double cutoff = std::min(
            residuals.size() * 4 / 5 < residuals.size()
                    ? residuals[residuals.size() * 4 / 5]
                    : std::numeric_limits<double>::infinity(),
            period * 0.1);
    std::vector<Point> kept;
    kept.reserve(snapped.size());
    for (const auto& point : snapped) {
        const auto& [x, y, w] = point;
        if (std::abs(y - intercept - slope * x) <= cutoff) {
            kept.push_back(point);
        }
    }
    if (kept.size() >= 4) {
        const auto secondLine = line(kept);
        if (!secondLine) {
            return std::nullopt;
        }
        slope = secondLine->first;
        intercept = secondLine->second;
    }

    if (std::abs(slope - period) > period * 0.02) {
        return std::nullopt;
    }
    double support = 0.0;
    for (const auto& [x, y, w] : kept) {
        support += w;
    }
    return std::make_pair(Fit{intercept, slope}, support);
}

std::optional<std::pair<double, double>> localPeak(std::span<const double> values,
        double around,
        double reach) {
    const size_t lo = toSize(std::floor(around - reach));
    const size_t hi = std::min(
            toSize(std::ceil(around + reach)),
            values.empty() ? size_t(0) : values.size() - 1);
    if (values.empty() || lo >= hi) {
        return std::nullopt;
    }
    std::pair<double, size_t> best{0.0, lo};
    for (size_t i = lo; i <= hi; ++i) {
        const double v = i < values.size() ? values[i] : 0.0;
        if (v > best.first) {
            best = {v, i};
        }
    }
    if (best.first <= 0.0) {
        return std::nullopt;
    }
    const size_t i = best.second;
    const double a = i > 0 ? values[i - 1] : best.first;
    const double c = i + 1 < values.size() ? values[i + 1] : 0.0;
    const double denom = a - 2.0 * best.first + c;
    const double offset = std::abs(denom) > std::numeric_limits<double>::epsilon()
            ? 0.5 * (a - c) / denom
            : 0.0;
    return std::make_pair(static_cast<double>(i) + clampOffset(offset), best.first);
}

double kickPeak(const OnsetEnvelope& kicks, double around, double reach) {
    const size_t lo = toSize(std::floor(around - reach));
    const size_t hi = std::min(
            toSize(std::ceil(around + reach)),
            kicks.empty() ? size_t(0) : kicks.size() - 1);
    if (kicks.empty() || lo > hi) {
        return 0.0;
    }
    double best = 0.0;
    for (size_t i = lo; i <= hi; ++i) {
        best = std::max(best, static_cast<double>(kicks.values[i]));
    }
    return best;
}

std::vector<Segment> segment(const Reader& reader,
        double bpm,
        const TempoOptions& options) {
    const std::vector<double>& values = *reader.values;
    const double rate = reader.rate;
    const double originSecs = reader.originSecs;
    const size_t n = values.size();
    if (!options.followChanges) {
        const auto f = fit(reader, bpm, 0, n, options);
        if (!f) {
            return {};
        }
        return {Segment{0.0,
                toSecsOf(reader, static_cast<double>(n)),
                f->period / rate,
                toSecsOf(reader, f->phase)}};
    }
    const size_t window = std::max(
            toSize(options.segmentWindowSecs * rate), size_t(64));
    const double reference = referenceBpm(reader, bpm, options);
    const size_t hop = window / 2;
    std::vector<std::optional<double>> local;
    std::vector<size_t> starts;
    size_t start = 0;
    while (start + window <= n) {
        starts.push_back(start);
        local.push_back(localTempo(
                std::span<const double>(values.data() + start, window),
                rate,
                reference,
                options));
        start += hop;
    }

    std::vector<Segment> segments;
    std::vector<Run> runs = labelRuns(
            local, starts, window, n, reference, options);
    runs = kickedRuns(reader, std::move(runs), reference, options);

    struct FitEntry {
        size_t changeFrom;
        size_t from;
        size_t to;
        Fit fit;
    };
    std::vector<FitEntry> fits;
    for (const Run& run : runs) {
        const auto f = fit(reader, run.bpm, run.from, run.to, options);
        if (f) {
            fits.push_back(FitEntry{run.changeFrom, run.from, run.to, *f});
        }
    }
    if (fits.empty()) {
        return segments;
    }

    const auto toSecs = [&reader](double x) { return toSecsOf(reader, x); };
    double fromSample = -originSecs * rate;
    std::optional<Fit> pending;
    const auto refit = [&reader, &options, n, rate](const Fit& current,
                               double from,
                               double to) {
        const double bpmHere = rate * 60.0 / current.period;
        const size_t fromI = toSize(from);
        const size_t toI = std::min(toSize(to), n);
        if (toI <= fromI) {
            return current;
        }
        const auto fitted = fit(reader, bpmHere, fromI, toI, options);
        return fitted ? *fitted : current;
    };
    for (size_t index = 0; index < fits.size(); ++index) {
        const FitEntry& entry = fits[index];
        const Fit current = pending ? *pending : entry.fit;
        if (index + 1 >= fits.size()) {
            if (static_cast<double>(n) > fromSample) {
                const Fit finalFit = pending
                        ? current
                        : refit(current, fromSample, static_cast<double>(n));
                segments.push_back(Segment{
                        toSecs(fromSample),
                        toSecs(static_cast<double>(n)),
                        finalFit.period / rate,
                        toSecs(finalFit.phase)});
            }
            break;
        }
        const FitEntry& nextEntry = fits[index + 1];
        const std::vector<std::pair<double, double>> walked = walkBeats(
                reader, current, nextEntry.fit, entry.changeFrom, nextEntry.to);
        double cut = 0.0;
        Fit nextFit = nextEntry.fit;
        if (!walked.empty()) {
            const double firstBeatStart = walked.front().first;
            const double lastEnd = walked.back().second;
            const double phase = nextEntry.fit.phase +
                    std::round((lastEnd - nextEntry.fit.phase) /
                            nextEntry.fit.period) *
                            nextEntry.fit.period;
            cut = firstBeatStart;
            nextFit = Fit{phase, nextEntry.fit.period};
        } else {
            cut = boundary(reader,
                    entry.changeFrom,
                    nextEntry.to,
                    nextEntry.from,
                    nextEntry.to,
                    current,
                    nextEntry.fit);
        }
        if (cut > fromSample) {
            const Fit fitHere = pending
                    ? current
                    : refit(current, fromSample, cut);
            segments.push_back(Segment{
                    toSecs(fromSample),
                    toSecs(cut),
                    fitHere.period / rate,
                    toSecs(fitHere.phase)});
        }
        for (const auto& [beatStart, beatEnd] : walked) {
            segments.push_back(Segment{
                    toSecs(beatStart),
                    toSecs(beatEnd),
                    (beatEnd - beatStart) / rate,
                    toSecs(beatStart)});
        }
        fromSample = walked.empty() ? cut : nextFit.phase;
        pending = nextFit;
    }
    return splitGaps(reader, segments, options);
}

std::optional<double> localTempo(std::span<const double> window,
        double rate,
        double bpm,
        const TempoOptions& options) {
    const size_t n = window.size();
    double mean = 0.0;
    for (const double value : window) {
        mean += value;
    }
    mean /= static_cast<double>(n);
    std::vector<double> centred(n);
    for (size_t i = 0; i < n; ++i) {
        centred[i] = window[i] - mean;
    }
    double variance = 0.0;
    for (const double value : centred) {
        variance += value * value;
    }
    variance /= static_cast<double>(n);
    if (variance <= 0.0) {
        return std::nullopt;
    }
    const auto lagOf = [rate](double b) { return rate * 60.0 / b; };
    const size_t minLag = std::max(toSize(std::floor(lagOf(options.maxBpm))), size_t(2));
    const size_t maxLag = std::min(toSize(std::ceil(lagOf(options.minBpm))), n / 3);
    if (maxLag <= minLag + 2) {
        return std::nullopt;
    }
    const std::vector<double> acf = autocorrelation(
            centred, variance, minLag, maxLag);
    const double home = interpolate(
            acf, lagOf(bpm) - static_cast<double>(minLag));
    std::pair<double, size_t> best{0.0, 0};
    for (size_t i = 1; i + 1 < acf.size(); ++i) {
        if (acf[i] > acf[i - 1] && acf[i] >= acf[i + 1] && acf[i] > best.first) {
            best = {acf[i], i};
        }
    }
    if (best.first <= 0.0) {
        return std::nullopt;
    }
    const double left = acf[best.second - 1];
    const double middle = acf[best.second];
    const double right = acf[best.second + 1];
    const double curvature = left - 2.0 * middle + right;
    const double offset = std::abs(curvature) > std::numeric_limits<double>::epsilon()
            ? 0.5 * (left - right) / curvature
            : 0.0;
    const double peakBpm = rate * 60.0 /
            (static_cast<double>(best.second + minLag) + clampOffset(offset));
    double ratio = peakBpm / bpm;
    while (ratio > 1.5 && ratio * bpm / 2.0 >= options.minBpm) {
        ratio /= 2.0;
    }
    while (ratio < 0.75 && ratio * bpm * 2.0 <= options.maxBpm) {
        ratio *= 2.0;
    }
    if (home >= best.first * 0.6) {
        return 1.0;
    }
    return ratio;
}

std::vector<Run> labelRuns(const std::vector<std::optional<double>>& local,
        const std::vector<size_t>& starts,
        size_t window,
        size_t n,
        double bpm,
        const TempoOptions& options) {
    const auto rhythmicRatio = [](double ratio) {
        constexpr std::array<double, 4> kRatios{1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75};
        for (const double r : kRatios) {
            if (std::abs(ratio - r) < 0.01 * r) {
                return true;
            }
        }
        return false;
    };
    const auto differs = [&options, &rhythmicRatio](double ratio) {
        return std::abs(ratio - 1.0) > options.segmentThreshold &&
                !rhythmicRatio(ratio);
    };
    std::vector<std::pair<double, size_t>> clusters;
    for (const auto& ratio : local) {
        if (!ratio || !differs(*ratio)) {
            continue;
        }
        bool found = false;
        for (auto& [centre, count] : clusters) {
            if (std::abs(*ratio - centre) < options.segmentThreshold * centre * 0.5) {
                centre = (centre * static_cast<double>(count) + *ratio) /
                        (static_cast<double>(count) + 1.0);
                ++count;
                found = true;
                break;
            }
        }
        if (!found) {
            clusters.emplace_back(*ratio, 1);
        }
    }
    clusters.erase(std::remove_if(clusters.begin(),
                           clusters.end(),
                           [](const auto& cluster) {
                               return cluster.second < kSegmentMinWindows;
                           }),
            clusters.end());

    std::vector<std::optional<double>> assigned;
    assigned.reserve(local.size());
    for (const auto& ratio : local) {
        if (!ratio) {
            assigned.push_back(std::nullopt);
            continue;
        }
        std::optional<double> best;
        double bestDistance = std::numeric_limits<double>::infinity();
        const auto consider = [&](double candidate) {
            if (std::abs(*ratio - candidate) < options.segmentThreshold * 1.5) {
                const double distance = std::abs(*ratio - candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
        };
        consider(1.0);
        for (const auto& [centre, count] : clusters) {
            consider(centre);
        }
        assigned.push_back(best);
    }
    double previous = 1.0;
    for (const auto& value : assigned) {
        if (value) {
            previous = *value;
            break;
        }
    }
    std::vector<double> labels(local.size(), 1.0);
    for (size_t i = 0; i < assigned.size(); ++i) {
        if (assigned[i]) {
            previous = *assigned[i];
        }
        labels[i] = previous;
    }
    for (size_t i = 1; i + 1 < labels.size(); ++i) {
        if (std::abs(labels[i - 1] - labels[i + 1]) < 1e-9 &&
                std::abs(labels[i] - labels[i - 1]) > 1e-9) {
            labels[i] = labels[i - 1];
        }
    }
    while (true) {
        const std::vector<std::pair<size_t, size_t>> runs = runBounds(labels);
        if (runs.size() < 2) {
            break;
        }
        const auto settledWindows = [&assigned, &labels](std::pair<size_t, size_t> run) {
            size_t count = 0;
            for (size_t w = run.first; w < run.second; ++w) {
                if (w < assigned.size() && assigned[w] &&
                        std::abs(*assigned[w] - labels[run.first]) < 1e-9) {
                    ++count;
                }
            }
            return count;
        };
        size_t foundIndex = runs.size();
        for (size_t i = 0; i < runs.size(); ++i) {
            if (settledWindows(runs[i]) < kSegmentMinWindows) {
                foundIndex = i;
                break;
            }
        }
        if (foundIndex >= runs.size()) {
            break;
        }
        const auto [start, end] = runs[foundIndex];
        const double label = foundIndex == 0
                ? labels[std::min(end, labels.size() - 1)]
                : labels[start - 1];
        for (size_t l = start; l < end; ++l) {
            labels[l] = label;
        }
    }

    std::vector<Run> runs;
    size_t i = 0;
    while (i < labels.size()) {
        size_t end = i;
        while (end < labels.size() && std::abs(labels[end] - labels[i]) < 1e-9) {
            ++end;
        }
        const auto isSettled = [&assigned, &labels, i](size_t w) {
            return w < assigned.size() && assigned[w] &&
                    std::abs(*assigned[w] - labels[i]) < 1e-9;
        };
        size_t firstSettled = i;
        for (size_t w = i; w < end; ++w) {
            if (isSettled(w)) {
                firstSettled = w;
                break;
            }
        }
        size_t lastSettled = end > 0 ? end - 1 : 0;
        for (size_t w = end; w > i; --w) {
            if (isSettled(w - 1)) {
                lastSettled = w - 1;
                break;
            }
        }
        const size_t from = i == 0
                ? 0
                : (firstSettled < starts.size() ? starts[firstSettled] : 0);
        const size_t to = end >= labels.size()
                ? n
                : std::min(starts[lastSettled] + window, n);
        const size_t changeFrom = lastSettled < starts.size()
                ? starts[lastSettled]
                : from;
        runs.push_back(Run{from, to, bpm * labels[i], changeFrom});
        i = end;
    }
    if (runs.empty()) {
        runs.push_back(Run{0, n, bpm, n});
    }
    return runs;
}

std::vector<std::pair<size_t, size_t>> runBounds(const std::vector<double>& labels) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t i = 0;
    while (i < labels.size()) {
        size_t end = i;
        while (end < labels.size() && std::abs(labels[end] - labels[i]) < 1e-9) {
            ++end;
        }
        out.emplace_back(i, end);
        i = end;
    }
    return out;
}

double referenceBpm(const Reader& reader, double bpm, const TempoOptions& options) {
    double fitted = bpm;
    if (const auto f = fit(reader, bpm, 0, reader.values->size(), options);
            f && f->period > 0.0) {
        fitted = reader.rate * 60.0 / f->period;
    }
    if (std::abs(fitted / bpm - 1.0) <= options.segmentThreshold) {
        return fitted;
    }
    return bpm;
}

std::pair<double, size_t> kickCollected(const Reader& reader,
        double period,
        size_t from,
        size_t to) {
    if (period < 2.0 || to <= from) {
        return {0.0, 0};
    }
    std::pair<double, size_t> best{0.0, 0};
    for (size_t step = 0; step < kKickPhases; ++step) {
        const double phase = static_cast<double>(from) + period * static_cast<double>(step) / static_cast<double>(kKickPhases);
        double total = 0.0;
        size_t beats = 0;
        double x = phase;
        while (x < static_cast<double>(to)) {
            const Evidence heard = reader.evidence(x, period);
            total += reader.kicks ? heard.kick : heard.attack;
            ++beats;
            x += period;
        }
        if (total > best.first || best.second == 0) {
            best = {total, beats};
        }
    }
    return best;
}

double kickMargin(const Reader& reader,
        double own,
        double other,
        size_t from,
        size_t to) {
    const auto [here, beats] = kickCollected(reader, own, from, to);
    if (beats == 0 || here < kKickPresent * static_cast<double>(beats)) {
        return 0.0;
    }
    const auto [there, otherBeats] = kickCollected(reader, other, from, to);
    (void)otherBeats;
    if (there <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return here / there;
}

std::vector<Run> kickedRuns(const Reader& reader,
        std::vector<Run> runs,
        double bpm,
        const TempoOptions& options) {
    if (!reader.kicks && !reader.attacks) {
        return runs;
    }
    const auto sameTempo = [&options](double a, double b) {
        return std::abs(a / b - 1.0) <= options.segmentThreshold;
    };
    while (runs.size() > 1) {
        std::vector<double> periods;
        periods.reserve(runs.size());
        for (const Run& run : runs) {
            const auto f = fit(reader, run.bpm, run.from, run.to, options);
            periods.push_back(f ? f->period : reader.rate * 60.0 / run.bpm);
        }
        struct Weakest {
            size_t i;
            size_t j;
            double margin;
        };
        std::optional<Weakest> weakest;
        for (size_t i = 0; i < runs.size(); ++i) {
            const std::array<std::optional<size_t>, 2> neighbours{
                    i > 0 ? std::optional<size_t>(i - 1) : std::nullopt,
                    i + 1 < runs.size() ? std::optional<size_t>(i + 1) : std::nullopt};
            for (const auto& j : neighbours) {
                if (!j) {
                    continue;
                }
                const double margin = kickMargin(reader,
                        periods[i],
                        periods[*j],
                        runs[i].from,
                        runs[i].to);
                if (margin >= kKickMargin) {
                    continue;
                }
                bool worse = true;
                if (weakest) {
                    const auto away = [&runs, bpm](size_t k) {
                        return std::abs(runs[k].bpm / bpm - 1.0);
                    };
                    const auto length = [&runs](size_t k) {
                        return runs[k].to > runs[k].from
                                ? runs[k].to - runs[k].from
                                : size_t(0);
                    };
                    worse = margin < weakest->margin - 1e-9 ||
                            (std::abs(margin - weakest->margin) <= 1e-9 &&
                                    (away(i) > away(weakest->i) + 1e-9 ||
                                            (std::abs(away(i) - away(weakest->i)) <= 1e-9 &&
                                                    length(i) < length(weakest->i))));
                }
                if (worse) {
                    weakest = Weakest{i, *j, margin};
                }
            }
        }
        if (!weakest) {
            break;
        }
        const size_t i = weakest->i;
        const size_t j = weakest->j;
        const Run gone = runs[i];
        runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(i));
        if (j < i) {
            runs[j].to = gone.to;
            runs[j].changeFrom = gone.changeFrom;
        } else {
            runs[i].from = gone.from;
        }
        size_t k = 1;
        while (k < runs.size()) {
            if (sameTempo(runs[k].bpm, runs[k - 1].bpm)) {
                const Run later = runs[k];
                runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(k));
                runs[k - 1].to = later.to;
                runs[k - 1].changeFrom = later.changeFrom;
            } else {
                ++k;
            }
        }
    }
    return runs;
}

std::vector<std::pair<double, double>> walkBeats(const Reader& reader,
        const Fit& a,
        const Fit& b,
        size_t from,
        size_t to) {
    if (a.period < 2.0 || b.period < 2.0) {
        return {};
    }
    const double ratio = b.period / a.period;
    if (ratio < 0.6 || ratio > 1.67) {
        return {};
    }
    const double rate = reader.rate;
    const auto transients = [&reader, from, to]() {
        struct Transients {
            std::vector<double> values;
            size_t start = 0;
        };
        const size_t context = static_cast<size_t>(std::ceil(reader.rate));
        const size_t start = std::min(
                from > context ? from - context : 0, reader.values->size());
        const size_t end = std::min(
                std::max(to + context, start), reader.values->size());
        const double decay = std::exp(
                -1.0 / (reader.rate * kTransientReleaseSecs));
        double previous = start > 0 ? (*reader.values)[start - 1] : 0.0;
        double released = 0.0;
        Transients result;
        result.start = start;
        result.values.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
            const double value = (*reader.values)[i];
            const double attack = std::max(value - previous, 0.0) * kTransientGain;
            previous = value;
            released = std::max(attack, released * decay);
            result.values.push_back(released);
        }
        return result;
    }();
    const auto snap = [&reader, &transients](double x, double period, double reach)
            -> std::optional<std::pair<double, double>> {
        const Evidence evidence = reader.evidence(x, period);
        if (evidence.kick >= kKickPresent) {
            return reader.snap(x, reach);
        }
        if (reader.attacks) {
            if (const auto hit = reader.snap(x, reach)) {
                return hit;
            }
        }
        const double local = x - static_cast<double>(transients.start);
        const auto peaks = peaksIn(transients.values,
                local - reach,
                local + reach,
                kHitFloor);
        std::optional<std::pair<double, double>> best;
        for (const auto& peak : peaks) {
            if (!best || peak.second >= best->second) {
                best = peak;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        const auto at = localPeak(*reader.values,
                best->first + static_cast<double>(transients.start),
                1.0);
        if (!at || std::abs(at->first - x) > reach) {
            return std::nullopt;
        }
        return std::make_pair(at->first, best->second);
    };

    const double k = std::ceil(
            (static_cast<double>(from) - a.phase) / a.period);
    const double gridPoint = a.phase + k * a.period;
    const auto firstHit = snap(gridPoint, a.period, std::max(kRampReach * a.period, 1.0));
    if (!firstHit) {
        return {};
    }
    double t = firstHit->first;
    double period = a.period;
    std::vector<double> beats{t};
    size_t settledRun = 0;
    while (beats.size() < kMaxWalkedBeats && t < static_cast<double>(to)) {
        const double predicted = t + period;
        const auto next = snap(predicted, period, std::max(kRampReach * period, 1.0));
        if (!next) {
            break;
        }
        const double nextPeriod = next->first - t;
        if (nextPeriod <= 0.0 || std::abs(nextPeriod - period) > period * kRampStep) {
            break;
        }
        period = nextPeriod;
        t = next->first;
        beats.push_back(t);
        const double settledBeat = b.phase +
                std::round((t - b.phase) / b.period) * b.period;
        if (std::abs(period - b.period) <= b.period * kSettledTolerance &&
                std::abs(t - settledBeat) <= rate * 0.002) {
            ++settledRun;
            if (settledRun >= 4) {
                break;
            }
        } else {
            settledRun = 0;
        }
    }
    if (settledRun < 4) {
        return {};
    }
    std::vector<std::pair<double, double>> walked;
    walked.reserve(beats.size() - 1);
    for (size_t i = 0; i + 1 < beats.size(); ++i) {
        walked.emplace_back(beats[i], beats[i + 1]);
    }
    return walked;
}

double boundary(const Reader& reader,
        size_t from,
        size_t to,
        size_t settledFrom,
        size_t settledTo,
        const Fit& a,
        const Fit& b) {
    const size_t n = reader.values->size();
    const size_t lo = std::min(from, to);
    const size_t hi = std::min(std::max(from, to), n);
    if (hi <= lo || b.period < 2.0 || a.period < 2.0) {
        return static_cast<double>(lo);
    }
    const auto beatsOf = [&reader](const Fit& f, size_t lo, size_t hi) {
        std::vector<std::pair<double, Evidence>> out;
        const int64_t first = static_cast<int64_t>(std::ceil(
                (static_cast<double>(lo) - f.phase) / f.period));
        const int64_t last = static_cast<int64_t>(std::floor(
                (static_cast<double>(hi) - f.phase) / f.period));
        for (int64_t k = first; k <= last; ++k) {
            const double at = f.phase + static_cast<double>(k) * f.period;
            out.emplace_back(at, reader.evidence(at, f.period));
        }
        return out;
    };
    const std::vector<std::pair<double, Evidence>> beatsB = beatsOf(
            b, lo, std::max(hi, std::min(settledTo, n)));
    const std::vector<std::pair<double, Evidence>> beatsA = beatsOf(a, lo, hi);
    const auto meanSum = [](const std::vector<std::pair<double, Evidence>>& beats,
                                 double from,
                                 double to) {
        double sum = 0.0;
        size_t count = 0;
        for (const auto& [at, evidence] : beats) {
            if (at >= from && at < to) {
                sum += evidence.sum();
                ++count;
            }
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };

    const std::array<double, Evidence::kSources> level = settledLevels(
            beatsB,
            static_cast<double>(settledFrom),
            static_cast<double>(settledTo));
    std::optional<size_t> kickSource;
    if (level[0] >= kLiveSource) {
        kickSource = 0;
    } else if (level[1] >= kLiveSource) {
        kickSource = 1;
    }

    if (kickSource) {
        const size_t ks = *kickSource;
        const auto meanOver = [&beatsB, ks](size_t i, size_t count, size_t source) {
            if (i + count > beatsB.size()) {
                return 0.0;
            }
            double sum = 0.0;
            for (size_t j = i; j < i + count; ++j) {
                sum += beatsB[j].second.source(source);
            }
            return sum / static_cast<double>(count);
        };
        const auto bar = [&meanOver](size_t i, size_t source) {
            return meanOver(i, 4, source);
        };
        const std::vector<std::tuple<size_t, size_t, double>> runs =
                kickRuns(beatsB, ks);
        double strongest = 0.0;
        for (const auto& [start, end, runLevel] : runs) {
            strongest = std::max(strongest, runLevel);
        }
        for (const auto& [start, end, runLevel] : runs) {
            if (runLevel < kRunStrength * strongest ||
                    end < start + 4 * kBoundaryBars ||
                    beatsB[start].first >= static_cast<double>(hi)) {
                continue;
            }
            const size_t limit = std::min(start + 4 * kBoundaryBars, end);
            for (size_t i = start; i < limit; ++i) {
                if (i + 4 * kBoundaryBars > beatsB.size() ||
                        beatsB[i].first >= static_cast<double>(hi)) {
                    break;
                }
                const double thisBar = bar(i, ks);
                const double nextBar = bar(i + 4, ks);
                const double opening = midpoint(thisBar, nextBar);
                const bool notAFill = nextBar <= kFillRatio * thisBar;
                const bool onTheBeat = meanOver(i, 2, ks) >= 0.5 * opening;
                const bool withTheMix = level[2] < kLiveSource ||
                        bar(i, 2) >= kBoundaryStrength * level[2];
                const double cut = beatsB[i].first;
                const double span = 4.0 * static_cast<double>(kBoundaryBars) * b.period;
                const bool betterThanA = meanSum(beatsB, cut, cut + span) >=
                        meanSum(beatsA, cut, cut + span);
                if (!(notAFill && onTheBeat && withTheMix && betterThanA)) {
                    continue;
                }
                double strongestBeat = 0.0;
                for (size_t j = i; j < i + 4; ++j) {
                    strongestBeat = std::max(
                            strongestBeat, beatsB[j].second.source(ks));
                }
                const auto isKick = [&beatsB, ks, strongestBeat](size_t j) {
                    return beatsB[j].second.source(ks) >= kCutOnAKick * strongestBeat;
                };
                const auto hasMix = [&beatsB, &level](size_t j) {
                    return level[2] < kLiveSource ||
                            beatsB[j].second.flux >= kBoundaryStrength * level[2];
                };
                const auto hasClick = [&beatsB, &level](size_t j) {
                    return level[1] < kLiveSource ||
                            beatsB[j].second.attack >= kCutOnAKick * level[1];
                };
                size_t onAKick = i;
                bool found = false;
                for (size_t j = i; j < i + 4; ++j) {
                    if (isKick(j) && hasMix(j) && hasClick(j)) {
                        onAKick = j;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    for (size_t j = i; j < i + 4; ++j) {
                        if (isKick(j) && hasMix(j)) {
                            onAKick = j;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    for (size_t j = i; j < i + 4; ++j) {
                        if (isKick(j)) {
                            onAKick = j;
                            break;
                        }
                    }
                }
                return beatsB[onAKick].first;
            }
        }
    }

    // No reliable kick run: exaggerate the other transients where the kick
    // is absent so quiet percussion can say which grid owns the change.
    const size_t context = static_cast<size_t>(std::ceil(reader.rate));
    const size_t start = std::min(lo > context ? lo - context : 0, n);
    const size_t end = std::min(std::max(hi + context, start), n);
    const double decay = std::exp(-1.0 / (reader.rate * kTransientReleaseSecs));
    double previous = start > 0 ? (*reader.values)[start - 1] : 0.0;
    double released = 0.0;
    std::vector<double> emphasised;
    emphasised.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        const double value = (*reader.values)[i];
        const double attack = std::max(value - previous, 0.0) * kTransientGain;
        previous = value;
        released = std::max(attack, released * decay);
        emphasised.push_back(released);
    }
    const auto emphasise = [&reader, &emphasised, start](
                                   std::vector<std::pair<double, Evidence>>& beats,
                                   double period) {
        for (auto& [at, evidence] : beats) {
            if (evidence.kick < kKickPresent) {
                const double reach = std::max(period * 0.1, 1.0);
                const double local = at - static_cast<double>(start);
                const auto peaks = peaksIn(emphasised,
                        local - reach,
                        local + reach,
                        kHitFloor);
                std::optional<std::pair<double, double>> best;
                for (const auto& candidate : peaks) {
                    if (!best || candidate.second >= best->second) {
                        best = candidate;
                    }
                }
                double flux = 0.0;
                if (best) {
                    // Use emphasis to select the hit, but keep the original
                    // envelope's timestamp.
                    const auto original = localPeak(*reader.values,
                            best->first + static_cast<double>(start),
                            1.0);
                    if (original && std::abs(original->first - at) <= reach) {
                        flux = best->second;
                    }
                }
                evidence.flux = flux;
            }
        }
    };
    std::vector<std::pair<double, Evidence>> beatsAEmphasised = beatsA;
    std::vector<std::pair<double, Evidence>> beatsBEmphasised = beatsB;
    emphasise(beatsAEmphasised, a.period);
    emphasise(beatsBEmphasised, b.period);

    std::pair<double, double> best{-std::numeric_limits<double>::infinity(),
            static_cast<double>(hi)};
    for (const auto& [cut, evidence] : beatsBEmphasised) {
        if (cut >= static_cast<double>(hi) || evidence.sum() < kHitFloor) {
            continue;
        }
        double score = 0.0;
        for (const auto& [at, beatEvidence] : beatsAEmphasised) {
            if (at < cut) {
                score += beatEvidence.sum();
            }
        }
        for (const auto& [at, beatEvidence] : beatsBEmphasised) {
            if (at >= cut && at < static_cast<double>(hi)) {
                score += beatEvidence.sum();
            }
        }
        if (score > best.first) {
            best = {score, cut};
        }
    }
    return best.second;
}

std::array<double, Evidence::kSources> settledLevels(
        const std::vector<std::pair<double, Evidence>>& beats,
        double settledFrom,
        double settledTo) {
    std::vector<Evidence> settled;
    for (const auto& [at, evidence] : beats) {
        if (at >= settledFrom && at < settledTo) {
            settled.push_back(evidence);
        }
    }
    std::array<double, Evidence::kSources> level{};
    for (size_t source = 0; source < level.size(); ++source) {
        std::vector<double> means;
        if (settled.size() >= 4) {
            for (size_t i = 0; i + 4 <= settled.size(); ++i) {
                double sum = 0.0;
                for (size_t j = i; j < i + 4; ++j) {
                    sum += settled[j].source(source);
                }
                means.push_back(sum / 4.0);
            }
        } else {
            for (const Evidence& evidence : settled) {
                means.push_back(evidence.source(source));
            }
        }
        std::stable_sort(means.begin(), means.end());
        level[source] = means.empty() ? 0.0 : means[means.size() / 2];
    }
    return level;
}

std::vector<std::tuple<size_t, size_t, double>> kickRuns(
        const std::vector<std::pair<double, Evidence>>& beats, size_t ks) {
    const auto bar = [&beats, ks](size_t i) {
        if (i + 4 > beats.size()) {
            return 0.0;
        }
        double sum = 0.0;
        for (size_t j = i; j < i + 4; ++j) {
            sum += beats[j].second.source(ks);
        }
        return sum / 4.0;
    };
    std::vector<bool> present(beats.size(), false);
    for (size_t i = 0; i < beats.size(); ++i) {
        present[i] = i + 4 <= beats.size() && bar(i) >= kKickPresent;
    }
    std::vector<std::tuple<size_t, size_t, double>> runs;
    size_t i = 0;
    while (i < present.size()) {
        if (!present[i]) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < present.size() && present[i]) {
            ++i;
        }
        std::vector<double> bars;
        for (size_t j = start; j < i; ++j) {
            bars.push_back(bar(j));
        }
        std::stable_sort(bars.begin(), bars.end());
        runs.emplace_back(start, i, bars.empty() ? 0.0 : bars[bars.size() / 2]);
    }
    return runs;
}

std::vector<Segment> splitGaps(const Reader& reader,
        const std::vector<Segment>& segments,
        const TempoOptions& options) {
    const std::vector<double>& values = *reader.values;
    const double rate = reader.rate;
    const double originSecs = reader.originSecs;
    const size_t n = values.size();
    const auto xOf = [originSecs, rate](double secs) {
        return (secs - originSecs) * rate;
    };
    const auto toSecs = [&reader](double x) { return toSecsOf(reader, x); };
    std::vector<Segment> out;
    for (const Segment& segment : segments) {
        double from = std::max(xOf(segment.fromSecs), 0.0);
        const double to = std::min(xOf(segment.toSecs), static_cast<double>(n));
        Fit f{xOf(segment.phaseSecs), segment.periodSecs * rate};
        const auto line = [&toSecs, rate](double from, double to, const Fit& fit) {
            return Segment{toSecs(from),
                    toSecs(to),
                    fit.period / rate,
                    toSecs(fit.phase)};
        };
        size_t searched = 0;
        bool startRefitted = false;
        while (true) {
            if (f.period < 2.0 || to <= from) {
                break;
            }
            const std::vector<Point> hits = beatHits(
                    values, f, toSize(from), toSize(to));
            std::vector<double> heights;
            for (const auto& [at, hit, height] : hits) {
                if (height >= kHitFloor) {
                    heights.push_back(height);
                }
            }
            std::stable_sort(heights.begin(), heights.end());
            const double floorLevel = heights.empty()
                    ? kHitFloor
                    : std::max(heights[heights.size() / 2] * 0.2, kHitFloor);
            std::vector<bool> supported;
            supported.reserve(hits.size());
            for (const auto& [at, hit, height] : hits) {
                supported.push_back(height >= floorLevel);
            }
            if (!startRefitted && searched == 0) {
                startRefitted = true;
                if (const auto g = refitStart(reader,
                            f,
                            from,
                            to,
                            hits,
                            supported,
                            options)) {
                    f = *g;
                    continue;
                }
            }
            const auto gap = firstGap(supported, searched);
            if (!gap) {
                break;
            }

            const double after = gap->lastSupported + 1 < hits.size()
                    ? std::get<0>(hits[gap->lastSupported + 1])
                    : to;
            const double period = f.period;
            const auto sameTempo = [period, &options](const Fit& candidate) {
                return std::abs(candidate.period - period) <=
                        period * options.segmentThreshold;
            };
            const double bpmHere = rate * 60.0 / period;
            const double probe = gap->resumes && *gap->resumes < hits.size()
                    ? std::get<0>(hits[*gap->resumes])
                    : midpoint(after, to);
            const auto judged = judgeGap(
                    reader, f, from, after, to, probe, options);
            const double shift = judged ? std::get<2>(*judged) : 0.0;
            const bool moved = judged &&
                    std::abs(shift) > kPhaseTolerance * period &&
                    (!reader.kicks ||
                            kickSupport(reader,
                                    std::get<1>(*judged),
                                    after,
                                    to) >
                                    kickSupport(reader,
                                            std::get<0>(*judged),
                                            after,
                                            to) *
                                            kKickMargin);
            const Fit before = judged ? std::get<0>(*judged) : f;
            std::optional<Fit> refit;
            if (judged) {
                refit = std::get<1>(*judged);
            }
            const auto nearest = [](const Fit& g, double x) {
                return g.phase +
                        std::round((x - g.phase) / g.period) * g.period;
            };
            double lastHit = 0.0;
            if (const auto supportedHit = lastSupportedHit(
                        values, before, from, after, floorLevel)) {
                lastHit = *supportedHit;
            } else {
                const double lastBeat = nearest(
                        before, std::get<0>(hits[gap->lastSupported]));
                const auto peak = localPeak(
                        values, lastBeat, std::max(before.period * 0.1, 1.0));
                lastHit = peak ? peak->first : lastBeat;
            }
            f = before;
            if (!moved) {
                if (!gap->resumes) {
                    break;
                }
                searched = *gap->resumes;
                continue;
            }
            if (!refit) {
                break;
            }
            const auto cutOption = resumeOf(
                    values, *refit, after, to, floorLevel * kResumeFraction);
            if (!cutOption) {
                break;
            }
            double cut = *cutOption;
            Fit resumed = *refit;
            if (const auto fitted = fit(reader,
                        bpmHere,
                        toSize(cut),
                        toSize(to),
                        options);
                    fitted && sameTempo(*fitted)) {
                if (const auto onKicks = onTheKicks(reader, *fitted, cut, to)) {
                    resumed = *onKicks;
                }
            }
            cut = nearest(resumed, cut);

            const auto [walked, growth] = walkRamp(
                    values, lastHit, f.period, cut);
            double departure = 0.0;
            for (size_t i = 0; i + 1 < walked.size(); ++i) {
                departure = std::max(departure,
                        std::abs((walked[i + 1] - walked[i]) / f.period - 1.0));
            }
            const bool ramp = walked.size() >= kRampMinBeats &&
                    departure >= kRampDeparture && oneWay(walked);
            if (ramp) {
                if (walked.front() - f.period / 2.0 > from) {
                    out.push_back(line(from, walked.front() - f.period / 2.0, f));
                }
                const std::vector<Segment> rampSegmentsResult = rampSegments(
                        walked, growth, cut, rate, originSecs);
                out.insert(out.end(),
                        rampSegmentsResult.begin(),
                        rampSegmentsResult.end());
            } else if (cut - f.period / 2.0 > from) {
                out.push_back(line(from, cut - f.period / 2.0, f));
            }
            from = cut;
            f = resumed;
            searched = 0;
        }
        if (to > from) {
            out.push_back(line(from, to, f));
        }
    }
    return out;
}

std::vector<Point> beatHits(std::span<const double> values,
        const Fit& f,
        size_t from,
        size_t to) {
    if (f.period < 2.0 || to <= from) {
        return {};
    }
    const double reach = std::max(f.period * 0.1, 1.0);
    const int64_t first = static_cast<int64_t>(std::ceil(
            (static_cast<double>(from) - f.phase) / f.period));
    const int64_t last = static_cast<int64_t>(std::floor(
            (static_cast<double>(to) - 1.0 - f.phase) / f.period));
    std::vector<Point> out;
    for (int64_t k = first; k <= last; ++k) {
        const double at = f.phase + static_cast<double>(k) * f.period;
        const auto peaks = peaksIn(values, at - reach, at + reach, kHitFloor);
        std::optional<std::pair<double, double>> best;
        for (const auto& peak : peaks) {
            if (!best || peak.second >= best->second) {
                best = peak;
            }
        }
        if (best) {
            out.emplace_back(at, best->first, best->second);
        } else {
            out.emplace_back(at, at, 0.0);
        }
    }
    return out;
}

double kickSupport(const Reader& reader, const Fit& f, double from, double to) {
    const OnsetEnvelope* kicks = reader.kicks;
    if (!kicks) {
        return 0.0;
    }
    if (f.period < 2.0 || kicks->rate <= 0.0 || kicks->empty()) {
        return 0.0;
    }
    const double reachSecs = f.period * 0.1 / reader.rate;
    const int64_t first = static_cast<int64_t>(std::ceil((from - f.phase) / f.period));
    const int64_t last = static_cast<int64_t>(std::floor((to - f.phase) / f.period));
    double total = 0.0;
    for (int64_t k = first; k <= last; ++k) {
        const double secs = reader.originSecs +
                (f.phase + static_cast<double>(k) * f.period) / reader.rate;
        const double x = (secs - kicks->originSecs) * kicks->rate;
        const size_t lo = toSize(std::floor(x - reachSecs * kicks->rate));
        const size_t hi = std::min(
                toSize(std::ceil(x + reachSecs * kicks->rate)),
                kicks->size() - 1);
        double best = 0.0;
        for (size_t i = lo; i <= hi && i < kicks->size(); ++i) {
            best = std::max(best, static_cast<double>(kicks->values[i]));
        }
        total += best;
    }
    return total;
}

std::optional<Fit> onTheKicks(const Reader& reader,
        const Fit& f,
        double from,
        double to) {
    if (!reader.kicks) {
        return f;
    }
    Fit moved = f;
    moved.phase += f.period / 2.0;
    const double here = kickSupport(reader, f, from, to);
    const double there = kickSupport(reader, moved, from, to);
    if (there > here * kKickMargin) {
        return moved;
    }
    if (here > there * kKickMargin) {
        return f;
    }
    return std::nullopt;
}

std::optional<std::tuple<Fit, Fit, double>> judgeGap(const Reader& reader,
        const Fit& f,
        double from,
        double after,
        double to,
        double probe,
        const TempoOptions& options) {
    const double rate = reader.rate;
    const double period = f.period;
    const auto sameTempo = [period, &options](const Fit& b) {
        return std::abs(b.period - period) <= period * options.segmentThreshold;
    };
    const auto nearest = [](const Fit& g, double x) {
        return g.phase + std::round((x - g.phase) / g.period) * g.period;
    };
    const auto ownPeriod = [period, &nearest](const Fit& g, double from, double to) {
        if ((to - from) / g.period >= static_cast<double>(kMinStretchBeats)) {
            return g;
        }
        return Fit{nearest(g, midpoint(from, to)), period};
    };
    if ((to - after) / period < static_cast<double>(kMinStretchBeats)) {
        return std::nullopt;
    }
    const double bpmHere = rate * 60.0 / period;
    Fit before = f;
    if (const auto fitted = fit(reader,
                bpmHere,
                toSize(from),
                toSize(after),
                options);
            fitted && sameTempo(*fitted)) {
        before = ownPeriod(*fitted, from, after);
    }
    const auto beforeOnKicks = onTheKicks(reader, before, from, after);
    if (!beforeOnKicks) {
        return std::nullopt;
    }
    before = *beforeOnKicks;
    std::optional<Fit> refit;
    if (const auto fitted = fit(reader,
                bpmHere,
                toSize(after),
                toSize(to),
                options);
            fitted && sameTempo(*fitted)) {
        if (const auto onKicks = onTheKicks(
                    reader, ownPeriod(*fitted, after, to), after, to)) {
            refit = *onKicks;
        }
    }
    if (!refit) {
        return std::nullopt;
    }
    const double d = nearest(*refit, probe) - nearest(before, probe);
    return std::make_tuple(before, *refit, d - std::round(d / period) * period);
}

std::optional<Gap> firstGap(const std::vector<bool>& supported, size_t from) {
    const auto runEndsAt = [&supported](size_t i) {
        for (size_t d = 0; d < kSupportedRun; ++d) {
            if (i < d || !supported[i - d]) {
                return false;
            }
        }
        return true;
    };
    const auto runStartsAt = [&supported](size_t i) {
        for (size_t d = 0; d < kGapBeats; ++d) {
            if (i + d >= supported.size() || !supported[i + d]) {
                return false;
            }
        }
        return true;
    };
    size_t i = from;
    while (i < supported.size()) {
        if (supported[i]) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < supported.size() && !supported[i]) {
            ++i;
        }
        if (i - start < kGapBeats) {
            continue;
        }
        std::optional<size_t> lastSupported;
        for (size_t j = start; j > from; --j) {
            if (runEndsAt(j - 1)) {
                lastSupported = j - 1;
                break;
            }
        }
        if (!lastSupported) {
            continue;
        }
        std::optional<size_t> resumes;
        for (size_t j = i; j < supported.size(); ++j) {
            if (runStartsAt(j)) {
                resumes = j;
                break;
            }
        }
        return Gap{*lastSupported, resumes};
    }
    return std::nullopt;
}

std::vector<std::pair<double, double>> peaksIn(std::span<const double> values,
        double lo,
        double hi,
        double floor) {
    const size_t first = std::max(toSize(std::floor(lo)), size_t(1));
    const size_t last = std::min(
            toSize(std::ceil(hi)),
            values.size() > 2 ? values.size() - 2 : size_t(0));
    std::vector<std::pair<double, double>> out;
    for (size_t i = first; i <= last && i + 1 < values.size(); ++i) {
        const double a = values[i - 1];
        const double b = values[i];
        const double c = values[i + 1];
        if (b < floor || b <= a || b < c) {
            continue;
        }
        const double denom = a - 2.0 * b + c;
        const double offset = std::abs(denom) > std::numeric_limits<double>::epsilon()
                ? 0.5 * (a - c) / denom
                : 0.0;
        out.emplace_back(static_cast<double>(i) + clampOffset(offset), b);
    }
    return out;
}

std::pair<std::vector<double>, double> walkRamp(std::span<const double> values,
        double start,
        double period,
        double until) {
    std::vector<double> beats{start};
    const auto firstPeak = localPeak(values, start, 1.0);
    std::vector<double> heights{firstPeak ? firstPeak->second : kHitFloor};
    double t = start;
    double p = period;
    double g = 1.0;
    while (beats.size() < kMaxWalkedBeats) {
        const double lo = t + kRampWindowMin * p * std::min(g, 1.0);
        const double hi = t + kRampWindowMax * p * std::max(g, 1.0);
        if (lo >= until) {
            break;
        }
        const double predicted = t + p * g;
        double mean = 0.0;
        const size_t count = std::min(heights.size(), kRampMemory);
        for (size_t i = 0; i < count; ++i) {
            mean += heights[heights.size() - 1 - i];
        }
        mean /= static_cast<double>(count);
        const double floorLevel = std::max(mean * kRampFade, kHitFloor);
        const double reach = (hi - lo) / 2.0;
        const auto score = [predicted, reach](const std::pair<double, double>& peak) {
            return peak.second /
                    (1.0 + std::abs(peak.first - predicted) / reach);
        };
        const auto peaks = peaksIn(values, lo, hi, floorLevel);
        std::optional<std::pair<double, double>> next;
        for (const auto& peak : peaks) {
            if (!next || score(peak) >= score(*next)) {
                next = peak;
            }
        }
        if (!next) {
            break;
        }
        const double nextPeriod = next->first - t;
        const double ratio = nextPeriod / p;
        if (ratio < kRampRatioMin || ratio > kRampRatioMax) {
            break;
        }
        if (std::abs(next->first - until) <= nextPeriod * 0.1 ||
                next->first >= until) {
            break;
        }
        beats.push_back(next->first);
        heights.push_back(next->second);
        t = next->first;
        p = nextPeriod;
        g = ratio;
    }
    return {beats, g};
}

bool oneWay(const std::vector<double>& beats) {
    std::vector<double> periods;
    for (size_t i = 0; i + 1 < beats.size(); ++i) {
        periods.push_back(beats[i + 1] - beats[i]);
    }
    if (periods.empty()) {
        return false;
    }
    const bool up = periods.back() > periods.front();
    size_t against = 0;
    for (size_t i = 0; i + 1 < periods.size(); ++i) {
        if (up ? periods[i + 1] < periods[i] * (1.0 - kRampWobble)
               : periods[i + 1] > periods[i] * (1.0 + kRampWobble)) {
            ++against;
        }
    }
    return against * 4 < periods.size();
}

std::vector<Segment> rampSegments(const std::vector<double>& walked,
        double growth,
        double cut,
        double rate,
        double originSecs) {
    const auto toSecs = [originSecs, rate](double x) {
        return originSecs + x / rate;
    };
    const auto beat = [&toSecs, rate](double start, double end) {
        return Segment{toSecs(start),
                toSecs(end),
                (end - start) / rate,
                toSecs(start)};
    };
    std::vector<Segment> out;
    for (size_t i = 0; i + 1 < walked.size(); ++i) {
        out.push_back(beat(walked[i], walked[i + 1]));
    }
    if (walked.size() < 2) {
        return out;
    }
    const double last = walked.back();
    const double lastPeriod = walked[walked.size() - 1] - walked[walked.size() - 2];
    const double remaining = cut - last;
    if (remaining > 0.0) {
        const size_t count = std::max(
                toSize(std::round(remaining / (lastPeriod * growth))), size_t(1));
        const double step = remaining / static_cast<double>(count);
        for (size_t i = 0; i < count; ++i) {
            const double start = last + static_cast<double>(i) * step;
            out.push_back(beat(start, start + step));
        }
    }
    return out;
}

std::optional<double> lastSupportedHit(std::span<const double> values,
        const Fit& f,
        double from,
        double until,
        double floor) {
    const std::vector<Point> hits = beatHits(
            values, f, toSize(from), toSize(until + f.period * 2.0));
    std::vector<bool> supported;
    supported.reserve(hits.size());
    for (const auto& [at, hit, height] : hits) {
        supported.push_back(height >= floor && at < until);
    }
    for (size_t i = supported.size(); i > 0; --i) {
        bool ok = true;
        for (size_t d = 0; d < kSupportedRun; ++d) {
            if (i - 1 < d || !supported[i - 1 - d]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return std::get<1>(hits[i - 1]);
        }
    }
    return std::nullopt;
}

std::optional<double> resumeOf(std::span<const double> values,
        const Fit& b,
        double from,
        double to,
        double floor) {
    const std::vector<Point> hits = beatHits(values, b, toSize(from), toSize(to));
    std::vector<bool> supported;
    supported.reserve(hits.size());
    for (const auto& [at, hit, height] : hits) {
        supported.push_back(height >= floor);
    }
    for (size_t i = 0; i < supported.size(); ++i) {
        bool ok = true;
        for (size_t d = 0; d < kGapBeats; ++d) {
            if (i + d >= supported.size() || !supported[i + d]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return std::get<0>(hits[i]);
        }
    }
    return std::nullopt;
}

std::optional<Fit> refitStart(const Reader& reader,
        const Fit& f,
        double from,
        double to,
        const std::vector<Point>& hits,
        const std::vector<bool>& supported,
        const TempoOptions& options) {
    size_t leading = 0;
    while (leading < supported.size() && !supported[leading]) {
        ++leading;
    }
    if (leading < kGapBeats) {
        return std::nullopt;
    }
    std::optional<size_t> resumes;
    for (size_t i = 0; i < supported.size(); ++i) {
        bool ok = true;
        for (size_t d = 0; d < kGapBeats; ++d) {
            if (i + d >= supported.size() || !supported[i + d]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            resumes = i;
            break;
        }
    }
    const double until = resumes && *resumes < hits.size()
            ? std::get<0>(hits[*resumes])
            : to;
    const double period = f.period;
    const auto fitted = fit(reader,
            reader.rate * 60.0 / period,
            toSize(from),
            toSize(until),
            options);
    if (!fitted ||
            std::abs(fitted->period - period) > period * options.segmentThreshold) {
        return std::nullopt;
    }
    const auto onKicks = onTheKicks(reader, *fitted, from, until);
    if (!onKicks) {
        return std::nullopt;
    }
    const double d = onKicks->phase - f.phase;
    if (std::abs(d - std::round(d / period) * period) > kPhaseTolerance * period) {
        return onKicks;
    }
    return std::nullopt;
}

} // namespace

double Segment::startSecs() const {
    if (periodSecs <= 0.0) {
        return fromSecs;
    }
    const double k = std::ceil((fromSecs - phaseSecs) / periodSecs - 1e-6);
    return phaseSecs + k * periodSecs;
}

size_t Segment::beats() const {
    if (periodSecs <= 0.0 || toSecs <= fromSecs) {
        return 0;
    }
    const double first = startSecs();
    if (first >= toSecs) {
        return 0;
    }
    return static_cast<size_t>(
            std::ceil((toSecs - first) / periodSecs - 1e-9));
}

Segment Segment::shiftedHalfBeat() const {
    Segment shifted = *this;
    shifted.phaseSecs = phaseSecs + periodSecs / 2.0;
    return shifted;
}

std::vector<Beat> beatsOf(const std::vector<Segment>& segments, size_t phase) {
    std::vector<Beat> beats;
    for (size_t index = 0; index < segments.size(); ++index) {
        const Segment& segment = segments[index];
        const uint16_t tempoX100 = toTempoX100(segment.bpm());
        const double start = segment.startSecs();
        size_t count = std::min(segment.beats(), size_t(100000));
        if (index + 1 < segments.size()) {
            const double last = start +
                    (count > 0 ? static_cast<double>(count - 1) : 0.0) *
                            segment.periodSecs;
            if (count > 0 &&
                    segments[index + 1].startSecs() - last <
                            kSameHit * segment.periodSecs) {
                --count;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            const double time = start + static_cast<double>(i) * segment.periodSecs;
            beats.push_back(Beat{
                    static_cast<uint16_t>((beats.size() + phase) % 4 + 1),
                    tempoX100,
                    toTimeMs(time)});
        }
    }
    return beats;
}

TempoResult detectTempo(const OnsetEnvelope& onsets,
        const OnsetEnvelope* kicks,
        const AttackMap* attacks,
        const TempoOptions& options) {
    if (onsets.size() < 64 || onsets.rate <= 0.0) {
        return TempoResult::empty();
    }
    std::vector<double> values(onsets.size());
    for (size_t i = 0; i < onsets.size(); ++i) {
        values[i] = static_cast<double>(onsets.values[i]);
    }
    const AttackMap* effectiveAttacks =
            options.placement == Placement::Attack ? attacks : nullptr;
    const OnsetEnvelope* effectiveKicks =
            kicks && kicks->rate > 0.0 && !kicks->empty() ? kicks : nullptr;
    const Reader reader{&values,
            onsets.rate,
            onsets.originSecs,
            effectiveAttacks,
            effectiveKicks};

    const std::vector<Candidate> candidatesList = candidates(values, onsets.rate, options);
    if (candidatesList.empty()) {
        return TempoResult::empty();
    }
    const Candidate& best = candidatesList.front();
    double rival = 0.0;
    for (size_t i = 1; i < candidatesList.size(); ++i) {
        if (!related(candidatesList[i].bpm, best.bpm)) {
            rival = candidatesList[i].acf * candidatesList[i].fourier;
            break;
        }
    }
    const double own = best.acf * best.fourier;
    const double confidence = own > 0.0
            ? std::clamp((own - rival) / own, 0.0, 1.0)
            : 0.0;

    TempoResult result;
    result.segments = segment(reader, best.bpm, options);
    result.beats = beatsOf(result.segments, 0);
    if (!result.segments.empty()) {
        result.bpm = result.segments.front().bpm();
        result.firstBeatSecs = result.segments.front().startSecs();
    }
    result.confidence = confidence;
    return result;
}

} // namespace mixxx::rbxport
