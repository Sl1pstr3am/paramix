#pragma once

#include <QObject>
#include <QVector>
#include <vector>

#include "analyzer/plugins/analyzerplugin.h"

namespace mixxx {

/// Beat and downbeat analysis using rbxport's rbl-analysis pipeline.
///
/// The pipeline is offline (it reads the whole track at once), so samples are
/// downmixed to mono and buffered until finalize(). It is tuned around
/// electronic music in 4/4: kick attacks place the markers, arrangement
/// changes choose beat 1, and tempo changes after a steady intro are
/// followed.
class AnalyzerRbxportBeats : public AnalyzerBeatsPlugin {
  public:
    static AnalyzerPluginInfo pluginInfo() {
        return AnalyzerPluginInfo(
                "rbxport-analysis:0",
                QObject::tr("rbxport"),
                QObject::tr("rbxport Beat Grid Analyzer"),
                true);
    }

    AnalyzerRbxportBeats();
    ~AnalyzerRbxportBeats() override;

    AnalyzerPluginInfo info() const override {
        return pluginInfo();
    }

    bool initialize(mixxx::audio::SampleRate sampleRate) override;
    bool processSamples(const CSAMPLE* pIn, SINT iLen) override;
    bool finalize() override;

    bool supportsBeatTracking() const override {
        return true;
    }

    mixxx::Bpm getBpm() const override {
        return m_resultBpm;
    }

    QVector<mixxx::audio::FramePos> getBeats() const override {
        return m_resultBeats;
    }

  private:
    mixxx::audio::SampleRate m_sampleRate;
    /// Mono downmix of the whole track, in the range [-1, 1].
    std::vector<float> m_samples;
    QVector<mixxx::audio::FramePos> m_resultBeats;
    mixxx::Bpm m_resultBpm;
};

} // namespace mixxx
