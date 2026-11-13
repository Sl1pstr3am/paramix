#include "analyzer/plugins/rbxport/grid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "analyzer/plugins/rbxport/attack.h"
#include "analyzer/plugins/rbxport/downbeat.h"
#include "analyzer/plugins/rbxport/onset.h"

namespace mixxx::rbxport {
namespace {

double remEuclid(double value, double modulus) {
    if (modulus <= 0.0) {
        return 0.0;
    }
    double result = std::fmod(value, modulus);
    if (result < 0.0) {
        result += modulus;
    }
    return result;
}

/// Beats before the first downbeat, in `0..4`, for a grid whose downbeat is
/// nearest `downbeatSecs`.
size_t phaseFor(const std::vector<Segment>& segments, double downbeatSecs) {
    const std::vector<Beat> unnumbered = beatsOf(segments, 0);
    if (unnumbered.empty()) {
        return 0;
    }
    size_t index = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < unnumbered.size(); ++i) {
        const double distance = std::abs(
                static_cast<double>(unnumbered[i].timeMs) / 1000.0 - downbeatSecs);
        if (distance < bestDistance) {
            bestDistance = distance;
            index = i;
        }
    }
    return (4 - index % 4) % 4;
}

/// Estimate how far the opening beat precedes the file.
double openingCutSecs(const Segment& first,
        const std::vector<float>& samples,
        uint32_t sampleRate) {
    const double next = first.startSecs();
    const double inferred = first.periodSecs - next;
    if (inferred <= 0.0 || inferred > 0.035) {
        return 0.0;
    }
    const double rate = static_cast<double>(sampleRate);
    const size_t lo = static_cast<size_t>(
            std::max(next - 0.015, 0.0) * rate);
    const double hiValue = (next + 0.015) * rate;
    const size_t hi = std::min(
            hiValue <= 0.0 ? size_t(0) : static_cast<size_t>(hiValue),
            samples.size());
    if (lo >= hi) {
        return inferred;
    }
    float peak = 0.0f;
    for (size_t i = lo; i < hi; ++i) {
        peak = std::max(peak, std::abs(samples[i]));
    }
    const float threshold = peak * 0.01f;
    std::optional<size_t> index;
    for (size_t i = lo; i < hi; ++i) {
        if (std::abs(samples[i]) > threshold) {
            index = i;
            break;
        }
    }
    if (!index) {
        return inferred;
    }
    const size_t firstSample = std::max(
            static_cast<size_t>(sampleRate) / 1000, size_t(1));
    if (*index - lo < firstSample || peak < 1e-5f) {
        return inferred;
    }
    const double now = static_cast<double>(std::abs(samples[*index]));
    const double previous = static_cast<double>(std::abs(samples[*index - 1]));
    const double onsetSample = std::round(
            static_cast<double>(*index) - now / (now - previous));
    return first.periodSecs - onsetSample / rate;
}

/// Restore a boundary beat lost when the fit falls just before zero.
void anchorFileStart(TempoResult& tempo,
        const std::vector<float>& samples,
        uint32_t sampleRate,
        double downbeatSecs) {
    if (tempo.segments.empty()) {
        return;
    }
    const Segment first = tempo.segments.front();
    const double reach = AttackOptions{}.reachSecs;
    if (sampleRate == 0 || first.fromSecs > reach ||
            first.periodSecs <= 0.0 || tempo.beats.empty()) {
        return;
    }
    const double cut = openingCutSecs(first, samples, sampleRate);
    const double bar = 4.0 * first.periodSecs;
    const double downbeatPhase = remEuclid(
            downbeatSecs + std::max(cut, 0.0), bar);
    if (downbeatSecs > first.startSecs() + reach &&
            std::min(downbeatPhase, bar - downbeatPhase) > reach) {
        return;
    }
    const double phase = remEuclid(first.phaseSecs, first.periodSecs);
    const double sampleSecs = 0.5 / static_cast<double>(sampleRate);
    const bool clipped = cut > sampleSecs;
    if ((clipped && cut >= 0.020 - sampleSecs) ||
            (!clipped &&
                    std::min(phase, first.periodSecs - phase) > reach)) {
        return;
    }
    const size_t opening = std::max(
            static_cast<size_t>(sampleRate) / 1000, size_t(1));
    const size_t window = static_cast<size_t>(
            std::ceil(static_cast<double>(sampleRate) * reach));
    const auto peak = [&samples](size_t count) {
        float result = 0.0f;
        for (size_t i = 0; i < count && i < samples.size(); ++i) {
            result = std::max(result, std::abs(samples[i]));
        }
        return result;
    };
    const float localPeak = peak(window);
    if (localPeak < 1e-5f || peak(opening) < localPeak * 0.1f) {
        return;
    }
    if (clipped) {
        const double next = first.startSecs();
        Segment openingSegment{0.0, next, first.periodSecs, 0.0};
        tempo.segments.front().fromSecs = next;
        tempo.segments.insert(tempo.segments.begin(), openingSegment);
        tempo.firstBeatSecs = 0.0;
        tempo.beats.insert(tempo.beats.begin(),
                Beat{1, tempo.beats.front().tempoX100, 0});
        for (size_t i = 0; i < tempo.beats.size(); ++i) {
            tempo.beats[i].beatNumber = static_cast<uint16_t>(i % 4 + 1);
        }
        return;
    }
    tempo.segments.front().fromSecs = 0.0;
    tempo.segments.front().phaseSecs = 0.0;
    tempo.firstBeatSecs = 0.0;
    tempo.beats = beatsOf(tempo.segments, 0);
}

} // namespace

TempoResult analyseGrid(const std::vector<float>& samples,
        uint32_t sampleRate,
        const TempoOptions& options) {
    if (samples.empty() || sampleRate == 0) {
        return TempoResult::empty();
    }
    const OnsetEnvelope onsets = onsetEnvelope(samples, sampleRate);
    const OnsetEnvelope kicks = onsetEnvelopeBand(samples, sampleRate, Band::LOW);
    std::optional<AttackMap> attacks;
    if (options.placement == Placement::Attack) {
        attacks.emplace(samples, sampleRate, AttackOptions{});
    }
    const BandFrames frames = bandFrames(samples, sampleRate);

    TempoResult tempo = detectTempo(onsets,
            kicks.empty() ? nullptr : &kicks,
            attacks ? &*attacks : nullptr,
            options);
    if (tempo.beats.empty()) {
        return tempo;
    }

    std::vector<double> beatSecs;
    beatSecs.reserve(tempo.beats.size());
    for (const Beat& beat : tempo.beats) {
        beatSecs.push_back(static_cast<double>(beat.timeMs) / 1000.0);
    }
    const GridPhase grid = gridPhaseFrom(frames, sampleRate, beatSecs);
    if (grid.halfBeatOff) {
        for (Segment& segment : tempo.segments) {
            segment = segment.shiftedHalfBeat();
        }
        tempo.firstBeatSecs = tempo.segments.empty()
                ? 0.0
                : tempo.segments.front().startSecs();
    }

    double downbeatSecs = grid.downbeatSecs;
    if (tempo.segments.size() > 1 &&
            tempo.segments.front().beats() >= kMinBeatsForOwnPhase) {
        const std::vector<Segment> ownSegments{tempo.segments.front()};
        std::vector<double> own;
        for (const Beat& beat : beatsOf(ownSegments, 0)) {
            own.push_back(static_cast<double>(beat.timeMs) / 1000.0);
        }
        downbeatSecs = gridPhaseFrom(frames, sampleRate, own).downbeatSecs;
    }
    tempo.beats = beatsOf(tempo.segments, phaseFor(tempo.segments, downbeatSecs));
    anchorFileStart(tempo, samples, sampleRate, downbeatSecs);
    return tempo;
}

} // namespace mixxx::rbxport
