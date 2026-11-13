#pragma once

#include <cstddef>
#include <vector>

namespace mixxx::stems {

/// Averages the outputs of the models in a bag (for example `htdemucs_ft`).
///
/// A bag runs several models on the same window and averages their results.
/// The inverse STFT is linear, so averaging the denormalized CaC spectra and
/// the time-domain estimates separately, then running one inverse STFT, is
/// equivalent to reconstructing every model and averaging the audio; it just
/// needs one inverse transform instead of four.
///
/// Every add() must provide outputs that have already been denormalized, so
/// all models share one scale. The buffer sizes are fixed at construction.
class DemucsBagAverage final {
  public:
    /// @param cacFloats number of floats in one CaC spectrogram
    /// @param wavFloats number of floats in one time-domain estimate; zero is
    ///        allowed for a spectrogram-only model
    DemucsBagAverage(std::size_t cacFloats, std::size_t wavFloats);

    /// Adds one model's outputs. `weight` must be positive and finite.
    /// `pCac` must not be null when cacFloats() is non-zero; likewise for
    /// `pWav` and wavFloats().
    void add(const float* pCac, const float* pWav, float weight = 1.0f);

    /// Writes the weighted average. Call once, after all add() calls.
    /// Either output may be null when the corresponding count is zero. If no
    /// weight was added, every value is written as zero.
    void finalize(float* pCac, float* pWav) const;

    /// Zeroes the accumulators so the same buffers can be reused for another
    /// window or stem without reallocating.
    void reset();

    bool isEmpty() const {
        return m_totalWeight <= 0.0;
    }

    double totalWeight() const {
        return m_totalWeight;
    }

    std::size_t cacFloats() const {
        return m_cacSum.size();
    }

    std::size_t wavFloats() const {
        return m_wavSum.size();
    }

  private:
    double m_totalWeight = 0.0;
    std::vector<double> m_cacSum;
    std::vector<double> m_wavSum;
};

} // namespace mixxx::stems
