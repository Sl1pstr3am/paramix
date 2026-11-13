#include "analyzer/plugins/rbxport/downbeat.h"

#include <dsp/transforms/FFT.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mixxx::rbxport {
namespace {

/// Frame and hop of the spectral profiles (~46 ms and ~12 ms at 44.1 kHz).
constexpr size_t kFrame = 2048;
constexpr size_t kHop = 512;

/// The band edges of a profile, in hertz.
constexpr std::array<double, kDownbeatBands + 1> kBandEdges{
        40.0, 65.0, 100.0, 160.0, 250.0, 400.0, 630.0, 1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0};

/// The bar lengths novelty is measured over, in half beats.
constexpr std::array<size_t, 4> kScales{8, 16, 32, 64};

/// Minimum four-bar change in the Euclidean distance of mean log-band
/// energies.
constexpr double kMinStructureNovelty = 0.5;

/// Half beats in a bar.
constexpr size_t kPositions = 8;

/// A novelty peak counts as a phrase start from this share of the
/// strongest peak in the track.
constexpr double kMinPhrasePeak = 0.3;

constexpr double kPi = 3.14159265358979323846;

using Profile = std::array<double, kDownbeatBands>;

std::vector<Profile> beatProfilesFrom(const BandFrames& frames,
        uint32_t sampleRate,
        const std::vector<double>& beatSecs) {
    if (frames.empty()) {
        return {};
    }
    const double frameRate = static_cast<double>(sampleRate) /
            static_cast<double>(kHop);
    std::vector<Profile> out;
    out.reserve(beatSecs.size());
    for (size_t i = 0; i < beatSecs.size(); ++i) {
        const double start = beatSecs[i];
        const double end = i + 1 < beatSecs.size()
                ? beatSecs[i + 1]
                : start + (beatSecs.size() > 1 ? beatSecs[1] - beatSecs[0] : 0.5);
        const size_t first = start <= 0.0
                ? 0
                : static_cast<size_t>(std::floor(start * frameRate));
        const double lastValue = std::ceil(end * frameRate);
        const size_t last = std::min(
                lastValue <= 0.0 ? size_t(0) : static_cast<size_t>(lastValue),
                frames.size());
        Profile profile{};
        if (first >= last) {
            out.push_back(profile);
            continue;
        }
        for (size_t f = first; f < last; ++f) {
            for (size_t b = 0; b < kDownbeatBands; ++b) {
                profile[b] += frames[f][b];
            }
        }
        const double count = static_cast<double>(last - first);
        for (double& slot : profile) {
            slot /= count;
        }
        out.push_back(profile);
    }
    return out;
}

} // namespace

BandFrames bandFrames(const std::vector<float>& samples, uint32_t sampleRate) {
    if (samples.size() < kFrame) {
        return {};
    }
    FFTReal fft(static_cast<int>(kFrame));
    std::vector<double> input(kFrame);
    std::vector<double> realOut(kFrame);
    std::vector<double> imagOut(kFrame);
    std::vector<double> window(kFrame);
    for (size_t i = 0; i < kFrame; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kFrame));
    }

    const size_t bins = kFrame / 2 + 1;
    const double hzPerBin = static_cast<double>(sampleRate) /
            static_cast<double>(kFrame);
    std::vector<int> bandOf(bins, -1);
    for (size_t bin = 0; bin < bins; ++bin) {
        const double hz = static_cast<double>(bin) * hzPerBin;
        for (size_t edge = 0; edge + 1 < kBandEdges.size(); ++edge) {
            if (hz >= kBandEdges[edge] && hz < kBandEdges[edge + 1]) {
                bandOf[bin] = static_cast<int>(edge);
                break;
            }
        }
    }

    const size_t count = (samples.size() - kFrame) / kHop + 1;
    BandFrames frames;
    frames.reserve(count);
    for (size_t f = 0; f < count; ++f) {
        const size_t start = f * kHop;
        for (size_t i = 0; i < kFrame; ++i) {
            input[i] = static_cast<double>(samples[start + i]) * window[i];
        }
        fft.forward(input.data(), realOut.data(), imagOut.data());
        Profile energy{};
        for (size_t bin = 0; bin < bins; ++bin) {
            if (bandOf[bin] >= 0) {
                const double re = realOut[bin];
                const double im = imagOut[bin];
                energy[static_cast<size_t>(bandOf[bin])] += re * re + im * im;
            }
        }
        Profile profile{};
        for (size_t b = 0; b < kDownbeatBands; ++b) {
            profile[b] = std::log(energy[b] + 1e-6);
        }
        frames.push_back(profile);
    }
    return frames;
}

std::vector<std::pair<size_t, double>> noveltyPeaks(const BandFrames& profiles,
        size_t scale) {
    const size_t n = profiles.size();
    if (scale == 0 || n < scale * 2) {
        return {};
    }
    std::vector<Profile> prefix(n + 1);
    for (size_t i = 0; i < n; ++i) {
        for (size_t b = 0; b < kDownbeatBands; ++b) {
            prefix[i + 1][b] = prefix[i][b] + profiles[i][b];
        }
    }
    const auto meanOver = [&prefix](size_t from, size_t to) {
        Profile out{};
        const double len = static_cast<double>(to - from);
        for (size_t b = 0; b < kDownbeatBands; ++b) {
            out[b] = (prefix[to][b] - prefix[from][b]) / len;
        }
        return out;
    };
    std::vector<double> novelty(n, 0.0);
    for (size_t i = scale; i + scale <= n; ++i) {
        const Profile before = meanOver(i - scale, i);
        const Profile after = meanOver(i, i + scale);
        double d = 0.0;
        for (size_t b = 0; b < kDownbeatBands; ++b) {
            const double diff = after[b] - before[b];
            d += diff * diff;
        }
        novelty[i] = std::sqrt(d);
    }
    std::vector<std::pair<size_t, double>> peaks;
    for (size_t i = 0; i < n; ++i) {
        const double left = i > 0 ? novelty[i - 1] : 0.0;
        const double right = i + 1 < n ? novelty[i + 1] : 0.0;
        if (novelty[i] > left && novelty[i] >= right && novelty[i] > 0.0) {
            peaks.emplace_back(i, novelty[i]);
        }
    }
    return peaks;
}

std::vector<double> positionScores(const BandFrames& profiles,
        size_t positions,
        const std::vector<size_t>& scales) {
    const size_t n = profiles.size();
    std::vector<double> scores(std::max(positions, size_t(1)), 0.0);
    for (const size_t scale : scales) {
        if (scale == 0 || n < scale * 2) {
            continue;
        }
        std::vector<Profile> prefix(n + 1);
        for (size_t i = 0; i < n; ++i) {
            for (size_t b = 0; b < kDownbeatBands; ++b) {
                prefix[i + 1][b] = prefix[i][b] + profiles[i][b];
            }
        }
        const auto meanOver = [&prefix](size_t from, size_t to) {
            Profile out{};
            const double len = static_cast<double>(to - from);
            for (size_t b = 0; b < kDownbeatBands; ++b) {
                out[b] = (prefix[to][b] - prefix[from][b]) / len;
            }
            return out;
        };
        std::vector<double> novelty(n, 0.0);
        for (size_t i = scale; i + scale <= n; ++i) {
            const Profile before = meanOver(i - scale, i);
            const Profile after = meanOver(i, i + scale);
            double d = 0.0;
            for (size_t b = 0; b < kDownbeatBands; ++b) {
                const double diff = after[b] - before[b];
                d += diff * diff;
            }
            novelty[i] = std::sqrt(d);
        }
        const std::vector<double> raw = novelty;
        for (size_t i = 0; i < novelty.size(); ++i) {
            const double left = i > 0 ? raw[i - 1] : 0.0;
            const double right = i + 1 < raw.size() ? raw[i + 1] : 0.0;
            if (!(raw[i] > left && raw[i] >= right)) {
                novelty[i] = 0.0;
            }
        }
        double total = 0.0;
        for (const double value : novelty) {
            total += value;
        }
        if (total <= 0.0) {
            continue;
        }
        for (size_t i = 0; i < novelty.size(); ++i) {
            scores[i % std::max(positions, size_t(1))] += novelty[i] / total;
        }
    }
    return scores;
}

std::vector<double> phraseStarts(const BandFrames& profiles,
        const std::vector<double>& halves,
        size_t downbeat) {
    constexpr size_t kScale = 32;
    const auto peaks = noveltyPeaks(profiles, kScale);
    double strongest = 0.0;
    for (const auto& peak : peaks) {
        strongest = std::max(strongest, peak.second);
    }
    std::vector<double> starts;
    for (const auto& peak : peaks) {
        if (peak.first % kPositions == downbeat &&
                peak.second >= strongest * kMinPhrasePeak &&
                peak.first < halves.size()) {
            starts.push_back(halves[peak.first]);
        }
    }
    return starts;
}

GridPhase gridPhaseFrom(const BandFrames& frames,
        uint32_t sampleRate,
        const std::vector<double>& beatSecs) {
    GridPhase unshifted;
    unshifted.downbeatSecs = beatSecs.empty() ? 0.0 : beatSecs.front();
    if (beatSecs.size() < 8 || frames.empty() || sampleRate == 0) {
        return unshifted;
    }
    std::vector<double> halves;
    halves.reserve(beatSecs.size() * 2);
    for (size_t i = 0; i + 1 < beatSecs.size(); ++i) {
        halves.push_back(beatSecs[i]);
        halves.push_back((beatSecs[i] + beatSecs[i + 1]) * 0.5);
    }
    const std::vector<Profile> profiles = beatProfilesFrom(frames, sampleRate, halves);
    if (profiles.size() < kPositions * 2) {
        return unshifted;
    }
    bool anyStrong = false;
    for (const auto& peak : noveltyPeaks(profiles, 32)) {
        if (peak.second >= kMinStructureNovelty) {
            anyStrong = true;
            break;
        }
    }
    if (!anyStrong) {
        return unshifted;
    }
    const std::vector<double> scores = positionScores(
            profiles, kPositions, std::vector<size_t>(kScales.begin(), kScales.end()));
    // Rust's max_by returns the last element on a tie.
    size_t best = 0;
    for (size_t i = 1; i < scores.size(); ++i) {
        if (scores[i] >= scores[best]) {
            best = i;
        }
    }
    GridPhase phase;
    phase.halfBeatOff = best % 2 == 1;
    phase.downbeatSecs = best < halves.size() ? halves[best] : 0.0;
    phase.phraseStarts = phraseStarts(profiles, halves, best);
    return phase;
}

} // namespace mixxx::rbxport
