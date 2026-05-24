#include "vietlint/classifier.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <numeric>
#include <sstream>

namespace vietlint {

// ---------------------------------------------------------------------------
// Vietnamese word corpus (curated subset of 500 most common identifier words)
// ---------------------------------------------------------------------------
static constexpr std::string_view VIET_WORD_TABLE[] = {
    "bai","ban","bao","bat","bien","bo","cai","cam","can","cao",
    "cau","chi","cho","chu","co","cong","cot","cua","cuoi","dang",
    "dao","dat","dau","den","di","dia","diem","du","dung","duoc",
    "gia","giao","gioi","giup","gui","hang","hien","hieu","hinh","ho",
    "hoc","hop","ke","ket","khai","kham","khach","khi","kiem",
    "ky","lai","lam","lien","loai","lon","lua",
    "luong","ly","ma","mat","mau","menu","mo","moi","mon","muc",
    "muc","nam","nhap","nhiem","nho","nhom","nut","phan","phat","phieu",
    "phong","quan","quyen","quet","san","so","soan","su","tai","tam",
    "ten","thay","them","thong","thu","thuc","tiet","tim","tin","tinh",
    "to","toan","toan","tong","trong","truyen","tuan","tuy","uong",
    "vai","van","vao","viec","vu","xem","xoa","xuat","yeu","khachhang","nhanvien","soluong","tenkhachhang","danhsach","quanly",
    // longer words
    "bienban","bienche","chinh","chinhsach","chucnang","chucvu",
    "danhsach","danhsachkhach","dieukien","donhang","donvi",
    "giamsat","giaodienvGUI","giaotiep","hethong","hoatdong",
    "kiemtra","kiemsoat","khachhang","loainhom","loaitin",
    "madat","mahoa","makhach","manhinh","matkhau","nhanvien",
    "nguoidung","nhanvienkinhdoanh","phanbiet","phanquyen",
    "quanly","quanlynhanvien","quanlykho","sanpham","soluong",
    "thanhphan","thongtinnguoidung","thongbao","trangthai",
    "tiepnhan","timkiem","tuychinhtuy","xulyanh","xuatbao",
};

static constexpr std::string_view VIET_ABBREV_TABLE[] = {
    "BH","BN","CB","CD","CH","CN","CT","CV","DA","DL","DM","QLKH","QLNV","NVKD",
    "DN","DS","DT","DV","GD","GV","HH","HT","ID","KH","KQ",
    "KT","MA","MB","MH","ML","ND","NH","NL","NV","PB","PC",
    "QC","QL","QLY","QT","SL","SP","ST","TB","TG","TH","TK",
    "TL","TM","TN","TP","TQ","TT","TV","TX","VA","VT","XL",
};

// ---------------------------------------------------------------------------
void VietnameseClassifier::init_word_lists() noexcept {
    for (auto w : VIET_WORD_TABLE)
        viet_word_list_.emplace_back(w);
    std::sort(viet_word_list_.begin(), viet_word_list_.end());

    for (auto a : VIET_ABBREV_TABLE)
        viet_abbrev_list_.emplace_back(a);
    std::sort(viet_abbrev_list_.begin(), viet_abbrev_list_.end());
}

void VietnameseClassifier::init_char_frequencies() noexcept {
    // Vietnamese most frequent diacritical codepoints (from corpus analysis)
    static constexpr struct { uint32_t cp; float freq; } freqs[] = {
        {0x1EBF,0.042f},{0x1EB9,0.038f},{0x1EBB,0.035f},{0x1EB7,0.031f},
        {0x1EC9,0.028f},{0x1ECA,0.025f},{0x1ECB,0.022f},{0x1ECC,0.019f},
        {0x1ED9,0.018f},{0x1ED1,0.017f},{0x1ED3,0.016f},{0x1ED5,0.015f},
        {0x1EAD,0.014f},{0x1EAF,0.013f},{0x1EB1,0.012f},{0x1EB3,0.011f},
        {0x00E0,0.010f},{0x00E1,0.009f},{0x00E2,0.009f},{0x00E3,0.008f},
        {0x00E8,0.008f},{0x00E9,0.008f},{0x00EA,0.007f},{0x00EC,0.007f},
        {0x00ED,0.007f},{0x00F2,0.006f},{0x00F3,0.006f},{0x00F4,0.006f},
        {0x00F5,0.005f},{0x00FA,0.005f},{0x00F9,0.005f},{0x00FD,0.004f},
    };
    for (auto& [cp, freq] : freqs)
        viet_char_frequency_[cp] = freq;
}

void VietnameseClassifier::init_patterns() {
    // Vietnamese Unicode character class (simplified)
    // Use has_vietnamese_fast() for multibyte detection; regex used for word boundaries
    viet_char_pattern_ = std::regex(
        "ten|tuoi|dia|dien|mat|khau|so|luong|khach|hang|nhan|vien",
        std::regex::ECMAScript | std::regex::optimize
    );

    // Transliteration: patterns that look like Vietnamese without diacritics
    // Common Vietnamese word patterns: C(V)(T) syllable structure
    transliteration_pattern_ = std::regex(
        "^(ban|bao|bat|bien|bo|cai|cam|can|cao|cau|chi|cho|chu|co|cong|"
        "cot|cua|dang|dao|dat|dau|den|dia|diem|du|dung|gia|giao|gioi|"
        "giup|gui|hang|hien|hieu|hinh|ho|hoc|hop|ke|ket|khai|khi|"
        "kiem|lai|lam|lien|lo|loai|lua|luong|mat|mau|mo|moi|mon|"
        "muc|nhap|nhom|nut|phan|phat|phong|quan|quyen|san|soan|su|"
        "tam|ten|thay|them|thong|thu|thuc|tim|tin|tinh|toan|tong|"
        "trong|tu|uong|van|vao|viec|viet|vu|xem|xoa|xuat|yeu)"
        "(_[a-z][a-z0-9_]*)?$",
        std::regex::ECMAScript | std::regex::optimize | std::regex::icase
    );
}

// ---------------------------------------------------------------------------
VietnameseClassifier::VietnameseClassifier() {
    init_word_lists();
    init_char_frequencies();
    init_patterns();
}

// ---------------------------------------------------------------------------
float VietnameseClassifier::compute_vietnamese_ratio(std::string_view id) const noexcept {
    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(id.data()), id.size());
    auto cps = scanner_.scan(raw_span);
    if (!cps || cps->empty()) return 0.0f;

    uint32_t total = 0, viet = 0;
    for (auto& cp : *cps) {
        if (cp.codepoint == '_' || cp.codepoint == '-') continue; // skip separators
        ++total;
        if (cp.is_vietnamese) ++viet;
    }
    return total > 0 ? static_cast<float>(viet) / static_cast<float>(total) : 0.0f;
}

// ---------------------------------------------------------------------------
NamingStyle VietnameseClassifier::detect_style(std::string_view id) noexcept {
    if (id.empty()) return NamingStyle::Unknown;
    if (id.size() == 1) return NamingStyle::Single;

    bool has_underscore = id.find('_') != std::string_view::npos;
    bool has_hyphen     = id.find('-') != std::string_view::npos;
    if (has_hyphen) return NamingStyle::KebabCase;

    bool all_upper = true, has_lower = false, has_upper = false;
    for (char c : id) {
        if (!std::isalpha((unsigned char)c) && c != '_' && static_cast<uint8_t>(c) < 0x80) continue;
        if (std::islower((unsigned char)c)) { has_lower = true; all_upper = false; }
        if (std::isupper((unsigned char)c)) { has_upper = true; }
    }

    if (has_underscore && all_upper && !has_lower)
        return NamingStyle::ScreamingSnake;
    if (has_underscore && has_lower)
        return NamingStyle::SnakeCase;
    if (!has_underscore && has_upper && has_lower && std::isupper((unsigned char)id[0]))
        return NamingStyle::PascalCase;
    if (!has_underscore && has_upper && has_lower && std::islower((unsigned char)id[0]))
        return NamingStyle::CamelCase;

    return NamingStyle::Unknown;
}

// ---------------------------------------------------------------------------
bool VietnameseClassifier::is_vietnamese_transliteration(std::string_view id) const noexcept {
    // Lowercase the identifier for comparison
    std::string lower(id);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c){ return std::tolower(c); });

    // Strip leading/trailing underscores
    auto start = lower.find_first_not_of('_');
    auto end   = lower.find_last_not_of('_');
    if (start == std::string::npos) return false;
    lower = lower.substr(start, end - start + 1);

    // Binary search in word list (exact match)
    if (std::binary_search(viet_word_list_.begin(), viet_word_list_.end(), lower))
        return true;
    // Also check if all underscore-split parts are Vietnamese words
    if (lower.find('_') != std::string::npos) {
        std::istringstream ss(lower);
        std::string part;
        bool all_viet = true; size_t parts = 0;
        while (std::getline(ss, part, '_')) {
            if (part.empty()) continue;
            ++parts;
            if (!std::binary_search(viet_word_list_.begin(), viet_word_list_.end(), part))
                all_viet = false;
        }
        if (all_viet && parts >= 2) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
IdentifierAnalysis VietnameseClassifier::classify_string(std::string_view id) const noexcept {
    IdentifierAnalysis result;
    result.identifier    = std::string(id);
    result.style         = detect_style(id);
    result.confidence    = 0.0f;

    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(id.data()), id.size());

    result.has_diacritics = scanner_.has_vietnamese_fast(raw_span);
    result.vietnamese_ratio = compute_vietnamese_ratio(id);

    if (result.vietnamese_ratio == 0.0f) {
        // Pure ASCII - check if transliterated Vietnamese
        if (is_vietnamese_transliteration(id)) {
            result.cls        = IdentifierClass::TransliteratedViet;
            result.confidence = 0.72f;
        } else {
            result.cls        = IdentifierClass::PureASCII;
            result.confidence = 0.95f;
        }
    } else if (result.vietnamese_ratio >= 0.90f) {
        result.cls        = IdentifierClass::PureVietnamese;
        result.confidence = 0.88f;
    } else {
        result.cls        = IdentifierClass::MixedVietnamese;
        result.confidence = 0.80f;
    }

    // Check abbreviation: all caps, 2-5 chars, in our list
    if (id.size() >= 2 && id.size() <= 5) {
        bool all_cap = true;
        for (char c : id) if (!std::isupper((unsigned char)c)) { all_cap = false; break; }
        if (all_cap && std::binary_search(viet_abbrev_list_.begin(), viet_abbrev_list_.end(), std::string(id))) {
            result.cls        = IdentifierClass::Abbreviation;
            result.confidence = 0.85f;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
IdentifierAnalysis VietnameseClassifier::classify(const Token& tok) const noexcept {
    auto result = classify_string(tok.raw);
    result.is_keyword = (tok.kind == TokenKind::Keyword);
    return result;
}

// ---------------------------------------------------------------------------
std::vector<IdentifierAnalysis>
VietnameseClassifier::classify_batch(std::span<const Token> tokens) const noexcept {
    std::vector<IdentifierAnalysis> out;
    out.reserve(tokens.size());
    for (const auto& t : tokens) {
        if (t.is_identifier())
            out.push_back(classify(t));
    }
    return out;
}

// ---------------------------------------------------------------------------
std::string VietnameseClassifier::suggest_ascii_equivalent(std::string_view viet_id) const noexcept {
    // Simple diacritic stripping using a lookup table
    // U+1EXX -> ASCII base char
    static constexpr struct { uint32_t cp; char base; } strip_table[] = {
        {0xc0,'a'},{0xc1,'a'},{0xc2,'a'},{0xc3,'a'},{0xc8,'e'},{0xc9,'e'},{0xca,'e'},{0xcc,'i'},
        {0xcd,'i'},{0xd2,'o'},{0xd3,'o'},{0xd4,'o'},{0xd5,'o'},{0xd9,'u'},{0xda,'u'},{0xdd,'y'},
        {0xe0,'a'},{0xe1,'a'},{0xe2,'a'},{0xe3,'a'},{0xe8,'e'},{0xe9,'e'},{0xea,'e'},{0xec,'i'},
        {0xed,'i'},{0xf2,'o'},{0xf3,'o'},{0xf4,'o'},{0xf5,'o'},{0xf9,'u'},{0xfa,'u'},{0xfd,'y'},
        {0x102,'a'},{0x103,'a'},{0x110,'d'},{0x111,'d'},{0x1a0,'o'},{0x1a1,'o'},{0x1af,'u'},{0x1b0,'u'},
        {0x1ea0,'a'},{0x1ea1,'a'},{0x1ea2,'a'},{0x1ea3,'a'},{0x1ea4,'a'},{0x1ea5,'a'},{0x1ea6,'a'},{0x1ea7,'a'},
        {0x1ea8,'a'},{0x1ea9,'a'},{0x1eaa,'a'},{0x1eab,'a'},{0x1eac,'a'},{0x1ead,'a'},{0x1eae,'a'},{0x1eaf,'a'},
        {0x1eb0,'a'},{0x1eb1,'a'},{0x1eb2,'a'},{0x1eb3,'a'},{0x1eb4,'a'},{0x1eb5,'a'},{0x1eb6,'a'},{0x1eb7,'a'},
        {0x1eb8,'e'},{0x1eb9,'e'},{0x1eba,'e'},{0x1ebb,'e'},{0x1ebc,'e'},{0x1ebd,'e'},{0x1ebe,'e'},{0x1ebf,'e'},
        {0x1ec0,'e'},{0x1ec1,'e'},{0x1ec2,'e'},{0x1ec3,'e'},{0x1ec4,'e'},{0x1ec5,'e'},{0x1ec6,'e'},{0x1ec7,'e'},
        {0x1ec8,'i'},{0x1ec9,'i'},{0x1eca,'i'},{0x1ecb,'i'},{0x1ecc,'o'},{0x1ecd,'o'},{0x1ece,'o'},{0x1ecf,'o'},
        {0x1ed0,'o'},{0x1ed1,'o'},{0x1ed2,'o'},{0x1ed3,'o'},{0x1ed4,'o'},{0x1ed5,'o'},{0x1ed6,'o'},{0x1ed7,'o'},
        {0x1ed8,'o'},{0x1ed9,'o'},{0x1eda,'o'},{0x1edb,'o'},{0x1edc,'o'},{0x1edd,'o'},{0x1ede,'o'},{0x1edf,'o'},
        {0x1ee0,'o'},{0x1ee1,'o'},{0x1ee2,'o'},{0x1ee3,'o'},{0x1ee4,'u'},{0x1ee5,'u'},{0x1ee6,'u'},{0x1ee7,'u'},
        {0x1ee8,'u'},{0x1ee9,'u'},{0x1eea,'u'},{0x1eeb,'u'},{0x1eec,'u'},{0x1eed,'u'},{0x1eee,'u'},{0x1eef,'u'},
        {0x1ef0,'u'},{0x1ef1,'u'},{0x1ef2,'y'},{0x1ef3,'y'},{0x1ef4,'y'},{0x1ef5,'y'},{0x1ef6,'y'},{0x1ef7,'y'},
        {0x1ef8,'y'},{0x1ef9,'y'},
    };

    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(viet_id.data()), viet_id.size());
    auto cps = scanner_.scan(raw_span);
    if (!cps) return std::string(viet_id);

    std::string result;
    result.reserve(viet_id.size());
    for (auto& cp : *cps) {
        if (cp.codepoint < 0x80) {
            result += static_cast<char>(cp.codepoint);
            continue;
        }
        // Binary search in strip table
        auto it = std::lower_bound(std::begin(strip_table), std::end(strip_table),
            cp.codepoint, [](const auto& entry, uint32_t val){ return entry.cp < val; });
        if (it != std::end(strip_table) && it->cp == cp.codepoint)
            result += it->base;
        else
            result += '?'; // unmapped
    }
    return result;
}

// ---------------------------------------------------------------------------
std::vector<std::string> VietnameseClassifier::compute_style_suggestions(
    std::string_view id, NamingStyle /*current*/, Language lang) const noexcept {

    std::string ascii = suggest_ascii_equivalent(id);
    std::vector<std::string> suggestions;

    // snake_case suggestion
    std::string snake;
    bool prev_upper = false;
    for (char c : ascii) {
        if (std::isupper((unsigned char)c) && !snake.empty() && !prev_upper) snake += '_';
        snake += static_cast<char>(std::tolower((unsigned char)c));
        prev_upper = std::isupper((unsigned char)c);
    }
    if (!snake.empty()) suggestions.push_back(snake);

    // camelCase suggestion
    if (!ascii.empty()) {
        std::string camel = ascii;
        camel[0] = static_cast<char>(std::tolower((unsigned char)camel[0]));
        suggestions.push_back(camel);
    }

    return suggestions;
}

// ---------------------------------------------------------------------------
std::vector<ConventionViolation>
VietnameseClassifier::check_conventions(std::span<const Token> tokens, Language lang) const noexcept {
    std::vector<ConventionViolation> violations;

    for (const auto& tok : tokens) {
        if (!tok.is_identifier()) continue;

        auto analysis = classify(tok);

        // Rule VL001: Vietnamese identifier detected
        if (analysis.cls == IdentifierClass::PureVietnamese
            || analysis.cls == IdentifierClass::MixedVietnamese) {
            ConventionViolation v;
            v.rule_id   = "VL001";
            v.severity  = Severity::Warning;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "Identifier '" + tok.raw + "' contains Vietnamese characters. "
                          "Consider using ASCII equivalents for better portability.";
            v.fixes     = compute_style_suggestions(tok.raw, analysis.style, lang);
            violations.push_back(std::move(v));
        }

        // Rule VL002: Mixed-language naming (Vietnamese + English parts)
        if (analysis.cls == IdentifierClass::MixedVietnamese) {
            ConventionViolation v;
            v.rule_id   = "VL002";
            v.severity  = Severity::Warning;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "Identifier '" + tok.raw + "' mixes Vietnamese and ASCII characters.";
            v.fixes     = { suggest_ascii_equivalent(tok.raw) };
            violations.push_back(std::move(v));
        }

        // Rule VL003: Transliterated Vietnamese (no diacritics but clearly Vietnamese)
        if (analysis.cls == IdentifierClass::TransliteratedViet && analysis.confidence >= 0.7f) {
            ConventionViolation v;
            v.rule_id   = "VL003";
            v.severity  = Severity::Info;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "Identifier '" + tok.raw + "' appears to be a transliterated "
                          "Vietnamese word. Consider using a more descriptive English name.";
            violations.push_back(std::move(v));
        }

        // Rule VL004: Inconsistent naming style in Vietnamese context
        if (analysis.has_diacritics && analysis.style == NamingStyle::MixedCase) {
            ConventionViolation v;
            v.rule_id   = "VL004";
            v.severity  = Severity::Warning;
            v.span      = tok.span;
            v.identifier = tok.raw;
            v.message   = "Vietnamese identifier '" + tok.raw + "' has inconsistent naming style.";
            violations.push_back(std::move(v));
        }
    }

    return violations;
}

} // namespace vietlint
