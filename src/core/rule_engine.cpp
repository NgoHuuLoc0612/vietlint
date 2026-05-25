#include <mutex>
#include <shared_mutex>
#include "vietlint/rule_engine.hpp"
#include "vietlint/ml_classifier.hpp"
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <unordered_set>
#include <charconv>

namespace vietlint {

// ---------------------------------------------------------------------------
// RuleConfig helpers
// ---------------------------------------------------------------------------
std::string RuleConfig::param(std::string_view key, std::string_view default_val) const noexcept {
    auto it = params.find(std::string(key));
    return it != params.end() ? it->second : std::string(default_val);
}
bool RuleConfig::param_bool(std::string_view key, bool default_val) const noexcept {
    auto s = param(key);
    if (s.empty()) return default_val;
    return s == "true" || s == "1" || s == "yes";
}
int RuleConfig::param_int(std::string_view key, int default_val) const noexcept {
    auto s = param(key);
    if (s.empty()) return default_val;
    int v = default_val;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// ---------------------------------------------------------------------------
// Diagnostic formatter
// ---------------------------------------------------------------------------
std::string Diagnostic::format() const noexcept {
    std::ostringstream oss;
    oss << file << ':'
        << violation.span.line << ':'
        << violation.span.col  << ": ";
    switch (violation.severity) {
        case Severity::Error:   oss << "error";   break;
        case Severity::Warning: oss << "warning"; break;
        case Severity::Info:    oss << "info";    break;
        case Severity::Fatal:   oss << "fatal";   break;
    }
    oss << " [" << violation.rule_id << "] " << violation.message;
    return oss.str();
}

// ---------------------------------------------------------------------------
// RuleRegistry
// ---------------------------------------------------------------------------
RuleRegistry::RuleRegistry() noexcept = default;

void RuleRegistry::register_rule(Rule rule) noexcept {
    std::unique_lock lock(mutex_);
    rules_[rule.id] = std::move(rule);
}

const Rule* RuleRegistry::find(std::string_view id) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = rules_.find(std::string(id));
    return it != rules_.end() ? &it->second : nullptr;
}

std::vector<std::string> RuleRegistry::rule_ids() const noexcept {
    std::shared_lock lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(rules_.size());
    for (auto& [id, _] : rules_) ids.push_back(id);
    return ids;
}

std::vector<const Rule*>
RuleRegistry::rules_by_category(std::string_view cat) const noexcept {
    std::shared_lock lock(mutex_);
    std::vector<const Rule*> out;
    for (auto& [id, rule] : rules_)
        if (rule.category == cat) out.push_back(&rule);
    return out;
}

// ---------------------------------------------------------------------------
// Minimal TOML parser (subset for .vietlint.toml)
// Only handles: [section], key = "value", key = true/false, key = 123
// ---------------------------------------------------------------------------
static LintConfig parse_toml_content(const std::string& content) {
    LintConfig cfg;
    std::istringstream iss(content);
    std::string line;
    std::string current_section;
    std::string current_rule;

    auto trim = [](std::string_view s) -> std::string_view {
        size_t l = s.find_first_not_of(" \t\r\n");
        size_t r = s.find_last_not_of(" \t\r\n");
        if (l == std::string_view::npos) return "";
        return s.substr(l, r - l + 1);
    };

    auto unquote = [](std::string_view s) -> std::string {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return std::string(s.substr(1, s.size()-2));
        return std::string(s);
    };

    while (std::getline(iss, line)) {
        std::string_view lv(line);
        lv = trim(lv);
        if (lv.empty() || lv[0] == '#') continue;

        // Section header
        if (lv[0] == '[') {
            current_section = std::string(trim(lv.substr(1, lv.rfind(']') - 1)));
            // Check if it's a rule section like [rules.VL001]
            if (current_section.find("rules.") == 0) {
                current_rule = current_section.substr(6);
                if (cfg.rule_configs.find(current_rule) == cfg.rule_configs.end()) {
                    cfg.rule_configs[current_rule].rule_id = current_rule;
                }
            } else {
                current_rule.clear();
            }
            continue;
        }

        // Key = value
        auto eq = lv.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key = std::string(trim(lv.substr(0, eq)));
        std::string val = std::string(trim(lv.substr(eq + 1)));

        if (!current_rule.empty()) {
            // Rule-specific config
            auto& rc = cfg.rule_configs[current_rule];
            if (key == "enabled") rc.enabled = (val == "true");
            else if (key == "severity") {
                if (val == "\"error\"") rc.severity = Severity::Error;
                else if (val == "\"warning\"") rc.severity = Severity::Warning;
                else rc.severity = Severity::Info;
            } else {
                rc.params[key] = unquote(val);
            }
        } else if (current_section == "project") {
            if (key == "root") cfg.project_root = unquote(val);
        } else if (current_section == "lint") {
            if (key == "fail_on_warning") cfg.fail_on_warning = (val == "true");
            else if (key == "max_violations") { std::from_chars(val.data(), val.data()+val.size(), cfg.max_violations); }
            else if (key == "target_encoding") cfg.target_encoding = unquote(val);
            else if (key == "model") cfg.model_path = unquote(val);
        }
    }
    return cfg;
}

std::expected<LintConfig, std::string>
LintConfig::load_toml(const std::filesystem::path& path) noexcept {
    std::ifstream f(path);
    if (!f) return std::unexpected("Cannot open config: " + path.string());
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return parse_toml_content(content);
}

std::expected<void, std::string>
LintConfig::write_default(const std::filesystem::path& path) noexcept {
    std::ofstream f(path);
    if (!f) return std::unexpected("Cannot write config: " + path.string());
    f << R"(# VietLint Configuration
# https://github.com/NgoHuuLoc0612/vietlint

[project]
root = "."

[lint]
fail_on_warning = false
max_violations = 0
target_encoding = "utf-8"

[rules.VL001]
enabled = true
severity = "warning"

[rules.VL002]
enabled = true
severity = "warning"

[rules.VL003]
enabled = true
severity = "info"

[rules.VL004]
enabled = true
severity = "warning"

[rules.VL005]
enabled = true
severity = "error"

[rules.VL006]
enabled = true
severity = "info"

[rules.VL007]
enabled = false
severity = "info"
)";
    return {};
}

// ---------------------------------------------------------------------------
// RuleEngine
// ---------------------------------------------------------------------------
RuleEngine::RuleEngine(LintConfig config) noexcept
    : config_(std::move(config))
    , lexer_(LexerOptions{.preserve_comments = true, .track_vietnamese = true})
{}

bool RuleEngine::is_rule_enabled(std::string_view rule_id) const noexcept {
    auto it = config_.rule_configs.find(std::string(rule_id));
    if (it == config_.rule_configs.end()) return true; // enabled by default
    return it->second.enabled;
}

Severity RuleEngine::effective_severity(std::string_view rule_id, Severity def) const noexcept {
    auto it = config_.rule_configs.find(std::string(rule_id));
    if (it == config_.rule_configs.end()) return def;
    return it->second.severity;
}

void RuleEngine::record_diagnostic(const Diagnostic& d) const noexcept {
    stat_files_.fetch_add(0, std::memory_order_relaxed); // files counted elsewhere
    switch (d.violation.severity) {
        case Severity::Error: case Severity::Fatal:
            stat_errors_.fetch_add(1, std::memory_order_relaxed); break;
        case Severity::Warning:
            stat_warnings_.fetch_add(1, std::memory_order_relaxed); break;
        case Severity::Info:
            stat_infos_.fetch_add(1, std::memory_order_relaxed); break;
    }
}

RuleEngine::Stats RuleEngine::stats() const noexcept {
    return Stats{
        stat_files_.load(),
        stat_errors_.load(),
        stat_warnings_.load(),
        stat_infos_.load()
    };
}

void RuleEngine::reset_stats() noexcept {
    stat_files_ = stat_errors_ = stat_warnings_ = stat_infos_ = 0;
}

// ---------------------------------------------------------------------------
// Built-in rules
// ---------------------------------------------------------------------------
RuleResult RuleEngine::rule_vl001_vietnamese_identifier(const RuleContext& ctx) {
    VietnameseClassifier clf;
    return clf.check_conventions(ctx.tokens, ctx.language);
}

RuleResult RuleEngine::rule_vl002_mixed_language_comment(const RuleContext& ctx) {
    RuleResult violations;
    UTF8Scanner scanner;

    for (const auto& tok : ctx.tokens) {
        if (!tok.is_comment()) continue;

        auto raw_span = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(tok.raw.data()), tok.raw.size());
        bool has_viet = scanner.has_vietnamese_fast(raw_span);
        if (!has_viet) continue;

        // Detect if comment switches mid-way between English and Vietnamese
        // Heuristic: if there are both ASCII words and Vietnamese words in same comment
        bool has_ascii_words = false;
        for (char c : tok.raw) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                has_ascii_words = true; break;
            }
        }

        if (has_ascii_words && has_viet) {
            ConventionViolation v;
            v.rule_id  = "VL002";
            v.severity = Severity::Info;
            v.span     = tok.span;
            v.message  = "Comment mixes Vietnamese and English text. "
                         "Consider using a single language for consistency.";
            violations.push_back(std::move(v));
        }
    }
    return violations;
}

RuleResult RuleEngine::rule_vl003_encoding_declaration(const RuleContext& ctx) {
    RuleResult violations;
    if (ctx.tokens.empty()) return violations;

    // For Python files: check for encoding declaration in first two lines
    if (ctx.language != Language::Python) return violations;

    bool found_encoding_decl = false;
    int lines_checked = 0;
    for (const auto& tok : ctx.tokens) {
        if (tok.span.line > 2) break;
        if (tok.kind == TokenKind::EncodingDecl) {
            found_encoding_decl = true;
            // Validate it says utf-8
            if (tok.raw.find("utf-8") == std::string::npos &&
                tok.raw.find("utf8")  == std::string::npos) {
                ConventionViolation v;
                v.rule_id  = "VL003";
                v.severity = Severity::Warning;
                v.span     = tok.span;
                v.message  = "Non-UTF-8 encoding declaration found. "
                             "Vietnamese files should use UTF-8.";
                v.fixes    = { "# -*- coding: utf-8 -*-" };
                violations.push_back(std::move(v));
            }
            break;
        }
    }
    return violations;
}

RuleResult RuleEngine::rule_vl004_transliteration(const RuleContext& ctx) {
    VietnameseClassifier heuristic;
    ml::MLClassifier ml_clf(ctx.lint_config && !ctx.lint_config->model_path.empty() ? std::filesystem::path(ctx.lint_config->model_path) : std::filesystem::path{});
    // Whitelist common English words that model may misclassify
    static const std::unordered_set<std::string> ENGLISH_WHITELIST = {
        "token","database","manager","handler","service","controller",
        "repository","factory","builder","provider","listener","observer",
        "formatter","validator","serializer","parser","scanner","lexer",
        "compiler","emitter","renderer","dispatcher","router","middleware",
        "interceptor","decorator","adapter","connector","transformer",
        "cs","fv","idx","ptr","buf","src","dst","cfg","ctx","err",
        "ret","tmp","val","res","req","msg","num","pos","off","fn",
        "ok","no","op","io","db","ui","id","os","fs","vm","api",
    };
    RuleResult violations;
    for (const auto& tok : ctx.tokens) {
        if (!tok.is_identifier()) continue;
        bool is_transliterated = false;
        float confidence = 0.0f;
        if (ml_clf.using_onnx()) {
            auto out = ml_clf.classify(tok.raw);
            is_transliterated = (static_cast<int>(out.predicted_class) == 3);
            confidence = out.confidence;
        } else {
            auto analysis = heuristic.classify(tok);
            is_transliterated = (analysis.cls == IdentifierClass::TransliteratedViet);
            confidence = analysis.confidence;
        }
        // Check whitelist
        std::string lower_tok = tok.raw;
        std::transform(lower_tok.begin(), lower_tok.end(), lower_tok.begin(),
            [](unsigned char c){ return std::tolower(c); });
        if (ENGLISH_WHITELIST.count(lower_tok)) { is_transliterated = false; }
        if (is_transliterated && confidence >= 0.7f) {
            ConventionViolation v;
            v.rule_id   = "VL004";
            v.severity  = Severity::Info;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "'" + tok.raw + "' looks like a transliterated Vietnamese word. "
                          "Use descriptive English names for clarity.";
            // Auto-fix: suggest snake_case and camelCase equivalents
            // For transliterated identifiers, the raw form is already the fix suggestion
            // (it's already ASCII — user should rename to descriptive English)
            // We suggest keeping as-is or converting style
            {
                VietnameseClassifier fix_clf;
                auto ascii = fix_clf.suggest_ascii_equivalent(tok.raw);
                if (!ascii.empty()) v.fixes.push_back(ascii);
            }
            violations.push_back(std::move(v));
        }
    }
    return violations;
}

RuleResult RuleEngine::rule_vl005_unicode_normalization(const RuleContext& ctx) {
    // Check for non-NFC normalized Vietnamese (precomposed vs decomposed)
    // e.g., e-acute = U+0065 U+0301 (NFD) vs U+00E9 (NFC)
    RuleResult violations;
    UTF8Scanner scanner;

    for (const auto& tok : ctx.tokens) {
        if (!tok.is_identifier()) continue;
        auto raw_span = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(tok.raw.data()), tok.raw.size());
        auto cps = scanner.scan(raw_span);
        if (!cps) continue;

        // Check for combining diacritical marks following base chars (NFD indicator)
        bool has_combining = false;
        for (size_t i = 0; i + 1 < cps->size(); ++i) {
            uint32_t cp = (*cps)[i+1].codepoint;
            if (cp >= 0x0300 && cp <= 0x036F) { has_combining = true; break; }
        }

        if (has_combining) {
            ConventionViolation v;
            v.rule_id   = "VL005";
            v.severity  = Severity::Error;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "Identifier '" + tok.raw + "' uses NFD (decomposed) Unicode normalization. "
                          "Use NFC (precomposed) forms for Vietnamese characters.";
            violations.push_back(std::move(v));
        }
    }
    return violations;
}

RuleResult RuleEngine::rule_vl006_consistent_naming(const RuleContext& ctx) {
    // Detect inconsistent naming styles within the same file
    RuleResult violations;
    std::unordered_map<std::string, int> style_counts;

    for (const auto& tok : ctx.tokens) {
        if (!tok.is_identifier() || tok.raw.size() <= 1) continue;
        auto style = VietnameseClassifier::detect_style(tok.raw);
        switch (style) {
            case NamingStyle::SnakeCase:     style_counts["snake"]++;    break;
            case NamingStyle::CamelCase:     style_counts["camel"]++;    break;
            case NamingStyle::PascalCase:    style_counts["pascal"]++;   break;
            case NamingStyle::ScreamingSnake:style_counts["screaming"]++;break;
            default: break;
        }
    }

    // If more than 2 styles are prevalent (>5% each), flag inconsistency
    int dominant_count = 0;
    std::string dominant_style;
    for (auto& [style, cnt] : style_counts) {
        if (cnt > dominant_count) { dominant_count = cnt; dominant_style = style; }
    }

    int total = 0;
    for (auto& [s, c] : style_counts) total += c;
    if (total == 0) return violations;

    int mixed_styles = 0;
    for (auto& [style, cnt] : style_counts) {
        if (style != dominant_style && static_cast<float>(cnt)/total > 0.05f)
            ++mixed_styles;
    }

    // File-level violation (point to first token)
    if (mixed_styles >= 2 && !ctx.tokens.empty()) {
        ConventionViolation v;
        v.rule_id  = "VL006";
        v.severity = Severity::Info;
        v.span     = ctx.tokens[0].span;
        v.message  = "File uses mixed naming styles. Dominant: " + dominant_style + ". "
                     "Consider standardizing to one style.";
        violations.push_back(std::move(v));
    }
    return violations;
}

RuleResult RuleEngine::rule_vl007_viet_in_string_literal(const RuleContext& ctx) {
    RuleResult violations;
    UTF8Scanner scanner;
    for (const auto& tok : ctx.tokens) {
        if (tok.kind != TokenKind::StringLiteral) continue;
        auto raw_span = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(tok.raw.data()), tok.raw.size());
        if (scanner.has_vietnamese_fast(raw_span)) {
            ConventionViolation v;
            v.rule_id  = "VL007";
            v.severity = Severity::Info;
            v.span     = tok.span;
            v.message  = "String literal contains Vietnamese characters. "
                         "Consider externalizing to a localization file.";
            violations.push_back(std::move(v));
        }
    }
    return violations;
}

// ---------------------------------------------------------------------------
void RuleEngine::register_builtin_rules() noexcept {
    registry_.register_rule({
        "VL001", "Vietnamese Identifier",
        "Detects identifiers containing Vietnamese Unicode characters",
        "naming", Severity::Warning,
        rule_vl001_vietnamese_identifier
    });
    registry_.register_rule({
        "VL002", "Mixed-Language Comment",
        "Detects comments that mix Vietnamese and English text",
        "comments", Severity::Info,
        rule_vl002_mixed_language_comment
    });
    registry_.register_rule({
        "VL003", "Encoding Declaration",
        "Validates encoding declaration in Python files",
        "encoding", Severity::Warning,
        rule_vl003_encoding_declaration
    });
    registry_.register_rule({
        "VL004", "Transliteration",
        "Detects Vietnamese words used without diacritics",
        "naming", Severity::Info,
        rule_vl004_transliteration
    });
    registry_.register_rule({
        "VL005", "Unicode Normalization",
        "Ensures Vietnamese chars use NFC (precomposed) normalization",
        "encoding", Severity::Error,
        rule_vl005_unicode_normalization
    });
    registry_.register_rule({
        "VL006", "Consistent Naming Style",
        "Checks for consistent naming convention usage within file",
        "style", Severity::Info,
        rule_vl006_consistent_naming
    });
    registry_.register_rule({
        "VL007", "Vietnamese in String Literals",
        "Detects Vietnamese text in string literals (i18n concern)",
        "style", Severity::Info,
        rule_vl007_viet_in_string_literal
    });
}

// ---------------------------------------------------------------------------
std::vector<Diagnostic>
RuleEngine::lint_source(std::string_view source, std::string_view filename) const noexcept {
    stat_files_.fetch_add(1, std::memory_order_relaxed);
    std::vector<Diagnostic> diagnostics;

    Language lang = detect_language(filename);
    auto tokens_result = lexer_.tokenize(source, filename);
    if (!tokens_result) {
        Diagnostic d;
        d.file = std::string(filename);
        d.violation.rule_id = "VL000";
        d.violation.severity = Severity::Error;
        d.violation.message  = "Lexer error: " + tokens_result.error().message;
        diagnostics.push_back(std::move(d));
        return diagnostics;
    }

    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(source.data()), source.size());
    auto enc = encoding_detector_.detect(raw_span);

    RuleContext ctx{
        source, filename, lang,
        std::span<const Token>(*tokens_result),
        &enc, nullptr, &config_
    };

    for (const auto& rule_id : registry_.rule_ids()) {
        if (!is_rule_enabled(rule_id)) continue;
        const Rule* rule = registry_.find(rule_id);
        if (!rule) continue;

        RuleConfig rc;
        auto cfg_it = config_.rule_configs.find(rule_id);
        if (cfg_it != config_.rule_configs.end()) rc = cfg_it->second;
        ctx.config = &rc;

        auto results = rule->check(ctx);
        for (auto& v : results) {
            v.severity = effective_severity(rule_id, v.severity);
            Diagnostic d;
            d.file       = std::string(filename);
            d.violation  = std::move(v);
            d.rule_name  = rule->name;
            record_diagnostic(d);
            diagnostics.push_back(std::move(d));
        }

        if (config_.max_violations > 0 &&
            (int)diagnostics.size() >= config_.max_violations) break;
    }

    return diagnostics;
}

std::expected<std::vector<Diagnostic>, std::string>
RuleEngine::lint_file(const std::filesystem::path& file) const noexcept {
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::unexpected("Cannot open file: " + file.string());

    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    auto enc = encoding_detector_.detect(raw_span);

    std::string source_utf8;
    if (enc.needs_conversion) {
        auto conv = encoding_detector_.normalize_to_utf8(raw_span, enc);
        if (!conv) return std::unexpected(conv.error());
        source_utf8 = std::move(*conv);
    } else {
        source_utf8 = std::move(raw);
    }

    stat_files_.fetch_add(1, std::memory_order_relaxed);
    return lint_source(source_utf8, file.string());
}

RuleResult RuleEngine::run_rule(std::string_view rule_id, const RuleContext& ctx) const noexcept {
    const Rule* rule = registry_.find(rule_id);
    if (!rule) return {};
    return rule->check(ctx);
}

} // namespace vietlint
