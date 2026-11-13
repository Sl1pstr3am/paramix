#include "stems/stemseparatorfactory.h"

namespace mixxx::stems {

std::unique_ptr<DemucsOnnxSeparator> createDemucsOnnxSeparator(
        const DemucsOnnxConfig& config, std::string* pError) {
#ifdef __STEM_SEPARATION__
    return DemucsOnnxSeparator::create(config, pError);
#else
    (void)config;
    if (pError != nullptr) {
        *pError = "Stem separation is not available in this build.";
    }
    return nullptr;
#endif
}

} // namespace mixxx::stems
