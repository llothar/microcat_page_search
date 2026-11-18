// page_index: small full-text search over prepared HTML pages.
// Query side: open an index file (memory-mapped, shared through the page cache) and search it.
//
// Dependencies: ICU (icu-uc), Snowball libstemmer_c. C++20.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace page_index {

inline constexpr uint32_t format_version    = 1;
inline constexpr uint32_t tokenizer_version = 1;

// A hit in the page, counted in UTF-16 code units over the page's text, which is the
// concatenation of all text nodes under <main> (or <body>) without [data-fs-skip], script
// and style subtrees. This is how MarkIt's and mark.js's markRanges() count.
struct Hit_Range {
    uint32_t start;
    uint32_t length;
};

struct Page_Hit {
    uint32_t page;                  // page number in the index
    std::string path;               // as given to build_index()
    std::string title;
    float score;                    // full-text score
    int tier;                       // title/index-term match: 3 exact, 2 prefix, 1 all words, 0 none
    std::vector<Hit_Range> ranges;  // matched words in document order
    std::string snippet_html;       // HTML-escaped context with <mark> around matched words
};

struct Search_Options {
    size_t max_results   = 20;
    size_t max_ranges    = 500;     // per page
    size_t snippet_bytes = 220;     // approximate snippet length in UTF-8 bytes
};

class Page_Index {
public:
    // Maps the file. Returns nullptr and sets *error on failure.
    static auto open(const std::string& path, std::string* error = nullptr) -> std::unique_ptr<Page_Index>;
    ~Page_Index();
    Page_Index(const Page_Index&) = delete;
    auto operator=(const Page_Index&) -> Page_Index& = delete;

    // Thread-safe. Query syntax: words (all must occur, stemmed, prefix and infix matches rank
    // lower) and "quoted phrases" (exact sequence; stop words keep their place).
    auto search(std::string_view query, const Search_Options& options = {}) const -> std::vector<Page_Hit>;

    auto page_count() const -> uint32_t { return page_count_; }
    auto language() const -> std::string_view { return language_; }
    auto page_path(uint32_t page) const -> std::string_view;
    auto page_title(uint32_t page) const -> std::string_view;
    auto page_text(uint32_t page) const -> std::string_view;   // UTF-8

private:
    Page_Index() = default;
    struct Page;
    auto page(uint32_t page) const -> Page;

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    void* mapping_ = nullptr;      // Windows mapping handle
    std::string language_;
    uint32_t page_count_ = 0;
    uint64_t pages_offset_ = 0, strings_offset_ = 0, records_offset_ = 0, text_offset_ = 0;
    double average_words_ = 1.0;
};

namespace detail {

// The analyzer is shared by build and query, so both sides tokenize identically.
// Not thread-safe; use one per thread.
struct Token {
    uint32_t start;         // UTF-8 byte offset of the original word
    uint32_t length;        // UTF-8 bytes of the original word
    std::string term;       // case-folded, stemmed, accents removed; empty for stop words
    std::string folded;     // case-folded, accents removed, not stemmed
};

class Analyzer {
public:
    explicit Analyzer(std::string_view language);
    ~Analyzer();
    Analyzer(const Analyzer&) = delete;
    auto operator=(const Analyzer&) -> Analyzer& = delete;

    auto analyze(std::string_view utf8, std::vector<Token>& tokens) -> void;
    auto fold(std::string_view utf8) -> std::string;                 // whole string, no stemming
    // End of the word starting at each of the sorted byte offsets.
    auto word_ends(std::string_view utf8, const std::vector<uint32_t>& starts) -> std::vector<uint32_t>;
    auto language() const -> const std::string& { return language_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string language_;
};

// Index file layout (little-endian), shared with the build side:
//   header (96 bytes) | page table (page_count × 14 × u32) | strings | records | text
// Records per page: [advance u8][weight:2 | stem length:6][stem bytes]. Advance is in UTF-8 bytes
// of the page text from the previous record's start. Length 0 + weight 0 = stop word,
// length 0 + weight 3 = filler (advance only, no word position).
inline constexpr char file_magic[8] = {'P', 'G', 'I', 'N', 'D', 'E', 'X', '1'};
inline constexpr size_t header_size = 96;
enum Header_Offset : size_t {   // byte offsets in the header; u32 up to h_pages, u64 from there
    h_format = 8, h_tokenizer = 12, h_language = 16, h_page_count = 32, h_pages = 40,
    h_strings = 48, h_strings_size = 56, h_records = 64, h_records_size = 72, h_text = 80, h_text_size = 88
};
enum Page_Field : uint32_t {
    path_off, path_len, title_off, title_len, ftitle_off, ftitle_len, terms_off, terms_len,
    records_off, records_len, word_count, text_off, text_len, prior_milli, page_fields
};
inline constexpr size_t max_stem_bytes = 63;
inline constexpr uint8_t record_filler = 0xC0;

}  // namespace detail
}  // namespace page_index
