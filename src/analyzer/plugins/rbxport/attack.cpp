#include "analyzer/plugins/rbxport/attack.h"

#include <algorithm>
#include <cmath>

namespace mixxx::rbxport {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// A spike's steepest step must be this many times the window's median
/// step-to-step change to count.
constexpr double kMinSpikeOverWobble = 4.0;
/// A step still belongs to the spike's run while its rise is at least this
/// share of the steepest one.
constexpr double kRunFraction = 0.1;
/// A rise at least this share of the window's steepest is a spike in its
/// own right, and the one nearest the asked-for time is taken.
constexpr double kNearestStrongFraction = 0.5;

/// Second-order high-pass then low-pass, direct form I.
std::vector<float> bandPass(const std::vector<float>& samples,
        uint32_t sampleRate,
        float lowHz,
        float highHz) {
    const float nyquist = static_cast<float>(sampleRate) / 2.0f;
    auto hp = Biquad::highPass(std::clamp(lowHz, 1.0f, nyquist * 0.99f), sampleRate);
    auto lp = Biquad::lowPass(std::clamp(highHz, 1.0f, nyquist * 0.99f), sampleRate);
    std::vector<float> out;
    out.reserve(samples.size());
    for (const float x : samples) {
        out.push_back(lp.run(hp.run(x)));
    }
    return out;
}

} // namespace

Biquad Biquad::highPass(float hz, uint32_t sampleRate) {
    const float w = 2.0f * kPi * hz / static_cast<float>(sampleRate);
    // RBJ cookbook, with the same Q as rbxport's port.
    const float alpha = std::sin(w) / (2.0f * std::sqrt(2.0f));
    const float c = std::cos(w);
    const float half = (1.0f + c) * 0.5f;
    Biquad q;
    const float a0 = 1.0f + alpha;
    q.b0 = half / a0;
    q.b1 = -(1.0f + c) / a0;
    q.b2 = half / a0;
    q.a1 = -2.0f * c / a0;
    q.a2 = (1.0f - alpha) / a0;
    return q;
}

Biquad Biquad::lowPass(float hz, uint32_t sampleRate) {
    const float w = 2.0f * kPi * hz / static_cast<float>(sampleRate);
    // RBJ cookbook, with the same Q as rbxport's port.
    const float alpha = std::sin(w) / (2.0f * std::sqrt(2.0f));
    const float c = std::cos(w);
    const float half = (1.0f - c) * 0.5f;
    Biquad q;
    const float a0 = 1.0f + alpha;
    q.b0 = half / a0;
    q.b1 = (1.0f - c) / a0;
    q.b2 = half / a0;
    q.a1 = -2.0f * c / a0;
    q.a2 = (1.0f - alpha) / a0;
    return q;
}

float Biquad::run(float x) {
    const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

AttackMap::AttackMap(const std::vector<float>& samples,
        uint32_t sampleRate,
        const AttackOptions& options)
        : m_step(std::max(
                  static_cast<size_t>(options.stepSecs * sampleRate), size_t(1))),
          m_sampleRate(sampleRate),
          m_options(options) {
    const std::vector<float> band = bandPass(
            samples, sampleRate, options.lowHz, options.highHz);
    m_rms.reserve((band.size() + m_step - 1) / m_step);
    for (size_t start = 0; start < band.size(); start += m_step) {
        const size_t end = std::min(start + m_step, band.size());
        double sum = 0.0;
        for (size_t i = start; i < end; ++i) {
            sum += static_cast<double>(band[i]) * band[i];
        }
        m_rms.push_back(static_cast<float>(
                std::sqrt(sum / static_cast<double>(std::max(end - start, size_t(1))))));
    }
}

std::optional<Attack> AttackMap::attackNear(double secs) const {
    return attackWithin(secs, m_options.reachSecs);
}

std::optional<Attack> AttackMap::attackWithin(double secs, double reachSecs) const {
    if (m_rms.size() < 3 || m_sampleRate == 0) {
        return std::nullopt;
    }
    const double stepSecs = static_cast<double>(m_step) /
            static_cast<double>(m_sampleRate);
    const double centreRaw = std::round(secs / stepSecs);
    const size_t centre = centreRaw <= 0.0
            ? 0
            : static_cast<size_t>(centreRaw);
    const size_t reach = std::max(
            reachSecs <= 0.0
                    ? size_t(0)
                    : static_cast<size_t>(std::ceil(reachSecs / stepSecs)),
            size_t(1));
    const size_t lo = centre > reach ? centre - reach : 1;
    const size_t hi = std::min(centre + reach, m_rms.size() - 1);
    if (hi <= lo) {
        return std::nullopt;
    }
    const auto rise = [this](size_t i) {
        return static_cast<double>(m_rms[i]) - static_cast<double>(m_rms[i - 1]);
    };
    std::pair<double, size_t> steepest{0.0, lo};
    for (size_t i = lo; i <= hi; ++i) {
        const double r = rise(i);
        if (r > steepest.first) {
            steepest = {r, i};
        }
    }
    if (steepest.first <= 0.0) {
        return std::nullopt;
    }
    std::vector<double> rises;
    rises.reserve(hi - lo + 1);
    for (size_t i = lo; i <= hi; ++i) {
        rises.push_back(std::abs(rise(i)));
    }
    std::stable_sort(rises.begin(), rises.end());
    const double ordinary = rises.empty() ? 0.0 : rises[rises.size() / 2];
    if (steepest.first < ordinary * kMinSpikeOverWobble) {
        return std::nullopt;
    }
    const double strong = steepest.first * kNearestStrongFraction;
    std::pair<double, size_t> chosen = steepest;
    for (size_t i = lo; i <= hi; ++i) {
        const double r = rise(i);
        const size_t chosenDistance =
                chosen.second > centre ? chosen.second - centre : centre - chosen.second;
        const size_t distance = i > centre ? i - centre : centre - i;
        if (r >= strong && distance < chosenDistance) {
            chosen = {r, i};
        }
    }
    const double steepestRise = chosen.first;
    size_t start = chosen.second;
    while (start > lo && rise(start - 1) > steepestRise * kRunFraction) {
        --start;
    }
    const double floorLevel = static_cast<double>(m_rms[start - 1]);
    double top = floorLevel;
    for (size_t i = start; i <= hi; ++i) {
        top = std::max(top, static_cast<double>(m_rms[i]));
    }
    const double sample = static_cast<double>(start - 1) * static_cast<double>(m_step) +
            static_cast<double>(m_step) / 2.0;
    return Attack{sample / static_cast<double>(m_sampleRate), top - floorLevel};
}

} // namespace mixxx::rbxport
