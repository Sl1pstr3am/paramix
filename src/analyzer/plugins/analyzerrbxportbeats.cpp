#include "analyzer/plugins/analyzerrbxportbeats.h"

#include <QtDebug>
#include <algorithm>
#include <utility>

#include "analyzer/constants.h"
#include "analyzer/plugins/rbxport/grid.h"

namespace mixxx {

AnalyzerRbxportBeats::AnalyzerRbxportBeats() = default;

AnalyzerRbxportBeats::~AnalyzerRbxportBeats() = default;

bool AnalyzerRbxportBeats::initialize(mixxx::audio::SampleRate sampleRate) {
    m_sampleRate = sampleRate;
    m_resultBeats.clear();
    m_resultBpm = mixxx::Bpm();
    m_samples.clear();
    if (m_samples.capacity() > 0) {
        m_samples.shrink_to_fit();
    }
    return sampleRate.isValid();
}

bool AnalyzerRbxportBeats::processSamples(const CSAMPLE* pIn, SINT iLen) {
    if (!m_sampleRate.isValid() || iLen <= 0) {
        return false;
    }
    DEBUG_ASSERT(iLen % kAnalysisChannels == 0);
    const SINT frames = iLen / kAnalysisChannels;
    m_samples.reserve(m_samples.size() + static_cast<size_t>(frames));
    for (SINT i = 0; i < frames; ++i) {
        const SINT sample = i * kAnalysisChannels;
        m_samples.push_back((pIn[sample] + pIn[sample + 1]) * 0.5f);
    }
    return true;
}

bool AnalyzerRbxportBeats::finalize() {
    if (!m_sampleRate.isValid() || m_samples.empty()) {
        return false;
    }
    // Release the buffer as soon as the analysis is done: the pipeline keeps
    // its own copies for as long as it runs.
    std::vector<float> samples = std::move(m_samples);
    m_samples.clear();

    const mixxx::rbxport::TempoResult result = mixxx::rbxport::analyseGrid(
            samples, m_sampleRate.value());
    if (result.beats.empty()) {
        return false;
    }
    // Paramix treats the first beat of the grid as the downbeat, so drop the
    // beats before the first beat 1 the downbeat stage chose.
    size_t firstDownbeat = 0;
    for (size_t i = 0; i < result.beats.size(); ++i) {
        if (result.beats[i].beatNumber == 1) {
            firstDownbeat = i;
            break;
        }
    }
    m_resultBeats.reserve(static_cast<int>(result.beats.size() - firstDownbeat));
    for (size_t i = firstDownbeat; i < result.beats.size(); ++i) {
        const double frame = static_cast<double>(result.beats[i].timeMs) *
                static_cast<double>(m_sampleRate.value()) / 1000.0;
        m_resultBeats.push_back(mixxx::audio::FramePos(frame));
    }
    m_resultBpm = mixxx::Bpm(result.bpm);
    qDebug() << "rbxport analysis detected" << m_resultBeats.size()
             << "beats." << "BPM:" << result.bpm
             << "confidence:" << result.confidence
             << "tempo segments:" << result.segments.size();
    return !m_resultBeats.isEmpty();
}

} // namespace mixxx
