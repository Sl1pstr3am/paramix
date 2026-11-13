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

} // namespace mixxx::stems
