#pragma once

#include <cstdint>
#include <vector>

#include "analyzer/plugins/rbxport/tempo.h"

// Port of rbxport's rbl-analysis grid finalization (GPL-2.0-or-later).
// https://github.com/chrisle/rbxport
namespace mixxx::rbxport {

/// Runs BPM, beat and downbeat analysis over mono audio, with every stage
/// under the caller's control. The returned beats are numbered 1..=4 with
/// beat 1 the downbeat, starting at the first grid beat in the file.
TempoResult analyseGrid(const std::vector<float>& samples,
        uint32_t sampleRate,
        const TempoOptions& options = TempoOptions());

} // namespace mixxx::rbxport
