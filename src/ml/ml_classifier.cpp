#include <cstdlib>
#include "vietlint/ml_classifier.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <numeric>
#include <random>

// Conditionally include ONNX Runtime
#if __has_include(<onnxruntime_cxx_api.h>)
#  define HAVE_ONNX_RUNTIME 1
#  include <onnxruntime_cxx_api.h>
#else
#  define HAVE_ONNX_RUNTIME 0
#endif

namespace vietlint::ml {

// ===========================================================================
// FeatureExtractor
// ===========================================================================

FeatureExtractor::FeatureExtractor() noexcept = default;

// Murmur3 32-bit hash for ngram hashing
uint32_t FeatureExtractor::murmur_hash32(const uint8_t* data,
                                           size_t len,
                                           uint32_t seed) noexcept {
    const uint32_t c1 = 0xcc9e2d51u;
    const uint32_t c2 = 0x1b873593u;
    uint32_t h1 = seed;
    size_t nblocks = len / 4;

    const auto* blocks = reinterpret_cast<const uint32_t*>(data);
    for (size_t i = 0; i < nblocks; ++i) {
        uint32_t k1;
        std::memcpy(&k1, &blocks[i], 4);
        k1 *= c1; k1 = (k1<<15)|(k1>>17); k1 *= c2;
        h1 ^= k1; h1 = (h1<<13)|(h1>>19); h1 = h1*5 + 0xe6546b64u;
    }
    const auto* tail = data + nblocks*4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= (uint32_t)tail[2] << 16; [[fallthrough]];
        case 2: k1 ^= (uint32_t)tail[1] << 8;  [[fallthrough]];
        case 1: k1 ^= tail[0];
                k1 *= c1; k1 = (k1<<15)|(k1>>17); k1 *= c2;
                h1 ^= k1;
    }
    h1 ^= (uint32_t)len;
    // fmix32
    h1 ^= h1>>16; h1 *= 0x85ebca6bu; h1 ^= h1>>13;
    h1 *= 0xc2b2ae35u; h1 ^= h1>>16;
    return h1;
}

float FeatureExtractor::compute_unicode_ratio(std::string_view id) const noexcept {
    if (id.empty()) return 0.0f;
    size_t non_ascii = 0, total = 0;
    const auto* p = reinterpret_cast<const uint8_t*>(id.data());
    const auto* e = p + id.size();
    while (p < e) {
        ++total;
        if (*p >= 0x80) { ++non_ascii; }
        // Skip continuation bytes
        if (*p >= 0xF0)      { p += 4; }
        else if (*p >= 0xE0) { p += 3; }
        else if (*p >= 0xC0) { p += 2; }
        else                 { p += 1; }
    }
    return total > 0 ? (float)non_ascii / total : 0.0f;
}

float FeatureExtractor::compute_viet_ratio(std::string_view id) const noexcept {
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(id.data()), id.size());
    auto cps = scanner_.scan(raw);
    if (!cps || cps->empty()) return 0.0f;
    size_t viet = 0, total = 0;
    for (auto& cp : *cps) {
        if (cp.codepoint == '_' || cp.codepoint == '-') continue;
        ++total;
        if (cp.is_vietnamese) ++viet;
    }
    return total > 0 ? (float)viet / total : 0.0f;
}

float FeatureExtractor::compute_combining_ratio(std::string_view id) const noexcept {
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(id.data()), id.size());
    auto cps = scanner_.scan(raw);
    if (!cps || cps->empty()) return 0.0f;
    size_t combining = 0;
    for (auto& cp : *cps) {
        if (cp.codepoint >= 0x0300 && cp.codepoint <= 0x036F) ++combining;
    }
    return (float)combining / cps->size();
}

uint32_t FeatureExtractor::length_bucket(size_t len) const noexcept {
    // Buckets: 0=1, 1=2-3, 2=4-7, 3=8-15, 4=16-31, 5=32+
    if (len <= 1)  return 0;
    if (len <= 3)  return 1;
    if (len <= 7)  return 2;
    if (len <= 15) return 3;
    if (len <= 31) return 4;
    return 5;
}

void FeatureExtractor::fill_ngram_features(std::string_view id,
                                             std::span<float> out,
                                             size_t offset) const noexcept {
    // Fill NGRAM_BUCKETS bigram hash features + NGRAM_BUCKETS trigram hash features
    std::fill(out.begin() + offset, out.begin() + offset + 2*NGRAM_BUCKETS, 0.0f);

    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(id.data()), id.size());
    auto cps_res = scanner_.scan(raw);
    if (!cps_res || cps_res->size() < 2) return;

    const auto& cps = *cps_res;

    // Bigrams (NGRAM_BUCKETS slots starting at offset)
    for (size_t i = 0; i + 1 < cps.size(); ++i) {
        uint32_t pair[2] = { cps[i].codepoint, cps[i+1].codepoint };
        uint32_t h = murmur_hash32(reinterpret_cast<const uint8_t*>(pair), 8, 42u);
        size_t idx = offset + (h % NGRAM_BUCKETS);
        if (idx < out.size()) out[idx] += 1.0f;
    }
    // Trigrams (NGRAM_BUCKETS slots starting at offset+NGRAM_BUCKETS)
    for (size_t i = 0; i + 2 < cps.size(); ++i) {
        uint32_t triple[3] = { cps[i].codepoint, cps[i+1].codepoint, cps[i+2].codepoint };
        uint32_t h = murmur_hash32(reinterpret_cast<const uint8_t*>(triple), 12, 137u);
        size_t idx = offset + NGRAM_BUCKETS + (h % NGRAM_BUCKETS);
        if (idx < out.size()) out[idx] += 1.0f;
    }

    // Normalize by sequence length
    float inv = cps.size() > 1 ? 1.0f / (float)(cps.size() - 1) : 1.0f;
    for (size_t i = offset; i < offset + 2*NGRAM_BUCKETS; ++i)
        out[i] *= inv;
}

std::array<float, FEATURE_DIM> FeatureExtractor::extract(std::string_view id) const noexcept {
    std::array<float, FEATURE_DIM> f{};

    // Basic statistics
    f[0]  = compute_unicode_ratio(id);
    f[1]  = compute_viet_ratio(id);
    f[2]  = 1.0f - f[0];                              // ascii_ratio
    f[3]  = compute_combining_ratio(id);
    f[4]  = (float)length_bucket(id.size()) / 5.0f;   // normalized bucket
    f[5]  = std::min((float)id.size(), 64.0f) / 64.0f; // raw length normalized

    // Has features
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(id.data()), id.size());
    f[6]  = scanner_.has_vietnamese_fast(raw) ? 1.0f : 0.0f;
    f[7]  = (id.find('_') != std::string_view::npos) ? 1.0f : 0.0f;
    f[8]  = (id.find('-') != std::string_view::npos) ? 1.0f : 0.0f;

    // Naming style one-hot
    auto style = VietnameseClassifier::detect_style(id);
    f[9]  = (style == NamingStyle::SnakeCase)      ? 1.0f : 0.0f;
    f[10] = (style == NamingStyle::CamelCase)      ? 1.0f : 0.0f;
    f[11] = (style == NamingStyle::PascalCase)     ? 1.0f : 0.0f;
    f[12] = (style == NamingStyle::ScreamingSnake) ? 1.0f : 0.0f;
    f[13] = (style == NamingStyle::MixedCase)      ? 1.0f : 0.0f;

    // Case statistics
    size_t upper_cnt = 0, lower_cnt = 0, digit_cnt = 0, ascii_cnt = 0;
    for (char c : id) {
        if (static_cast<uint8_t>(c) < 0x80) {
            ++ascii_cnt;
            if (std::isupper((unsigned char)c)) ++upper_cnt;
            else if (std::islower((unsigned char)c)) ++lower_cnt;
            else if (std::isdigit((unsigned char)c)) ++digit_cnt;
        }
    }
    if (ascii_cnt > 0) {
        f[14] = (float)upper_cnt / ascii_cnt;
        f[15] = (float)lower_cnt / ascii_cnt;
        f[16] = (float)digit_cnt / ascii_cnt;
    }

    // First/last char features
    if (!id.empty()) {
        f[17] = std::isupper((unsigned char)id[0]) ? 1.0f : 0.0f;
        f[18] = (id[0] == '_') ? 1.0f : 0.0f;
        f[19] = std::isdigit((unsigned char)id.back()) ? 1.0f : 0.0f;
    }

    // Unicode block histogram (simplified 8 buckets)
    auto cps_res = scanner_.scan(raw);
    if (cps_res && !cps_res->empty()) {
        size_t block_counts[8] = {};
        for (auto& cp : *cps_res) {
            uint32_t c = cp.codepoint;
            if      (c < 0x80)    block_counts[0]++;
            else if (c < 0x100)   block_counts[1]++;
            else if (c < 0x300)   block_counts[2]++;
            else if (c < 0x370)   block_counts[3]++;  // Combining
            else if (c < 0x1E00)  block_counts[4]++;
            else if (c < 0x1F00)  block_counts[5]++;  // Latin Ext Additional
            else if (c < 0x2000)  block_counts[6]++;
            else                  block_counts[7]++;
        }
        float inv2 = 1.0f / (float)cps_res->size();
        for (size_t i = 0; i < 8; ++i) f[20 + i] = block_counts[i] * inv2;
    }

    // N-gram features (features 28..27+64 = 28..91 → but FEATURE_DIM=64 so 28..63)
    fill_ngram_features(id, std::span<float>(f.data(), FEATURE_DIM), 28);

    return f;
}

std::vector<float> FeatureExtractor::extract_batch(
    std::span<const std::string_view> ids) const noexcept {
    std::vector<float> result(ids.size() * FEATURE_DIM);
    for (size_t i = 0; i < ids.size(); ++i) {
        auto feat = extract(ids[i]);
        std::copy(feat.begin(), feat.end(), result.data() + i * FEATURE_DIM);
    }
    return result;
}

std::array<std::string_view, FEATURE_DIM> FeatureExtractor::feature_names() noexcept {
    static const std::array<std::string_view, FEATURE_DIM> names = {
        "unicode_ratio","viet_ratio","ascii_ratio","combining_ratio",       // 0-3
        "length_bucket_norm","length_raw_norm","has_viet","has_underscore", // 4-7
        "has_hyphen","style_snake","style_camel","style_pascal",            // 8-11
        "style_screaming","style_mixed","upper_ratio","lower_ratio",        // 12-15
        "digit_ratio","starts_upper","starts_underscore","ends_digit",      // 16-19
        "block_ascii","block_latin_ext","block_latin_ext_a","block_combining",    // 20-23
        "block_other_unicode","block_latin_ext_add","block_general_punct","block_high", // 24-27
        // bigram ngram buckets [28..43]
        "bg0","bg1","bg2","bg3","bg4","bg5","bg6","bg7",
        "bg8","bg9","bg10","bg11","bg12","bg13","bg14","bg15",
        // trigram ngram buckets [44..59]
        "tg0","tg1","tg2","tg3","tg4","tg5","tg6","tg7",
        "tg8","tg9","tg10","tg11","tg12","tg13","tg14","tg15",
        // padding [60..63]
        "pad0","pad1","pad2","pad3",
    };
    return names;
}

// ===========================================================================
// OnnxClassifier::OrtState - RAII wrapper around ONNX Runtime objects
// ===========================================================================
#if HAVE_ONNX_RUNTIME
struct OnnxClassifier::OrtState {
    Ort::Env            env;
    Ort::SessionOptions session_opts;
    Ort::Session        session;
    Ort::MemoryInfo     memory_info;

    OrtState(const std::filesystem::path& model_path)
        : env(ORT_LOGGING_LEVEL_WARNING, "vietlint")
        , session_opts()
        , session(env, model_path.c_str(), session_opts)
        , memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        // Enable optimizations
        session_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_opts.SetIntraOpNumThreads(1); // Single-threaded inference per request
    }
};

OnnxClassifier::OnnxClassifier(const std::filesystem::path& model_path)
    : state_(std::make_unique<OrtState>(model_path))
{}

OnnxClassifier::~OnnxClassifier() noexcept = default;

bool OnnxClassifier::is_ready() const noexcept { return state_ != nullptr; }
bool OnnxClassifier::runtime_available() noexcept { return true; }

ModelOutput OnnxClassifier::parse_outputs(const float* probs, size_t num_classes) noexcept {
    ModelOutput out{};
    out.class_probabilities.fill(0.0f);
    size_t n = std::min(num_classes, size_t(6));
    for (size_t i = 0; i < n; ++i) out.class_probabilities[i] = probs[i];

    // Argmax only — probs already softmax from ONNX model
    size_t best = 0;
    float best_p = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        out.class_probabilities[i] = probs[i];
        if (probs[i] > best_p) { best_p = probs[i]; best = i; }
    }
    out.predicted_class = static_cast<ModelLabel>(best);
    out.confidence = best_p;
    return out;
}

std::expected<ModelOutput, std::string>
OnnxClassifier::classify(std::string_view identifier) const noexcept {
    std::string_view sv = identifier;
    auto batch = classify_batch(std::span<const std::string_view>(&sv, 1));
    if (!batch) return std::unexpected(batch.error());
    return batch->front();
}

std::expected<std::vector<ModelOutput>, std::string>
OnnxClassifier::classify_batch(std::span<const std::string_view> ids) const noexcept {
    if (!state_) return std::unexpected("ONNX session not initialized");
    if (ids.empty()) return std::vector<ModelOutput>{};

    auto features = extractor_.extract_batch(ids);

    std::lock_guard lock(infer_mutex_);

    // Input tensor: [batch_size, FEATURE_DIM]
    std::array<int64_t, 2> input_shape = {(int64_t)ids.size(), (int64_t)FEATURE_DIM};
    auto input_tensor = Ort::Value::CreateTensor<float>(
        state_->memory_info,
        features.data(), features.size(),
        input_shape.data(), input_shape.size());

    const char* input_names[]  = { INPUT_NAME };
    const char* output_names[] = { OUTPUT_PROBS, OUTPUT_LABELS };

    auto outputs = state_->session.Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 2);

    auto* probs_data = outputs[0].GetTensorData<float>();
    size_t num_classes = 5;

    std::vector<ModelOutput> results;
    results.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        results.push_back(parse_outputs(probs_data + i * num_classes, num_classes));

    return results;
}

std::expected<OnnxClassifier::ModelInfo, std::string>
OnnxClassifier::model_info() const noexcept {
    if (!state_) return std::unexpected("Session not initialized");
    Ort::AllocatorWithDefaultOptions alloc;
    ModelInfo info{};
    info.producer_name  = state_->session.GetModelMetadata().GetProducerNameAllocated(alloc).get();
    info.model_version  = std::to_string(state_->session.GetModelMetadata().GetVersion());
    info.domain         = state_->session.GetModelMetadata().GetDomainAllocated(alloc).get();
    info.description    = state_->session.GetModelMetadata().GetDescriptionAllocated(alloc).get();
    info.opset_version  = 17; // Assume latest supported
    return info;
}

#else // !HAVE_ONNX_RUNTIME

struct OnnxClassifier::OrtState {};

OnnxClassifier::OnnxClassifier(const std::filesystem::path&)
    : state_(nullptr) {}
OnnxClassifier::~OnnxClassifier() noexcept = default;
OnnxClassifier::OnnxClassifier(OnnxClassifier&& other) noexcept
    : state_(std::move(other.state_)) {}
OnnxClassifier& OnnxClassifier::operator=(OnnxClassifier&& other) noexcept {
    state_ = std::move(other.state_); return *this;
}
bool OnnxClassifier::is_ready() const noexcept { return false; }
bool OnnxClassifier::runtime_available() noexcept { return false; }

std::expected<ModelOutput, std::string>
OnnxClassifier::classify(std::string_view) const noexcept {
    return std::unexpected("ONNX Runtime not available. Build with -DHAVE_ONNX_RUNTIME=1");
}
std::expected<std::vector<ModelOutput>, std::string>
OnnxClassifier::classify_batch(std::span<const std::string_view>) const noexcept {
    return std::unexpected("ONNX Runtime not available");
}
std::expected<OnnxClassifier::ModelInfo, std::string>
OnnxClassifier::model_info() const noexcept {
    return std::unexpected("ONNX Runtime not available");
}
ModelOutput OnnxClassifier::parse_outputs(const float*, size_t) noexcept { return {}; }

#endif // HAVE_ONNX_RUNTIME

// ===========================================================================
// FallbackClassifier
// ===========================================================================
ModelOutput FallbackClassifier::from_analysis(const IdentifierAnalysis& a) noexcept {
    ModelOutput out{};
    out.class_probabilities.fill(0.0f);
    switch (a.cls) {
        case IdentifierClass::PureASCII:
            out.predicted_class = ModelLabel::PureASCII;          break;
        case IdentifierClass::PureVietnamese:
            out.predicted_class = ModelLabel::PureVietnamese;     break;
        case IdentifierClass::MixedVietnamese:
            out.predicted_class = ModelLabel::MixedVietnamese;    break;
        case IdentifierClass::TransliteratedViet:
            out.predicted_class = ModelLabel::TransliteratedViet; break;
        case IdentifierClass::Abbreviation:
            out.predicted_class = ModelLabel::Abbreviation;       break;
        default:
            out.predicted_class = ModelLabel::Unknown;            break;
    }
    out.confidence = a.confidence;
    out.class_probabilities[static_cast<size_t>(out.predicted_class)] = a.confidence;
    return out;
}

ModelOutput FallbackClassifier::classify(std::string_view id) const noexcept {
    // Build a minimal Token for the classifier
    Token tok;
    tok.raw  = std::string(id);
    tok.text = tok.raw;
    tok.kind = TokenKind::Identifier;
    return from_analysis(heuristic_.classify(tok));
}

std::vector<ModelOutput>
FallbackClassifier::classify_batch(std::span<const std::string_view> ids) const noexcept {
    std::vector<ModelOutput> out;
    out.reserve(ids.size());
    for (auto id : ids) out.push_back(classify(id));
    return out;
}

// ===========================================================================
// MLClassifier
// ===========================================================================
MLClassifier::MLClassifier(const std::filesystem::path& model_path) noexcept {
    if (OnnxClassifier::runtime_available() && !model_path.empty()
        && std::filesystem::exists(model_path)) {
        try {
            onnx_ = std::make_unique<OnnxClassifier>(model_path);
            if (onnx_->is_ready()) {
                onnx_ready_.store(true, std::memory_order_release);
            }
        } catch (...) {
            onnx_.reset();
        }
    }
}

bool MLClassifier::using_onnx() const noexcept {
    return onnx_ready_.load(std::memory_order_acquire);
}

ModelOutput MLClassifier::classify(std::string_view id) const noexcept {
    if (using_onnx()) {
        auto res = onnx_->classify(id);
        if (res) return *res;
    }
    return fallback_.classify(id);
}

std::vector<ModelOutput>
MLClassifier::classify_batch(std::span<const std::string_view> ids) const noexcept {
    if (using_onnx()) {
        auto res = onnx_->classify_batch(ids);
        if (res) return std::move(*res);
    }
    return fallback_.classify_batch(ids);
}

void MLClassifier::warmup() noexcept {
    static const std::string_view warmup_ids[] = {
        "tenKhachHang", "so_luong", "danhSachNV", "TONG_CONG",
        "getUserInfo", "calculate", "index", "value",
        "tên", "số lượng", "giá trị", "người dùng",
    };
    for (auto id : warmup_ids) classify(id); // discard results
}

// ===========================================================================
// TrainingPipeline
// ===========================================================================
std::expected<std::vector<TrainingExample>, std::string>
TrainingPipeline::load_jsonl(const std::filesystem::path& path) noexcept {
    std::ifstream f(path);
    if (!f) return std::unexpected("Cannot open: " + path.string());

    std::vector<TrainingExample> examples;
    std::string line;
    size_t line_num = 0;

    while (std::getline(f, line)) {
        ++line_num;
        if (line.empty() || line[0] == '#') continue;

        // Minimal JSONL parser: {"id": "...", "label": N, "confidence": F}
        auto id_start = line.find("\"id\"");
        auto label_start = line.find("\"label\"");
        if (id_start == std::string::npos || label_start == std::string::npos) continue;

        // Extract identifier string
        auto q1 = line.find('"', id_start + 5);
        auto q2 = line.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        std::string identifier = line.substr(q1 + 1, q2 - q1 - 1);

        // Extract label integer
        auto colon = line.find(':', label_start + 7);
        if (colon == std::string::npos) continue;
        size_t val_start = line.find_first_not_of(" \t", colon + 1);
        if (val_start == std::string::npos) continue;
        int label_int = 0;
        char* end_ptr2 = nullptr;
        label_int = (int)std::strtol(line.data() + val_start, &end_ptr2, 10);
        (void)end_ptr2;

        // Extract confidence (optional)
        float confidence = 1.0f;
        auto conf_pos = line.find("\"confidence\"");
        if (conf_pos != std::string::npos) {
            auto c2 = line.find(':', conf_pos + 12);
            if (c2 != std::string::npos) {
                size_t cs = line.find_first_not_of(" \t", c2 + 1);
                if (cs != std::string::npos) {
                    char* ep = nullptr;
                    float cv = std::strtof(line.data() + cs, &ep);
                    if (ep != line.data() + cs) confidence = cv;
                }
            }
        }

        if (label_int >= 0 && label_int <= 5) {
            examples.push_back({
                std::move(identifier),
                static_cast<ModelLabel>(label_int),
                confidence
            });
        }
    }
    return examples;
}

std::expected<void, std::string>
TrainingPipeline::export_features_csv(std::span<const TrainingExample> examples,
                                        const std::filesystem::path& out_path) noexcept {
    std::ofstream f(out_path);
    if (!f) return std::unexpected("Cannot write: " + out_path.string());

    // Header
    auto names = FeatureExtractor::feature_names();
    for (size_t i = 0; i < FEATURE_DIM; ++i) {
        if (i) f << ',';
        f << names[i];
    }
    f << ",label,confidence\n";

    // Rows
    for (const auto& ex : examples) {
        auto feat = extractor_.extract(ex.identifier);
        for (size_t i = 0; i < FEATURE_DIM; ++i) {
            if (i) f << ',';
            f << feat[i];
        }
        f << ',' << static_cast<int>(ex.label)
          << ',' << ex.confidence << '\n';
    }
    return {};
}

std::expected<void, std::string>
TrainingPipeline::export_unlabeled(std::span<const std::string> ids,
                                     const std::filesystem::path& out_path) noexcept {
    std::ofstream f(out_path);
    if (!f) return std::unexpected("Cannot write: " + out_path.string());
    for (const auto& id : ids)
        f << "{\"id\": \"" << id << "\", \"label\": -1}\n";
    return {};
}

std::string TrainingPipeline::augment_one(std::string_view id, ModelLabel label) const noexcept {
    std::string result(id);
    static std::mt19937 rng(42);

    switch (label) {
        case ModelLabel::PureASCII: {
            // Random case changes
            std::uniform_int_distribution<size_t> pos_dist(0, result.size() - 1);
            size_t p = pos_dist(rng);
            if (std::islower((unsigned char)result[p]))
                result[p] = std::toupper((unsigned char)result[p]);
            break;
        }
        case ModelLabel::TransliteratedViet: {
            // Add common Vietnamese prefix/suffix
            static const char* prefixes[] = {"get","set","is","has","do","on"};
            static const char* suffixes[] = {"List","Map","Info","Data","Obj"};
            std::uniform_int_distribution<int> choice(0, 3);
            int c = choice(rng);
            if (c < 2) result = prefixes[c % 6] + result;
            else       result = result + suffixes[c % 5];
            break;
        }
        default: break;
    }
    return result;
}

std::vector<TrainingExample>
TrainingPipeline::augment(std::span<const TrainingExample> base,
                           size_t target_per_class) const noexcept {
    // Count per class
    std::array<size_t, 6> class_counts{};
    for (auto& ex : base) {
        size_t idx = static_cast<size_t>(ex.label);
        if (idx < 6) ++class_counts[idx];
    }

    std::vector<TrainingExample> result(base.begin(), base.end());
    result.reserve(base.size() + 6 * target_per_class);

    static std::mt19937 rng(123);

    for (size_t cls = 0; cls < 6; ++cls) {
        size_t needed = target_per_class > class_counts[cls]
                      ? target_per_class - class_counts[cls] : 0;
        if (needed == 0) continue;

        // Collect examples of this class
        std::vector<const TrainingExample*> pool;
        for (auto& ex : base)
            if (static_cast<size_t>(ex.label) == cls) pool.push_back(&ex);
        if (pool.empty()) continue;

        std::uniform_int_distribution<size_t> idx_dist(0, pool.size() - 1);
        for (size_t i = 0; i < needed; ++i) {
            const auto& src = *pool[idx_dist(rng)];
            std::string aug_id = augment_one(src.identifier, src.label);
            result.push_back({
                std::move(aug_id),
                src.label,
                src.confidence * 0.85f  // slight confidence degradation for augmented
            });
        }
    }
    return result;
}

} // namespace vietlint::ml
