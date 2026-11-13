#include "analyzer/plugins/rbxport/onset.h"

#include <dsp/transforms/FFT.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>

#include "analyzer/plugins/rbxport/attack.h"

namespace mixxx::rbxport {

const Band Band::FULL{0.0f, std::numeric_limits<float>::infinity(), kOnsetFrame};
const Band Band::LOW{0.0f, 200.0f, kOnsetLowFrame};

namespace {

constexpr double kPi = 3.14159265358979323846;

/// The share of an envelope's peaks that read at or above one after
/// `scaleToStrongPeaks`.
constexpr double kStrongPeaks = 0.02;

size_t toSize(double value) {
    return value <= 0.0 ? 0 : static_cast<size_t>(value);
}

/// Rescales an envelope so that its strong peaks, not its single highest,
/// read as one.
void scaleToStrongPeaks(std::vector<float>& values) {
    std::vector<float> peaks;
    peaks.reserve(values.size());
    for (size_t i = 0; i + 2 < values.size(); ++i) {
        const float a = values[i];
        const float b = values[i + 1];
        const float c = values[i + 2];
        if (b > 0.0f && b > a && b >= c) {
            peaks.push_back(b);
        }
    }
    if (peaks.size() < 16) {
        return;
    }
    std::stable_sort(peaks.begin(), peaks.end());
    const size_t at = std::min(
            static_cast<size_t>(static_cast<double>(peaks.size()) *
                    (1.0 - kStrongPeaks)),
            peaks.size() - 1);
    const float strong = peaks[at];
    if (strong <= 0.0f) {
        return;
    }
    for (float& value : values) {
        value /= strong;
    }
}

/// The power of two the audio may be decimated by for a band.
size_t decimationFor(const Band& band, uint32_t sampleRate, size_t frame) {
    if (!std::isfinite(band.highHz) || band.highHz <= 0.0f) {
        return 1;
    }
    size_t d = 1;
    while (d < 64 &&
            static_cast<double>(sampleRate) / static_cast<double>(d * 2) >=
                    4.0 * static_cast<double>(band.highHz) &&
            frame % (d * 2) == 0 &&
            kOnsetHop % (d * 2) == 0 &&
            frame / (d * 2) >= 64) {
        d *= 2;
    }
    return d;
}

/// The audio low-passed at `highHz` (two second-order sections) with every
/// `decimation`th sample kept.
std::vector<float> lowPassAndDecimate(const std::vector<float>& samples,
        uint32_t sampleRate,
        float highHz,
        size_t decimation) {
    auto first = Biquad::lowPass(highHz, sampleRate);
    auto second = first;
    std::vector<float> out;
    out.reserve(samples.size() / decimation + 1);
    for (size_t i = 0; i < samples.size(); ++i) {
        const float y = second.run(first.run(samples[i]));
        if (i % decimation == 0) {
            out.push_back(y);
        }
    }
    return out;
}

/// Spectral flux inside `band`: one value per `hop` samples of audio at
/// `sampleRate`, each from a frame of `frame` samples. Not normalised.
std::vector<float> spectralFlux(const std::vector<float>& samples,
        uint32_t sampleRate,
        size_t frame,
        size_t hop,
        const Band& band) {
    if (samples.size() < frame || sampleRate == 0 || hop == 0) {
        return {};
    }
    FFTReal fft(static_cast<int>(frame));
    std::vector<double> input(frame);
    std::vector<double> realOut(frame);
    std::vector<double> imagOut(frame);

    std::vector<double> window(frame);
    for (size_t i = 0; i < frame; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(frame));
    }

    const size_t bins = frame / 2 + 1;
    const double hzPerBin = static_cast<double>(sampleRate) /
            static_cast<double>(frame);
    const size_t firstBin = toSize(std::floor(
            static_cast<double>(band.lowHz) / hzPerBin));
    const size_t lastBin = std::isfinite(band.highHz)
            ? toSize(std::ceil(
                      static_cast<double>(band.highHz) / hzPerBin))
            : std::numeric_limits<size_t>::max();

    const size_t frames = (samples.size() - frame) / hop + 1;
    std::vector<float> values;
    values.reserve(frames);
    std::vector<float> previous(bins, 0.0f);

    for (size_t index = 0; index < frames; ++index) {
        const size_t start = index * hop;
        for (size_t i = 0; i < frame; ++i) {
            input[i] = static_cast<double>(samples[start + i]) * window[i];
        }
        fft.forward(input.data(), realOut.data(), imagOut.data());
        float flux = 0.0f;
        for (size_t i = 0; i < bins; ++i) {
            const float magnitude = static_cast<float>(
                    std::hypot(realOut[i], imagOut[i]));
            const float rise = magnitude - previous[i];
            previous[i] = magnitude;
            if (rise > 0.0f && i >= firstBin && i <= lastBin) {
                flux += rise;
            }
        }
        values.push_back(flux);
    }
    return values;
}

/// Subtracts a local mean and clips at zero, then normalises to its peak.
void normalise(std::vector<float>& values) {
    constexpr size_t kWindow = 16;
    if (values.size() <= kWindow) {
        return;
    }
    const std::vector<float> original = values;
    for (size_t i = 0; i < values.size(); ++i) {
        const size_t lo = i > kWindow ? i - kWindow : 0;
        const size_t hi = std::min(i + kWindow, original.size());
        double sum = 0.0;
        for (size_t j = lo; j < hi; ++j) {
            sum += static_cast<double>(original[j]);
        }
        const double mean = sum / static_cast<double>(hi - lo);
        values[i] = static_cast<float>(
                std::max(static_cast<double>(values[i]) - mean, 0.0));
    }
    float peak = 0.0f;
    for (const float value : values) {
        peak = std::max(peak, value);
    }
    if (peak > 0.0f) {
        for (float& value : values) {
            value /= peak;
        }
    }
}

} // namespace

float OnsetEnvelope::sampleAt(double x) const {
    if (x < 0.0) {
        return 0.0f;
    }
    const size_t i = toSize(std::floor(x));
    const float frac = static_cast<float>(x - static_cast<double>(i));
    const float a = i < values.size() ? values[i] : 0.0f;
    const float b = i + 1 < values.size() ? values[i + 1] : 0.0f;
    return a + (b - a) * frac;
}

OnsetEnvelope onsetEnvelope(const std::vector<float>& samples, uint32_t sampleRate) {
    return onsetEnvelopeBand(samples, sampleRate, Band::FULL);
}

OnsetEnvelope onsetEnvelopeBand(const std::vector<float>& samples,
        uint32_t sampleRate,
        const Band& band) {
    OnsetEnvelope result;
    result.rate = static_cast<double>(sampleRate) / static_cast<double>(kOnsetHop);
    const size_t frame = std::max(band.frame, kOnsetHop);
    result.originSecs = sampleRate == 0
            ? 0.0
            : static_cast<double>(frame) / 2.0 / static_cast<double>(sampleRate);
    if (samples.size() < frame || sampleRate == 0) {
        return result;
    }
    const size_t decimation = decimationFor(band, sampleRate, frame);
    if (decimation > 1) {
        const std::vector<float> reduced = lowPassAndDecimate(
                samples, sampleRate, band.highHz, decimation);
        result.values = spectralFlux(reduced,
                sampleRate / static_cast<uint32_t>(decimation),
                frame / decimation,
                kOnsetHop / decimation,
                band);
    } else {
        result.values = spectralFlux(samples, sampleRate, frame, kOnsetHop, band);
    }
    normalise(result.values);
    if (decimation > 1) {
        scaleToStrongPeaks(result.values);
    }
    return result;
}

} // namespace mixxx::rbxport
