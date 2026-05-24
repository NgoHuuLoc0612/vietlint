#include "vietlint/emitter.hpp"
#include "vietlint/rule_engine.hpp"
#include "vietlint/utf8_scanner.hpp"

// ---------------------------------------------------------------------------
// VietLint Example Plugin
// Demonstrates how to write a third-party rule plugin.
//
// Build as shared library and place in plugins/ directory.
// Loaded automatically if plugin_paths is configured in .vietlint.toml.
//
// Required export: extern "C" void vietlint_register(PluginContext*)
// ---------------------------------------------------------------------------

namespace {

/// Custom rule VL-EX-001: Detects identifiers that look like phone numbers
/// (common in Vietnamese business apps where phone numbers appear as variable names)
vietlint::RuleResult rule_phone_number_identifier(const vietlint::RuleContext& ctx) {
    vietlint::RuleResult violations;

    // Pattern: identifiers that are purely numeric or look like VN phone numbers
    // e.g., sdt0905123456, so_dien_thoai_0905123456
    static const std::regex phone_re(
        R"((?:sdt|dienthoai|sodienthoai|phone)[_]?0[0-9]{9})",
        std::regex::ECMAScript | std::regex::icase
    );

    for (const auto& tok : ctx.tokens) {
        if (!tok.is_identifier()) continue;
        if (std::regex_match(tok.raw, phone_re)) {
            vietlint::ConventionViolation v;
            v.rule_id   = "VL-EX-001";
            v.severity  = vietlint::Severity::Warning;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "Identifier '" + tok.raw + "' embeds a phone number. "
                          "Use a generic name like 'customer_phone' instead.";
            v.fixes     = { "customer_phone", "phone_number" };
            violations.push_back(std::move(v));
        }
    }
    return violations;
}

/// Custom rule VL-EX-002: Detects common Vietnamese abbreviations used as
/// variable names without context (e.g., 'hs' for 'hoc_sinh', 'nv' for 'nhan_vien')
vietlint::RuleResult rule_ambiguous_abbreviation(const vietlint::RuleContext& ctx) {
    vietlint::RuleResult violations;

    static const std::unordered_map<std::string, std::string> ambiguous_abbrevs = {
        {"hs",  "hoc_sinh (student)"},
        {"nv",  "nhan_vien (employee)"},
        {"kh",  "khach_hang (customer)"},
        {"gv",  "giao_vien (teacher)"},
        {"bv",  "benh_vien (hospital)"},
        {"ql",  "quan_ly (manager)"},
        {"sp",  "san_pham (product)"},
        {"dh",  "dai_hoc (university)"},
        {"tk",  "tai_khoan (account)"},
        {"mk",  "mat_khau (password)"},
    };

    for (const auto& tok : ctx.tokens) {
        if (!tok.is_identifier()) continue;
        // Only flag standalone 2-letter lowercase identifiers
        if (tok.raw.size() != 2) continue;

        std::string lower = tok.raw;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c){ return std::tolower(c); });

        auto it = ambiguous_abbrevs.find(lower);
        if (it != ambiguous_abbrevs.end()) {
            vietlint::ConventionViolation v;
            v.rule_id    = "VL-EX-002";
            v.severity   = vietlint::Severity::Info;
            v.span       = tok.span;
            v.identifier = tok.raw;
            v.message    = "Ambiguous abbreviation '" + tok.raw + "' — "
                           "likely Vietnamese shorthand for " + it->second + ". "
                           "Use the full word for readability.";
            violations.push_back(std::move(v));
        }
    }
    return violations;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin registration entry point (must be exported with C linkage)
// ---------------------------------------------------------------------------
extern "C" void vietlint_register(vietlint::PluginContext* ctx) {
    if (!ctx || ctx->api_version != vietlint::PLUGIN_API_VERSION) {
        return; // API version mismatch — refuse to load
    }

    ctx->registry->register_rule({
        "VL-EX-001",
        "Phone Number in Identifier",
        "Detects phone numbers embedded in Vietnamese identifier names",
        "naming",
        vietlint::Severity::Warning,
        rule_phone_number_identifier
    });

    ctx->registry->register_rule({
        "VL-EX-002",
        "Ambiguous Vietnamese Abbreviation",
        "Detects common two-letter Vietnamese shorthand identifiers (hs, nv, kh, ...)",
        "naming",
        vietlint::Severity::Info,
        rule_ambiguous_abbreviation
    });
}
