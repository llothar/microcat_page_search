// page_index: query side and the shared analyzer.
#include "page_index_query.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>

#include <unicode/brkiter.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/unistr.h>
#include <unicode/utext.h>

#include <libstemmer.h>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

static_assert(std::endian::native == std::endian::little, "page_index assumes a little-endian host");

namespace page_index {
namespace detail {
namespace {

// Lucene EnglishAnalyzer stop set.
constexpr std::array<std::string_view, 33> english_stop_words = {
    "a", "an", "and", "are", "as", "at", "be", "but", "by", "for", "if", "in", "into", "is", "it",
    "no", "not", "of", "on", "or", "such", "that", "the", "their", "then", "there", "these",
    "they", "this", "to", "was", "will", "with"};

auto snowball_algorithm(std::string_view base) -> const char*
{
    static constexpr std::pair<std::string_view, const char*> table[] = {
        {"ar", "arabic"}, {"ca", "catalan"}, {"da", "danish"}, {"de", "german"}, {"el", "greek"},
        {"en", "english"}, {"es", "spanish"}, {"eu", "basque"}, {"fi", "finnish"}, {"fr", "french"},
        {"ga", "irish"}, {"hi", "hindi"}, {"hu", "hungarian"}, {"hy", "armenian"}, {"id", "indonesian"},
        {"it", "italian"}, {"lt", "lithuanian"}, {"nb", "norwegian"}, {"ne", "nepali"}, {"nl", "dutch"},
        {"nn", "norwegian"}, {"no", "norwegian"}, {"pl", "polish"}, {"pt", "portuguese"}, {"ro", "romanian"},
        {"ru", "russian"}, {"sr", "serbian"}, {"sv", "swedish"}, {"ta", "tamil"}, {"tr", "turkish"},
        {"uk", "ukrainian"}, {"yi", "yiddish"}};
    for (const auto& [code, name] : table) {
        if (code == base)
            return name;
    }
    return nullptr;
}

auto truncate_utf8(std::string& s, size_t max_bytes) -> void
{
    if (s.size() <= max_bytes)
        return;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80)
        --n;
    s.resize(n);
}

}  // namespace

struct Analyzer::Impl {
    std::unique_ptr<icu::BreakIterator> words;
    const icu::Normalizer2* casefold = nullptr;
    const icu::Normalizer2* nfd = nullptr;
    const icu::Normalizer2* nfc = nullptr;
    sb_stemmer* stemmer = nullptr;
    bool english = false;
    std::unordered_set<std::string_view> stop_words;

    ~Impl()
    {
        if (stemmer)
            sb_stemmer_delete(stemmer);
    }

    // Removes combining marks that follow a Latin letter (é → e); other scripts keep their marks.
    auto strip_latin_accents(const icu::UnicodeString& in) -> icu::UnicodeString
    {
        UErrorCode status = U_ZERO_ERROR;
        icu::UnicodeString decomposed = nfd->normalize(in, status);
        if (U_FAILURE(status))
            return in;
        icu::UnicodeString out;
        bool after_latin = false;
        for (int32_t i = 0; i < decomposed.length();) {
            UChar32 c = decomposed.char32At(i);
            i += U16_LENGTH(c);
            if (u_charType(c) == U_NON_SPACING_MARK) {
                if (!after_latin)
                    out.append(c);
                continue;
            }
            UErrorCode script_status = U_ZERO_ERROR;
            after_latin = uscript_getScript(c, &script_status) == USCRIPT_LATIN;
            out.append(c);
        }
        icu::UnicodeString composed = nfc->normalize(out, status);
        return U_SUCCESS(status) ? composed : out;
    }

    auto make_token(std::string_view text, uint32_t start, uint32_t end, Token& token) -> void
    {
        std::string_view word = text.substr(start, end - start);
        icu::UnicodeString original = icu::UnicodeString::fromUTF8(icu::StringPiece(word.data(), static_cast<int32_t>(word.size())));
        // Identifiers, numbers and camelCase are never stemmed.
        bool stem = stemmer != nullptr;
        for (int32_t i = 0; stem && i < original.length();) {
            UChar32 c = original.char32At(i);
            if (c == '_' || c == '.' || u_isdigit(c) || (i > 0 && u_isupper(c)))
                stem = false;
            i += U16_LENGTH(c);
        }
        UErrorCode status = U_ZERO_ERROR;
        icu::UnicodeString folded = casefold->normalize(original, status);
        if (U_FAILURE(status))
            folded = original;
        if (english && folded.length() > 2 && (folded.endsWith(u"'s") || folded.endsWith(u"’s")))
            folded.truncate(folded.length() - 2);

        token.start = start;
        token.length = end - start;
        token.term.clear();
        token.folded.clear();
        strip_latin_accents(folded).toUTF8String(token.folded);
        std::string case_folded;
        folded.toUTF8String(case_folded);
        if (case_folded.empty() || stop_words.contains(case_folded))
            return;
        if (stem) {
            const sb_symbol* s = sb_stemmer_stem(stemmer, reinterpret_cast<const sb_symbol*>(case_folded.data()),
                                                 static_cast<int>(case_folded.size()));
            if (s)
                case_folded.assign(reinterpret_cast<const char*>(s), static_cast<size_t>(sb_stemmer_length(stemmer)));
        }
        icu::UnicodeString stemmed = icu::UnicodeString::fromUTF8(case_folded);
        strip_latin_accents(stemmed).toUTF8String(token.term);
        truncate_utf8(token.term, max_stem_bytes);
    }
};

Analyzer::Analyzer(std::string_view language) : impl_(std::make_unique<Impl>()), language_(language)
{
    std::string base;
    for (char c : language) {
        if (c == '-' || c == '_')
            break;
        base += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    UErrorCode status = U_ZERO_ERROR;
    impl_->words.reset(icu::BreakIterator::createWordInstance(icu::Locale(base.c_str()), status));
    impl_->casefold = icu::Normalizer2::getNFKCCasefoldInstance(status);
    impl_->nfd = icu::Normalizer2::getNFDInstance(status);
    impl_->nfc = icu::Normalizer2::getNFCInstance(status);
    if (U_FAILURE(status) || !impl_->words)
        throw std::runtime_error("page_index: ICU initialisation failed");
    if (const char* algorithm = snowball_algorithm(base))
        impl_->stemmer = sb_stemmer_new(algorithm, "UTF_8");
    impl_->english = base == "en";
    if (impl_->english)
        impl_->stop_words.insert(english_stop_words.begin(), english_stop_words.end());
}

Analyzer::~Analyzer() = default;

auto Analyzer::analyze(std::string_view utf8, std::vector<Token>& tokens) -> void
{
    tokens.clear();
    UErrorCode status = U_ZERO_ERROR;
    UText* text = utext_openUTF8(nullptr, utf8.data(), static_cast<int64_t>(utf8.size()), &status);
    impl_->words->setText(text, status);
    if (U_FAILURE(status)) {
        utext_close(text);
        return;
    }
    int32_t start = impl_->words->first();
    for (int32_t end = impl_->words->next(); end != icu::BreakIterator::DONE; start = end, end = impl_->words->next()) {
        if (impl_->words->getRuleStatus() < UBRK_WORD_NONE_LIMIT)
            continue;
        impl_->make_token(utf8, static_cast<uint32_t>(start), static_cast<uint32_t>(end), tokens.emplace_back());
    }
    utext_close(text);
}

auto Analyzer::fold(std::string_view utf8) -> std::string
{
    std::vector<Token> tokens;
    analyze(utf8, tokens);
    std::string out;
    for (const Token& t : tokens) {
        if (t.folded.empty())
            continue;
        if (!out.empty())
            out += ' ';
        out += t.folded;
    }
    return out;
}

auto Analyzer::word_ends(std::string_view utf8, const std::vector<uint32_t>& starts) -> std::vector<uint32_t>
{
    std::vector<uint32_t> ends;
    ends.reserve(starts.size());
    UErrorCode status = U_ZERO_ERROR;
    UText* text = utext_openUTF8(nullptr, utf8.data(), static_cast<int64_t>(utf8.size()), &status);
    impl_->words->setText(text, status);
    for (uint32_t start : starts) {
        int32_t end = U_SUCCESS(status) ? impl_->words->following(static_cast<int32_t>(start)) : icu::BreakIterator::DONE;
        ends.push_back(end == icu::BreakIterator::DONE ? static_cast<uint32_t>(utf8.size()) : static_cast<uint32_t>(end));
    }
    utext_close(text);
    return ends;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Index file

namespace {

// memmem() is missing on Windows; elsewhere it is the fastest substring search available.
auto find_bytes(const void* hay, size_t hay_len, const void* needle, size_t needle_len) -> bool
{
#ifdef _WIN32
    std::string_view h(static_cast<const char*>(hay), hay_len), n(static_cast<const char*>(needle), needle_len);
    return h.find(n) != std::string_view::npos;
#else
    return memmem(hay, hay_len, needle, needle_len) != nullptr;
#endif
}

auto load32(const uint8_t* p) -> uint32_t { uint32_t v; std::memcpy(&v, p, 4); return v; }
auto load64(const uint8_t* p) -> uint64_t { uint64_t v; std::memcpy(&v, p, 8); return v; }

auto set_error(std::string* error, std::string message) -> void
{
    if (error)
        *error = std::move(message);
}

}  // namespace

struct Page_Index::Page {
    std::string_view path, title, ftitle, terms, text, summary;
    const uint8_t* records;
    uint32_t records_len, words;
    float prior;
};

auto Page_Index::page(uint32_t n) const -> Page
{
    const uint8_t* e = data_ + pages_offset_ + size_t(n) * page_fields_ * 4;
    auto f = [e](detail::Page_Field field) { return load32(e + field * 4); };
    auto str = [&](detail::Page_Field off, detail::Page_Field len) {
        return std::string_view(reinterpret_cast<const char*>(data_ + strings_offset_ + f(off)), f(len));
    };
    std::string_view summary = page_fields_ > detail::summary_len ? str(detail::summary_off, detail::summary_len)
                                                                  : std::string_view{};
    return Page{str(detail::path_off, detail::path_len), str(detail::title_off, detail::title_len),
                str(detail::ftitle_off, detail::ftitle_len), str(detail::terms_off, detail::terms_len),
                std::string_view(reinterpret_cast<const char*>(data_ + text_offset_ + f(detail::text_off)), f(detail::text_len)),
                summary, data_ + records_offset_ + f(detail::records_off), f(detail::records_len), f(detail::word_count),
                f(detail::prior_milli) / 1000.0f};
}

auto Page_Index::page_path(uint32_t n) const -> std::string_view { return page(n).path; }
auto Page_Index::page_title(uint32_t n) const -> std::string_view { return page(n).title; }
auto Page_Index::page_text(uint32_t n) const -> std::string_view { return page(n).text; }
auto Page_Index::page_summary(uint32_t n) const -> std::string_view { return page(n).summary; }

auto Page_Index::open(const std::string& path, std::string* error) -> std::unique_ptr<Page_Index>
{
    std::unique_ptr<Page_Index> index(new Page_Index());
#ifdef _WIN32
    std::filesystem::path fs_path(std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    HANDLE file = CreateFileW(fs_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error, "cannot open " + path);
        return nullptr;
    }
    LARGE_INTEGER file_size{};
    GetFileSizeEx(file, &file_size);
    HANDLE mapping = file_size.QuadPart > 0 ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(file);
    if (!mapping) {
        set_error(error, "cannot map " + path);
        return nullptr;
    }
    index->mapping_ = mapping;
    index->data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    index->size_ = static_cast<size_t>(file_size.QuadPart);
    if (!index->data_) {
        set_error(error, "cannot map " + path);
        return nullptr;
    }
#else
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(error, "cannot open " + path);
        return nullptr;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        set_error(error, "cannot stat " + path);
        return nullptr;
    }
    void* data = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) {
        set_error(error, "cannot map " + path);
        return nullptr;
    }
    index->data_ = static_cast<const uint8_t*>(data);
    index->size_ = static_cast<size_t>(st.st_size);
#endif
    const uint8_t* d = index->data_;
    if (index->size_ < detail::header_size || std::memcmp(d, detail::file_magic, 8) != 0) {
        set_error(error, path + " is not a page index");
        return nullptr;
    }
    uint32_t format = load32(d + detail::h_format);
    if ((format != format_version && format != 1) || load32(d + detail::h_tokenizer) != tokenizer_version) {
        set_error(error, path + " was built by a different format or tokenizer version");
        return nullptr;
    }
    index->page_fields_ = format == 1 ? detail::page_fields_v1 : detail::page_fields;
    const char* lang = reinterpret_cast<const char*>(d + detail::h_language);
    index->language_.assign(lang, strnlen(lang, 16));
    index->page_count_ = load32(d + detail::h_page_count);
    index->pages_offset_ = load64(d + detail::h_pages);
    index->strings_offset_ = load64(d + detail::h_strings);
    index->records_offset_ = load64(d + detail::h_records);
    index->text_offset_ = load64(d + detail::h_text);
    uint64_t strings_size = load64(d + detail::h_strings_size);
    uint64_t records_size = load64(d + detail::h_records_size);
    uint64_t text_size = load64(d + detail::h_text_size);
    auto inside = [&](uint64_t off, uint64_t len, uint64_t limit) { return off <= limit && len <= limit - off; };
    uint64_t table_size = uint64_t(index->page_count_) * index->page_fields_ * 4;
    if (!inside(index->pages_offset_, table_size, index->size_) || !inside(index->strings_offset_, strings_size, index->size_) ||
        !inside(index->records_offset_, records_size, index->size_) || !inside(index->text_offset_, text_size, index->size_)) {
        set_error(error, path + " is truncated or damaged");
        return nullptr;
    }
    uint64_t total_words = 0;
    for (uint32_t n = 0; n < index->page_count_; ++n) {
        const uint8_t* e = d + index->pages_offset_ + size_t(n) * index->page_fields_ * 4;
        auto f = [e](detail::Page_Field field) { return uint64_t(load32(e + field * 4)); };
        bool ok = inside(f(detail::path_off), f(detail::path_len), strings_size) &&
                  inside(f(detail::title_off), f(detail::title_len), strings_size) &&
                  inside(f(detail::ftitle_off), f(detail::ftitle_len), strings_size) &&
                  inside(f(detail::terms_off), f(detail::terms_len), strings_size) &&
                  inside(f(detail::records_off), f(detail::records_len), records_size) &&
                  inside(f(detail::text_off), f(detail::text_len), text_size) &&
                  (index->page_fields_ <= detail::summary_len ||
                   inside(f(detail::summary_off), f(detail::summary_len), strings_size));
        if (!ok) {
            set_error(error, path + ": page table entry " + std::to_string(n) + " is out of range");
            return nullptr;
        }
        total_words += f(detail::word_count);
    }
    index->average_words_ = index->page_count_ ? std::max(1.0, double(total_words) / index->page_count_) : 1.0;
    return index;
}

Page_Index::~Page_Index()
{
#ifdef _WIN32
    if (data_)
        UnmapViewOfFile(data_);
    if (mapping_)
        CloseHandle(static_cast<HANDLE>(mapping_));
#else
    if (data_)
        munmap(const_cast<uint8_t*>(data_), size_);
#endif
}

// ---------------------------------------------------------------------------------------------
// Search

namespace {

constexpr size_t max_terms = 31;
constexpr uint32_t stop_bit = 1u << 31;
constexpr float weight_factor[4] = {1.0f, 2.0f, 3.0f, 5.0f};   // body, emphasis, caption, heading
constexpr float quality_exact = 1.0f, quality_prefix = 0.7f, quality_infix = 0.3f;

struct Term {
    std::string bytes;
    bool prefix_ok;
    bool infix_ok;
};

struct Query {
    std::vector<Term> terms;
    std::vector<std::vector<int>> phrases;      // term index per word, -1 = stop word
    std::string normalized;                     // folded words, single spaces (title tiers)
    std::vector<std::string> words;             // folded non-stop words (title tiers)
};

struct Walk_Result {
    std::array<float, max_terms> tf{};
    uint32_t matched = 0;           // bit per term
    bool phrases_ok = true;
    uint32_t window = 0;            // smallest span of positions holding all matched terms
    uint32_t first_body = UINT32_MAX;   // offset of the first word that is not in a heading:
                                        // the headings before it (the H1) are the page's title
    struct Hit {
        uint32_t offset;     // UTF-8 byte offset of the word
        uint32_t terms;      // matched term bits
        uint32_t limit;      // start of the next word record: the word ends there at the latest
        uint32_t weight;     // 0 body, 1 emphasis, 2 caption, 3 heading
    };
    std::vector<Hit> hits;
};

auto add_term(Query& q, const std::string& bytes) -> int
{
    for (size_t i = 0; i < q.terms.size(); ++i) {
        if (q.terms[i].bytes == bytes)
            return static_cast<int>(i);
    }
    if (q.terms.size() >= max_terms)
        return -2;
    q.terms.push_back(Term{bytes, bytes.size() >= 3, bytes.size() >= 4});
    return static_cast<int>(q.terms.size() - 1);
}

auto parse_query(detail::Analyzer& analyzer, std::string_view text) -> Query
{
    Query q;
    std::vector<detail::Token> tokens;
    bool quoted = false;
    for (size_t pos = 0; pos <= text.size();) {
        size_t quote = text.find('"', pos);
        std::string_view segment = text.substr(pos, quote == std::string_view::npos ? std::string_view::npos : quote - pos);
        analyzer.analyze(segment, tokens);
        std::vector<int> phrase;
        for (const detail::Token& t : tokens) {
            if (!t.folded.empty()) {
                if (!q.normalized.empty())
                    q.normalized += ' ';
                q.normalized += t.folded;
            }
            if (t.term.empty()) {
                phrase.push_back(-1);
                continue;
            }
            q.words.push_back(t.folded);
            int index = add_term(q, t.term);
            phrase.push_back(index);
        }
        if (quoted && std::any_of(phrase.begin(), phrase.end(), [](int i) { return i >= 0; }) &&
            std::none_of(phrase.begin(), phrase.end(), [](int i) { return i == -2; }))
            q.phrases.push_back(std::move(phrase));
        if (quote == std::string_view::npos)
            break;
        pos = quote + 1;
        quoted = !quoted;
    }
    return q;
}

auto match_quality(const uint8_t* stem, size_t len, const Term& t) -> float
{
    size_t n = t.bytes.size();
    const uint8_t* term = reinterpret_cast<const uint8_t*>(t.bytes.data());
    if (len < n)
        return 0.0f;
    if (stem[0] == term[0] && std::memcmp(stem + 1, term + 1, n - 1) == 0)
        return len == n ? quality_exact : t.prefix_ok ? quality_prefix : 0.0f;
    if (!t.infix_ok || len == n)
        return 0.0f;
    for (size_t i = 1; i + n <= len; ++i) {   // stems are short: a plain loop beats memmem()
        if (stem[i] == term[0] && std::memcmp(stem + i + 1, term + 1, n - 1) == 0)
            return quality_infix;
    }
    return 0.0f;
}

// One pass over a page's records: weighted term frequencies, phrase check, proximity window, hits.
auto walk_page(const uint8_t* r, uint32_t len, uint32_t words, const Query& q, bool collect_hits) -> Walk_Result
{
    Walk_Result w;
    bool need_positions = q.terms.size() > 1 || !q.phrases.empty();
    std::vector<uint32_t> any_mask, exact_mask;
    if (need_positions) {
        any_mask.assign(words, 0);
        exact_mask.assign(words, 0);
    }
    const uint8_t* end = r + len;
    uint32_t offset = 0, pos = 0;
    while (end - r >= 2) {
        uint32_t advance = r[0];
        uint32_t stem_len = r[1] & 63u;
        uint32_t weight = r[1] >> 6;
        r += 2;
        offset += advance;
        if (stem_len == 0 && weight == 3)
            continue;   // filler
        if (collect_hits && !w.hits.empty() && w.hits.back().limit == UINT32_MAX)
            w.hits.back().limit = offset;
        if (stem_len > 0 && weight != 3 && w.first_body == UINT32_MAX)
            w.first_body = offset;
        if (stem_len == 0) {
            if (need_positions && pos < words)
                any_mask[pos] = exact_mask[pos] = stop_bit;
            ++pos;
            continue;
        }
        if (static_cast<uint32_t>(end - r) < stem_len)
            break;
        uint32_t any = 0, exact = 0;
        for (size_t t = 0; t < q.terms.size(); ++t) {
            float quality = match_quality(r, stem_len, q.terms[t]);
            if (quality > 0.0f) {
                w.tf[t] += weight_factor[weight] * quality;
                any |= 1u << t;
                if (quality == quality_exact)
                    exact |= 1u << t;
            }
        }
        r += stem_len;
        w.matched |= any;
        if (need_positions && pos < words) {
            any_mask[pos] = any;
            exact_mask[pos] = exact;
        }
        if (collect_hits && any)
            w.hits.push_back(Walk_Result::Hit{offset, any, UINT32_MAX, weight});
        ++pos;
    }
    if (!need_positions)
        return w;

    for (const std::vector<int>& phrase : q.phrases) {
        bool found = false;
        for (size_t start = 0; !found && start + phrase.size() <= any_mask.size(); ++start) {
            found = true;
            for (size_t i = 0; found && i < phrase.size(); ++i)
                found = phrase[i] < 0 ? exact_mask[start + i] == stop_bit : (exact_mask[start + i] & (1u << phrase[i])) != 0;
        }
        w.phrases_ok = w.phrases_ok && found;
    }

    // Smallest window of positions that contains every matched term at least once.
    int needed = std::popcount(w.matched);
    if (needed >= 2) {
        std::array<uint32_t, max_terms> count{};
        int have = 0;
        size_t left = 0;
        uint32_t best = UINT32_MAX;
        for (size_t right = 0; right < any_mask.size(); ++right) {
            uint32_t m = any_mask[right] & ~stop_bit;
            for (uint32_t bits = m; bits; bits &= bits - 1)
                have += count[std::countr_zero(bits)]++ == 0;
            while (have == needed) {
                best = std::min<uint32_t>(best, static_cast<uint32_t>(right - left + 1));
                for (uint32_t bits = any_mask[left] & ~stop_bit; bits; bits &= bits - 1)
                    have -= --count[std::countr_zero(bits)] == 0;
                ++left;
            }
        }
        w.window = best;
    }
    return w;
}

auto contains_word_prefix(std::string_view hay, std::string_view word) -> bool
{
    for (size_t p = hay.find(word); p != std::string_view::npos; p = hay.find(word, p + 1)) {
        if (p == 0 || hay[p - 1] == ' ')
            return true;
    }
    return false;
}

auto title_tier(std::string_view text, const Query& q) -> int
{
    if (q.normalized.empty() || text.empty())
        return 0;
    if (text == q.normalized)
        return 3;
    if (text.starts_with(q.normalized))
        return 2;
    if (q.words.empty())
        return 0;
    return std::all_of(q.words.begin(), q.words.end(), [&](const std::string& w) { return contains_word_prefix(text, w); }) ? 1 : 0;
}

auto page_tier(std::string_view ftitle, std::string_view terms, const Query& q) -> int
{
    int tier = title_tier(ftitle, q);
    for (size_t p = 0; tier < 3 && p < terms.size();) {
        size_t nl = terms.find('\n', p);
        std::string_view line = terms.substr(p, nl == std::string_view::npos ? std::string_view::npos : nl - p);
        tier = std::max(tier, title_tier(line, q));
        if (nl == std::string_view::npos)
            break;
        p = nl + 1;
    }
    return tier;
}

auto utf8_char_start(std::string_view s, size_t i) -> size_t
{
    while (i > 0 && i < s.size() && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80)
        --i;
    return i;
}

auto is_space(char c) -> bool { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }

auto append_escaped(std::string& out, char c) -> void
{
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    default: out += c; break;
    }
}

// text[start, end) as HTML: whitespace collapsed, matched words in <mark>, "…" where cut.
auto render_window(std::string_view text, const std::vector<std::pair<uint32_t, uint32_t>>& spans, size_t start,
                   size_t end, bool cut_before, bool cut_after) -> std::string
{
    std::string out;
    if (cut_before)
        out += "…";
    size_t next = 0;
    while (next < spans.size() && spans[next].first < start)
        ++next;
    bool in_mark = false;
    size_t mark_end = 0;
    bool pending_space = false;
    for (size_t i = start; i < end; ++i) {
        if (in_mark && i == mark_end) {
            out += "</mark>";
            in_mark = false;
        }
        if (!in_mark && next < spans.size() && i == spans[next].first && spans[next].second <= end) {
            if (pending_space)
                out += ' ';
            pending_space = false;
            out += "<mark>";
            in_mark = true;
            mark_end = spans[next].second;
            ++next;
        }
        if (is_space(text[i])) {
            pending_space = !out.empty() && out != "…";
            continue;
        }
        if (pending_space)
            out += ' ';
        pending_space = false;
        append_escaped(out, text[i]);
    }
    if (in_mark)
        out += "</mark>";
    if (cut_after)
        out += "…";
    return out;
}

// Snippet around the densest group of hits; whitespace collapsed, matched words in <mark>.
auto make_snippet(std::string_view text, const std::vector<std::pair<uint32_t, uint32_t>>& spans,
                  const std::vector<uint32_t>& span_terms, size_t width) -> std::string
{
    size_t best = 0;
    int best_value = -1;
    for (size_t i = 0; i < spans.size(); ++i) {
        uint32_t distinct = 0;
        int count = 0;
        for (size_t j = i; j < spans.size() && spans[j].second <= spans[i].first + width; ++j) {
            distinct |= span_terms[j];
            ++count;
        }
        int value = std::popcount(distinct) * 1000 + count;
        if (value > best_value) {
            best_value = value;
            best = i;
        }
    }
    size_t start = 0;
    if (!spans.empty() && spans[best].first > width / 5) {
        start = spans[best].first - width / 5;
        size_t limit = std::min<size_t>(start + 30, spans[best].first);
        size_t s = start;
        while (s < limit && !is_space(text[s]))
            ++s;
        start = s < limit ? s + 1 : utf8_char_start(text, start);
    }
    size_t end = std::min(text.size(), start + width);
    if (end < text.size()) {
        size_t e = end;
        while (e > end - std::min<size_t>(30, end - start) && !is_space(text[e]))
            --e;
        end = is_space(text[e]) ? e : utf8_char_start(text, end);
    }

    return render_window(text, spans, start, end, start > 0, end < text.size());
}

auto back_chars(std::string_view text, size_t i, size_t count, size_t floor) -> size_t
{
    while (count > 0 && i > floor) {
        --i;
        while (i > floor && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80)
            --i;
        --count;
    }
    return i;
}

auto forward_chars(std::string_view text, size_t i, size_t count, size_t ceiling) -> size_t
{
    while (count > 0 && i < ceiling) {
        ++i;
        while (i < ceiling && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80)
            ++i;
        --count;
    }
    return i;
}

// Keyword-in-context lines: about `chars` characters around a matched word each, inside its
// paragraph (block boundaries are newlines in the text), cut at spaces where there are any.
// The first occurrence of each query term comes first, then further occurrences; lines do not
// overlap and appear in page order. Body text comes before headings, which are used only for
// words found nowhere else. Matches before first_body (the headings the page starts with, i.e.
// its title) are left out.
auto make_contexts(std::string_view text, const std::vector<std::pair<uint32_t, uint32_t>>& spans,
                   const std::vector<uint32_t>& span_terms, const std::vector<uint32_t>& span_weights,
                   uint32_t first_body, size_t lines, size_t chars) -> std::vector<std::string>
{
    struct Window {
        size_t start, end;
    };
    size_t radius = std::max<size_t>(chars / 2, 10);
    auto window_of = [&](size_t i) -> Window {
        size_t s = spans[i].first, e = spans[i].second;
        size_t para_begin = s == 0 ? 0 : text.rfind('\n', s - 1);
        para_begin = para_begin == std::string_view::npos ? 0 : para_begin + 1;
        size_t para_end = text.find('\n', e);
        if (para_end == std::string_view::npos)
            para_end = text.size();
        size_t start = back_chars(text, s, radius, para_begin);
        size_t end = forward_chars(text, e, radius, para_end);
        if (start > para_begin) {
            size_t space = start;
            while (space < s && !is_space(text[space]))
                ++space;
            if (space < s)
                start = space + 1;
        }
        if (end < para_end) {
            size_t space = end;
            while (space > e && !is_space(text[space]))
                --space;
            if (space > e)
                end = space;
        }
        return Window{start, end};
    };

    std::vector<Window> chosen;
    auto overlaps = [&](const Window& w) {
        for (const Window& c : chosen) {
            if (w.start < c.end && c.start < w.end)
                return true;
        }
        return false;
    };
    uint32_t covered = 0;
    // passes: new terms in body text, new terms anywhere, more body text, anything
    for (int pass = 0; pass < 4; ++pass) {
        bool new_terms_only = pass < 2, body_only = pass % 2 == 0;
        for (size_t i = 0; i < spans.size() && chosen.size() < lines; ++i) {
            if (spans[i].first < first_body)
                continue;
            if (new_terms_only && (span_terms[i] & ~covered) == 0)
                continue;
            if (body_only && span_weights[i] == 3)
                continue;
            Window w = window_of(i);
            if (overlaps(w))
                continue;
            covered |= span_terms[i];
            chosen.push_back(w);
        }
    }
    std::sort(chosen.begin(), chosen.end(), [](const Window& a, const Window& b) { return a.start < b.start; });

    std::vector<std::string> out;
    for (const Window& w : chosen) {
        bool cut_before = w.start > 0 && text[w.start - 1] != '\n';
        bool cut_after = w.end < text.size() && text[w.end] != '\n';
        out.push_back(render_window(text, spans, w.start, w.end, cut_before, cut_after));
    }
    return out;
}

// UTF-8 byte offsets (sorted) to UTF-16 unit offsets.
auto to_utf16(std::string_view text, const std::vector<uint32_t>& sorted_offsets) -> std::vector<uint32_t>
{
    std::vector<uint32_t> out;
    out.reserve(sorted_offsets.size());
    uint32_t units = 0;
    size_t i = 0;
    for (uint32_t target : sorted_offsets) {
        for (; i < target && i < text.size(); ++i) {
            uint8_t b = static_cast<uint8_t>(text[i]);
            units += (b & 0xC0) != 0x80;
            units += b >= 0xF0;
        }
        out.push_back(units);
    }
    return out;
}

struct Candidate {
    uint32_t page;
    int tier;
    float score;
};

}  // namespace

auto Page_Index::search(std::string_view text, const Search_Options& options) const -> std::vector<Page_Hit>
{
    // One analyzer per thread and language, so a server with several language indexes reuses them.
    thread_local std::vector<std::unique_ptr<detail::Analyzer>> analyzers;
    detail::Analyzer* analyzer = nullptr;
    for (const auto& a : analyzers) {
        if (a->language() == language_)
            analyzer = a.get();
    }
    if (!analyzer)
        analyzer = analyzers.emplace_back(std::make_unique<detail::Analyzer>(language_)).get();

    Query q = parse_query(*analyzer, text);
    if (q.terms.empty() && q.normalized.empty())
        return {};
    size_t nterms = q.terms.size();
    uint32_t all_terms = nterms ? static_cast<uint32_t>((uint64_t(1) << nterms) - 1) : 0;

    // Pass 1: byte search per term over each page's records (superset of real matches),
    // document frequencies, title and index-term tiers.
    std::vector<uint32_t> present(page_count_);
    std::vector<uint32_t> df(nterms, 0);
    std::vector<int> tiers(page_count_);
    for (uint32_t n = 0; n < page_count_; ++n) {
        Page p = page(n);
        uint32_t mask = 0;
        for (size_t t = 0; t < nterms; ++t) {
            if (find_bytes(p.records, p.records_len, q.terms[t].bytes.data(), q.terms[t].bytes.size())) {
                mask |= 1u << t;
                ++df[t];
            }
        }
        present[n] = mask;
        tiers[n] = page_tier(p.ftitle, p.terms, q);
    }

    // Pass 2: walk candidate pages and score. AND first; OR with a penalty if AND finds nothing.
    auto score_page = [&](uint32_t n, const Walk_Result& w) -> float {
        Page p = page(n);
        constexpr float k1 = 1.2f, b = 0.75f;
        float norm = k1 * (1.0f - b + b * float(p.words) / float(average_words_));
        float score = 0.0f;
        for (size_t t = 0; t < nterms; ++t) {
            if (w.tf[t] <= 0.0f)
                continue;
            float d = float(std::max<uint32_t>(df[t], 1));
            float idf = std::log1p((float(page_count_) - d + 0.5f) / (d + 0.5f));
            score += idf * w.tf[t] * (k1 + 1.0f) / (w.tf[t] + norm);
        }
        int matched = std::popcount(w.matched);
        if (matched >= 2 && w.window)
            score *= 1.0f + float(matched) / float(w.window);
        return score * p.prior;
    };
    std::vector<Candidate> candidates;
    bool any_full = false;
    for (int round = 0; round < 2 && !any_full; ++round) {
        bool or_mode = round == 1;
        if (or_mode && (nterms < 2 || !q.phrases.empty()))
            break;
        candidates.clear();
        for (uint32_t n = 0; n < page_count_; ++n) {
            bool walk = nterms && (or_mode ? present[n] != 0 : present[n] == all_terms);
            float score = 0.0f;
            if (walk) {
                Page p = page(n);
                Walk_Result w = walk_page(p.records, p.records_len, p.words, q, false);
                bool full = w.matched == all_terms && w.phrases_ok;
                if (full || or_mode) {
                    score = score_page(n, w);
                    if (or_mode) {
                        float share = float(std::popcount(w.matched)) / float(nterms);
                        score *= share * share;
                    }
                    any_full = any_full || full || or_mode;
                } else {
                    walk = false;
                }
            }
            if (walk || tiers[n] > 0)
                candidates.push_back(Candidate{n, tiers[n], score});
        }
    }

    size_t keep = std::min(options.max_results, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + keep, candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.tier != b.tier)
            return a.tier > b.tier;
        if (a.score != b.score)
            return a.score > b.score;
        return a.page < b.page;
    });
    candidates.resize(keep);

    // Pass 3: hits, UTF-16 ranges and snippets for the results only.
    std::vector<Page_Hit> results;
    results.reserve(keep);
    for (const Candidate& c : candidates) {
        Page p = page(c.page);
        Page_Hit hit{c.page, std::string(p.path), std::string(p.title), c.score, c.tier, {}, {}, std::string(p.summary), {}, {}};
        Walk_Result w = nterms ? walk_page(p.records, p.records_len, p.words, q, true) : Walk_Result{};
        std::vector<std::pair<uint32_t, uint32_t>> spans;   // byte start, byte end
        std::vector<uint32_t> span_terms, span_weights;
        std::vector<uint32_t> starts;
        for (const Walk_Result::Hit& h : w.hits) {
            if (starts.size() >= options.max_ranges || h.offset >= p.text.size())
                break;
            starts.push_back(h.offset);
            span_terms.push_back(h.terms);
            span_weights.push_back(h.weight);
        }
        std::vector<uint32_t> ends = analyzer->word_ends(p.text, starts);
        for (size_t i = 0; i < starts.size(); ++i)   // words split at block boundaries end at the next word
            spans.emplace_back(starts[i], std::min(ends[i], w.hits[i].limit));
        std::vector<uint32_t> boundaries;
        boundaries.reserve(spans.size() * 2);
        for (const auto& [s, e] : spans) {
            boundaries.push_back(s);
            boundaries.push_back(e);
        }
        std::vector<uint32_t> units = to_utf16(p.text, boundaries);
        for (size_t i = 0; i < spans.size(); ++i)
            hit.ranges.push_back(Hit_Range{units[2 * i], units[2 * i + 1] - units[2 * i]});
        hit.snippet_html = make_snippet(p.text, spans, span_terms, options.snippet_bytes);
        if (options.context_lines > 0)
            hit.contexts_html = make_contexts(p.text, spans, span_terms, span_weights, w.first_body,
                                              options.context_lines, options.context_chars);
        for (const auto& [s, e] : spans) {
            std::string word(p.text.substr(s, e - s));
            if (!word.empty() && std::find(hit.words.begin(), hit.words.end(), word) == hit.words.end())
                hit.words.push_back(std::move(word));
        }
        results.push_back(std::move(hit));
    }
    return results;
}

}  // namespace page_index
