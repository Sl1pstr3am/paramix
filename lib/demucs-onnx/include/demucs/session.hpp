// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// ONNX Runtime session construction and execution-provider selection.
//
// CUDA, TensorRT and ROCm are declared by the stock onnxruntime headers, so
// they always compile; whether they *load* depends on the runtime build, and a
// failure there is reported and falls back to CPU. DirectML and CoreML need
// their own headers, so they are behind CMake options.

#pragma once

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#ifdef DEMUCS_WITH_DML
#include <dml_provider_factory.h>
#endif
#ifdef DEMUCS_WITH_COREML
#include <coreml_provider_factory.h>
#endif

namespace demucs {

enum class Device { Auto, Cpu, Cuda, TensorRT, DirectML, CoreML, Rocm };

inline const char* device_name(Device device) {
    switch (device) {
        case Device::Auto: return "auto";
        case Device::Cpu: return "cpu";
        case Device::Cuda: return "cuda";
        case Device::TensorRT: return "tensorrt";
        case Device::DirectML: return "directml";
        case Device::CoreML: return "coreml";
        case Device::Rocm: return "rocm";
    }
    return "cpu";
}

inline bool parse_device(const std::string& text, Device& out) {
    if (text == "auto") { out = Device::Auto; return true; }
    if (text == "cpu") { out = Device::Cpu; return true; }
    if (text == "cuda") { out = Device::Cuda; return true; }
    if (text == "tensorrt" || text == "trt") { out = Device::TensorRT; return true; }
    if (text == "directml" || text == "dml") { out = Device::DirectML; return true; }
    if (text == "coreml") { out = Device::CoreML; return true; }
    if (text == "rocm") { out = Device::Rocm; return true; }
    return false;
}

// Providers this binary was compiled against and that the loaded runtime
// reports as available.
inline std::vector<Device> available_devices() {
    const std::vector<std::string> present = Ort::GetAvailableProviders();
    auto has = [&](const char* name) {
        return std::find(present.begin(), present.end(), name) != present.end();
    };

    std::vector<Device> devices;
    if (has("TensorrtExecutionProvider")) devices.push_back(Device::TensorRT);
    if (has("CUDAExecutionProvider")) devices.push_back(Device::Cuda);
    if (has("ROCMExecutionProvider")) devices.push_back(Device::Rocm);
#ifdef DEMUCS_WITH_DML
    if (has("DmlExecutionProvider")) devices.push_back(Device::DirectML);
#endif
#ifdef DEMUCS_WITH_COREML
    if (has("CoreMLExecutionProvider")) devices.push_back(Device::CoreML);
#endif
    devices.push_back(Device::Cpu);
    return devices;
}

struct SessionConfig {
    Device device = Device::Auto;
    int device_id = 0;
    int threads = 0;                 // 0 -> hardware concurrency
    bool fp16 = false;               // ask TensorRT for half precision
    std::string cache_dir;           // TensorRT engine cache
    bool verbose = false;
};

// Appends the accelerator for `device`. Returns the device that was actually
// configured: on failure it reports the problem and falls back to CPU, because
// a missing CUDA DLL should not stop the tool from running.
inline Device append_provider(Ort::SessionOptions& options, const SessionConfig& config,
                              Device device) {
    try {
        switch (device) {
            case Device::Cuda: {
                OrtCUDAProviderOptions cuda{};
                cuda.device_id = config.device_id;
                // Every chunk has identical shapes, so paying once for an
                // exhaustive convolution search is repaid from chunk two on.
                cuda.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
                cuda.do_copy_in_default_stream = 1;
                options.AppendExecutionProvider_CUDA(cuda);
                return Device::Cuda;
            }
            case Device::TensorRT: {
                OrtTensorRTProviderOptions trt{};
                trt.device_id = config.device_id;
                trt.trt_fp16_enable = config.fp16 ? 1 : 0;
                // Zero-initialising the struct leaves these at 0, which TensorRT rejects
                // and then warns about on every session. These are its own defaults,
                // said out loud.
                trt.trt_max_partition_iterations = 1000;
                trt.trt_min_subgraph_size = 1;
                trt.trt_max_workspace_size = 0;   // 0 lets TensorRT use what it needs
                if (!config.cache_dir.empty()) {
                    trt.trt_engine_cache_enable = 1;
                    trt.trt_engine_cache_path = config.cache_dir.c_str();
                }
                options.AppendExecutionProvider_TensorRT(trt);
                return Device::TensorRT;
            }
            case Device::Rocm: {
                OrtROCMProviderOptions rocm{};
                rocm.device_id = config.device_id;
                options.AppendExecutionProvider_ROCM(rocm);
                return Device::Rocm;
            }
#ifdef DEMUCS_WITH_DML
            case Device::DirectML: {
                // DirectML manages its own allocations; ORT's memory pattern
                // planner has to be off or the two fight over buffers.
                options.DisableMemPattern();
                options.SetExecutionMode(ORT_SEQUENTIAL);
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(
                    options, config.device_id));
                return Device::DirectML;
            }
#endif
#ifdef DEMUCS_WITH_COREML
            case Device::CoreML: {
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(options, 0));
                return Device::CoreML;
            }
#endif
            default:
                return Device::Cpu;
        }
    } catch (const Ort::Exception& error) {
        std::fprintf(stderr, "[warn] %s is unavailable (%s); falling back to CPU\n",
                     device_name(device), error.what());
        return Device::Cpu;
    }
}

// Opens `model_path`. `chosen` receives the provider that ended up in charge.
inline Ort::Session create_session(const Ort::Env& env, const std::string& model_path,
                                   const SessionConfig& config, Device& chosen) {
    auto fresh_options = [&] {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetExecutionMode(ORT_SEQUENTIAL);
        options.SetInterOpNumThreads(1);
        options.SetIntraOpNumThreads(config.threads > 0 ? config.threads : 0);
        if (config.verbose) options.SetLogSeverityLevel(ORT_LOGGING_LEVEL_INFO);
        return options;
    };

    auto open = [&](Ort::SessionOptions& options) {
#ifdef _WIN32
        const std::wstring wide(model_path.begin(), model_path.end());
        return Ort::Session(env, wide.c_str(), options);
#else
        return Ort::Session(env, model_path.c_str(), options);
#endif
    };

    // GetAvailableProviders() reports what this binary was built against, not what will
    // actually start: a TensorRT-enabled build still needs NVIDIA's own TensorRT libraries
    // beside it, and a CUDA one needs cuDNN. So `auto` walks the list rather than
    // committing to the first entry, and reaches the CPU only once every accelerator has
    // refused. Each attempt gets its own options, because a provider that throws half way
    // through registering leaves them in no state to reuse.
    if (config.device == Device::Auto) {
        for (const Device candidate : available_devices()) {
            if (candidate == Device::Cpu) break;
            Ort::SessionOptions options = fresh_options();
            if (append_provider(options, config, candidate) == candidate) {
                chosen = candidate;
                return open(options);
            }
        }
        chosen = Device::Cpu;
        Ort::SessionOptions options = fresh_options();
        return open(options);
    }

    Ort::SessionOptions options = fresh_options();
    chosen = config.device == Device::Cpu ? Device::Cpu
                                          : append_provider(options, config, config.device);
    return open(options);
}

}  // namespace demucs
