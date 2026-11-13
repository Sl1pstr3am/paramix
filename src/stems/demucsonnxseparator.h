#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "util/types.h"

namespace mixxx::stems {

/// Configuration for the vendored demucs-onnx runtime.
struct DemucsOnnxConfig {
    /// A graph (`*.onnx`), a bag manifest (`*.json`), or a bare model name.
    std::string modelPath;
    /// auto, cpu, cuda, tensorrt, directml, coreml or rocm.
    std::string device = "auto";
    /// ONNX Runtime intra-op threads; zero uses the runtime default.
    int threads = 0;
    /// Chunks per inference call; zero picks 1 on CPU and 4 on an accelerator.
    int batch = 0;
    /// Chunk overlap as a fraction of the segment, matching demucs' 0.25.
    float overlap = 0.25f;
};

/// Receives finalised interleaved eight-channel audio in drums/bass/other/
/// vocals order. Spans arrive in order and never overlap; `offset` is the
/// absolute frame position and `frames` the number of frames in `pBuffer`.
/// Returning false cancels the separation.
using DemucsOutputCallback =
        std::function<bool(SINT offset, const CSAMPLE* pBuffer, SINT frames)>;

/// Reports separation progress as a fraction in [0, 1].
using DemucsProgressCallback = std::function<void(double progress)>;

/// Returns true to cancel. Polled between output blocks.
using DemucsCancelCallback = std::function<bool()>;

/// Whole-track stem separation on top of the vendored demucs-onnx runtime.
///
/// This class delegates chunking, track-level normalization, overlap-add,
/// bag blending and execution-provider fallback to `demucs::Separator`. It is
/// the Demucs counterpart to the generic `StemSeparation`: that class owns the
/// windowing loop for any `StemSeparatorBackend`, while this one uses the
/// upstream runtime's whole-track scheduler unchanged.
///
/// `separate()` expects planar stereo at `sampleRate()` and runs on the
/// calling thread. It allocates and uses several worker threads, so call it
/// from a background worker, never from the real-time audio thread.
class DemucsOnnxSeparator final {
  public:
    /// Creates the runtime and opens the model. Returns null and fills
    /// `pError` when the build has no ONNX Runtime, the model cannot be read,
    /// or the graph does not match the runtime's contract.
    static std::unique_ptr<DemucsOnnxSeparator> create(
            const DemucsOnnxConfig& config, std::string* pError);

    ~DemucsOnnxSeparator();

    DemucsOnnxSeparator(const DemucsOnnxSeparator&) = delete;
    DemucsOnnxSeparator& operator=(const DemucsOnnxSeparator&) = delete;

    /// Sample rate the model expects and produces, from the sidecar.
    int sampleRate() const;

    /// Number of audio channels the model expects (2 for HTDemucs).
    int channels() const;

    /// Source names in the order they appear in the output, from the sidecar.
    const std::vector<std::string>& sources() const;

    /// The provider that actually initialised; `auto` may have fallen back.
    std::string device() const;

    /// Describes the last failure from separate().
    std::string lastError() const;

    /// Separates a whole track.
    ///
    /// @param pPlanarStereo `[channels][frames]` at sampleRate()
    /// @param frames number of frames; zero is allowed
    /// @param writeOutput receives interleaved eight-channel blocks
    /// @param progress optional progress in [0, 1]
    /// @param cancel optional cancellation poll
    /// @return false if the runtime failed or the caller cancelled
    bool separate(const float* pPlanarStereo,
            SINT frames,
            const DemucsOutputCallback& writeOutput,
            const DemucsProgressCallback& progress = DemucsProgressCallback(),
            const DemucsCancelCallback& cancel = DemucsCancelCallback());

  private:
    class Impl;

    explicit DemucsOnnxSeparator(std::unique_ptr<Impl> pImpl);

    std::unique_ptr<Impl> m_pImpl;
};

/// True when this build was compiled with ONNX Runtime support.
inline constexpr bool isDemucsOnnxAvailable() {
#ifdef __STEM_SEPARATION__
    return true;
#else
    return false;
#endif
}

} // namespace mixxx::stems
