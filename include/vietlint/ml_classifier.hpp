#pragma once
#include "vietlint/classifier.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <expected>
#include <span>
#include <array>
#include <filesystem>
#include <cstdint>
#include <atomic>
#include <mutex>

// Forward-declare ONNX Runtime types to avoid heavy header inclusion
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtMemoryInfo;
struct OrtAllocator;
struct OrtValue;
struct OrtApi;

namespace vietlint::ml {

/// Feature vector length for identifier classification
/// Features: unicode_ratio, viet_ratio, ascii_ratio, length_bucket,
///           has_diacritics, has_combining, style_snake, style_camel,
///           style_pascal, style_screaming, char_bigram_hash[16], ...
constexpr size_t FEATURE_DIM = 64;

/// Classification label index mapping
enum class ModelLabel : uint8_t {
    PureASCII          = 0,
    PureVietnamese     = 1,
    MixedVietnamese    = 2,
    TransliteratedViet = 3,
    Abbreviation       = 4,
    Unknown            = 5,
};

/// Raw model output
struct ModelOutput {
    ModelLabel  predicted_class;
    float       confidence;
    std::array<float, 6> class_probabilities; ///< softmax per class
};

/// Feature extractor: identifier string → float[FEATURE_DIM]
/// Deterministic, no heap allocation after construction
class FeatureExtractor {
public:
    FeatureExtractor() noexcept;
    ~FeatureExtractor() noexcept = default;

    /// Extract feature vector for a single identifier
    [[nodiscard]] std::array<float, FEATURE_DIM>
    extract(std::string_view identifier) const noexcept;

    /// Batch extract - result is row-major [n * FEATURE_DIM]
    [[nodiscard]] std::vector<float>
    extract_batch(std::span<const std::string_view> identifiers) const noexcept;

    /// Feature names for interpretability
    [[nodiscard]] static std::array<std::string_view, FEATURE_DIM> feature_names() noexcept;

private:
    UTF8Scanner scanner_;

    // Char n-gram hash buckets for Vietnamese character patterns
    static constexpr size_t NGRAM_BUCKETS = 16;

    [[nodiscard]] float compute_unicode_ratio(std::string_view id) const noexcept;
    [[nodiscard]] float compute_viet_ratio(std::string_view id) const noexcept;
    [[nodiscard]] float compute_combining_ratio(std::string_view id) const noexcept;
    [[nodiscard]] uint32_t length_bucket(size_t len) const noexcept;
    [[nodiscard]] void fill_ngram_features(std::string_view id,
                                            std::span<float> out,
                                            size_t offset) const noexcept;
    [[nodiscard]] static uint32_t murmur_hash32(const uint8_t* data,
                                                  size_t len,
                                                  uint32_t seed) noexcept;
};

/// ONNX Runtime inference engine for Vietnamese identifier classification
class OnnxClassifier {
public:
    /// Initialize with path to .onnx model file
    explicit OnnxClassifier(const std::filesystem::path& model_path);
    ~OnnxClassifier() noexcept;

    OnnxClassifier(const OnnxClassifier&) = delete;
    OnnxClassifier& operator=(const OnnxClassifier&) = delete;
    OnnxClassifier(OnnxClassifier&&) noexcept;
    OnnxClassifier& operator=(OnnxClassifier&&) noexcept;

    /// Classify a single identifier
    [[nodiscard]] std::expected<ModelOutput, std::string>
    classify(std::string_view identifier) const noexcept;

    /// Batch classify - more efficient than calling classify() in a loop
    [[nodiscard]] std::expected<std::vector<ModelOutput>, std::string>
    classify_batch(std::span<const std::string_view> identifiers) const noexcept;

    /// Check if model is loaded and ready
    [[nodiscard]] bool is_ready() const noexcept;

    /// Get model metadata
    struct ModelInfo {
        std::string  producer_name;
        std::string  model_version;
        std::string  domain;
        std::string  description;
        int64_t      opset_version;
    };
    [[nodiscard]] std::expected<ModelInfo, std::string> model_info() const noexcept;

    /// Check if ONNX Runtime is available (may not be on all platforms)
    [[nodiscard]] static bool runtime_available() noexcept;

private:
    struct OrtState;
    std::unique_ptr<OrtState> state_;
    FeatureExtractor           extractor_;
    mutable std::mutex         infer_mutex_; ///< OrtSession is not thread-safe

    static constexpr const char* INPUT_NAME  = "float_input";
    static constexpr const char* OUTPUT_PROBS  = "probabilities";
    static constexpr const char* OUTPUT_LABELS = "label";

    [[nodiscard]] static ModelOutput parse_outputs(
        const float* probs, size_t num_classes) noexcept;
};

/// Fallback classifier when ONNX Runtime is not available
/// Uses the heuristic VietnameseClassifier but with the same interface
class FallbackClassifier {
public:
    FallbackClassifier() noexcept = default;

    [[nodiscard]] ModelOutput classify(std::string_view identifier) const noexcept;
    [[nodiscard]] std::vector<ModelOutput>
    classify_batch(std::span<const std::string_view> identifiers) const noexcept;

private:
    VietnameseClassifier heuristic_;

    static ModelOutput from_analysis(const IdentifierAnalysis& a) noexcept;
};

/// Unified classifier: uses ONNX if available, falls back to heuristic
class MLClassifier {
public:
    /// Construct - tries to load ONNX model, falls back silently
    explicit MLClassifier(const std::filesystem::path& model_path = "") noexcept;
    ~MLClassifier() noexcept = default;

    MLClassifier(const MLClassifier&) = delete;
    MLClassifier& operator=(const MLClassifier&) = delete;

    /// Classify single identifier
    [[nodiscard]] ModelOutput classify(std::string_view identifier) const noexcept;

    /// Batch classify
    [[nodiscard]] std::vector<ModelOutput>
    classify_batch(std::span<const std::string_view> ids) const noexcept;

    /// True if using ONNX model, false if using heuristic fallback
    [[nodiscard]] bool using_onnx() const noexcept;

    /// Warm up the model with sample data (reduces first-inference latency)
    void warmup() noexcept;

private:
    std::unique_ptr<OnnxClassifier>  onnx_;
    FallbackClassifier               fallback_;
    std::atomic<bool>                onnx_ready_{false};
};

/// Training data point
struct TrainingExample {
    std::string  identifier;
    ModelLabel   label;
    float        confidence; ///< Human annotation confidence
};

/// Training data pipeline - corpus collection and feature extraction
class TrainingPipeline {
public:
    TrainingPipeline() noexcept = default;

    /// Load corpus from JSONL file (one {"id": "...", "label": N} per line)
    [[nodiscard]] std::expected<std::vector<TrainingExample>, std::string>
    load_jsonl(const std::filesystem::path& path) noexcept;

    /// Save training features to CSV for sklearn training
    [[nodiscard]] std::expected<void, std::string>
    export_features_csv(std::span<const TrainingExample> examples,
                         const std::filesystem::path& out_path) noexcept;

    /// Export raw identifier list for external labeling
    [[nodiscard]] std::expected<void, std::string>
    export_unlabeled(std::span<const std::string> identifiers,
                      const std::filesystem::path& out_path) noexcept;

    /// Generate synthetic training data by augmentation
    [[nodiscard]] std::vector<TrainingExample>
    augment(std::span<const TrainingExample> base,
            size_t target_per_class = 5000) const noexcept;

private:
    FeatureExtractor extractor_;

    /// Add noise/variation to identifiers for augmentation
    [[nodiscard]] std::string augment_one(std::string_view id, ModelLabel label) const noexcept;
};

} // namespace vietlint::ml
