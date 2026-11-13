#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Port of rbxport's rbl-analysis downbeat stage (GPL-2.0-or-later).
// https://github.com/chrisle/rbxport
namespace mixxx::rbxport {

/// The band edges of a profile are log-spaced over the range that carries
/// structure: the kick and bass at the bottom, hats at the top.
constexpr size_t kDownbeatBands = 12;

using BandFrames = std::vector<std::array<double, kDownbeatBands>>;

/// Beats a tempo segment needs before its bar position is decided from its
/// own music rather than carried over.
constexpr size_t kMinBeatsForOwnPhase = 32;

/// Where the grid sits in the bar.
struct GridPhase {
    /// The music's beats are half a beat after the grid's.
    bool halfBeatOff = false;
    /// A downbeat, in seconds from the start of the file.
    double downbeatSecs = 0.0;
    /// Where phrases start, in seconds, in order.
    std::vector<double> phraseStarts;
};

/// Log band energies per frame, over the whole track.
BandFrames bandFrames(const std::vector<float>& samples, uint32_t sampleRate);

/// Decides where the grid's beats sit in the bar, and whether they sit on
/// the beat at all.
GridPhase gridPhaseFrom(const BandFrames& frames,
        uint32_t sampleRate,
        const std::vector<double>& beatSecs);

/// Where phrases start: the half-beat positions, on the downbeat, at which
/// the music changes most at the four-bar scale.
std::vector<double> phraseStarts(const BandFrames& profiles,
        const std::vector<double>& halves,
        size_t downbeat);

/// The novelty at every slot at one scale, with everything but local maxima
/// set to zero, as `(slot, novelty)` for the maxima.
std::vector<std::pair<size_t, double>> noveltyPeaks(
        const BandFrames& profiles, size_t scale);

/// How much novelty lands on each of `positions` slots per bar.
std::vector<double> positionScores(const BandFrames& profiles,
        size_t positions,
        const std::vector<size_t>& scales);

} // namespace mixxx::rbxport
