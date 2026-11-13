#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Port of rbxport's rbl-analysis onset stage (GPL-2.0-or-later).
// https://github.com/chrisle/rbxport
namespace mixxx::rbxport {

/// The onset strength signal, one value per hop.
struct OnsetEnvelope {
    std::vector<float> values;
    /// Envelope samples per second.
    double rate = 0.0;
    /// Seconds into the file of envelope sample 0.
    double originSecs = 0.0;

    size_t size() const {
        return values.size();
    }
    bool empty() const {
        return values.empty();
    }
    /// Seconds into the file of envelope sample `x`, which may be fractional.
    double timeOf(double x) const {
        return rate <= 0.0 ? 0.0 : originSecs + x / rate;
    }
    /// Seconds represented by `n` envelope samples.
    double seconds(size_t n) const {
        return rate <= 0.0 ? 0.0 : static_cast<double>(n) / rate;
    }
    /// The envelope between samples, by linear interpolation; zero outside.
    float sampleAt(double x) const;
};

/// The frequencies an envelope is built from.
struct Band {
    float lowHz;
    /// `INFINITY` for everything up to Nyquist.
    float highHz;
    /// Samples in one analysis frame. A power of two, and at least HOP.
    size_t frame;

    /// Every frequency the signal carries.
    static const Band FULL;
    /// Kick territory, over a frame long enough to resolve it.
    static const Band LOW;
};

/// Analysis frame size (~23 ms at 44.1 kHz).
constexpr size_t kOnsetFrame = 1024;
/// The frame the low band is taken over (~93 ms at 44.1 kHz).
constexpr size_t kOnsetLowFrame = 4096;
/// Frames advance by this many samples (~5.8 ms at 44.1 kHz).
constexpr size_t kOnsetHop = 256;

/// Computes the spectral-flux onset envelope over every frequency.
OnsetEnvelope onsetEnvelope(const std::vector<float>& samples, uint32_t sampleRate);

/// Computes the spectral-flux onset envelope over one band.
OnsetEnvelope onsetEnvelopeBand(const std::vector<float>& samples,
        uint32_t sampleRate,
        const Band& band);

} // namespace mixxx::rbxport
