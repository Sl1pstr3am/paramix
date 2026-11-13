#include "stems/demucsbag.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace mixxx::stems {

DemucsBagAverage::DemucsBagAverage(std::size_t cacFloats, std::size_t wavFloats)
        : m_cacSum(cacFloats, 0.0),
          m_wavSum(wavFloats, 0.0) {
}

void DemucsBagAverage::add(const float* pCac, const float* pWav, float weight) {
    if (!(std::isfinite(weight) && weight > 0.0f)) {
        DEBUG_ASSERT(!"invalid bag weight");
        return;
    }
    DEBUG_ASSERT(m_cacSum.empty() || pCac != nullptr);
    DEBUG_ASSERT(m_wavSum.empty() || pWav != nullptr);

    const double weighted = weight;
    for (std::size_t i = 0; i < m_cacSum.size(); ++i) {
        m_cacSum[i] += static_cast<double>(pCac[i]) * weighted;
    }
    for (std::size_t i = 0; i < m_wavSum.size(); ++i) {
        m_wavSum[i] += static_cast<double>(pWav[i]) * weighted;
    }
    m_totalWeight += weighted;
}

void DemucsBagAverage::finalize(float* pCac, float* pWav) const {
    const double scale = m_totalWeight > 0.0 ? 1.0 / m_totalWeight : 0.0;
    if (!m_cacSum.empty()) {
        DEBUG_ASSERT(pCac != nullptr);
        for (std::size_t i = 0; i < m_cacSum.size(); ++i) {
            pCac[i] = static_cast<float>(m_cacSum[i] * scale);
        }
    }
    if (!m_wavSum.empty()) {
        DEBUG_ASSERT(pWav != nullptr);
        for (std::size_t i = 0; i < m_wavSum.size(); ++i) {
            pWav[i] = static_cast<float>(m_wavSum[i] * scale);
        }
    }
}

void DemucsBagAverage::reset() {
    std::fill(m_cacSum.begin(), m_cacSum.end(), 0.0);
    std::fill(m_wavSum.begin(), m_wavSum.end(), 0.0);
    m_totalWeight = 0.0;
}

} // namespace mixxx::stems
