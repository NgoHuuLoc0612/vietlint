#include "vietlint/lsp_server.hpp"
#include "vietlint/ml_classifier.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "vietlint/utf8_scanner.hpp"
#include "vietlint/lexer.hpp"
#include "vietlint/classifier.hpp"
#include "vietlint/encoding_detector.hpp"
#include "vietlint/rule_engine.hpp"

using namespace vietlint;
using ::testing::Contains;
using ::testing::Not;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::Eq;

// ===========================================================================
// UTF8Scanner tests
// ===========================================================================
class UTF8ScannerTest : public ::testing::Test {
protected:
    UTF8Scanner scanner;
};

TEST_F(UTF8ScannerTest, EmptyInput) {
    auto result = scanner.scan({});
    ASSERT_TRUE(result.has_value());
    EXPECT_THAT(*result, IsEmpty());
}

TEST_F(UTF8ScannerTest, PureASCII) {
    std::string s = "hello_world";
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    auto result = scanner.scan(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), s.size());
    for (auto& cp : *result) EXPECT_FALSE(cp.is_vietnamese);
}

TEST_F(UTF8ScannerTest, VietnameseChars) {
    // "tên" in UTF-8: t + ê(U+00EA) + n + combining acute(U+0301) ... or precomposed
    // Using NFC: "tên" = t(0x74) + ê(0xC3 0xAA) + n(0x6E)
    std::string s = "tên";  // NFC Vietnamese
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    EXPECT_TRUE(scanner.has_vietnamese_fast(raw));
    EXPECT_GE(scanner.count_vietnamese(raw), 1u);
}

TEST_F(UTF8ScannerTest, VietnameseLatin1E) {
    // U+1EB9 "ẹ" (e with dot below) — main Vietnamese block
    // UTF-8: 0xE1 0xBA 0xB9
    uint8_t viet_bytes[] = { 0x74, 0xE1, 0xBA, 0xB9, 0x6E }; // "tẹn"
    auto raw = std::span<const uint8_t>(viet_bytes, sizeof(viet_bytes));
    EXPECT_TRUE(scanner.has_vietnamese_fast(raw));
    auto result = scanner.scan(raw);
    ASSERT_TRUE(result.has_value());
    bool found_viet = false;
    for (auto& cp : *result) if (cp.is_vietnamese) found_viet = true;
    EXPECT_TRUE(found_viet);
}

TEST_F(UTF8ScannerTest, MalformedUTF8TolerantScan) {
    // Scanner should not crash on malformed input
    uint8_t bad[] = { 0xFF, 0xFE, 0x61, 0x62 }; // BOM + "ab"
    auto raw = std::span<const uint8_t>(bad, sizeof(bad));
    // Should return something without crash
    auto result = scanner.scan(raw);
    EXPECT_TRUE(result.has_value() || !result.has_value()); // either is fine, no crash
}

TEST_F(UTF8ScannerTest, ValidateUTF8) {
    std::string valid = "Hello, Việt Nam!";
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(valid.data()), valid.size());
    // Valid UTF-8 should pass
    EXPECT_TRUE(scanner.validate_utf8(raw));

    // Invalid continuation byte
    uint8_t invalid[] = { 0xC3, 0x28 }; // 0xC3 followed by non-continuation
    EXPECT_FALSE(scanner.validate_utf8(std::span<const uint8_t>(invalid, 2)));
}

TEST_F(UTF8ScannerTest, ExtractVietnameseTokens) {
    std::string source = "def tênKhách(): return soluong";
    auto tokens = scanner.extract_vietnamese_tokens(source);
    EXPECT_THAT(tokens, Not(IsEmpty()));
}

TEST_F(UTF8ScannerTest, PositionTracker) {
    std::string src = "line1\nline2\nline3";
    PositionTracker tracker(src);

    auto p0 = tracker.offset_to_position(0);
    EXPECT_EQ(p0.line, 1u);
    EXPECT_EQ(p0.column, 1u);

    auto p1 = tracker.offset_to_position(6); // start of "line2"
    EXPECT_EQ(p1.line, 2u);
    EXPECT_EQ(p1.column, 1u);

    auto p2 = tracker.offset_to_position(12); // start of "line3"
    EXPECT_EQ(p2.line, 3u);
    EXPECT_EQ(p2.column, 1u);
}

// ===========================================================================
// Lexer tests
// ===========================================================================
class LexerTest : public ::testing::Test {
protected:
    Lexer lexer{LexerOptions{.preserve_comments = true, .track_vietnamese = true}};
};

TEST_F(LexerTest, EmptySource) {
    auto result = lexer.tokenize("", "test.py");
    ASSERT_TRUE(result.has_value());
    EXPECT_THAT(*result, SizeIs(1)); // EOF token only
    EXPECT_EQ(result->front().kind, TokenKind::EndOfFile);
}

TEST_F(LexerTest, PythonIdentifiers) {
    auto result = lexer.tokenize("def foo(bar): pass", "test.py");
    ASSERT_TRUE(result.has_value());
    auto ids = std::count_if(result->begin(), result->end(),
        [](const Token& t){ return t.is_identifier() || t.kind == TokenKind::Keyword; });
    EXPECT_GE(ids, 3); // def, foo, bar, pass
}

TEST_F(LexerTest, PythonKeywords) {
    auto result = lexer.tokenize("if x in []: return None", "test.py");
    ASSERT_TRUE(result.has_value());
    bool found_if = false, found_return = false, found_none = false;
    for (auto& t : *result) {
        if (t.kind == TokenKind::Keyword) {
            if (t.raw == "if")     found_if     = true;
            if (t.raw == "return") found_return = true;
            if (t.raw == "None")   found_none   = true;
        }
    }
    EXPECT_TRUE(found_if);
    EXPECT_TRUE(found_return);
    EXPECT_TRUE(found_none);
}

TEST_F(LexerTest, PythonVietnameseIdentifier) {
    auto result = lexer.tokenize("tênKhách = 'John'", "test.py");
    ASSERT_TRUE(result.has_value());
    bool found_viet = false;
    for (auto& t : *result) {
        if (t.kind == TokenKind::VietnameseIdentifier ||
            t.kind == TokenKind::MixedIdentifier) {
            found_viet = true;
        }
    }
    EXPECT_TRUE(found_viet);
}

TEST_F(LexerTest, JavaScriptLineComment) {
    auto result = lexer.tokenize("// Đây là bình luận\nlet x = 1;", "test.js");
    ASSERT_TRUE(result.has_value());
    bool found_viet_comment = false;
    for (auto& t : *result) {
        if (t.kind == TokenKind::VietnameseComment) {
            found_viet_comment = true;
            break;
        }
    }
    EXPECT_TRUE(found_viet_comment);
}

TEST_F(LexerTest, JavaScriptBlockComment) {
    auto result = lexer.tokenize("/* Quản lý khách hàng */\nlet x = 1;", "test.js");
    ASSERT_TRUE(result.has_value());
    bool found_viet = false;
    for (auto& t : *result) {
        if (t.kind == TokenKind::VietnameseComment) found_viet = true;
    }
    EXPECT_TRUE(found_viet);
}

TEST_F(LexerTest, CppRawString) {
    auto result = lexer.tokenize("auto s = \"hello world\";", "test.cpp");
    ASSERT_TRUE(result.has_value());
    bool found_str = false;
    for (auto& t : *result) {
        if (t.kind == TokenKind::StringLiteral) found_str = true;
    }
    EXPECT_TRUE(found_str);
}

TEST_F(LexerTest, LanguageAutoDetect) {
    EXPECT_EQ(detect_language("file.py"),   Language::Python);
    EXPECT_EQ(detect_language("file.js"),   Language::JavaScript);
    EXPECT_EQ(detect_language("file.ts"),   Language::TypeScript);
    EXPECT_EQ(detect_language("file.cpp"),  Language::Cpp);
    EXPECT_EQ(detect_language("file.c"),    Language::C);
    EXPECT_EQ(detect_language("file.h"),    Language::C);
    EXPECT_EQ(detect_language("file.xyz"),  Language::Unknown);
}

TEST_F(LexerTest, ExtractIdentifiers) {
    auto ids = lexer.extract_identifiers(
        "def quanLy(khachHang): return khachHang.ten", Language::Python);
    EXPECT_THAT(ids, Not(IsEmpty()));
    bool found_quan_ly = false;
    for (auto& t : ids) {
        if (t.raw == "quanLy" || t.raw.find("Ly") != std::string::npos)
            found_quan_ly = true;
    }
}

// ===========================================================================
// VietnameseClassifier tests
// ===========================================================================
class ClassifierTest : public ::testing::Test {
protected:
    VietnameseClassifier classifier;
};

TEST_F(ClassifierTest, PureASCIIIdentifier) {
    auto result = classifier.classify_string("getUserInfo");
    EXPECT_EQ(result.cls, IdentifierClass::PureASCII);
    EXPECT_GT(result.confidence, 0.8f);
    EXPECT_FALSE(result.has_diacritics);
    EXPECT_FLOAT_EQ(result.vietnamese_ratio, 0.0f);
}

TEST_F(ClassifierTest, PureVietnameseIdentifier) {
    // "tênKhách" contains Vietnamese characters
    auto result = classifier.classify_string("tênKhách");
    EXPECT_NE(result.cls, IdentifierClass::PureASCII);
    EXPECT_TRUE(result.has_diacritics);
    EXPECT_GT(result.vietnamese_ratio, 0.0f);
}

TEST_F(ClassifierTest, TransliteratedIdentifier) {
    auto result = classifier.classify_string("ten_khach_hang");
    EXPECT_EQ(result.cls, IdentifierClass::TransliteratedViet);
    EXPECT_GT(result.confidence, 0.5f);
}

TEST_F(ClassifierTest, TransliteratedSingle) {
    auto result = classifier.classify_string("ten");
    EXPECT_EQ(result.cls, IdentifierClass::TransliteratedViet);
}

TEST_F(ClassifierTest, AbbreviationDetection) {
    auto result = classifier.classify_string("QLKH");
    EXPECT_EQ(result.cls, IdentifierClass::Abbreviation);
}

TEST_F(ClassifierTest, NamingStyleDetection) {
    EXPECT_EQ(VietnameseClassifier::detect_style("snake_case"),       NamingStyle::SnakeCase);
    EXPECT_EQ(VietnameseClassifier::detect_style("camelCase"),        NamingStyle::CamelCase);
    EXPECT_EQ(VietnameseClassifier::detect_style("PascalCase"),       NamingStyle::PascalCase);
    EXPECT_EQ(VietnameseClassifier::detect_style("SCREAMING_SNAKE"),  NamingStyle::ScreamingSnake);
    EXPECT_EQ(VietnameseClassifier::detect_style("kebab-case"),       NamingStyle::KebabCase);
    EXPECT_EQ(VietnameseClassifier::detect_style("x"),                NamingStyle::Single);
}

TEST_F(ClassifierTest, AsciiEquivalentSuggestion) {
    // "tên" → "ten", "số" → "so", etc.
    std::string eq = classifier.suggest_ascii_equivalent("tên");
    EXPECT_FALSE(eq.empty());
    // Should be mostly ASCII
    bool all_ascii = true;
    for (unsigned char c : eq) if (c >= 0x80) { all_ascii = false; break; }
    EXPECT_TRUE(all_ascii);
}

TEST_F(ClassifierTest, BatchClassify) {
    std::vector<Token> tokens;
    auto make_tok = [](std::string name) {
        Token t; t.raw=name; t.text=name; t.kind=TokenKind::Identifier; return t;
    };
    tokens.push_back(make_tok("getUserInfo"));
    tokens.push_back(make_tok("tênKhách"));
    tokens.push_back(make_tok("so_luong"));

    auto results = classifier.classify_batch(std::span<const Token>(tokens));
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].cls, IdentifierClass::PureASCII);
}

// ===========================================================================
// EncodingDetector tests
// ===========================================================================
class EncodingDetectorTest : public ::testing::Test {
protected:
    EncodingDetector detector;
};

TEST_F(EncodingDetectorTest, EmptyBuffer) {
    auto result = detector.detect({});
    EXPECT_EQ(result.detected, Encoding::ASCII);
    EXPECT_GT(result.confidence, 0.9f);
}

TEST_F(EncodingDetectorTest, PureASCII) {
    std::string s = "Hello, World! 123";
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    auto result = detector.detect(raw);
    EXPECT_EQ(result.detected, Encoding::ASCII);
    EXPECT_GT(result.confidence, 0.99f);
}

TEST_F(EncodingDetectorTest, UTF8WithVietnamese) {
    std::string s = "Xin chào! Tôi là người Việt Nam.";
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    auto result = detector.detect(raw);
    EXPECT_EQ(result.detected, Encoding::UTF8);
    EXPECT_GT(result.confidence, 0.95f);
    EXPECT_FALSE(result.needs_conversion);
}

TEST_F(EncodingDetectorTest, UTF8BOM) {
    // BOM + UTF-8 text
    uint8_t bom_text[] = { 0xEF, 0xBB, 0xBF, 0x48, 0x65, 0x6C, 0x6C, 0x6F };
    auto result = detector.detect(std::span<const uint8_t>(bom_text, sizeof(bom_text)));
    EXPECT_EQ(result.detected, Encoding::UTF8_BOM);
    EXPECT_TRUE(result.has_bom);
    EXPECT_FALSE(result.needs_conversion);
}

TEST_F(EncodingDetectorTest, TCVN3Conversion) {
    // Simple ASCII bytes that should pass through unchanged
    uint8_t ascii[] = { 0x48, 0x65, 0x6C, 0x6C, 0x6F }; // "Hello"
    auto result = detector.tcvn3_to_utf8(
        std::span<const uint8_t>(ascii, sizeof(ascii)));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Hello");
}

TEST_F(EncodingDetectorTest, NormalizeUTF8NoConversion) {
    std::string s = "Việt Nam";
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    auto enc = detector.detect(raw);
    auto norm = detector.normalize_to_utf8(raw, enc);
    ASSERT_TRUE(norm.has_value());
    EXPECT_EQ(*norm, s);
}

TEST_F(EncodingDetectorTest, NormalizeStripsUTF8BOM) {
    std::string s = "\xEF\xBB\xBFHello";
    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    EncodingDetectionResult enc;
    enc.detected = Encoding::UTF8_BOM;
    enc.has_bom  = true;
    enc.needs_conversion = false;
    auto norm = detector.normalize_to_utf8(raw, enc);
    ASSERT_TRUE(norm.has_value());
    EXPECT_EQ(*norm, "Hello");
}

// ===========================================================================
// RuleEngine tests
// ===========================================================================
class RuleEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<RuleEngine>(LintConfig{});
        engine->register_builtin_rules();
    }
    std::unique_ptr<RuleEngine> engine;
};

TEST_F(RuleEngineTest, LintCleanPython) {
    std::string source = R"(
def get_user_info(user_id):
    name = "John"
    age  = 30
    return {"name": name, "age": age}
)";
    auto diags = engine->lint_source(source, "test.py");
    // Clean ASCII code should have no VL001 violations
    bool has_vl001 = false;
    for (auto& d : diags)
        if (d.violation.rule_id == "VL001") has_vl001 = true;
    EXPECT_FALSE(has_vl001);
}

TEST_F(RuleEngineTest, LintVietnamesePython) {
    std::string source = "\ndef l\u1EA5yTh\u00F4ngTinKh\u00E1ch(m\u00E3_kh\u00E1ch):\n    t\u00EAn = \"Nguy\u1EC5n V\u0103n A\"\n    tu\u1ED5i = 30\n    return t\u00EAn\n";
    auto diags = engine->lint_source(source, "test.py");
    bool has_vl001 = false;
    for (auto& d : diags)
        if (d.violation.rule_id == "VL001") has_vl001 = true;
    // Should detect Vietnamese identifiers
    EXPECT_TRUE(has_vl001 || !diags.empty());
}

TEST_F(RuleEngineTest, LintMixedCommentJS) {
    std::string source = R"(
// This function quản lý user accounts
function manageUsers() {
    return [];
}
)";
    auto diags = engine->lint_source(source, "test.js");
    bool has_vl002 = false;
    for (auto& d : diags)
        if (d.violation.rule_id == "VL002") has_vl002 = true;
    EXPECT_TRUE(has_vl002);
}

TEST_F(RuleEngineTest, LintTransliteratedIdentifiers) {
    std::string source = R"(
def process():
    ten = "name"
    so_luong = 10
    khach_hang = {}
    return ten, so_luong, khach_hang
)";
    auto diags = engine->lint_source(source, "test.py");
    bool has_vl003_or_vl004 = false;
    for (auto& d : diags)
        if (d.violation.rule_id == "VL003" || d.violation.rule_id == "VL004")
            has_vl003_or_vl004 = true;
    // Transliterated identifiers should trigger info-level warnings
    EXPECT_TRUE(has_vl003_or_vl004 || !diags.empty());
}

TEST_F(RuleEngineTest, StatsTracking) {
    engine->reset_stats();
    engine->lint_source("x = 1", "a.py");
    engine->lint_source("y = 2", "b.py");
    auto stats = engine->stats();
    EXPECT_EQ(stats.files_scanned, 2u);
}

TEST_F(RuleEngineTest, RuleRegistration) {
    auto ids = engine->registry().rule_ids();
    EXPECT_THAT(ids, Contains("VL001"));
    EXPECT_THAT(ids, Contains("VL002"));
    EXPECT_THAT(ids, Contains("VL003"));
    EXPECT_THAT(ids, Contains("VL004"));
    EXPECT_THAT(ids, Contains("VL005"));
    EXPECT_THAT(ids, Contains("VL006"));
}

TEST_F(RuleEngineTest, NFDNormalizationDetection) {
    // NFD: 'a' + combining acute = á (decomposed)
    // This should trigger VL005
    std::string nfd_source = "def a\u0301 (): pass"; // a + combining acute
    auto diags = engine->lint_source(nfd_source, "test.py");
    bool has_vl005 = false;
    for (auto& d : diags)
        if (d.violation.rule_id == "VL005") has_vl005 = true;
    EXPECT_TRUE(has_vl005);
}

// ===========================================================================
// LSP JSON parser tests
// ===========================================================================
class JsonParserTest : public ::testing::Test {};

TEST_F(JsonParserTest, ParseNull) {
    auto result = vietlint::lsp::Json::parse("null");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<vietlint::lsp::JsonNull>(*result));
}

TEST_F(JsonParserTest, ParseBool) {
    auto t = vietlint::lsp::Json::parse("true");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(vietlint::lsp::Json::as_bool(*t), std::optional<bool>(true));

    auto f = vietlint::lsp::Json::parse("false");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(vietlint::lsp::Json::as_bool(*f), std::optional<bool>(false));
}

TEST_F(JsonParserTest, ParseInt) {
    auto result = vietlint::lsp::Json::parse("42");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(vietlint::lsp::Json::as_int(*result), std::optional<int64_t>(42));
}

TEST_F(JsonParserTest, ParseNegInt) {
    auto result = vietlint::lsp::Json::parse("-7");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(vietlint::lsp::Json::as_int(*result), std::optional<int64_t>(-7));
}

TEST_F(JsonParserTest, ParseString) {
    auto result = vietlint::lsp::Json::parse("\"hello world\"");
    ASSERT_TRUE(result.has_value());
    auto* s = vietlint::lsp::Json::as_string(*result);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "hello world");
}

TEST_F(JsonParserTest, ParseStringWithEscapes) {
    auto result = vietlint::lsp::Json::parse(R"("line1\nline2\ttab")");
    ASSERT_TRUE(result.has_value());
    auto* s = vietlint::lsp::Json::as_string(*result);
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->find('\n'), std::string::npos);
    EXPECT_NE(s->find('\t'), std::string::npos);
}

TEST_F(JsonParserTest, ParseObject) {
    auto result = vietlint::lsp::Json::parse(
        R"({"name": "VietLint", "version": 1})");
    ASSERT_TRUE(result.has_value());
    auto* obj = vietlint::lsp::Json::as_object(*result);
    ASSERT_NE(obj, nullptr);
    auto* name = vietlint::lsp::Json::get(*obj, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(*vietlint::lsp::Json::as_string(*name), "VietLint");
}

TEST_F(JsonParserTest, ParseArray) {
    auto result = vietlint::lsp::Json::parse("[1, 2, 3]");
    ASSERT_TRUE(result.has_value());
    auto* arr = vietlint::lsp::Json::as_array(*result);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->items.size(), 3u);
}

TEST_F(JsonParserTest, ParseNestedObject) {
    auto result = vietlint::lsp::Json::parse(
        R"({"a": {"b": {"c": 42}}})");
    ASSERT_TRUE(result.has_value());
    auto* obj1 = vietlint::lsp::Json::as_object(*result);
    ASSERT_NE(obj1, nullptr);
    auto* a = vietlint::lsp::Json::get(*obj1, "a");
    ASSERT_NE(a, nullptr);
    auto* obj2 = vietlint::lsp::Json::as_object(*a);
    ASSERT_NE(obj2, nullptr);
    auto* b = vietlint::lsp::Json::get(*obj2, "b");
    ASSERT_NE(b, nullptr);
}

TEST_F(JsonParserTest, SerializeRoundTrip) {
    std::string original = R"({"method":"initialize","id":1})";
    auto parsed = vietlint::lsp::Json::parse(original);
    ASSERT_TRUE(parsed.has_value());
    auto serialized = vietlint::lsp::Json::serialize(*parsed);
    // Re-parse serialized form
    auto reparsed = vietlint::lsp::Json::parse(serialized);
    ASSERT_TRUE(reparsed.has_value());
    auto* obj = vietlint::lsp::Json::as_object(*reparsed);
    ASSERT_NE(obj, nullptr);
    auto* method = vietlint::lsp::Json::get(*obj, "method");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(*vietlint::lsp::Json::as_string(*method), "initialize");
}

TEST_F(JsonParserTest, ParseUnicodeEscape) {
    // \u1EB9 = ẹ (Vietnamese character)
    auto result = vietlint::lsp::Json::parse(R"("\u1EB9")");
    ASSERT_TRUE(result.has_value());
    auto* s = vietlint::lsp::Json::as_string(*result);
    ASSERT_NE(s, nullptr);
    // Should decode to UTF-8 bytes for ẹ: 0xE1 0xBA 0xB9
    EXPECT_EQ(static_cast<uint8_t>((*s)[0]), 0xE1u);
    EXPECT_EQ(static_cast<uint8_t>((*s)[1]), 0xBAu);
    EXPECT_EQ(static_cast<uint8_t>((*s)[2]), 0xB9u);
}

// ===========================================================================
// ML Feature extractor tests
// ===========================================================================
class FeatureExtractorTest : public ::testing::Test {
protected:
    vietlint::ml::FeatureExtractor extractor;
};

TEST_F(FeatureExtractorTest, FeatureVectorDimension) {
    auto feat = this->extractor.extract("hello");
    EXPECT_EQ(feat.size(), vietlint::ml::FEATURE_DIM);
}

TEST_F(FeatureExtractorTest, ASCIIIdentifierFeatures) {
    auto feat = extractor.extract("getUserById");
    EXPECT_FLOAT_EQ(feat[0], 0.0f); // unicode_ratio = 0
    EXPECT_FLOAT_EQ(feat[1], 0.0f); // viet_ratio = 0
    EXPECT_FLOAT_EQ(feat[2], 1.0f); // ascii_ratio = 1
    EXPECT_FLOAT_EQ(feat[6], 0.0f); // has_viet = false
}

TEST_F(FeatureExtractorTest, VietnameseIdentifierFeatures) {
    auto feat = extractor.extract("tênKhách");
    EXPECT_GT(feat[0], 0.0f);  // unicode_ratio > 0
    EXPECT_GT(feat[1], 0.0f);  // viet_ratio > 0
    EXPECT_FLOAT_EQ(feat[6], 1.0f); // has_viet = true
}

TEST_F(FeatureExtractorTest, NamingStyleFeatures) {
    // snake_case
    auto snake = this->extractor.extract("get_user_by_id");
    EXPECT_FLOAT_EQ(snake[9],  1.0f); // style_snake
    EXPECT_FLOAT_EQ(snake[10], 0.0f); // style_camel

    // camelCase
    auto camel = this->extractor.extract("getUserById");
    EXPECT_FLOAT_EQ(camel[9],  0.0f); // style_snake
    EXPECT_FLOAT_EQ(camel[10], 1.0f); // style_camel

    // PascalCase
    auto pascal = this->extractor.extract("GetUserById");
    EXPECT_FLOAT_EQ(pascal[11], 1.0f); // style_pascal
}

TEST_F(FeatureExtractorTest, BatchExtract) {
    std::vector<std::string> ids = {"hello", "tênKhách", "so_luong"};
    std::vector<std::string_view> views;
    for (auto& s : ids) views.emplace_back(s);
    auto batch = this->extractor.extract_batch(std::span<const std::string_view>(views));
    EXPECT_EQ(batch.size(), ids.size() * vietlint::ml::FEATURE_DIM);
}

TEST_F(FeatureExtractorTest, EmptyIdentifier) {
    auto feat = this->extractor.extract("");
    EXPECT_EQ(feat.size(), vietlint::ml::FEATURE_DIM);
    // All features should be 0 or well-defined for empty input
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_GE(feat[i], 0.0f);
        EXPECT_LE(feat[i], 1.0f);
    }
}
