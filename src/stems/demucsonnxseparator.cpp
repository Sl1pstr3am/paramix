#include "stems/demucsonnxseparator.h"

#include <algorithm>
#include <cstdint>
#include <demucs/separator.hpp>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "stems/stemseparatorbackend.h"

namespace mixxx::stems {
namespace {

/// Thrown from the output sink to abort the upstream runtime, which has no
/// cancellation callback of its own.
struct Cancelled final {};

std::string failure(std::string* pError, const std::string& message) {
    if (pError != nullptr) {
        *pError = message;
    }
    return message;
}

} // namespace

class DemucsOnnxSeparator::Impl final {
  public:
    explicit Impl(const DemucsOnnxConfig& config) {
        demucs::SeparatorOptions options;
        if (!demucs::parse_device(config.device, options.session.device)) {
            throw std::runtime_error(
                    "unknown execution provider: " + config.device);
        }
        if (!(config.overlap >= 0.0f && config.overlap < 1.0f)) {
            throw std::runtime_error("overlap must be in [0, 1)");
        }
        // Keep the DSP pool off every core the engine and GUI need. ADR-001
        // bounds the workers at min(4, cores / 2); zero means this default.
        const int hardwareThreads =
                static_cast<int>(std::thread::hardware_concurrency());
        const int boundedThreads =
                std::max(1, std::min(4, hardwareThreads / 2));
        options.session.threads =
                config.threads > 0 ? config.threads : boundedThreads;
        options.dsp_threads = boundedThreads;
        options.batch = config.batch;
        options.overlap = config.overlap;
        m_separator = std::make_unique<demucs::Separator>(
                config.modelPath, options);
    }

    std::unique_ptr<demucs::Separator> m_separator;
    std::string m_lastError;
};

DemucsOnnxSeparator::DemucsOnnxSeparator(std::unique_ptr<Impl> pImpl)
        : m_pImpl(std::move(pImpl)) {
}

DemucsOnnxSeparator::~DemucsOnnxSeparator() = default;

std::unique_ptr<DemucsOnnxSeparator> DemucsOnnxSeparator::create(
        const DemucsOnnxConfig& config, std::string* pError) {
    if (config.modelPath.empty()) {
        failure(pError, "No Demucs ONNX model was selected.");
        return nullptr;
    }

    try {
        auto pImpl = std::make_unique<Impl>(config);
        const auto& modelConfig = pImpl->m_separator->config();
        if (modelConfig.channels != 2) {
            failure(pError,
                    "the model expects " +
                            std::to_string(modelConfig.channels) +
                            " channels; Paramix needs stereo");
            return nullptr;
        }
        if (modelConfig.num_sources() != kStemCount) {
            failure(pError,
                    "the model separates " +
                            std::to_string(modelConfig.num_sources()) +
                            " stems; Paramix needs four");
            return nullptr;
        }
        return std::unique_ptr<DemucsOnnxSeparator>(
                new DemucsOnnxSeparator(std::move(pImpl)));
    } catch (const std::exception& error) {
        failure(pError, error.what());
        return nullptr;
    }
}

int DemucsOnnxSeparator::sampleRate() const {
    return m_pImpl->m_separator->config().samplerate;
}

int DemucsOnnxSeparator::channels() const {
    return m_pImpl->m_separator->config().channels;
}

const std::vector<std::string>& DemucsOnnxSeparator::sources() const {
    return m_pImpl->m_separator->config().sources;
}

std::string DemucsOnnxSeparator::device() const {
    return demucs::device_name(m_pImpl->m_separator->device());
}

std::string DemucsOnnxSeparator::lastError() const {
    return m_pImpl->m_lastError;
}

bool DemucsOnnxSeparator::separate(const float* pPlanarStereo,
        SINT frames,
        const DemucsOutputCallback& writeOutput,
        const DemucsProgressCallback& progress,
        const DemucsCancelCallback& cancel) {
    if (frames <= 0) {
        return true;
    }
    if (pPlanarStereo == nullptr || !writeOutput) {
        m_pImpl->m_lastError =
                "separate() needs planar audio and an output callback";
        return false;
    }

    const auto& modelConfig = m_pImpl->m_separator->config();
    const int sources = modelConfig.num_sources();
    const int channels = modelConfig.channels;
    std::vector<CSAMPLE> interleaved;

    try {
        m_pImpl->m_separator->separate(pPlanarStereo, static_cast<std::int64_t>(frames), [&](std::int64_t offset, std::int64_t count, const float* pData) {
                    if (cancel && cancel()) {
                        throw Cancelled{};
                    }
                    interleaved.resize(static_cast<std::size_t>(count) *
                            kStemChannelCount);
                    for (std::int64_t frame = 0; frame < count; ++frame) {
                        for (int source = 0; source < sources; ++source) {
                            for (int channel = 0; channel < channels; ++channel) {
                                const std::size_t sourceIndex =
                                        (static_cast<std::size_t>(source) *
                                                        channels +
                                                channel) *
                                                count +
                                        frame;
                                interleaved[static_cast<std::size_t>(frame) *
                                                        kStemChannelCount +
                                        source * channels + channel] =
                                        pData[sourceIndex];
                            }
                        }
                    }
                    if (!writeOutput(static_cast<SINT>(offset),
                                interleaved.data(),
                                static_cast<SINT>(count))) {
                        throw Cancelled{};
                    } }, progress ? demucs::Progress([&](int done, int total) { progress(total > 0
                                                                                                                                                                                                                                      ? static_cast<double>(done) / total
                                                                                                                                                                                                                                      : 0.0); }) : demucs::Progress{});
    } catch (const Cancelled&) {
        m_pImpl->m_lastError = "separation cancelled";
        return false;
    } catch (const std::exception& error) {
        m_pImpl->m_lastError = error.what();
        return false;
    }
    return true;
}

} // namespace mixxx::stems
