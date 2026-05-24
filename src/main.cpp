#include "vietlint/rule_engine.hpp"
#include "vietlint/emitter.hpp"
#include "vietlint/encoding_detector.hpp"
#include "vietlint/ml_classifier.hpp"
#include "vietlint/lsp_server.hpp"
#include "vietlint/clang_visitor.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <unordered_set>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace vietlint;

// ---------------------------------------------------------------------------
// CLI argument parsing (no external dependency)
// ---------------------------------------------------------------------------
struct CliArgs {
    // Mode
    bool        mode_lsp      = false;
    bool        mode_lint     = false;
    bool        mode_classify = false;
    bool        mode_detect   = false;
    bool        mode_version  = false;
    bool        mode_init     = false;

    // Lint options
    std::vector<std::string> files;
    std::string              format       = "text";
    std::string              config_path;
    std::string              model_path;
    bool                     fix          = false;
    bool                     verbose      = false;
    bool                     no_color     = false;
    int                      jobs         = 0;       // 0 = auto
    int                      max_violations = 0;

    // LSP options
    bool lsp_verbose = false;
};

static void print_usage(const char* argv0) {
    std::cout << R"(VietLint )" << "1.0.0" << R"( — Vietnamese coding convention linter

USAGE:
  )" << argv0 << R"( [MODE] [OPTIONS] [FILES...]

MODES:
  lint          Lint source files (default)
  lsp           Run as LSP server (JSON-RPC over stdio)
  classify      Classify identifiers from stdin (one per line)
  detect        Detect file encoding
  init          Generate .vietlint.toml in current directory
  version       Print version and exit

LINT OPTIONS:
  --format=<fmt>      Output format: text|json|sarif|gcc|github (default: text)
  --config=<path>     Path to .vietlint.toml (default: auto-detect)
  --model=<path>      Path to ONNX classifier model
  --fix               Apply auto-fix suggestions in-place
  --jobs=<n>          Parallel file processing (default: CPU count)
  --max-violations=N  Stop after N violations (0 = unlimited)
  --no-color          Disable ANSI color output
  --verbose           Verbose output

LSP OPTIONS:
  --verbose           Enable trace logging to stderr

EXAMPLES:
  vietlint lint src/*.py --format=json
  vietlint lint --config=.vietlint.toml src/
  vietlint lsp --verbose
  vietlint classify < identifiers.txt
  vietlint detect myfile.c
  vietlint init

EXIT CODES:
  0  No violations (or only informational)
  1  Warnings found
  2  Errors found
  3  Fatal errors / tool failure
)";
}

static CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    bool mode_set = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);

        if (arg == "lint")     { args.mode_lint = true;     mode_set = true; continue; }
        if (arg == "lsp")      { args.mode_lsp  = true;     mode_set = true; continue; }
        if (arg == "classify") { args.mode_classify = true; mode_set = true; continue; }
        if (arg == "detect")   { args.mode_detect = true;   mode_set = true; continue; }
        if (arg == "version")  { args.mode_version = true;  mode_set = true; continue; }
        if (arg == "init")     { args.mode_init = true;     mode_set = true; continue; }

        if (arg.starts_with("--format="))  { args.format       = std::string(arg.substr(9));  continue; }
        if (arg.starts_with("--config="))  { args.config_path  = std::string(arg.substr(9));  continue; }
        if (arg.starts_with("--model="))   { args.model_path   = std::string(arg.substr(8));  continue; }
        if (arg.starts_with("--jobs=")) {
            std::from_chars(arg.data()+7, arg.data()+arg.size(), args.jobs);
            continue;
        }
        if (arg.starts_with("--max-violations=")) {
            std::from_chars(arg.data()+17, arg.data()+arg.size(), args.max_violations);
            continue;
        }
        if (arg == "--fix")      { args.fix      = true; continue; }
        if (arg == "--verbose")  { args.verbose  = true; args.lsp_verbose = true; continue; }
        if (arg == "--no-color") { args.no_color = true; continue; }
        if (arg == "--help" || arg == "-h") { print_usage(argv[0]); std::exit(0); }

        // Positional: file or directory
        if (!arg.starts_with("--")) {
            args.files.emplace_back(arg);
        }
    }

    if (!mode_set) args.mode_lint = true;
    return args;
}

// ---------------------------------------------------------------------------
// Collect source files recursively from paths
// ---------------------------------------------------------------------------
static std::vector<fs::path> collect_files(const std::vector<std::string>& inputs) {
    static const std::unordered_set<std::string> SOURCE_EXTS = {
        ".py",".js",".ts",".jsx",".tsx",
        ".c",".h",".cpp",".cxx",".cc",".hpp",".hxx"
    };
    std::vector<fs::path> out;

    for (const auto& inp : inputs) {
        fs::path p(inp);
        if (!fs::exists(p)) {
            std::cerr << "Warning: path not found: " << inp << '\n';
            continue;
        }
        if (fs::is_regular_file(p)) {
            out.push_back(p);
        } else if (fs::is_directory(p)) {
            for (auto& entry : fs::recursive_directory_iterator(p,
                    fs::directory_options::skip_permission_denied)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (SOURCE_EXTS.count(ext)) out.push_back(entry.path());
            }
        }
    }

    // Deduplicate
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Find .vietlint.toml by traversing up from cwd
// ---------------------------------------------------------------------------
static fs::path find_config() noexcept {
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        auto candidate = dir / ".vietlint.toml";
        if (fs::exists(candidate)) return candidate;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Apply fix: overwrite file with violations replaced by their first fix
// ---------------------------------------------------------------------------
static bool apply_fix(const fs::path& file,
                       std::span<const Diagnostic> diags) noexcept {
    std::ifstream inf(file, std::ios::binary);
    if (!inf) return false;
    std::string source((std::istreambuf_iterator<char>(inf)),
                        std::istreambuf_iterator<char>());
    inf.close();

    // Sort violations by byte offset descending so we can replace without shift
    std::vector<const Diagnostic*> sorted_diags;
    sorted_diags.reserve(diags.size());
    for (const auto& d : diags) {
        if (!d.violation.fixes.empty() && d.violation.span.start < d.violation.span.end)
            sorted_diags.push_back(&d);
    }
    std::sort(sorted_diags.begin(), sorted_diags.end(),
        [](const Diagnostic* a, const Diagnostic* b){
            return a->violation.span.start > b->violation.span.start;
        });

    bool changed = false;
    std::unordered_set<uint32_t> applied_spans;
    for (const auto* d : sorted_diags) {
        uint32_t start = d->violation.span.start;
        uint32_t end   = d->violation.span.end;
        if (end > source.size() || start >= end) continue;
        // Skip if we already applied a fix at this span
        if (applied_spans.count(start)) continue;
        applied_spans.insert(start);
        source.replace(start, end - start, d->violation.fixes[0]);
        changed = true;
    }

    if (!changed) return false;

    std::ofstream outf(file, std::ios::binary | std::ios::trunc);
    if (!outf) return false;
    outf.write(source.data(), static_cast<std::streamsize>(source.size()));
    return true;
}

// ---------------------------------------------------------------------------
// Mode: lint
// ---------------------------------------------------------------------------
static int run_lint(const CliArgs& args) {
    // Load config
    LintConfig config;
    config.max_violations = args.max_violations;

    fs::path config_path = args.config_path.empty() ? find_config()
                                                      : fs::path(args.config_path);
    if (!config_path.empty() && fs::exists(config_path)) {
        auto loaded = LintConfig::load_toml(config_path);
        if (loaded) {
            config = std::move(*loaded);
            if (args.verbose)
                std::cerr << "[vietlint] loaded config: " << config_path << '\n';
        }
    }

    // Build engine
    if (!args.model_path.empty()) config.model_path = args.model_path;
    RuleEngine engine(std::move(config));
    engine.register_builtin_rules();

    // Setup emitter
    DiagnosticFormat fmt = DiagnosticFormat::Text;
    if      (args.format == "json")   fmt = DiagnosticFormat::JSON;
    else if (args.format == "sarif")  fmt = DiagnosticFormat::SARIF;
    else if (args.format == "gcc")    fmt = DiagnosticFormat::GCC;
    else if (args.format == "github") fmt = DiagnosticFormat::GitHubActions;
    else if (args.format == "lsp")    fmt = DiagnosticFormat::LSP;

    EmitterOptions eopts;
    eopts.format = fmt;
    eopts.color  = args.no_color ? ColorMode::Never : ColorMode::Auto;
    DiagnosticEmitter emitter(eopts);

    // Collect files
    std::vector<fs::path> files;
    if (args.files.empty()) {
        // Default: lint current directory
        files = collect_files({"."});
    } else {
        files = collect_files(args.files);
    }

    if (files.empty()) {
        std::cerr << "No source files found.\n";
        return 3;
    }

    if (args.verbose) {
        std::cerr << "[vietlint] " << files.size() << " files to lint\n";
    }

    // Parallel lint
    int n_jobs = args.jobs > 0 ? args.jobs
                                : static_cast<int>(std::thread::hardware_concurrency());
    n_jobs = std::max(1, std::min(n_jobs, (int)files.size()));

    std::vector<std::vector<Diagnostic>> all_diags(files.size());
    std::vector<std::string>             file_sources(files.size());
    std::atomic<int> file_idx{0};

    auto worker = [&]() {
        while (true) {
            int idx = file_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= (int)files.size()) break;

            auto res = engine.lint_file(files[idx]);
            if (res) {
                all_diags[idx]   = std::move(*res);
                // Load source for context display
                std::ifstream f(files[idx], std::ios::binary);
                if (f) file_sources[idx] = std::string(
                    std::istreambuf_iterator<char>(f), {});
            } else if (args.verbose) {
                std::cerr << "[vietlint] error: " << res.error() << '\n';
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(n_jobs) - 1);
    for (int t = 1; t < n_jobs; ++t) threads.emplace_back(worker);
    worker(); // Use main thread too
    for (auto& t : threads) t.join();

    // Emit results
    bool has_errors   = false;
    bool has_warnings = false;

    for (size_t i = 0; i < files.size(); ++i) {
        if (all_diags[i].empty()) continue;
        emitter.emit(std::span<const Diagnostic>(all_diags[i]),
                     file_sources[i], std::cout);

        for (const auto& d : all_diags[i]) {
            if (d.violation.severity == Severity::Error ||
                d.violation.severity == Severity::Fatal)
                has_errors = true;
            else if (d.violation.severity == Severity::Warning)
                has_warnings = true;
        }

        // Apply fixes if requested
        if (args.fix) {
            bool fixed = apply_fix(files[i], std::span<const Diagnostic>(all_diags[i]));
            if (fixed && args.verbose)
                std::cerr << "[vietlint] fixed: " << files[i] << '\n';
        }
    }

    // Summary
    if (fmt == DiagnosticFormat::Text)
        emitter.emit_summary(engine.stats(), std::cout);

    if (has_errors)   return 2;
    if (has_warnings) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Mode: LSP server
// ---------------------------------------------------------------------------
static int run_lsp(const CliArgs& args) {
    if (args.lsp_verbose)
        std::cerr << "[vietlint-lsp] Starting LSP server on stdio\n";

    // Flush stderr immediately for LSP debugging
    std::cerr.setf(std::ios::unitbuf);

    LintConfig config;
    auto cfg_path = find_config();
    if (!cfg_path.empty()) {
        auto loaded = LintConfig::load_toml(cfg_path);
        if (loaded) config = std::move(*loaded);
    }

    auto engine = std::make_unique<RuleEngine>(std::move(config));
    engine->register_builtin_rules();

    lsp::LspServer server(std::move(engine), std::cin, std::cout);
    server.run();
    return 0;
}

// ---------------------------------------------------------------------------
// Mode: classify identifiers from stdin
// ---------------------------------------------------------------------------
static int run_classify(const CliArgs& args) {
    ml::MLClassifier clf(args.model_path.empty() ? fs::path{}
                                                  : fs::path(args.model_path));
    clf.warmup();

    std::cout << (clf.using_onnx() ? "Using ONNX model" : "Using heuristic classifier")
              << '\n';
    std::cout << std::string(50, '-') << '\n';

    static const char* CLASS_NAMES[] = {
        "pure_ascii","pure_vietnamese","mixed_vietnamese",
        "transliterated","abbreviation","unknown"
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto out = clf.classify(line);
        size_t cls = static_cast<size_t>(out.predicted_class);
        std::cout << '"' << line << '"'
                  << " -> " << CLASS_NAMES[cls < 6 ? cls : 5]
                  << " (confidence=" << out.confidence << ")\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mode: detect encoding
// ---------------------------------------------------------------------------
static int run_detect(const CliArgs& args) {
    EncodingDetector detector;
    auto paths = args.files.empty() ? std::vector<std::string>{"."} : args.files;

    for (const auto& p : paths) {
        std::ifstream f(p, std::ios::binary);
        if (!f) { std::cerr << "Cannot open: " << p << '\n'; continue; }
        std::string buf((std::istreambuf_iterator<char>(f)), {});
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
        auto result = detector.detect(raw);
        std::cout << p << ": " << result.charset_name
                  << " (confidence=" << result.confidence
                  << (result.has_bom ? ", BOM" : "")
                  << (result.needs_conversion ? ", NEEDS-CONVERSION" : "")
                  << ")\n";
        if (args.verbose)
            std::cout << "  " << result.message << '\n';
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mode: init — generate .vietlint.toml
// ---------------------------------------------------------------------------
static int run_init() {
    fs::path out = fs::current_path() / ".vietlint.toml";
    if (fs::exists(out)) {
        std::cout << ".vietlint.toml already exists. Overwrite? [y/N] ";
        char c = '\0';
        std::cin >> c;
        if (c != 'y' && c != 'Y') { std::cout << "Aborted.\n"; return 0; }
    }
    auto res = LintConfig::write_default(out);
    if (!res) { std::cerr << "Error: " << res.error() << '\n'; return 3; }
    std::cout << "Created: " << out << '\n';
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);

    if (args.mode_version) {
        std::cout << "VietLint 1.0.0\n"
                  << "C++20 core | "
#if defined(__AVX2__)
                  << "AVX2 SIMD | "
#else
                  << "Scalar | "
#endif
#if HAVE_CLANG
                  << "Clang LibTooling | "
#endif
#if HAVE_ONNX_RUNTIME
                  << "ONNX Runtime | "
#endif
                  << "pybind11\n"
                  << "https://github.com/vietlint/vietlint\n";
        return 0;
    }

    if (args.mode_lsp)      return run_lsp(args);
    if (args.mode_classify) return run_classify(args);
    if (args.mode_detect)   return run_detect(args);
    if (args.mode_init)     return run_init();

    // Default: lint
    return run_lint(args);
}
