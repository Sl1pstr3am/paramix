// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// Chunked separation on top of the exported graph.
//
// The chunk offsets, the triangular cross-fade and the global normalisation are
// the same as demucs' apply_model(split=True), so the output matches it sample
// for sample. What differs is the scheduling:
//
//   * consecutive chunks are batched into one inference call, which is what
//     keeps a GPU busy -- upstream demucs feeds chunks one at a time;
//   * the spectrogram of the next batch is built on a worker thread while the
//     current batch is in the network;
//   * output is finalised front-to-back and streamed out, so peak memory does
//     not grow with track length.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "demucs/model_config.hpp"
#include "demucs/overlap_add.hpp"
#include "demucs/session.hpp"
#include "demucs/spectrogram.hpp"
#include "demucs/thread_pool.hpp"

namespace demucs {

struct SeparatorOptions {
    SessionConfig session;
    int batch = 0;                  // 0 -> 1 on CPU, 4 on an accelerator
    int dsp_threads = 0;            // 0 -> hardware concurrency
    float overlap = 0.25f;          // demucs default; lower is faster
    float transition_power = 1.0f;
    bool pipeline = true;           // overlap spectrogram work with inference
};

// Receives finalised output: `data` is [sources][channels][count], planar,
// starting at absolute sample `offset`. Spans arrive in order and never overlap.
using Sink = std::function<void(std::int64_t offset, std::int64_t count, const float* data)>;
using Progress = std::function<void(int done, int total)>;

class Separator {
public:
    Separator(const std::string& model_path, const SeparatorOptions& options)
        : options_(options),
          env_(ORT_LOGGING_LEVEL_WARNING, "demucs-onnx"),
          bundle_(resolve_model(model_path)),
          pool_(options.dsp_threads > 0 ? options.dsp_threads
                                        : static_cast<int>(std::thread::hardware_concurrency())) {
        sessions_.reserve(bundle_.members.size());
        for (const BagMember& member : bundle_.members)
            sessions_.push_back(create_session(env_, member.path, options.session, device_));

        read_sidecar(bundle_.members.front().path);
        config_.name = bundle_.name;
        check_stems_agree();
        read_signature();
        mixing_ = mixing_matrix(bundle_, config_.num_sources());

        batch_ = options_.batch > 0 ? options_.batch
                                    : (device_ == Device::Cpu ? 1 : 4);
        if (batch_ > 1 && !config_.dynamic_batch)
            throw std::runtime_error(
                "this graph has a fixed batch of 1; re-export it with "
                "tools/export_onnx.py, which enables a dynamic batch axis");

        spectro_ = std::make_unique<dsp::Spectrogram>(config_.nfft, config_.hop,
                                                      config_.channels, config_.segment, &pool_);
        if (spectro_->frames() != config_.frames)
            throw std::runtime_error("the STFT frame count disagrees with the graph");

        const std::size_t mix_size = plane_count(batch_, config_.channels) * config_.segment;
        const std::size_t mag_size = plane_count(batch_, 2 * config_.channels) *
                                     config_.freq_bins * config_.frames;
        for (auto& frame : frames_) {
            frame.mix.assign(mix_size, 0.0f);
            frame.mag.assign(mag_size, 0.0f);
        }
        spec_.assign(plane_count(batch_, config_.num_sources() * 2 * config_.channels) *
                         config_.freq_bins * config_.frames, 0.0f);
        wav_.assign(plane_count(batch_, config_.num_sources() * config_.channels) *
                        config_.segment, 0.0f);
        stems_.assign(wav_.size(), 0.0f);
        if (!mixing_.empty()) {
            // Somewhere to land each member's answer before it is folded in.
            part_spec_.assign(spec_.size(), 0.0f);
            part_wav_.assign(wav_.size(), 0.0f);
        }

        weight_ = transition_weight(config_.segment, options_.transition_power);
    }

    const ModelConfig& config() const { return config_; }
    Device device() const { return device_; }
    int batch() const { return batch_; }
    int threads() const { return pool_.size(); }
    // How many networks each chunk goes through.
    int models() const { return static_cast<int>(sessions_.size()); }

    // `mix` is planar [channels][samples] at the model's sample rate.
    void separate(const float* mix, std::int64_t samples, const Sink& sink,
                  const Progress& progress = nullptr) {
        if (samples <= 0) return;

        const int sources = config_.num_sources();
        const int channels = config_.channels;
        const int segment = config_.segment;
        const std::pair<float, float> stats = reference_stats(mix, samples, channels);
        const float mean = stats.first;
        const float gain = stats.second;

        const std::int64_t stride =
            std::max<std::int64_t>(1, static_cast<std::int64_t>((1.0f - options_.overlap) * segment));
        const int chunks = static_cast<int>((samples + stride - 1) / stride);
        const int groups = (chunks + batch_ - 1) / batch_;

        OverlapAdd accumulator(sources * channels, segment + (batch_ - 1) * stride);
        std::vector<float> ready;

        auto build = [&](int group, Frame& frame) {
            fill_frame(frame, group, stride, mix, samples, mean, gain);
        };

        build(0, frames_[0]);
        std::future<void> pending;

        for (int group = 0; group < groups; ++group) {
            if (pending.valid()) pending.get();
            Frame& current = frames_[group % 2];

            if (group + 1 < groups) {
                Frame& next = frames_[(group + 1) % 2];
                if (options_.pipeline)
                    pending = std::async(std::launch::async,
                                         [&, group] { build(group + 1, next); });
                else
                    build(group + 1, next);
            }

            infer(current);
            if (group == 0) check_finite(spec_, "spec");
            spectro_->inverse(spec_.data(), current.count * sources, stems_.data());

            const std::size_t plane = static_cast<std::size_t>(segment);
            for (int i = 0; i < current.count; ++i) {
                const Chunk& chunk = current.chunks[i];
                const std::size_t base =
                    static_cast<std::size_t>(i) * sources * channels * plane + chunk.trim;
                // The network splits its prediction between a waveform branch
                // and a spectral branch; the stem is the sum of the two.
                for (std::size_t p = 0; p < static_cast<std::size_t>(sources) * channels; ++p) {
                    float* out = stems_.data() + base + p * plane;
                    const float* add = wav_.data() + base + p * plane;
                    for (std::int64_t s = 0; s < chunk.length; ++s) out[s] += add[s];
                }
                accumulator.add(chunk.offset, stems_.data() + base,
                                static_cast<std::int64_t>(plane), chunk.length, weight_.data());
            }

            const std::int64_t upto =
                std::min<std::int64_t>(samples, current.chunks.back().offset + stride);
            const std::int64_t count = accumulator.flush(upto, ready);
            if (count > 0) {
                for (auto& value : ready) value = value * gain + mean;
                sink(accumulator.base() - count, count, ready.data());
            }
            if (progress) progress(std::min(chunks, (group + 1) * batch_), chunks);
        }
    }

    // Convenience wrapper that keeps the whole result in memory, laid out as
    // [sources][channels][samples].
    std::vector<float> separate_to_buffer(const float* mix, std::int64_t samples,
                                          const Progress& progress = nullptr) {
        std::vector<float> out(static_cast<std::size_t>(config_.num_sources()) *
                               config_.channels * samples);
        const std::size_t planes = static_cast<std::size_t>(config_.num_sources()) * config_.channels;
        separate(mix, samples, [&](std::int64_t offset, std::int64_t count, const float* data) {
            for (std::size_t p = 0; p < planes; ++p)
                std::copy_n(data + p * count, count,
                            out.data() + p * samples + offset);
        }, progress);
        return out;
    }

private:
    struct Chunk {
        std::int64_t offset;   // absolute position in the track
        std::int64_t length;   // useful samples (short only for the last chunk)
        std::int64_t trim;     // samples to drop from the front of the model output
    };

    struct Frame {
        std::vector<float> mix;
        std::vector<float> mag;
        std::vector<Chunk> chunks;
        int count = 0;
    };

    static std::size_t plane_count(int batch, int planes) {
        return static_cast<std::size_t>(batch) * planes;
    }

    void read_signature() {
        Ort::AllocatorWithDefaultOptions allocator;
        Ort::Session& session = sessions_.front();
        if (session.GetInputCount() != 2 || session.GetOutputCount() != 2)
            throw std::runtime_error("expected a 2-in/2-out graph");
        for (std::size_t i = 0; i < 2; ++i)
            input_names_.push_back(session.GetInputNameAllocated(i, allocator).get());
        for (std::size_t i = 0; i < 2; ++i)
            output_names_.push_back(session.GetOutputNameAllocated(i, allocator).get());

        const auto mix = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto mag = session.GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
        const auto spec = session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (mix.size() != 3 || mag.size() != 4 || spec.size() != 5)
            throw std::runtime_error("unexpected tensor ranks in the graph");

        // Every member of a bag is fed the same tensors, so they have to agree on
        // their shape. They also have to agree on their names, since one set is
        // passed to Run() for all of them.
        for (std::size_t m = 1; m < sessions_.size(); ++m) {
            const auto other = sessions_[m].GetInputTypeInfo(0)
                                   .GetTensorTypeAndShapeInfo().GetShape();
            if (other != mix)
                throw std::runtime_error(bundle_.members[m].path +
                                         " takes a different input shape from the "
                                         "first model in the bag");
            for (std::size_t i = 0; i < 2; ++i) {
                if (input_names_[i] != sessions_[m].GetInputNameAllocated(i, allocator).get() ||
                    output_names_[i] != sessions_[m].GetOutputNameAllocated(i, allocator).get())
                    throw std::runtime_error(bundle_.members[m].path +
                                             " names its tensors differently from the "
                                             "first model in the bag");
            }
        }

        config_.channels = dimension(mix[1], config_.channels, "mix channels");
        config_.segment = dimension(mix[2], config_.segment, "mix length");
        config_.freq_bins = dimension(mag[2], config_.freq_bins, "frequency bins");
        config_.frames = dimension(mag[3], config_.frames, "frames");
        config_.dynamic_batch = mix[0] < 0;
        if (!sidecar_geometry_) {
            config_.nfft = config_.freq_bins * 2;
            config_.hop = config_.nfft / 4;        // htdemucs uses 75 % frame overlap
        }

        // torch.onnx.export cannot prove the output shapes, so it leaves every one of their
        // dimensions symbolic, which arrives here as -1. The stem names therefore come from the
        // sidecar, and the graph is only consulted when it does state a number.
        if (spec[1] > 0 && spec[1] != static_cast<std::int64_t>(config_.sources.size()))
            throw std::runtime_error("the sidecar lists a different number of stems "
                                     "than the graph produces");
    }

    void read_sidecar(const std::string& model_path) {
        const Sidecar sidecar = Sidecar::load(sidecar_path(model_path));
        if (!sidecar.present()) return;

        std::vector<std::string> names;
        if (sidecar.string_array("sources", names) && !names.empty())
            config_.sources = std::move(names);
        sidecar.string_value("name", config_.name);
        sidecar.int_value("samplerate", config_.samplerate);
        sidecar.int_value("audio_channels", config_.channels);
        sidecar.int_value("segment_samples", config_.segment);
        sidecar.int_value("freq_bins", config_.freq_bins);
        sidecar.int_value("frames", config_.frames);
        sidecar_geometry_ = sidecar.int_value("nfft", config_.nfft);
        sidecar.int_value("hop", config_.hop);
    }

    // The graph cannot say what its stems are called, so a bag whose members disagree
    // would silently label its output by whatever the first sidecar happened to say.
    void check_stems_agree() const {
        for (std::size_t m = 1; m < bundle_.members.size(); ++m) {
            const Sidecar sidecar = Sidecar::load(sidecar_path(bundle_.members[m].path));
            std::vector<std::string> names;
            if (sidecar.string_array("sources", names) && names != config_.sources)
                throw std::runtime_error(bundle_.members[m].path +
                                         " separates different stems from the first "
                                         "model in the bag");
        }
    }

    // A concrete dimension from the graph, or what the sidecar said if the graph is symbolic.
    static int dimension(std::int64_t value, int fallback, const char* label) {
        if (value > 0) return static_cast<int>(value);
        if (fallback > 0) return fallback;
        throw std::runtime_error(std::string("the graph leaves ") + label +
                                 " symbolic and the sidecar does not say");
    }

    static std::pair<float, float> reference_stats(const float* mix, std::int64_t samples,
                                                   int channels) {
        // demucs normalises by the mean and standard deviation of the mono
        // downmix of the whole track.
        double sum = 0.0;
        double sum_squares = 0.0;
        for (std::int64_t i = 0; i < samples; ++i) {
            double value = 0.0;
            for (int c = 0; c < channels; ++c)
                value += mix[static_cast<std::size_t>(c) * samples + i];
            value /= channels;
            sum += value;
            sum_squares += value * value;
        }
        const double mean = sum / static_cast<double>(samples);
        const double variance = std::max(0.0, sum_squares / static_cast<double>(samples) - mean * mean);
        return {static_cast<float>(mean), static_cast<float>(std::sqrt(variance)) + 1e-8f};
    }

    void fill_frame(Frame& frame, int group, std::int64_t stride, const float* mix,
                    std::int64_t samples, float mean, float gain) {
        const int channels = config_.channels;
        const int segment = config_.segment;

        frame.chunks.clear();
        std::fill(frame.mix.begin(), frame.mix.end(), 0.0f);

        for (int i = 0; i < batch_; ++i) {
            const std::int64_t offset = (static_cast<std::int64_t>(group) * batch_ + i) * stride;
            if (offset >= samples) break;

            const std::int64_t length = std::min<std::int64_t>(samples - offset, segment);
            const std::int64_t delta = segment - length;
            const std::int64_t start = offset - delta / 2;     // keep short chunks centred
            const std::int64_t from = std::max<std::int64_t>(0, start);
            const std::int64_t to = std::min<std::int64_t>(samples, start + segment);
            const std::int64_t at = from - start;

            for (int c = 0; c < channels; ++c) {
                float* dst = frame.mix.data() +
                             (static_cast<std::size_t>(i) * channels + c) * segment + at;
                const float* src = mix + static_cast<std::size_t>(c) * samples + from;
                for (std::int64_t s = 0; s < to - from; ++s) dst[s] = (src[s] - mean) / gain;
            }
            frame.chunks.push_back({offset, length, delta / 2});
        }

        frame.count = static_cast<int>(frame.chunks.size());
        spectro_->forward(frame.mix.data(), frame.count, frame.mag.data());
    }

    // Checked once, on the first batch. A provider that overflows -- TensorRT asked for
    // fp16 on this model is the known case -- returns NaN for every sample, and the only
    // symptom downstream is a silent file full of NaN. Better to say so and stop.
    static void check_finite(const std::vector<float>& values, const char* what) {
        for (const float value : values) {
            if (!std::isfinite(value)) {
                throw std::runtime_error(
                    std::string("the network returned a non-finite '") + what +
                    "'. Half precision overflows on this model with some providers; "
                    "try the fp32 graph, or the fp16 one built by "
                    "export_onnx.py --fp16 rather than asking the provider to convert.");
            }
        }
    }

    // Runs every member over the batch and leaves the blend in spec_ and wav_.
    void infer(const Frame& frame) {
        if (mixing_.empty()) {
            run(sessions_.front(), frame, spec_.data(), wav_.data());
            return;
        }

        const int sources = config_.num_sources();
        bool first = true;
        for (std::size_t m = 0; m < sessions_.size(); ++m) {
            const float* weights = mixing_.data() + m * sources;
            if (std::all_of(weights, weights + sources, [](float w) { return w == 0.0f; }))
                continue;                       // this member owns none of the stems
            run(sessions_[m], frame, part_spec_.data(), part_wav_.data());
            blend(spec_.data(), part_spec_.data(), weights, frame.count, sources,
                  spec_plane(), first);
            blend(wav_.data(), part_wav_.data(), weights, frame.count, sources,
                  wav_plane(), first);
            first = false;
        }
    }

    std::size_t spec_plane() const {
        return static_cast<std::size_t>(2) * config_.channels * config_.freq_bins * config_.frames;
    }

    std::size_t wav_plane() const {
        return static_cast<std::size_t>(config_.channels) * config_.segment;
    }

    // out[b][s] += weights[s] * part[b][s], over `plane` values per stem.
    static void blend(float* out, const float* part, const float* weights, int batch,
                      int sources, std::size_t plane, bool overwrite) {
        for (int b = 0; b < batch; ++b) {
            for (int s = 0; s < sources; ++s) {
                const float weight = weights[s];
                const std::size_t base = (static_cast<std::size_t>(b) * sources + s) * plane;
                if (overwrite) {
                    for (std::size_t i = 0; i < plane; ++i) out[base + i] = weight * part[base + i];
                } else if (weight != 0.0f) {
                    for (std::size_t i = 0; i < plane; ++i) out[base + i] += weight * part[base + i];
                }
            }
        }
    }

    void run(Ort::Session& session, const Frame& frame, float* spec_out, float* wav_out) {
        const int batch = frame.count;
        const int sources = config_.num_sources();
        const int channels = config_.channels;

        auto memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
        const std::int64_t mix_shape[] = {batch, channels, config_.segment};
        const std::int64_t mag_shape[] = {batch, 2 * channels, config_.freq_bins, config_.frames};
        const std::int64_t spec_shape[] = {batch, sources, 2 * channels,
                                           config_.freq_bins, config_.frames};
        const std::int64_t wav_shape[] = {batch, sources, channels, config_.segment};

        const std::size_t mix_count = static_cast<std::size_t>(batch) * channels * config_.segment;
        const std::size_t mag_count = static_cast<std::size_t>(batch) * 2 * channels *
                                      config_.freq_bins * config_.frames;
        const std::size_t spec_count = static_cast<std::size_t>(batch) * sources * 2 * channels *
                                       config_.freq_bins * config_.frames;
        const std::size_t wav_count = static_cast<std::size_t>(batch) * sources * channels *
                                      config_.segment;

        Ort::Value inputs[] = {
            Ort::Value::CreateTensor<float>(memory, const_cast<float*>(frame.mix.data()),
                                            mix_count, mix_shape, 3),
            Ort::Value::CreateTensor<float>(memory, const_cast<float*>(frame.mag.data()),
                                            mag_count, mag_shape, 4),
        };
        Ort::Value outputs[] = {
            Ort::Value::CreateTensor<float>(memory, spec_out, spec_count, spec_shape, 5),
            Ort::Value::CreateTensor<float>(memory, wav_out, wav_count, wav_shape, 4),
        };

        const char* input_names[] = {input_names_[0].c_str(), input_names_[1].c_str()};
        const char* output_names[] = {output_names_[0].c_str(), output_names_[1].c_str()};
        session.Run(Ort::RunOptions{nullptr}, input_names, inputs, 2, output_names, outputs, 2);
    }

    SeparatorOptions options_;
    Device device_ = Device::Cpu;
    Ort::Env env_;
    ModelBundle bundle_;
    std::vector<Ort::Session> sessions_;
    util::ThreadPool pool_;

    ModelConfig config_;
    std::unique_ptr<dsp::Spectrogram> spectro_;
    std::vector<std::string> input_names_, output_names_;
    std::vector<float> mixing_;      // [model][source], empty unless this is a bag
    bool sidecar_geometry_ = false;
    int batch_ = 1;

    Frame frames_[2];
    std::vector<float> spec_, wav_, stems_, weight_;
    std::vector<float> part_spec_, part_wav_;   // one member's answer, before blending
};

}  // namespace demucs
