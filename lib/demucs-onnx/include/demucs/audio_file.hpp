// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// Self-contained RIFF/WAVE reading and writing: PCM 8/16/24/32 and IEEE float
// 32/64 in, float32 / PCM 16 / PCM 24 out. Everything is exchanged planar --
// [channels][frames] -- because that is what the model and the STFT want.
//
// Other container formats are out of scope on purpose; pipe them through
// ffmpeg first (`ffmpeg -i song.mp3 -ar 44100 -ac 2 song.wav`).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace demucs::audio {

struct Buffer {
    std::vector<float> samples;   // planar, channels * frames
    int channels = 0;
    int samplerate = 0;
    std::int64_t frames = 0;

    double duration() const {
        return samplerate ? static_cast<double>(frames) / samplerate : 0.0;
    }
    float* channel(int index) { return samples.data() + static_cast<std::size_t>(index) * frames; }
    const float* channel(int index) const {
        return samples.data() + static_cast<std::size_t>(index) * frames;
    }
};

enum class SampleFormat { Float32, Int16, Int24 };

namespace detail {

inline std::uint32_t read_u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint16_t read_u16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline void write_u32(std::ostream& out, std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
    out.write(reinterpret_cast<const char*>(bytes), 4);
}

inline void write_u16(std::ostream& out, std::uint16_t value) {
    const unsigned char bytes[2] = {static_cast<unsigned char>(value),
                                    static_cast<unsigned char>(value >> 8)};
    out.write(reinterpret_cast<const char*>(bytes), 2);
}

inline float clamp_unit(float value) {
    return value > 1.0f ? 1.0f : (value < -1.0f ? -1.0f : value);
}

}  // namespace detail

// Decodes a WAV file into planar float32.
inline Buffer read_wav(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);

    unsigned char header[12];
    file.read(reinterpret_cast<char*>(header), 12);
    if (!file || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0)
        throw std::runtime_error(path + " is not a RIFF/WAVE file");

    int format_tag = 0, channels = 0, bits = 0, samplerate = 0;
    std::vector<unsigned char> raw;
    bool have_format = false;

    while (file) {
        unsigned char chunk[8];
        file.read(reinterpret_cast<char*>(chunk), 8);
        if (file.gcount() != 8) break;
        const std::uint32_t size = detail::read_u32(chunk + 4);

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            file.read(reinterpret_cast<char*>(fmt.data()), size);
            if (size < 16) throw std::runtime_error("truncated fmt chunk in " + path);
            format_tag = detail::read_u16(fmt.data());
            channels = detail::read_u16(fmt.data() + 2);
            samplerate = static_cast<int>(detail::read_u32(fmt.data() + 4));
            bits = detail::read_u16(fmt.data() + 14);
            if (format_tag == 0xFFFE && size >= 26)      // WAVE_FORMAT_EXTENSIBLE
                format_tag = detail::read_u16(fmt.data() + 24);
            have_format = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            raw.resize(size);
            file.read(reinterpret_cast<char*>(raw.data()), size);
            raw.resize(static_cast<std::size_t>(file.gcount()));
            break;
        } else {
            file.seekg(size + (size & 1), std::ios::cur);   // chunks are word aligned
        }
    }

    if (!have_format || raw.empty()) throw std::runtime_error("no audio data in " + path);
    if (channels < 1) throw std::runtime_error("bad channel count in " + path);

    const int bytes = bits / 8;
    const std::int64_t frames = static_cast<std::int64_t>(raw.size()) / (bytes * channels);

    Buffer buffer;
    buffer.channels = channels;
    buffer.samplerate = samplerate;
    buffer.frames = frames;
    buffer.samples.resize(static_cast<std::size_t>(channels) * frames);

    for (std::int64_t f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            const unsigned char* p = raw.data() + ((f * channels) + c) * bytes;
            float value = 0.0f;
            if (format_tag == 3) {                                   // IEEE float
                if (bits == 32) {
                    std::uint32_t bitsval = detail::read_u32(p);
                    std::memcpy(&value, &bitsval, 4);
                } else if (bits == 64) {
                    double wide;
                    std::memcpy(&wide, p, 8);
                    value = static_cast<float>(wide);
                } else {
                    throw std::runtime_error("unsupported float width in " + path);
                }
            } else if (bits == 16) {
                value = static_cast<std::int16_t>(detail::read_u16(p)) / 32768.0f;
            } else if (bits == 24) {
                std::int32_t wide = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
                value = static_cast<float>(wide >> 8) / 8388608.0f;
            } else if (bits == 32) {
                value = static_cast<float>(static_cast<std::int32_t>(detail::read_u32(p))) /
                        2147483648.0f;
            } else if (bits == 8) {
                value = (static_cast<int>(p[0]) - 128) / 128.0f;      // 8-bit WAV is unsigned
            } else {
                throw std::runtime_error("unsupported bit depth in " + path);
            }
            buffer.samples[static_cast<std::size_t>(c) * frames + f] = value;
        }
    }
    return buffer;
}

// Fold or duplicate channels to reach exactly `channels`.
inline Buffer remix(const Buffer& in, int channels) {
    if (in.channels == channels) return in;

    Buffer out;
    out.channels = channels;
    out.samplerate = in.samplerate;
    out.frames = in.frames;
    out.samples.assign(static_cast<std::size_t>(channels) * in.frames, 0.0f);

    if (in.channels == 1) {
        for (int c = 0; c < channels; ++c)
            std::copy_n(in.channel(0), in.frames, out.channel(c));
    } else if (channels == 1) {
        for (int c = 0; c < in.channels; ++c) {
            const float* src = in.channel(c);
            float* dst = out.channel(0);
            for (std::int64_t f = 0; f < in.frames; ++f) dst[f] += src[f] / in.channels;
        }
    } else {
        for (int c = 0; c < channels; ++c)
            std::copy_n(in.channel(std::min(c, in.channels - 1)), in.frames, out.channel(c));
    }
    return out;
}

// Incremental writer: the separator finalises its output front-to-back, so
// stems can be written as they are produced.
class WavWriter {
public:
    WavWriter(const std::string& path, int samplerate, int channels, SampleFormat format)
        : path_(path), samplerate_(samplerate), channels_(channels), format_(format) {
        file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file_) throw std::runtime_error("cannot write " + path);
        write_header(0);
    }

    ~WavWriter() {
        try {
            close();
        } catch (const std::exception&) {
            // A failure while closing is reported by close() when called
            // explicitly; the destructor must not throw.
        }
    }

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // `planar` holds `channels` rows of `count` samples, `stride` apart.
    void write(const float* planar, std::int64_t stride, std::int64_t count) {
        const int width = bytes_per_sample();
        scratch_.resize(static_cast<std::size_t>(count) * channels_ * width);
        unsigned char* out = scratch_.data();

        for (std::int64_t f = 0; f < count; ++f) {
            for (int c = 0; c < channels_; ++c) {
                const float value = planar[static_cast<std::size_t>(c) * stride + f];
                encode(value, out);
                out += width;
            }
        }
        file_.write(reinterpret_cast<const char*>(scratch_.data()),
                    static_cast<std::streamsize>(scratch_.size()));
        frames_ += count;
    }

    void close() {
        if (!file_.is_open()) return;
        file_.flush();
        std::fstream patch(path_, std::ios::binary | std::ios::in | std::ios::out);
        file_.close();
        if (!patch) throw std::runtime_error("cannot finalise " + path_);
        write_sizes(patch);
    }

    std::int64_t frames() const { return frames_; }

private:
    int bytes_per_sample() const {
        switch (format_) {
            case SampleFormat::Float32: return 4;
            case SampleFormat::Int24: return 3;
            case SampleFormat::Int16: return 2;
        }
        return 4;
    }

    void encode(float value, unsigned char* out) const {
        switch (format_) {
            case SampleFormat::Float32: {
                std::uint32_t bits;
                std::memcpy(&bits, &value, 4);
                out[0] = static_cast<unsigned char>(bits);
                out[1] = static_cast<unsigned char>(bits >> 8);
                out[2] = static_cast<unsigned char>(bits >> 16);
                out[3] = static_cast<unsigned char>(bits >> 24);
                break;
            }
            case SampleFormat::Int24: {
                const std::int32_t sample =
                    static_cast<std::int32_t>(detail::clamp_unit(value) * 8388607.0f);
                out[0] = static_cast<unsigned char>(sample);
                out[1] = static_cast<unsigned char>(sample >> 8);
                out[2] = static_cast<unsigned char>(sample >> 16);
                break;
            }
            case SampleFormat::Int16: {
                const std::int16_t sample =
                    static_cast<std::int16_t>(detail::clamp_unit(value) * 32767.0f);
                out[0] = static_cast<unsigned char>(sample);
                out[1] = static_cast<unsigned char>(sample >> 8);
                break;
            }
        }
    }

    void write_header(std::int64_t frames) {
        const int width = bytes_per_sample();
        const std::uint32_t data_bytes =
            static_cast<std::uint32_t>(frames * channels_ * width);
        const std::uint16_t tag = format_ == SampleFormat::Float32 ? 3 : 1;

        file_.write("RIFF", 4);
        detail::write_u32(file_, 36 + data_bytes);
        file_.write("WAVEfmt ", 8);
        detail::write_u32(file_, 16);
        detail::write_u16(file_, tag);
        detail::write_u16(file_, static_cast<std::uint16_t>(channels_));
        detail::write_u32(file_, static_cast<std::uint32_t>(samplerate_));
        detail::write_u32(file_, static_cast<std::uint32_t>(samplerate_ * channels_ * width));
        detail::write_u16(file_, static_cast<std::uint16_t>(channels_ * width));
        detail::write_u16(file_, static_cast<std::uint16_t>(width * 8));
        file_.write("data", 4);
        detail::write_u32(file_, data_bytes);
    }

    // Sizes are only known once everything has been written.
    void write_sizes(std::fstream& patch) const {
        const std::uint32_t data_bytes =
            static_cast<std::uint32_t>(frames_ * channels_ * bytes_per_sample());
        patch.seekp(4);
        detail::write_u32(patch, 36 + data_bytes);
        patch.seekp(40);
        detail::write_u32(patch, data_bytes);
    }

    std::string path_;
    std::ofstream file_;
    int samplerate_, channels_;
    SampleFormat format_;
    std::int64_t frames_ = 0;
    std::vector<unsigned char> scratch_;
};

}  // namespace demucs::audio
