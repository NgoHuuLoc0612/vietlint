# VietLint Changelog

All notable changes to VietLint are documented in this file.

---

## [1.0.5] — 2026-05-26

### Fixed
- **Python builtins whitelist expanded** — `ValueError`, `KeyError`, `TypeError`, `AttributeError`, `IndexError`, `RuntimeError`, `Exception`, `__init__`, `__str__`, `__repr__`, `__len__`, `__get__`, `__set__` are no longer flagged as transliterated Vietnamese by VL004. Previously, Python dunder methods and standard exception classes were incorrectly reported as violations, producing significant noise in Python codebases.
- **ONNX classifier integrated into VL010 and VL011** — rules that previously fell back to the heuristic `VietnameseClassifier` for identifier classification now correctly use the ONNX `MLClassifier` when a model path is configured, producing more accurate results for transliterated identifiers such as `bangDuLieu`, `heDieuHanh`, and `tinhTong`.

---

## [1.0.4] — 2026-05-26

### Added

#### Rule VL008 — Inconsistent Transliteration Style
Detects files where transliterated Vietnamese identifiers use mixed naming conventions within the same file. For example, a file containing both `ten_khach` (snake_case) and `tenKhach` (camelCase) representations of the same Vietnamese concept will now trigger VL008 on the minority style, encouraging developers to standardize on one convention throughout a codebase. The rule identifies the dominant style by counting snake_case versus camelCase transliterated identifiers and flags outliers.

#### Rule VL009 — Vietnamese Text in Exception Messages
Detects Vietnamese text inside exception and error messages. When a `raise` or `throw` statement contains a string literal with Vietnamese characters, VL009 fires with a warning encouraging the developer to use English for error messages. English error messages are more compatible with international logging systems, monitoring tools, and support pipelines. The rule correctly skips the exception class identifier (e.g., `ValueError`) between the `raise` keyword and the string argument.

#### Rule VL010 — Missing English Docstring for Transliterated Function
Detects transliterated Vietnamese function names that lack an English docstring. Functions with names such as `tinhTong`, `xoaNhanVien`, or `themKhachHang` are clearly Vietnamese in intent and should carry an English docstring for international readability. The rule fires at info severity when a transliterated function definition is not followed by a string literal docstring within five lines. Currently applies to Python only.

#### Rule VL011 — Vietnamese Import Alias
Detects Vietnamese words used as import aliases in `import ... as` statements. Patterns such as `import numpy as mang`, `import pandas as bangDuLieu`, or `import os as heDieuHanh` violate conventional alias naming (e.g., `np`, `pd`, `os`) and reduce code readability for non-Vietnamese readers. The rule uses the ONNX classifier for accurate detection of transliterated aliases.

### Changed
- **Total rules increased from 8 to 12** — VietLint now covers VL000 through VL011.
- **`.vietlint.toml` updated** — default configuration file now includes entries for VL008, VL009, VL010, and VL011 with appropriate default severities.

---

## [1.0.3] — 2026-05-25

### Added
- **Tree-sitter native grammar integration** — 14 language grammars compiled from source as native shared libraries (`.so`) and loaded at runtime via `dlopen`. Grammars included: C, C++, C#, CSS, Go, Java, JavaScript, JSON, Julia, OCaml, PHP, Python, Rust, Scala, TypeScript. The previous `.wasm` files in the `grammars/` directory are superseded; the lexer now uses true AST-based parsing for all supported languages, enabling correct tokenization of complex constructs such as C++ templates, lambda expressions, nested structs, and TypeScript generics that the regex-based fallback lexer could not handle reliably.
- **GitHub corpus expanded to 117,612 identifiers** — the training corpus scraper was rewritten with 93 search queries covering all 14 tree-sitter languages and 10+ Vietnamese domain verticals:
  - **Languages:** Python, JavaScript, TypeScript, Java, C++, C, C#, Go, PHP, Rust, Scala, CSS, JSON, Julia, OCaml
  - **Domains:** e-commerce, healthcare, education, finance, HR/payroll, logistics, real estate, IoT/embedded systems, AI/ML
  - A total of 410 Vietnamese GitHub repositories were scanned, yielding 117,612 labeled identifiers (up from 39,417 in v1.0.0).
- **Model retrained on expanded corpus** — GradientBoosting classifier retrained with oversampling for minority classes (transliterated, pure Vietnamese, abbreviations). Final metrics on held-out test set: F1-macro 0.96, accuracy 0.99. The model correctly classifies identifiers across five categories: `pure_ascii`, `pure_vietnamese`, `mixed_vietnamese`, `transliterated`, `abbreviation`.
- **GitHub Actions release automation** — CI workflow now triggers the `release` job on `refs/tags/v*` pushes, automatically building the Linux x86-64 binary and attaching it as a release asset. The `release` job requires `contents: write` permission.
- **Portable release binary** — the release binary is now built without ONNX Runtime linkage, eliminating the `libonnxruntime.so` shared library dependency and making the binary runnable on any Ubuntu 24.04 system without additional installation.

### Changed
- **GitHub Actions CI trigger extended** — workflow now triggers on tag pushes (`tags: ['v*']`) in addition to branch pushes, enabling automatic release creation.
- **ONNX Runtime dependency made optional in release builds** — release CI uses `-DVIETLINT_ENABLE_ONNX=OFF` to produce a statically self-contained binary. Local development builds continue to support ONNX.
- **Corpus scraper early-exit removed** — the `search_repos()` function previously broke out of the query loop as soon as `max_repos` unique repositories had been found, which caused only the first few queries to ever execute. The break condition has been removed; all 93 queries are now executed and results are deduplicated before truncation.

### Fixed
- **GitHub repository URL corrected** — source files previously referenced `https://github.com/vietlint/vietlint` (a placeholder). All references have been updated to `https://github.com/NgoHuuLoc0612/vietlint`.
- **Version string hardcoded correctly** — the `version` subcommand now reports the correct semantic version rather than the stale `1.0.0` placeholder embedded at compile time.

---

## [1.0.2] — 2026-05-25

### Added
- **`--fix-style` flag** — the `--fix` flag now accepts a companion `--fix-style=snake` (default) or `--fix-style=camel` argument. When fixing VL001 violations on identifiers containing Vietnamese diacritical marks, the fixer selects the snake_case suggestion (e.g., `tênKhách` → `ten_khach`) or camelCase suggestion (`tênKhách` → `tenKhach`) according to the specified style. The flag is parsed in `CliArgs` and forwarded to `apply_fix()`.

### Fixed
- **VL003 and VL004 deduplication** — when both VL003 (heuristic transliteration) and VL004 (ONNX transliteration) fired on the same identifier at the same source location, the output contained duplicate warnings for the same violation. A deduplication pass has been added in `run_lint` that removes VL003 violations whose `span.start` coincides with an existing VL004 violation. The ONNX-backed VL004 result is authoritative and is retained; the heuristic VL003 result is suppressed.
- **Multi-byte UTF-8 fix replacement** — the `apply_fix()` function previously applied replacements in descending offset order to avoid shift errors, but did not guard against applying multiple fixes at the same byte offset (which could happen when VL001 and VL002 both reported the same identifier). A `std::unordered_set<uint32_t>` of applied spans now prevents double-replacement at the same position, which previously caused corrupted output such as `def tenKhachsoLuong` instead of `def ten_khach(so_luong)`.

---

## [1.0.1] — 2026-05-24

### Fixed
- **`pyproject.toml` build backend corrected** — the build backend was set to `setuptools.backends.legacy:build`, which is not valid for current setuptools versions. Updated to `setuptools.build_meta`.
- **`pyproject.toml` section structure fixed** — the `dependencies` array was accidentally placed inside the `[project.urls]` section due to a malformed patch, causing `pip` and `build` to reject the configuration with a JSON validation error. The section structure has been corrected.
- **Python wheel build isolation** — the wheel build now uses a temporary clean directory containing only the `vietlint/` package, `pyproject.toml`, `README.md`, and `setup.py`, preventing the `build/_deps/pybind11-src/` tree from being included in the wheel distribution.
- **`setup.py` added** — an explicit `setup.py` with `packages=["vietlint"]` ensures setuptools does not auto-discover unintended packages from the repository root.

### Added
- **`scripts/build_wheel.sh`** — a shell script that automates the clean-directory wheel build process, copies the result to `dist/`, and removes the temporary directory.
- **`MANIFEST.in`** — added to control source distribution contents.
- **`pyproject.toml` classifiers and URLs** — added PyPI trove classifiers (`Development Status :: 4 - Beta`, Python version classifiers, topic classifiers) and project URLs (`Homepage`, `Repository`, `Issues`) pointing to the correct GitHub repository.

---

## [1.0.0] — 2026-05-23

### Initial Release

VietLint 1.0.0 is the first public release of the first dedicated static analysis tool for Vietnamese coding conventions. This release establishes the complete architecture and core feature set.

#### C++20 Core Engine
- **AVX2 SIMD UTF-8 scanner** (`utf8_scanner.hpp`) — processes 32 bytes per cycle using 256-bit AVX2 intrinsics for high-throughput Vietnamese Unicode detection. Falls back to scalar processing when AVX2 is unavailable at compile time (`-DVIETLINT_ENABLE_AVX2=OFF`).
- **Multi-language lexer** (`lexer.hpp`, `src/core/lexer.cpp`) — regex-based tokenizer supporting Python, JavaScript/TypeScript, and C/C++. Classifies tokens as identifiers, keywords, string literals, comments, numbers, and punctuation. Promotes identifiers containing Vietnamese Unicode to `VietnameseIdentifier` and comments containing Vietnamese to `VietnameseComment`.
- **Vietnamese identifier classifier** (`classifier.hpp`, `src/core/classifier.cpp`) — heuristic six-class classifier using a binary-searched word list of 200+ Vietnamese syllables, a regex-based transliteration pattern, Vietnamese syllable frequency analysis, and naming style detection (camelCase, PascalCase, snake_case, SCREAMING_SNAKE, kebab-case). Provides ASCII equivalent suggestions via diacritic stripping and style normalization.
- **Encoding detector** (`encoding_detector.hpp`) — identifies UTF-8, TCVN3, VNI, and Windows-1258 (CP1258) encodings with confidence scoring. Supports byte-order mark detection and encoding declaration parsing for Python files.
- **Rule engine** (`rule_engine.hpp`, `src/core/rule_engine.cpp`) — eight built-in lint rules with configurable severity, enable/disable toggles, and auto-fix suggestions. Supports parallel file processing via `std::thread` workers. Emits diagnostics with byte-accurate source spans and line/column information.
- **Diagnostic emitter** (`emitter.hpp`) — formats diagnostics in six output modes: plain text with ANSI color, JSON, SARIF 2.1.0, GCC-compatible (`file:line:col: severity: message`), LSP JSON-RPC, and GitHub Actions workflow commands. SARIF output is compatible with GitHub Code Scanning.
- **Plugin loader** — supports custom rules as shared libraries loaded via `dlopen`/`LoadLibrary`. Plugins export a `vietlint_register(PluginContext*)` entry point and register rules against the engine's `RuleRegistry`.

#### Built-in Lint Rules (VL000–VL007)

| ID | Name | Default Severity | Description |
|----|------|-----------------|-------------|
| VL000 | Parse Error | error | Source file failed to parse or lex |
| VL001 | Vietnamese Identifier | warning | Identifier contains Vietnamese Unicode characters (diacritical marks, tone indicators). Suggests ASCII snake_case and camelCase equivalents. |
| VL002 | Mixed-Language Naming | warning | Identifier or comment mixes Vietnamese Unicode characters with ASCII letters in a single token |
| VL003 | Encoding Declaration | info | Python file missing a `# -*- coding: utf-8 -*-` declaration, or declaration specifies a non-UTF-8 charset. Auto-fixable. |
| VL004 | Transliteration | warning | Identifier is a transliterated Vietnamese word written without diacritical marks (e.g., `tinhTong`, `soLuong`, `nguoiDung`) |
| VL005 | Unicode Normalization | error | Identifier uses NFD-decomposed Vietnamese characters (base character + combining diacritical mark sequences) instead of NFC precomposed forms |
| VL006 | Consistent Naming Style | info | File uses two or more distinct naming conventions (camelCase, snake_case, PascalCase) for identifiers in the same scope |
| VL007 | Vietnamese String Literal | info | String literal contains Vietnamese text that should be externalized to a localization file for i18n compliance |

#### Machine Learning Layer
- **64-dimensional feature extractor** — extracts Unicode character class ratios (Vietnamese ratio, ASCII ratio, digit ratio), naming style flags (camelCase, snake_case, PascalCase, all-caps), identifier length statistics, bigram hash features (NGRAM_BUCKETS buckets), and trigram hash features from identifier strings. Implemented in C++ with pybind11 bindings for training pipeline access.
- **ONNX Runtime inference** — `OnnxClassifier` wraps the ONNX Runtime C++ API (`onnxruntime_cxx_api.h`) in a RAII `OrtState` struct. Loads a GradientBoosting model exported from scikit-learn via skl2onnx. Input name: `float_input`. Output names: `label` (int64 predicted class), `probabilities` (float32 array of per-class probabilities). Falls back to heuristic classifier when ONNX Runtime is not compiled in or model file is not found.
- **Training pipeline** (`scripts/collect_corpus.py`) — GitHub API scraper that collects Vietnamese identifier corpora from public repositories. Performs auto-labeling across six classes using Unicode detection, word-list lookup, camelCase decomposition, and Vietnamese abbreviation pattern matching. Exports labeled examples in JSON Lines format.
- **Initial training corpus** — 39,417 identifiers scraped from 100 Vietnamese GitHub repositories using 14 search queries targeting Python, JavaScript, TypeScript, and C++. Includes 305 curated synthetic examples covering all label classes.
- **Model training** — GradientBoosting classifier (300 estimators, max depth 5, learning rate 0.08) with oversampling for minority classes (transliterated, pure Vietnamese, abbreviation) to address label imbalance. Exported to ONNX with `zipmap=False` for efficient probability array output. Initial F1-macro: 0.98 on synthetic corpus.

#### Language Server Protocol
- **Full JSON-RPC 2.0 implementation** over stdio (`lsp_server.hpp`, `src/lsp/lsp_server.cpp`). No external LSP framework dependency; the entire protocol is implemented from scratch using `std::getline`-based message framing with `Content-Length` headers.
- **Supported methods:** `initialize`, `initialized`, `shutdown`, `exit`, `textDocument/didOpen`, `textDocument/didChange` (incremental delta application), `textDocument/didClose`, `textDocument/publishDiagnostics` (server-push), `textDocument/hover` (rule documentation on hover), `textDocument/codeAction` (quick-fix `WorkspaceEdit` actions).
- **Background linting thread** — diagnostics are computed on a dedicated worker thread and published asynchronously to avoid blocking the LSP message loop.

#### REST API Backend
- FastAPI application (`python/backend.py`) providing a complete REST and WebSocket API for programmatic linting:
  - `POST /api/v1/lint` — lint a source string from request body
  - `POST /api/v1/lint/file` — lint a file by server-side path
  - `GET /api/v1/lint/stream` — Server-Sent Events stream for incremental results
  - `WebSocket /ws/lint` — real-time as-you-type linting with delta updates
  - `POST /api/v1/projects` / `GET /api/v1/projects` — project management
  - `GET /api/v1/projects/{id}/violations` — violation history with pagination
  - `GET /api/v1/projects/{id}/stats` — per-project violation statistics
  - `GET /api/v1/rules` — enumerate all registered rules with metadata
  - `GET /api/v1/health` — health check with engine availability status
- SQLite-backed violation history via raw `sqlite3` Python bindings (no ORM dependency).

#### Python Binding
- pybind11 extension module `vietlint_core` exposing:
  - `IdentifierClassifier` — wraps `MLClassifier`; `classify(str)`, `classify_batch(List[str])`, `extract_features(List[str]) -> np.ndarray`, `using_onnx() -> bool`, `warmup()`
  - `LintEngine` — `lint_source(source: str, filename: str) -> List[dict]`
  - `EncodingDetector` — `detect(bytes) -> dict`
  - `UTF8Scanner` — `has_vietnamese(str) -> bool`, `scan(bytes) -> List[dict]`
  - `has_vietnamese(str) -> bool` — module-level convenience function
  - `lint_string(source: str, filename: str) -> List[dict]` — module-level convenience function
  - `FEATURE_DIM`, `PLUGIN_API_VER`, `__version__` — module constants

#### Command-Line Interface
- `vietlint lint [FILES...]` — lint one or more files or directories
- `vietlint lsp` — start LSP server on stdio
- `vietlint classify` — classify identifiers from stdin (one per line)
- `vietlint detect <FILE>` — detect file encoding
- `vietlint init` — generate `.vietlint.toml` in current directory
- `vietlint version` — print version, build features, and repository URL
- Options: `--format`, `--config`, `--model`, `--fix`, `--fix-style`, `--jobs`, `--max-violations`, `--no-color`, `--verbose`

#### Configuration
- `.vietlint.toml` — TOML configuration file with `[project]`, `[lint]`, and per-rule `[rules.VLxxx]` sections. Supports `model` key in `[lint]` section to specify ONNX model path, eliminating the need to pass `--model` on every invocation. Loaded automatically from project root or via `--config`.

#### Build System
- CMake 3.22+ with the following feature flags:
  - `-DVIETLINT_ENABLE_AVX2=ON/OFF` — AVX2 SIMD (default: ON)
  - `-DVIETLINT_ENABLE_CLANG=ON/OFF` — Clang LibTooling AST integration (default: OFF)
  - `-DVIETLINT_ENABLE_TREE_SITTER=ON/OFF` — tree-sitter lexer (default: ON if library found)
  - `-DVIETLINT_ENABLE_ONNX=ON/OFF` — ONNX Runtime inference (default: OFF)
  - `-DVIETLINT_ENABLE_PYTHON=ON/OFF` — pybind11 Python binding (default: OFF)
  - `-DVIETLINT_ENABLE_TESTS=ON/OFF` — Google Test unit tests (default: OFF)
  - `-DVIETLINT_ENABLE_SANITIZERS=ON/OFF` — AddressSanitizer + UBSan (default: OFF)
- Custom CMake module for LLVM/Clang discovery (`cmake/`)
- Example plugin CMake target (`cmake/example_plugin.cpp`)

#### GitHub Actions CI
- `build-linux` job: Ubuntu 24.04, installs `libtree-sitter-dev`, downloads ONNX Runtime 1.17.3, configures and builds with CMake, runs a smoke test.
- `release` job: triggered on `refs/tags/v*` pushes, downloads the `build-linux` artifact, and creates a GitHub Release with the binary attached using `softprops/action-gh-release@v2`.

---

*VietLint is, to the best of the author's knowledge, the first static analysis tool designed specifically for Vietnamese coding conventions. It was built to address a genuine gap in tooling for the Vietnamese software engineering community.*
