// page_index: small full-text search over prepared HTML pages.
// Build side: turn the pages of one language into one index file.
//
// Dependencies: ICU (icu-uc), Snowball libstemmer_c. C++20.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace page_index {

struct Build_Report {
    uint32_t pages = 0;
    uint64_t words = 0;           // word and stop-word records
    uint64_t text_bytes = 0;      // extracted text, UTF-8
    uint64_t record_bytes = 0;    // word records
    uint64_t file_bytes = 0;      // whole index file
    std::vector<std::string> warnings;
};

// Indexes the given prepared HTML files (all in one language, e.g. "en", "de", "pt_BR") and
// writes output_path through a temporary file and a rename, so servers that still have the
// old file mapped keep working.
//
// Relative paths in files are opened relative to base_directory (with openat() on POSIX; an
// empty base_directory means the current directory); absolute paths are opened as they are.
// Paths are stored as given and returned by search(), so relative paths become the page URLs.
// output_path is not affected by base_directory.
//
// What is indexed: the text of <main> (or <body> if there is no <main>), without elements that
// carry a data-fs-skip attribute and without script, style and template. Weights: h1-h6,
// figcaption/caption, em/strong/b/i/mark. Title from <title>, index terms from
// <meta name="keywords" content="a, b">, ranking prior from <meta name="search-prior" content="0.5">.
auto build_index(const std::vector<std::string>& files, const std::string& base_directory, std::string_view language,
                 const std::string& output_path, Build_Report* report = nullptr, std::string* error = nullptr) -> bool;

enum class Issue_Level : uint8_t { error, warning };

struct Html_Issue {
    Issue_Level level;
    uint32_t line;        // 1-based
    uint32_t column;      // 1-based, in bytes
    std::string message;
};

// Strict check of one page against the prepared-HTML contract. Errors mean the browser's DOM
// may differ from what the indexer reads (wrong highlights, missing or extra text), or the page
// is parsed in a way the indexer does not model. Warnings mean the page works but search or
// snippets are worse. A page without errors gives the indexer exactly the browser's text.
// language: the index language; empty skips the <html lang> comparison.
auto validate_page(std::string_view html, std::string_view language = {}) -> std::vector<Html_Issue>;

}  // namespace page_index
