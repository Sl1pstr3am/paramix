#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Port of rbxport's rbl-analysis attack stage (GPL-2.0-or-later).
// https://github.com/chrisle/rbxport
namespace mixxx::rbxport {

/// What the attack finder is tuned by.
struct AttackOptions {
    /// The band the click lives in.
    float lowHz = 900.0f;
    float highHz = 9000.0f;
    /// The RMS step, in seconds.
    double stepSecs = 0.001;
    /// How far either side of a predicted beat an attack is looked for.
    double reachSecs = 0.015;
};

/// A kick's attack: when, and how big a spike it started.
struct Attack {
    double secs = 0.0;
    /// The rise of the spike above the level before it, in RMS units of
    /// the band-passed signal.
    double height = 0.0;
};

/// The track band-passed to the click band, kept as its RMS in steps.
class AttackMap {
  public:
    AttackMap(const std::vector<float>& samples,
            uint32_t sampleRate,
            const AttackOptions& options = AttackOptions());

    const AttackOptions& options() const {
        return m_options;
    }

    /// The attack nearest `secs`, within the options' reach.
    std::optional<Attack> attackNear(double secs) const;
    /// The attack nearest `secs` within `reachSecs` either side.
    std::optional<Attack> attackWithin(double secs, double reachSecs) const;

  private:
    std::vector<float> m_rms;
    size_t m_step;
    uint32_t m_sampleRate;
    AttackOptions m_options;
};

/// One biquad section with its state.
struct Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;

    static Biquad highPass(float hz, uint32_t sampleRate);
    static Biquad lowPass(float hz, uint32_t sampleRate);

    float run(float x);
};

} // namespace mixxx::rbxport
