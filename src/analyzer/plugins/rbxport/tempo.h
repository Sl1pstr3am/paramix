#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "analyzer/plugins/rbxport/onset.h"

// Port of rbxport's rbl-analysis tempo stage (GPL-2.0-or-later).
// https://github.com/chrisle/rbxport
namespace mixxx::rbxport {

class AttackMap;

/// One beat of the grid.
struct Beat {
    /// 1..=4, where 1 is the downbeat.
    uint16_t beatNumber = 1;
    /// BPM x100 at this beat, as ANLZ stores it.
    uint16_t tempoX100 = 0;
    uint32_t timeMs = 0;
};

/// A stretch of the track at one tempo: every `phaseSecs + k * periodSecs`
/// that falls in `fromSecs..toSecs` is a beat.
struct Segment {
    /// Where the stretch begins, in seconds from the start of the file.
    double fromSecs = 0.0;
    /// Where it ends: the next change, or the end of the track.
    double toSecs = 0.0;
    /// Seconds between beats.
    double periodSecs = 0.0;
    /// Any beat's time; the grid is this plus whole periods either way.
    double phaseSecs = 0.0;

    double bpm() const {
        return periodSecs > 0.0 ? 60.0 / periodSecs : 0.0;
    }
    /// The first beat, in seconds from the start of the file.
    double startSecs() const;
    /// Beats in the segment.
    size_t beats() const;
    /// The same grid moved half a beat.
    Segment shiftedHalfBeat() const;
};

/// The result of the tempo and beat stage.
struct TempoResult {
    /// The tempo at the start of the track, which is what a library shows.
    double bpm = 0.0;
    /// Confidence in 0..=1.
    double confidence = 0.0;
    /// Seconds from the start to the first beat.
    double firstBeatSecs = 0.0;
    /// One entry per tempo, in order.
    std::vector<Segment> segments;
    /// Every beat, numbered 1..=4 cyclically from the first.
    std::vector<Beat> beats;

    static TempoResult empty() {
        return {};
    }
};

/// What a beat is snapped to before the line is fitted through the beats.
enum class Placement {
    /// The nearest peak of the onset envelope.
    Envelope,
    /// The start of the kick's click in the 900-9000 Hz band.
    Attack,
};

/// What the candidate stage is tuned by.
struct TempoOptions {
    double minBpm = 70.0;
    double maxBpm = 180.0;
    /// Where the tempo prior is centred, in BPM.
    double priorCentre = 132.0;
    /// The prior's spread, in natural logs of tempo ratio.
    double priorWidth = 0.5;
    /// Phases tried when the comb refines a period.
    size_t refinePhases = 32;
    /// Seconds per window when looking for a tempo change.
    double segmentWindowSecs = 16.0;
    /// A window whose own tempo differs from the track's by more than this
    /// fraction, and which is followed by another that agrees with it,
    /// starts a new segment.
    double segmentThreshold = 0.02;
    /// Where each beat is placed when the line is fitted.
    Placement placement = Placement::Attack;
    /// Whether the grid follows the music through tempo changes.
    bool followChanges = true;
};

/// Every beat of every segment, in order, numbered so that beat `i` is a
/// downbeat when `(i + phase) % 4 == 0`.
std::vector<Beat> beatsOf(const std::vector<Segment>& segments, size_t phase);

/// Estimates tempo and builds the beat grid from the envelopes alone.
TempoResult detectTempo(const OnsetEnvelope& onsets,
        const OnsetEnvelope* kicks,
        const AttackMap* attacks,
        const TempoOptions& options = TempoOptions());

} // namespace mixxx::rbxport
