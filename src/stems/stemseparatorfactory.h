#pragma once

#include <memory>
#include <string>

#include "stems/demucsonnxseparator.h"

namespace mixxx::stems {

/// Creates the Demucs ONNX separator when this build has ONNX Runtime support.
/// Returns null and fills `pError` otherwise, so callers can keep the action
/// visible-but-disabled instead of failing at separation time.
std::unique_ptr<DemucsOnnxSeparator> createDemucsOnnxSeparator(
        const DemucsOnnxConfig& config, std::string* pError);

} // namespace mixxx::stems
