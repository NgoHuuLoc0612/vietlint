#include "vietlint/rule_engine.hpp"
#include "vietlint/emitter.hpp"
#include "vietlint/ml_classifier.hpp"
#include "vietlint/lsp_server.hpp"
#include "vietlint/encoding_detector.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/numpy.h>

namespace py = pybind11;
using namespace vietlint;
using namespace vietlint::ml;

// ===========================================================================
// Helper: convert C++ Diagnostic to Python dict
// ===========================================================================
static py::dict diagnostic_to_py(const Diagnostic& d) {
    py::dict result;
    result["file"]       = d.file;
    result["rule_id"]    = d.violation.rule_id;
    result["rule_name"]  = d.rule_name;
    result["message"]    = d.violation.message;
    result["identifier"] = d.violation.identifier;
    result["line"]       = d.violation.span.line;
    result["col"]        = d.violation.span.col;
    result["start_byte"] = d.violation.span.start;
    result["end_byte"]   = d.violation.span.end;

    std::string sev;
    switch (d.violation.severity) {
        case Severity::Error:   sev = "error";   break;
        case Severity::Warning: sev = "warning"; break;
        case Severity::Info:    sev = "info";    break;
        case Severity::Fatal:   sev = "fatal";   break;
    }
    result["severity"] = sev;

    py::list fixes;
    for (const auto& f : d.violation.fixes) fixes.append(f);
    result["fixes"] = fixes;

    return result;
}

// ===========================================================================
// Python-facing LintEngine wrapper
// ===========================================================================
class PyLintEngine {
public:
    explicit PyLintEngine(py::dict config_dict) {
        LintConfig cfg;
        if (config_dict.contains("project_root"))
            cfg.project_root = config_dict["project_root"].cast<std::string>();
        if (config_dict.contains("fail_on_warning"))
            cfg.fail_on_warning = config_dict["fail_on_warning"].cast<bool>();
        if (config_dict.contains("max_violations"))
            cfg.max_violations = config_dict["max_violations"].cast<int>();

        engine_ = std::make_unique<RuleEngine>(std::move(cfg));
        engine_->register_builtin_rules();
    }

    /// Lint source text, return list of diagnostic dicts
    py::list lint_source(const std::string& source, const std::string& filename) {
        auto diags = engine_->lint_source(source, filename);
        py::list result;
        for (const auto& d : diags) result.append(diagnostic_to_py(d));
        return result;
    }

    /// Lint a file on disk
    py::list lint_file(const std::string& path) {
        auto res = engine_->lint_file(std::filesystem::path(path));
        if (!res) {
            throw py::value_error("Failed to lint file: " + res.error());
        }
        py::list result;
        for (const auto& d : *res) result.append(diagnostic_to_py(d));
        return result;
    }

    /// Lint multiple files, return dict keyed by filename
    py::dict lint_files(const std::vector<std::string>& paths) {
        py::dict result;
        for (const auto& p : paths) {
            auto res = engine_->lint_file(std::filesystem::path(p));
            py::list diags;
            if (res) {
                for (const auto& d : *res) diags.append(diagnostic_to_py(d));
            }
            result[p.c_str()] = diags;
        }
        return result;
    }

    /// Format diagnostics as string (text/json/sarif)
    std::string format_diagnostics(const py::list& diag_list,
                                    const std::string& fmt,
                                    const std::string& source = "") {
        // Reconstruct Diagnostic objects from Python dicts
        std::vector<Diagnostic> diags;
        for (auto item : diag_list) {
            py::dict d = item.cast<py::dict>();
            Diagnostic diag;
            diag.file         = d["file"].cast<std::string>();
            diag.rule_name    = d.contains("rule_name") ? d["rule_name"].cast<std::string>() : "";
            diag.violation.rule_id    = d["rule_id"].cast<std::string>();
            diag.violation.message    = d["message"].cast<std::string>();
            diag.violation.identifier = d.contains("identifier") ? d["identifier"].cast<std::string>() : "";
            diag.violation.span.line  = d.contains("line") ? d["line"].cast<uint32_t>() : 0;
            diag.violation.span.col   = d.contains("col")  ? d["col"].cast<uint32_t>()  : 0;

            std::string sev = d.contains("severity") ? d["severity"].cast<std::string>() : "info";
            if      (sev == "error")   diag.violation.severity = Severity::Error;
            else if (sev == "warning") diag.violation.severity = Severity::Warning;
            else                       diag.violation.severity = Severity::Info;

            diags.push_back(std::move(diag));
        }

        DiagnosticFormat format = DiagnosticFormat::Text;
        if      (fmt == "json")  format = DiagnosticFormat::JSON;
        else if (fmt == "sarif") format = DiagnosticFormat::SARIF;
        else if (fmt == "lsp")   format = DiagnosticFormat::LSP;
        else if (fmt == "gcc")   format = DiagnosticFormat::GCC;

        EmitterOptions opts;
        opts.format = format;
        opts.color  = ColorMode::Never;
        DiagnosticEmitter emitter(opts);

        std::ostringstream oss;
        emitter.emit(std::span<const Diagnostic>(diags), source, oss);
        return oss.str();
    }

    /// Get engine stats as dict
    py::dict stats() {
        auto s = engine_->stats();
        py::dict result;
        result["files_scanned"] = s.files_scanned;
        result["errors"]        = s.errors;
        result["warnings"]      = s.warnings;
        result["infos"]         = s.infos;
        return result;
    }

    void reset_stats() { engine_->reset_stats(); }

    /// List registered rule IDs
    py::list rule_ids() {
        auto ids = engine_->registry().rule_ids();
        py::list result;
        for (const auto& id : ids) result.append(id);
        return result;
    }

private:
    std::unique_ptr<RuleEngine> engine_;
};

// ===========================================================================
// Python-facing IdentifierClassifier wrapper
// ===========================================================================
class PyIdentifierClassifier {
public:
    PyIdentifierClassifier(const std::string& model_path = "") noexcept
        : clf_(model_path.empty() ? std::filesystem::path{} : std::filesystem::path(model_path))
    {}

    /// Classify a single identifier, return dict
    py::dict classify(const std::string& identifier) {
        auto output = clf_.classify(identifier);
        py::dict result;
        result["identifier"] = identifier;
        result["predicted_class"] = static_cast<int>(output.predicted_class);
        result["confidence"]      = output.confidence;
        result["using_onnx"]      = clf_.using_onnx();

        static const char* class_names[] = {
            "pure_ascii","pure_vietnamese","mixed_vietnamese",
            "transliterated","abbreviation","unknown"
        };
        size_t idx = static_cast<size_t>(output.predicted_class);
        result["class_name"] = class_names[idx < 6 ? idx : 5];

        py::list probs;
        for (float p : output.class_probabilities) probs.append(p);
        result["probabilities"] = probs;

        return result;
    }

    /// Batch classify - returns numpy array of class indices + probabilities
    py::tuple classify_batch(const std::vector<std::string>& ids) {
        std::vector<std::string_view> views;
        views.reserve(ids.size());
        for (const auto& id : ids) views.emplace_back(id);

        auto outputs = clf_.classify_batch(std::span<const std::string_view>(views));

        py::array_t<int32_t> labels({(py::ssize_t)outputs.size()});
        py::array_t<float>   confs ({(py::ssize_t)outputs.size()});
        py::array_t<float>   probs ({(py::ssize_t)outputs.size(), (py::ssize_t)6});

        auto lb = labels.mutable_unchecked<1>();
        auto cb = confs.mutable_unchecked<1>();
        auto pb = probs.mutable_unchecked<2>();

        for (size_t i = 0; i < outputs.size(); ++i) {
            lb(i) = static_cast<int32_t>(outputs[i].predicted_class);
            cb(i) = outputs[i].confidence;
            for (size_t j = 0; j < 6; ++j)
                pb(i, j) = outputs[i].class_probabilities[j];
        }
        return py::make_tuple(labels, confs, probs);
    }

    /// Extract features as numpy array
    py::array_t<float> extract_features(const std::vector<std::string>& ids) {
        FeatureExtractor extractor;
        std::vector<std::string_view> views;
        views.reserve(ids.size());
        for (const auto& id : ids) views.emplace_back(id);

        auto features = extractor.extract_batch(std::span<const std::string_view>(views));

        py::array_t<float> arr({(py::ssize_t)ids.size(), (py::ssize_t)FEATURE_DIM});
        std::copy(features.begin(), features.end(), arr.mutable_data());
        return arr;
    }

    bool using_onnx() const { return clf_.using_onnx(); }
    void warmup() { clf_.warmup(); }

private:
    MLClassifier clf_;
};

// ===========================================================================
// Python-facing EncodingDetector wrapper
// ===========================================================================
class PyEncodingDetector {
public:
    PyEncodingDetector() noexcept = default;

    py::dict detect(const py::bytes& data) {
        auto buf = std::string(data);
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf.data()), buf.size());

        auto result = detector_.detect(raw);

        py::dict d;
        d["charset"]          = result.charset_name;
        d["confidence"]       = result.confidence;
        d["has_bom"]          = result.has_bom;
        d["needs_conversion"] = result.needs_conversion;
        d["message"]          = result.message;

        static const char* enc_names[] = {
            "utf-8","utf-8-bom","tcvn3","vni","vps","viscii",
            "windows-1258","iso-8859-1","ascii","unknown"
        };
        size_t enc_idx = static_cast<size_t>(result.detected);
        d["encoding"] = enc_names[enc_idx < 10 ? enc_idx : 9];

        return d;
    }

    std::string normalize_to_utf8(const py::bytes& data) {
        auto buf = std::string(data);
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
        auto enc = detector_.detect(raw);
        auto res = detector_.normalize_to_utf8(raw, enc);
        if (!res) throw py::value_error("Conversion failed: " + res.error());
        return std::move(*res);
    }

private:
    EncodingDetector detector_;
};

// ===========================================================================
// Python-facing UTF8Scanner wrapper
// ===========================================================================
class PyUTF8Scanner {
public:
    PyUTF8Scanner() noexcept = default;

    bool has_vietnamese(const std::string& text) {
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text.data()), text.size());
        return scanner_.has_vietnamese_fast(raw);
    }

    uint32_t count_vietnamese(const std::string& text) {
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text.data()), text.size());
        return scanner_.count_vietnamese(raw);
    }

    py::list extract_vietnamese_tokens(const std::string& text) {
        auto tokens = scanner_.extract_vietnamese_tokens(text);
        py::list result;
        for (auto sv : tokens) result.append(std::string(sv));
        return result;
    }

    bool validate_utf8(const py::bytes& data) {
        auto buf = std::string(data);
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
        return scanner_.validate_utf8(raw);
    }

private:
    UTF8Scanner scanner_;
};

// ===========================================================================
// pybind11 module definition
// ===========================================================================
PYBIND11_MODULE(vietlint_core, m) {
    m.doc() = "VietLint C++ core engine Python bindings";

    // -----------------------------------------------------------------------
    // UTF8Scanner
    // -----------------------------------------------------------------------
    py::class_<PyUTF8Scanner>(m, "UTF8Scanner")
        .def(py::init<>())
        .def("has_vietnamese",
             &PyUTF8Scanner::has_vietnamese,
             py::arg("text"),
             "Fast check: does text contain Vietnamese Unicode codepoints?")
        .def("count_vietnamese",
             &PyUTF8Scanner::count_vietnamese,
             py::arg("text"),
             "Count Vietnamese codepoints in text")
        .def("extract_vietnamese_tokens",
             &PyUTF8Scanner::extract_vietnamese_tokens,
             py::arg("text"),
             "Extract identifier tokens containing Vietnamese characters")
        .def("validate_utf8",
             &PyUTF8Scanner::validate_utf8,
             py::arg("data"),
             "Validate that bytes are well-formed UTF-8");

    // -----------------------------------------------------------------------
    // EncodingDetector
    // -----------------------------------------------------------------------
    py::class_<PyEncodingDetector>(m, "EncodingDetector")
        .def(py::init<>())
        .def("detect",
             &PyEncodingDetector::detect,
             py::arg("data"),
             "Detect encoding of raw bytes. Returns dict with encoding info.")
        .def("normalize_to_utf8",
             &PyEncodingDetector::normalize_to_utf8,
             py::arg("data"),
             "Auto-detect encoding and convert to UTF-8 string");

    // -----------------------------------------------------------------------
    // IdentifierClassifier
    // -----------------------------------------------------------------------
    py::class_<PyIdentifierClassifier>(m, "IdentifierClassifier")
        .def(py::init<std::string>(),
             py::arg("model_path") = "",
             "Initialize classifier. If model_path is empty, uses heuristic fallback.")
        .def("classify",
             &PyIdentifierClassifier::classify,
             py::arg("identifier"),
             "Classify a single identifier. Returns dict with class, confidence, probabilities.")
        .def("classify_batch",
             &PyIdentifierClassifier::classify_batch,
             py::arg("identifiers"),
             "Batch classify. Returns (labels_array, confidence_array, probs_array).")
        .def("extract_features",
             &PyIdentifierClassifier::extract_features,
             py::arg("identifiers"),
             "Extract feature matrix [N x 64] for training/inspection.")
        .def("using_onnx",
             &PyIdentifierClassifier::using_onnx,
             "True if ONNX Runtime model is loaded")
        .def("warmup",
             &PyIdentifierClassifier::warmup,
             "Warm up the model with sample identifiers to reduce first-inference latency");

    // -----------------------------------------------------------------------
    // LintEngine
    // -----------------------------------------------------------------------
    py::class_<PyLintEngine>(m, "LintEngine")
        .def(py::init<py::dict>(),
             py::arg("config") = py::dict(),
             "Initialize the lint engine with optional configuration dict.")
        .def("lint_source",
             &PyLintEngine::lint_source,
             py::arg("source"),
             py::arg("filename") = "",
             "Lint source code string. Returns list of diagnostic dicts.")
        .def("lint_file",
             &PyLintEngine::lint_file,
             py::arg("path"),
             "Lint a source file on disk. Returns list of diagnostic dicts.")
        .def("lint_files",
             &PyLintEngine::lint_files,
             py::arg("paths"),
             "Lint multiple files. Returns dict {filename: [diagnostics]}.")
        .def("format_diagnostics",
             &PyLintEngine::format_diagnostics,
             py::arg("diagnostics"),
             py::arg("format") = "text",
             py::arg("source") = "",
             "Format diagnostics as string. format: 'text'|'json'|'sarif'|'lsp'|'gcc'")
        .def("stats",
             &PyLintEngine::stats,
             "Get cumulative lint statistics as dict.")
        .def("reset_stats",
             &PyLintEngine::reset_stats,
             "Reset cumulative statistics.")
        .def("rule_ids",
             &PyLintEngine::rule_ids,
             "List all registered rule IDs.");

    // -----------------------------------------------------------------------
    // Module-level convenience functions
    // -----------------------------------------------------------------------
    m.def("lint_string",
          [](const std::string& source, const std::string& filename) -> py::list {
              LintConfig cfg;
              RuleEngine engine(cfg);
              engine.register_builtin_rules();
              auto diags = engine.lint_source(source, filename);
              py::list result;
              for (const auto& d : diags) result.append(diagnostic_to_py(d));
              return result;
          },
          py::arg("source"),
          py::arg("filename") = "input.py",
          "Convenience: lint a source string with default config. Returns diagnostics list.");

    m.def("detect_encoding",
          [](const py::bytes& data) -> py::dict {
              EncodingDetector det;
              auto buf = std::string(data);
              auto raw = std::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
              auto result = det.detect(raw);
              py::dict d;
              d["charset"]    = result.charset_name;
              d["confidence"] = result.confidence;
              d["has_bom"]    = result.has_bom;
              d["message"]    = result.message;
              return d;
          },
          py::arg("data"),
          "Detect encoding of raw bytes buffer.");

    m.def("has_vietnamese",
          [](const std::string& text) -> bool {
              UTF8Scanner scanner;
              auto raw = std::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(text.data()), text.size());
              return scanner.has_vietnamese_fast(raw);
          },
          py::arg("text"),
          "Fast check: does the string contain Vietnamese characters?");

    m.def("feature_names",
          []() -> py::list {
              auto names = FeatureExtractor::feature_names();
              py::list result;
              for (auto sv : names) result.append(std::string(sv));
              return result;
          },
          "Get the 64 feature names used by the ML classifier.");

    // Version info
    m.attr("__version__")    = "1.0.0";
    m.attr("FEATURE_DIM")    = FEATURE_DIM;
    m.attr("PLUGIN_API_VER") = PLUGIN_API_VERSION;
}
