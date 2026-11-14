#include "stems/demucsonnxseparator.h"

namespace mixxx::stems {

/// Definition for builds without ONNX Runtime. The runtime implementation in
/// demucsonnxseparator.cpp is only compiled when STEM_SEPARATION is enabled;
/// the factory never constructs this class in that configuration, but the
/// unique_ptr destructor still needs a complete Impl.
class DemucsOnnxSeparator::Impl final {};

DemucsOnnxSeparator::DemucsOnnxSeparator(std::unique_ptr<Impl> pImpl)
        : m_pImpl(std::move(pImpl)) {
}

DemucsOnnxSeparator::~DemucsOnnxSeparator() = default;

// The factory never returns an instance in this build, so these are
// unreachable; they exist because StemService references them.
int DemucsOnnxSeparator::sampleRate() const {
    return 0;
}

int DemucsOnnxSeparator::channels() const {
    return 0;
}

const std::vector<std::string>& DemucsOnnxSeparator::sources() const {
    static const std::vector<std::string> kNoSources;
    return kNoSources;
}

std::string DemucsOnnxSeparator::device() const {
    return std::string();
}

std::string DemucsOnnxSeparator::lastError() const {
    return std::string();
}

bool DemucsOnnxSeparator::separate(const float*,
        SINT,
        const DemucsOutputCallback&,
        const DemucsProgressCallback&,
        const DemucsCancelCallback&) {
    return false;
}

} // namespace mixxx::stems
