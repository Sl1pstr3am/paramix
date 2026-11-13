// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// Model geometry. Tensor shapes come from the graph, which is the only thing
// that cannot disagree with itself; the human-readable parts -- stem names,
// sample rate -- come from the sidecar JSON written by tools/export_onnx.py.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace demucs {

struct ModelConfig {
    std::string name = "htdemucs";
    std::vector<std::string> sources{"drums", "bass", "other", "vocals"};
    int samplerate = 44100;
    int channels = 2;
    int segment = 0;         // samples per chunk fed to the network
    int nfft = 4096;
    int hop = 1024;
    int freq_bins = 0;       // nfft / 2, the Nyquist bin is dropped
    int frames = 0;
    bool dynamic_batch = false;

    int num_sources() const { return static_cast<int>(sources.size()); }
    double segment_seconds() const { return static_cast<double>(segment) / samplerate; }

    std::string describe() const {
        std::ostringstream out;
        out << name << ": " << num_sources() << " stems [";
        for (std::size_t i = 0; i < sources.size(); ++i)
            out << (i ? ", " : "") << sources[i];
        out << "], " << samplerate << " Hz, " << channels << " ch, segment " << segment
            << " samples (" << segment_seconds() << " s), nfft " << nfft << "/hop " << hop
            << " -> " << freq_bins << "x" << frames
            << ", batch " << (dynamic_batch ? "dynamic" : "fixed at 1");
        return out.str();
    }
};

// A deliberately small JSON reader: the sidecar is machine-generated and nearly
// flat, so this only has to find a key at the top level and read a string, a
// number, an array of either, or -- for a bag manifest -- an array of objects.
// Anything it does not understand is ignored, and the caller keeps its defaults.
class Sidecar {
public:
    static Sidecar load(const std::string& path) {
        Sidecar sidecar;
        std::ifstream file(path);
        if (!file) return sidecar;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        sidecar.text_ = buffer.str();
        sidecar.present_ = true;
        return sidecar;
    }

    static Sidecar from_text(std::string text) {
        Sidecar sidecar;
        sidecar.text_ = std::move(text);
        sidecar.present_ = true;
        return sidecar;
    }

    bool present() const { return present_; }

    bool string_value(const std::string& key, std::string& out) const {
        std::size_t pos = find_value(key);
        if (pos == std::string::npos || text_[pos] != '"') return false;
        return read_string(pos, out);
    }

    bool int_value(const std::string& key, int& out) const {
        std::size_t pos = find_value(key);
        if (pos == std::string::npos) return false;
        try {
            out = std::stoi(text_.substr(pos, 32));
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    bool string_array(const std::string& key, std::vector<std::string>& out) const {
        std::size_t pos = find_value(key);
        if (pos == std::string::npos || text_[pos] != '[') return false;
        std::vector<std::string> values;
        ++pos;
        while (pos < text_.size() && text_[pos] != ']') {
            if (text_[pos] == '"') {
                std::string item;
                if (!read_string(pos, item)) return false;
                values.push_back(item);
            } else {
                ++pos;
            }
        }
        out = std::move(values);
        return true;
    }

    bool float_array(const std::string& key, std::vector<float>& out) const {
        std::size_t pos = find_value(key);
        if (pos == std::string::npos || text_[pos] != '[') return false;
        std::vector<float> values;
        for (++pos; pos < text_.size() && text_[pos] != ']';) {
            const char c = text_[pos];
            if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
                ++pos;
                continue;
            }
            std::size_t used = 0;
            try {
                values.push_back(std::stof(text_.substr(pos, 32), &used));
            } catch (const std::exception&) {
                return false;
            }
            pos += used;
        }
        out = std::move(values);
        return true;
    }

    // An array of objects, each handed back as a Sidecar of its own. Used for the
    // "models" list of a bag manifest, whose entries carry a nested array, so this
    // has to track brace depth rather than just look for the next '}'.
    bool object_array(const std::string& key, std::vector<Sidecar>& out) const {
        std::size_t pos = find_value(key);
        if (pos == std::string::npos || text_[pos] != '[') return false;
        std::vector<Sidecar> items;
        std::size_t start = 0;
        int depth = 0;
        bool in_string = false;
        for (++pos; pos < text_.size(); ++pos) {
            const char c = text_[pos];
            if (in_string) {
                if (c == '\\') ++pos;
                else if (c == '"') in_string = false;
            } else if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                if (depth++ == 0) start = pos;
            } else if (c == '}') {
                if (--depth == 0) items.push_back(from_text(text_.substr(start, pos - start + 1)));
            } else if (c == ']' && depth == 0) {
                break;
            }
        }
        out = std::move(items);
        return true;
    }

private:
    // Position of the first non-space character after "key":
    std::size_t find_value(const std::string& key) const {
        const std::string needle = "\"" + key + "\"";
        std::size_t pos = text_.find(needle);
        if (pos == std::string::npos) return std::string::npos;
        pos = text_.find(':', pos + needle.size());
        if (pos == std::string::npos) return std::string::npos;
        ++pos;
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos]))) ++pos;
        return pos < text_.size() ? pos : std::string::npos;
    }

    // Reads the string starting at text_[pos] == '"' and leaves pos past it.
    bool read_string(std::size_t& pos, std::string& out) const {
        if (text_[pos] != '"') return false;
        std::string value;
        for (++pos; pos < text_.size(); ++pos) {
            const char c = text_[pos];
            if (c == '\\' && pos + 1 < text_.size()) {
                value.push_back(text_[++pos]);
            } else if (c == '"') {
                ++pos;
                out = std::move(value);
                return true;
            } else {
                value.push_back(c);
            }
        }
        return false;
    }

    std::string text_;
    bool present_ = false;
};

inline std::string strip_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

inline std::string extension_of(const std::string& path) {
    std::string extension = path.substr(strip_extension(path).size());
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

inline std::string sidecar_path(const std::string& model_path) {
    return strip_extension(model_path) + ".json";
}

inline bool file_exists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

inline std::string directory_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline std::string join_path(const std::string& directory, const std::string& name) {
    const bool absolute = !name.empty() && (name[0] == '/' || name[0] == '\\' ||
                                            (name.size() > 1 && name[1] == ':'));
    if (directory.empty() || absolute) return name;
    return directory + "/" + name;
}

// One network of a bag, and how much of each stem it contributes. An empty
// `weights` means an equal share of every stem.
struct BagMember {
    std::string path;
    std::vector<float> weights;
};

// What -m resolved to: one graph, or several to be blended.
//
// htdemucs_ft, mdx, mdx_extra and the quantised pair are bags -- four
// independently trained networks whose stems are averaged with per-stem
// weights. Upstream runs each of them over the whole track and adds up the
// results; because every step after the network (ISTFT, cross-fade,
// denormalisation) is linear, the same answer comes out of blending the raw
// graph outputs chunk by chunk, which is what Separator does.
struct ModelBundle {
    std::string name;
    std::vector<BagMember> members;

    bool is_bag() const { return members.size() > 1; }
};

inline ModelBundle single_model(const std::string& path) {
    ModelBundle bundle;
    const std::string base = strip_extension(path);
    const std::size_t slash = base.find_last_of("/\\");
    bundle.name = slash == std::string::npos ? base : base.substr(slash + 1);
    bundle.members.push_back(BagMember{path, {}});
    return bundle;
}

inline ModelBundle bundle_from_manifest(const std::string& path) {
    const Sidecar manifest = Sidecar::load(path);
    if (!manifest.present()) throw std::runtime_error("cannot read " + path);

    std::vector<Sidecar> entries;
    if (!manifest.object_array("models", entries) || entries.empty()) {
        // A plain sidecar, not a manifest: the graph sits next to it.
        return single_model(strip_extension(path) + ".onnx");
    }

    ModelBundle bundle = single_model(path);
    manifest.string_value("name", bundle.name);
    bundle.members.clear();

    const std::string directory = directory_of(path);
    for (const Sidecar& entry : entries) {
        BagMember member;
        std::string file;
        if (!entry.string_value("model", file))
            throw std::runtime_error(path + ": a bag entry has no \"model\"");
        member.path = join_path(directory, file);
        if (!file_exists(member.path))
            throw std::runtime_error(path + " lists " + file + ", which is not beside it");
        entry.float_array("weights", member.weights);
        bundle.members.push_back(std::move(member));
    }
    return bundle;
}

// Turn a user-supplied -m into the list of graphs to open. Accepts a graph
// (htdemucs.onnx), a bag manifest (htdemucs_ft.json), or either with the
// extension left off.
inline ModelBundle resolve_model(const std::string& path) {
    const std::string extension = extension_of(path);
    if (extension == ".onnx") return single_model(path);
    if (extension == ".json") return bundle_from_manifest(path);
    if (file_exists(path + ".onnx")) return single_model(path + ".onnx");
    if (file_exists(path + ".json")) return bundle_from_manifest(path + ".json");
    throw std::runtime_error("neither " + path + ".onnx nor " + path + ".json exists");
}

// Normalised [model][source] weights, row-major, or empty for a single graph.
//
// Upstream sums weight * estimate over the bag and divides each stem by the
// total weight it received; folding that division in here makes the per-chunk
// blend a plain weighted sum.
inline std::vector<float> mixing_matrix(const ModelBundle& bundle, int num_sources) {
    if (!bundle.is_bag()) return {};

    const std::size_t models = bundle.members.size();
    std::vector<float> matrix(models * static_cast<std::size_t>(num_sources), 1.0f);
    for (std::size_t m = 0; m < models; ++m) {
        const std::vector<float>& row = bundle.members[m].weights;
        if (row.empty()) continue;                    // an equal share of every stem
        if (row.size() != static_cast<std::size_t>(num_sources))
            throw std::runtime_error("bag weights do not match the model's stem count");
        std::copy(row.begin(), row.end(), matrix.begin() + m * num_sources);
    }

    for (int s = 0; s < num_sources; ++s) {
        float total = 0.0f;
        for (std::size_t m = 0; m < models; ++m) total += matrix[m * num_sources + s];
        if (total <= 0.0f)
            throw std::runtime_error("no model in the bag contributes to stem " +
                                     std::to_string(s));
        for (std::size_t m = 0; m < models; ++m) matrix[m * num_sources + s] /= total;
    }
    return matrix;
}

}  // namespace demucs
