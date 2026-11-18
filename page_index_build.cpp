// page_index: build side. Prepared HTML → text exactly as the browser's DOM holds it → word records.
#include "page_index_build.h"
#include "page_index_query.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#ifndef _WIN32
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace page_index {
namespace {

// ---------------------------------------------------------------------------------------------
// HTML text extraction. The result must equal the concatenation of the DOM's text nodes under
// the root (what MarkIt/mark.js walk), so it follows the HTML parser where that matters:
// CR/LF normalisation, entity decoding, the dropped newline after <pre>, raw-text elements.

constexpr std::array<std::string_view, 14> void_elements = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
constexpr std::array<std::string_view, 39> block_elements = {
    "address", "article", "aside", "blockquote", "br", "caption", "dd", "details", "dialog", "div", "dl", "dt",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr",
    "li", "main", "nav", "ol", "p", "pre", "section", "summary", "table", "td", "th", "tr", "ul", "option"};
constexpr std::array<std::string_view, 7> raw_text_elements = {
    "script", "style", "xmp", "iframe", "noembed", "noframes", "noscript"};

struct Entity {
    std::string_view name;
    char32_t code;
};
// The entities a prepared page may use by name; anything else should be written as UTF-8.
constexpr Entity named_entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"shy", 0xAD}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"laquo", 0xAB}, {"raquo", 0xBB},
    {"bull", 0x2022}, {"middot", 0xB7}, {"deg", 0xB0}, {"times", 0xD7}, {"divide", 0xF7},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6},
    {"plusmn", 0xB1}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193},
    {"zwj", 0x200D}, {"zwnj", 0x200C}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}};

auto is_one_of(std::string_view name, const auto& list) -> bool
{
    return std::find(std::begin(list), std::end(list), name) != std::end(list);
}

auto append_utf8(std::string& out, char32_t c) -> void
{
    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        c = 0xFFFD;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

auto lower(std::string_view s) -> std::string
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return out;
}

auto is_ws(char c) -> bool { return c == ' ' || c == '\n' || c == '\t' || c == '\f' || c == '\r'; }
auto is_alpha(char c) -> bool { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct Page_Text {
    std::string text;               // UTF-8, as the DOM holds it
    std::vector<uint8_t> weight;    // per text byte: 0 body, 1 emphasis, 2 caption, 3 heading
    std::vector<uint32_t> blocks;   // text offsets of block element boundaries (words never cross them)
    std::string title;
    std::vector<std::string> keywords;
    float prior = 1.0f;
    std::string lang;
};

class Html_Extractor {
public:
    // Root detection needs real tags (not "<main" inside a comment or script), so parse twice.
    static auto extract(std::string_view html, std::vector<std::string>& warnings, const std::string& path) -> Page_Text
    {
        return Html_Extractor(html, warnings, path, root_of(html)).run();
    }

    static auto root_of(std::string_view html) -> std::string
    {
        std::vector<std::string> ignored;
        std::string path;
        Html_Extractor probe(html, ignored, path, "#probe");
        probe.run();
        return probe.seen_main_ ? "main" : probe.seen_body_ ? "body" : "";
    }

    Html_Extractor(std::string_view html, std::vector<std::string>& warnings, const std::string& path, std::string_view root)
        : warnings_(warnings), path_(path), root_(root)
    {
        html_.reserve(html.size());
        for (size_t i = 0; i < html.size(); ++i) {   // the parser turns CR LF and CR into LF
            if (html[i] == '\r') {
                html_ += '\n';
                i += i + 1 < html.size() && html[i + 1] == '\n';
            } else {
                html_ += html[i];
            }
        }
    }

    auto run() -> Page_Text
    {
        stack_.push_back(Open{"#document", 0, false, root_.empty(), false});
        while (pos_ < html_.size()) {
            if (html_[pos_] == '<' && start_markup())
                continue;
            size_t lt = html_.find('<', pos_ + 1);
            size_t end = lt == std::string::npos ? html_.size() : lt;
            emit_text(std::string_view(html_).substr(pos_, end - pos_), true);
            pos_ = end;
        }
        return std::move(out_);
    }

private:
    struct Open {
        std::string name;
        uint8_t weight;
        bool skip;
        bool in_root;
        bool foreign;
    };

    auto warn(const std::string& message) -> void
    {
        if (warned_.size() < 8 && std::find(warned_.begin(), warned_.end(), message) == warned_.end()) {
            warned_.push_back(message);
            warnings_.push_back(path_ + ": " + message);
        }
    }

    auto decode(std::string_view raw) -> std::string
    {
        std::string out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            size_t semi = raw.find(';', i + 1);
            if (semi == std::string_view::npos || semi - i > 32) {
                out += '&';
                continue;
            }
            std::string_view name = raw.substr(i + 1, semi - i - 1);
            char32_t code = 0;
            if (name.size() > 1 && name[0] == '#') {
                bool hex = name[1] == 'x' || name[1] == 'X';
                std::string_view digits = name.substr(hex ? 2 : 1);
                bool ok = !digits.empty();
                for (char c : digits) {
                    int v = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    ok = ok && v >= 0;
                    code = ok && code < 0x110000 ? code * (hex ? 16 : 10) + char32_t(v) : code;
                }
                if (!ok) {
                    out += '&';
                    continue;
                }
            } else {
                for (const Entity& e : named_entities) {
                    if (e.name == name)
                        code = e.code;
                }
                if (code == 0) {
                    warn("unknown entity &" + std::string(name) + "; kept literally, the browser may decode it");
                    out += '&';
                    continue;
                }
            }
            append_utf8(out, code);
            i = semi;
        }
        return out;
    }

    auto emit_text(std::string_view raw, bool entities) -> void
    {
        const Open& top = stack_.back();
        if (!top.in_root || top.skip)
            return;
        std::string text = entities ? decode(raw) : std::string(raw);
        if (drop_newline_ && !text.empty() && text[0] == '\n')
            text.erase(0, 1);
        drop_newline_ = false;
        if (block_pending_ && !text.empty() && !out_.text.empty() && !is_ws(text[0]) && !is_ws(out_.text.back()))
            warn("block elements without whitespace between them: their words run together in the DOM text and in "
                 "snippets (near \"" + out_.text.substr(out_.text.size() - std::min<size_t>(out_.text.size(), 20)) + "\")");
        block_pending_ = false;
        out_.text += text;
        out_.weight.insert(out_.weight.end(), text.size(), top.weight);
    }

    // At '<'. Returns false if this '<' starts no markup (then it is text).
    auto start_markup() -> bool
    {
        size_t p = pos_ + 1;
        if (html_.compare(p, 3, "!--") == 0) {
            size_t end = html_.find("-->", p + 3);
            pos_ = end == std::string::npos ? html_.size() : end + 3;
            return true;
        }
        if (p < html_.size() && (html_[p] == '!' || html_[p] == '?')) {
            size_t end = html_.find('>', p);
            pos_ = end == std::string::npos ? html_.size() : end + 1;
            return true;
        }
        bool closing = p < html_.size() && html_[p] == '/';
        p += closing;
        if (p >= html_.size() || !is_alpha(html_[p])) {
            if (closing) {   // "</" + non-letter is a bogus comment
                size_t end = html_.find('>', p);
                pos_ = end == std::string::npos ? html_.size() : end + 1;
                return true;
            }
            return false;
        }
        size_t name_end = p;
        while (name_end < html_.size() && !is_ws(html_[name_end]) && html_[name_end] != '/' && html_[name_end] != '>')
            ++name_end;
        std::string name = lower(std::string_view(html_).substr(p, name_end - p));
        std::vector<std::pair<std::string, std::string>> attributes;
        bool self_closing = false;
        size_t q = parse_attributes(name_end, attributes, self_closing);
        pos_ = q;
        drop_newline_ = false;
        if (is_one_of(name, block_elements) && stack_.back().in_root) {
            if (out_.blocks.empty() || out_.blocks.back() != out_.text.size())
                out_.blocks.push_back(static_cast<uint32_t>(out_.text.size()));
            block_pending_ = true;
        }
        if (closing)
            end_tag(name);
        else
            start_tag(name, attributes, self_closing);
        return true;
    }

    auto parse_attributes(size_t p, std::vector<std::pair<std::string, std::string>>& attributes, bool& self_closing) -> size_t
    {
        while (p < html_.size()) {
            while (p < html_.size() && is_ws(html_[p]))
                ++p;
            if (p >= html_.size())
                break;
            if (html_[p] == '>')
                return p + 1;
            if (html_[p] == '/') {
                self_closing = p + 1 < html_.size() && html_[p + 1] == '>';
                ++p;
                continue;
            }
            size_t n = p;
            while (n < html_.size() && !is_ws(html_[n]) && html_[n] != '/' && html_[n] != '>' && (html_[n] != '=' || n == p))
                ++n;
            std::string attr = lower(std::string_view(html_).substr(p, n - p));
            p = n;
            while (p < html_.size() && is_ws(html_[p]))
                ++p;
            std::string value;
            if (p < html_.size() && html_[p] == '=') {
                ++p;
                while (p < html_.size() && is_ws(html_[p]))
                    ++p;
                if (p < html_.size() && (html_[p] == '"' || html_[p] == '\'')) {
                    char quote = html_[p];
                    size_t end = html_.find(quote, p + 1);
                    end = end == std::string::npos ? html_.size() : end;
                    value = decode(std::string_view(html_).substr(p + 1, end - p - 1));
                    p = std::min(end + 1, html_.size());
                } else {
                    size_t end = p;
                    while (end < html_.size() && !is_ws(html_[end]) && html_[end] != '>')
                        ++end;
                    value = decode(std::string_view(html_).substr(p, end - p));
                    p = end;
                }
            }
            attributes.emplace_back(std::move(attr), std::move(value));
        }
        return html_.size();
    }

    auto start_tag(const std::string& name, const std::vector<std::pair<std::string, std::string>>& attributes, bool self_closing) -> void
    {
        auto attribute = [&](std::string_view key) -> const std::string* {
            for (const auto& [k, v] : attributes) {
                if (k == key)
                    return &v;
            }
            return nullptr;
        };
        if (name == "html" && attribute("lang"))
            out_.lang = *attribute("lang");
        if (name == "meta" && attribute("name") && attribute("content")) {
            std::string meta = lower(*attribute("name"));
            if (meta == "keywords")
                split_keywords(*attribute("content"));
            if (meta == "search-prior")
                out_.prior = std::clamp(std::strtof(attribute("content")->c_str(), nullptr), 0.0f, 100.0f);
        }
        if (name == "title" || name == "textarea") {   // RCDATA: entities, no tags
            std::string content = decode(raw_content(name));
            if (name == "title" && !stack_.back().in_root) {
                if (out_.title.empty())
                    out_.title = content;
                return;
            }
            drop_newline_ = name == "textarea";
            emit_text(content, false);
            return;
        }
        if (name == "script" || name == "style") {
            raw_content(name);
            return;
        }
        if (is_one_of(name, raw_text_elements)) {
            std::string content(raw_content(name));
            if (stack_.back().in_root && !stack_.back().skip && !content.empty())
                warn("<" + name + "> content is a text node in the browser and is indexed raw; mark it data-fs-skip");
            emit_text(content, false);
            return;
        }
        seen_main_ = seen_main_ || name == "main";
        seen_body_ = seen_body_ || name == "body";
        const Open& parent = stack_.back();
        bool foreign = parent.foreign || name == "svg" || name == "math";
        if (is_one_of(name, void_elements) || (self_closing && foreign))
            return;
        uint8_t weight = parent.weight;
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
            weight = 3;
        else if (name == "figcaption" || name == "caption")
            weight = std::max<uint8_t>(weight, 2);
        else if (name == "em" || name == "strong" || name == "b" || name == "i" || name == "mark")
            weight = std::max<uint8_t>(weight, 1);
        bool skip = parent.skip || attribute("data-fs-skip") || name == "template" || name == "head";
        bool in_root = parent.in_root || (!root_seen_ && name == root_);
        root_seen_ = root_seen_ || name == root_;
        stack_.push_back(Open{name, weight, skip, in_root, foreign});
        drop_newline_ = name == "pre" || name == "listing";
    }

    auto end_tag(const std::string& name) -> void
    {
        if (name == "body" || name == "html")   // the parser keeps the body open: later whitespace still goes into it
            return;
        for (size_t i = stack_.size(); i-- > 1;) {
            if (stack_[i].name == name) {
                stack_.resize(i);
                return;
            }
        }
    }

    // Content of a raw-text/RCDATA element up to its end tag; moves pos_ past the end tag.
    auto raw_content(const std::string& name) -> std::string_view
    {
        size_t start = pos_;
        for (size_t p = html_.find("</", start); p != std::string::npos; p = html_.find("</", p + 2)) {
            if (lower(std::string_view(html_).substr(p + 2, name.size())) == name) {
                char after = p + 2 + name.size() < html_.size() ? html_[p + 2 + name.size()] : '>';
                if (is_ws(after) || after == '>' || after == '/') {
                    size_t gt = html_.find('>', p);
                    pos_ = gt == std::string::npos ? html_.size() : gt + 1;
                    return std::string_view(html_).substr(start, p - start);
                }
            }
        }
        pos_ = html_.size();
        return std::string_view(html_).substr(start);
    }

    auto split_keywords(const std::string& content) -> void
    {
        size_t p = 0;
        while (p <= content.size()) {
            size_t comma = content.find(',', p);
            size_t end = comma == std::string::npos ? content.size() : comma;
            size_t a = p, b = end;
            while (a < b && is_ws(content[a]))
                ++a;
            while (b > a && is_ws(content[b - 1]))
                --b;
            if (b > a)
                out_.keywords.emplace_back(content.substr(a, b - a));
            if (comma == std::string::npos)
                break;
            p = comma + 1;
        }
    }

    std::string html_;
    std::vector<std::string>& warnings_;
    std::vector<std::string> warned_;
    const std::string& path_;
    std::string root_;
    bool root_seen_ = false;
    bool seen_main_ = false;
    bool seen_body_ = false;
    bool drop_newline_ = false;
    bool block_pending_ = false;
    size_t pos_ = 0;
    std::vector<Open> stack_;
    Page_Text out_;
};

// ---------------------------------------------------------------------------------------------
// Strict validator: accepts only HTML whose DOM the extractor reproduces exactly.

constexpr std::string_view p_closers[] = {   // start tags that close an open <p> (HTML parser)
    "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset",
    "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol", "p", "search", "section",
    "summary", "ul", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "listing", "form", "plaintext", "table", "hr",
    "li", "dd", "dt", "xmp"};
constexpr std::string_view scope_boundaries[] = {
    "applet", "caption", "html", "table", "td", "th", "marquee", "object", "template",
    "mi", "mo", "mn", "ms", "mtext", "annotation-xml", "foreignobject", "desc", "title"};
constexpr std::string_view head_elements[] = {
    "base", "basefont", "bgsound", "link", "meta", "title", "style", "script", "noscript", "template"};
constexpr std::string_view foreign_breakout[] = {
    "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2",
    "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre",
    "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var", "font"};
constexpr std::string_view html_elements[] = {
    "a", "abbr", "address", "area", "article", "aside", "audio", "b", "base", "bdi", "bdo", "blockquote", "body", "br",
    "button", "canvas", "caption", "cite", "code", "col", "colgroup", "data", "datalist", "dd", "del", "details", "dfn",
    "dialog", "div", "dl", "dt", "em", "embed", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
    "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "i", "iframe", "img", "input", "ins", "kbd", "label",
    "legend", "li", "link", "main", "map", "mark", "math", "menu", "meta", "meter", "nav", "noscript", "object", "ol",
    "optgroup", "option", "output", "p", "param", "picture", "pre", "progress", "q", "rp", "rt", "ruby", "s", "samp",
    "script", "search", "section", "select", "slot", "small", "source", "span", "strong", "style", "sub", "summary",
    "sup", "svg", "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "time", "title", "tr",
    "track", "u", "ul", "var", "video", "wbr"};
constexpr std::string_view obsolete_elements[] = {
    "acronym", "applet", "basefont", "bgsound", "big", "blink", "center", "dir", "font", "frame", "frameset", "image",
    "isindex", "keygen", "listing", "marquee", "menuitem", "nobr", "noembed", "noframes", "plaintext", "rb", "rtc",
    "spacer", "strike", "tt", "xmp"};
constexpr std::string_view not_in_root[] = {"noscript", "iframe", "xmp", "noembed", "noframes"};

class Html_Validator {
public:
    Html_Validator(std::string_view html, std::string_view language, std::string_view root)
        : h_(html), language_(language), root_(root)
    {
        line_starts_.push_back(0);
        for (size_t i = 0; i < h_.size(); ++i) {
            if (h_[i] == '\n')
                line_starts_.push_back(i + 1);
        }
    }

    auto run() -> std::vector<Html_Issue>
    {
        check_encoding();
        if (h_.starts_with("\xEF\xBB\xBF"))
            pos_ = 3;
        while (pos_ < h_.size() && !plaintext_) {
            if (h_[pos_] == '<' && markup())
                continue;
            size_t lt = h_.find('<', pos_ + 1);
            size_t end = lt == std::string_view::npos ? h_.size() : lt;
            text(pos_, end);
            pos_ = end;
        }
        finish();
        std::stable_sort(issues_.begin(), issues_.end(), [](const Html_Issue& a, const Html_Issue& b) {
            return a.line != b.line ? a.line < b.line : a.column < b.column;
        });
        return std::move(issues_);
    }

private:
    enum class Mode { initial, before_head, in_head, after_head, in_body, after_body, after_html };
    struct Open {
        std::string name;
        size_t at;
        bool skip;
        bool in_root;
        bool foreign;
        bool reported;      // an error was already reported for this element: no follow-up errors
    };
    using Attributes = std::vector<std::pair<std::string, std::string>>;

    auto issue(Issue_Level level, size_t at, std::string message) -> void
    {
        if (issues_.size() >= 300)
            return;
        auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), at);
        size_t line = static_cast<size_t>(it - line_starts_.begin());
        size_t column = at - line_starts_[line - 1] + 1;
        issues_.push_back(Html_Issue{level, static_cast<uint32_t>(line), static_cast<uint32_t>(column), std::move(message)});
    }
    auto error(size_t at, std::string message) -> void { issue(Issue_Level::error, at, std::move(message)); }
    auto warning(size_t at, std::string message) -> void { issue(Issue_Level::warning, at, std::move(message)); }

    auto line_of(size_t at) const -> std::string
    {
        return std::to_string(std::upper_bound(line_starts_.begin(), line_starts_.end(), at) - line_starts_.begin());
    }

    auto top() const -> const Open* { return stack_.empty() ? nullptr : &stack_.back(); }
    auto indexed() const -> bool { return !stack_.empty() ? stack_.back().in_root && !stack_.back().skip : root_.empty(); }

    auto in_scope(std::string_view name, std::initializer_list<std::string_view> extra) const -> bool
    {
        for (size_t i = stack_.size(); i-- > 0;) {
            if (stack_[i].name == name)
                return true;
            if (is_one_of(stack_[i].name, scope_boundaries) || std::find(extra.begin(), extra.end(), stack_[i].name) != extra.end())
                return false;
        }
        return false;
    }

    // UTF-8 validity and characters the parser changes or reports.
    auto check_encoding() -> void
    {
        int reported = 0;
        for (size_t i = 0; i < h_.size() && reported < 10;) {
            uint8_t b = static_cast<uint8_t>(h_[i]);
            size_t n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
            char32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
            bool ok = n != 0 && i + n <= h_.size();
            for (size_t k = 1; ok && k < n; ++k) {
                uint8_t cb = static_cast<uint8_t>(h_[i + k]);
                ok = (cb & 0xC0) == 0x80;
                c = (c << 6) | (cb & 0x3F);
            }
            ok = ok && !(n == 2 && c < 0x80) && !(n == 3 && c < 0x800) && !(n == 4 && (c < 0x10000 || c > 0x10FFFF)) &&
                 !(c >= 0xD800 && c <= 0xDFFF);
            if (!ok) {
                error(i, "invalid UTF-8: the browser shows U+FFFD here, the index keeps the bytes");
                ++reported;
                ++i;
                continue;
            }
            if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') || (c >= 0x7F && c <= 0x9F)) {
                error(i, "control character U+" + hex(c) + ": the browser may drop or replace it");
                ++reported;
            } else if ((c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE) {
                warning(i, "noncharacter U+" + hex(c));
                ++reported;
            }
            i += n;
        }
    }

    static auto hex(char32_t c) -> std::string
    {
        char buf[12];
        std::snprintf(buf, sizeof buf, "%04X", static_cast<unsigned>(c));
        return buf;
    }

    // Character references: only what the extractor decodes, always with ';'.
    auto check_references(size_t from, size_t to) -> void
    {
        for (size_t i = h_.find('&', from); i < to; i = h_.find('&', i + 1)) {
            size_t semi = h_.find(';', i + 1);
            size_t j = i + 1;
            bool numeric = j < to && h_[j] == '#';
            j += numeric;
            bool hexadecimal = numeric && j < to && (h_[j] == 'x' || h_[j] == 'X');
            j += hexadecimal;
            size_t name_start = j;
            while (j < to && (std::isalnum(static_cast<unsigned char>(h_[j]))))
                ++j;
            if (j == name_start) {
                error(i, "bare '&': write &amp;");
                continue;
            }
            if (j >= to || h_[j] != ';' || semi != j) {
                error(i, "character reference without ';': the browser may still decode it, the index does not");
                continue;
            }
            std::string_view name = h_.substr(name_start, j - name_start);
            if (numeric) {
                char32_t c = 0;
                bool valid = true;
                for (char ch : name) {
                    int v = ch >= '0' && ch <= '9' ? ch - '0'
                          : hexadecimal && ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                          : hexadecimal && ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                    valid = valid && v >= 0;
                    c = valid && c < 0x110000 ? c * (hexadecimal ? 16 : 10) + char32_t(v) : c;
                }
                if (!valid || c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || (c >= 0x80 && c <= 0x9F) ||
                    (c < 0x20 && c != '\t' && c != '\n' && c != '\f'))
                    error(i, "character reference &" + std::string(h_.substr(i + 1, j - i - 1)) +
                                 "; is invalid or remapped by the browser");
                continue;
            }
            bool known = std::any_of(std::begin(named_entities), std::end(named_entities), [&](const Entity& e) { return e.name == name; });
            if (!known)
                error(i, "&" + std::string(name) + "; is not decoded by the indexer: write the character as UTF-8");
        }
    }

    auto text(size_t from, size_t to) -> void
    {
        check_references(from, to);
        std::string_view t = h_.substr(from, to - from);
        size_t first = 0;
        while (first < t.size() && is_ws(t[first]))
            ++first;
        if (first == t.size()) {
            last_ws_ = indexed() ? true : last_ws_;
            return;
        }
        switch (mode_) {
        case Mode::initial: case Mode::before_head: case Mode::in_head: case Mode::after_head:
            error(from + first, "text before <body>: the browser starts the body here");
            mode_ = Mode::in_body;
            break;
        case Mode::after_body: case Mode::after_html:
            error(from + first, "text after </body>: the browser moves it into the body");
            break;
        default: break;
        }
        if (const Open* o = top(); o && (o->name == "table" || o->name == "tbody" || o->name == "thead" || o->name == "tfoot" || o->name == "tr"))
            error(from + first, "text directly inside <" + o->name + ">: the browser moves it in front of the table");
        if (const Open* o = top(); o && (o->name == "select" || o->name == "optgroup"))
            error(from + first, "text directly inside <" + o->name + ">: browsers parse select content differently");
        if (indexed()) {
            if (block_pending_ && !last_ws_ && first == 0 && indexed_bytes_ > 0)
                warning(from, "no whitespace between this block and the previous one: words run together in the DOM "
                              "text and in snippets (search still separates them)");
            indexed_bytes_ += t.size();
            last_ws_ = is_ws(t.back());
        }
        block_pending_ = false;
    }

    // At '<'. Returns false if it starts no markup.
    auto markup() -> bool
    {
        size_t at = pos_;
        size_t p = pos_ + 1;
        if (h_.compare(p, 3, "!--") == 0) {
            size_t end = h_.find("-->", p + 3);
            if (end == std::string_view::npos) {
                error(at, "comment is never closed");
                pos_ = h_.size();
                return true;
            }
            if (end == p + 3 || end == p + 2 || h_.substr(p + 3, end - p - 3).find("--!>") != std::string_view::npos ||
                h_.compare(p + 3, 1, ">") == 0)
                error(at, "malformed comment: the browser ends it at a different place");
            pos_ = end + 3;
            return true;
        }
        if (p < h_.size() && h_[p] == '!') {
            size_t end = h_.find('>', p);
            std::string inner = lower(h_.substr(p + 1, (end == std::string_view::npos ? h_.size() : end) - p - 1));
            if (inner.starts_with("doctype")) {
                std::string rest(inner.substr(7));
                rest.erase(std::remove_if(rest.begin(), rest.end(), is_ws), rest.end());
                if (mode_ != Mode::initial || seen_doctype_)
                    error(at, "<!DOCTYPE> must come first and only once");
                else if (rest != "html")
                    error(at, "use exactly <!DOCTYPE html>: other doctypes switch the browser to (limited-)quirks mode");
                seen_doctype_ = true;
            } else {
                error(at, "<!" + inner.substr(0, 10) + "…> is a bogus comment in HTML (CDATA works only in SVG/MathML)");
            }
            pos_ = end == std::string_view::npos ? h_.size() : end + 1;
            return true;
        }
        if (p < h_.size() && h_[p] == '?') {
            error(at, "<? … > is a bogus comment in HTML");
            size_t end = h_.find('>', p);
            pos_ = end == std::string_view::npos ? h_.size() : end + 1;
            return true;
        }
        bool closing = p < h_.size() && h_[p] == '/';
        p += closing;
        if (p >= h_.size() || !is_alpha(h_[p])) {
            error(at, closing ? "'</' not followed by a tag name" : "unescaped '<': write &lt;");
            if (!closing)
                return false;
            size_t end = h_.find('>', p);
            pos_ = end == std::string_view::npos ? h_.size() : end + 1;
            return true;
        }
        size_t name_end = p;
        while (name_end < h_.size() && !is_ws(h_[name_end]) && h_[name_end] != '/' && h_[name_end] != '>')
            ++name_end;
        std::string name = lower(h_.substr(p, name_end - p));
        Attributes attributes;
        bool self_closing = false;
        size_t issues_before = issues_.size();
        if (!attributes_of(name_end, attributes, self_closing))
            return true;
        bool foreign = top() && top()->foreign;
        if (!foreign && name != "svg" && name != "math" && name.find('-') == std::string::npos &&
            !is_one_of(name, html_elements) && !is_one_of(name, obsolete_elements)) {
            issues_.resize(std::min(issues_.size(), issues_before));
            error(at, "unknown element <" + std::string(h_.substr(p, name_end - p)) +
                          ">: if this is text, write &lt; (custom elements need a hyphen in the name)");
            reported_next_ = !closing;
            if (closing) {
                close_to(name);
                return true;
            }
        } else if (!foreign && is_one_of(name, obsolete_elements) && name != "listing" && name != "xmp" && name != "plaintext" &&
                   name != "image" && name != "frameset" && name != "frame" && name != "isindex" && name != "noembed" && name != "noframes") {
            warning(at, "obsolete element <" + name + ">");
        }
        if (!seen_doctype_ && mode_ == Mode::initial) {
            error(at, "missing <!DOCTYPE html>: the browser uses quirks mode");
            seen_doctype_ = true;
        }
        if (closing) {
            if (!attributes.empty() || self_closing)
                error(at, "end tag </" + name + "> with attributes or '/'");
            end_tag(name, at);
        } else {
            start_tag(name, attributes, self_closing, at);
        }
        return true;
    }

    auto attributes_of(size_t p, Attributes& attributes, bool& self_closing) -> bool
    {
        size_t tag_start = pos_;
        while (true) {
            while (p < h_.size() && is_ws(h_[p]))
                ++p;
            if (p >= h_.size()) {
                error(tag_start, "tag is never closed with '>'");
                pos_ = h_.size();
                return false;
            }
            if (h_[p] == '>') {
                pos_ = p + 1;
                return true;
            }
            if (h_[p] == '/') {
                if (p + 1 < h_.size() && h_[p + 1] == '>') {
                    self_closing = true;
                    pos_ = p + 2;
                    return true;
                }
                error(p, "stray '/' in tag");
                ++p;
                continue;
            }
            size_t n = p;
            while (n < h_.size() && !is_ws(h_[n]) && h_[n] != '/' && h_[n] != '>' && (h_[n] != '=' || n == p))
                ++n;
            std::string attr = lower(h_.substr(p, n - p));
            if (attr.find_first_of("\"'<=") != std::string::npos)
                error(p, "malformed attribute name '" + attr + "'");
            p = n;
            while (p < h_.size() && is_ws(h_[p]))
                ++p;
            std::string value;
            if (p < h_.size() && h_[p] == '=') {
                ++p;
                while (p < h_.size() && is_ws(h_[p]))
                    ++p;
                if (p < h_.size() && (h_[p] == '"' || h_[p] == '\'')) {
                    size_t end = h_.find(h_[p], p + 1);
                    if (end == std::string_view::npos) {
                        error(p, "attribute value is never closed");
                        pos_ = h_.size();
                        return false;
                    }
                    check_references(p + 1, end);
                    value = std::string(h_.substr(p + 1, end - p - 1));
                    p = end + 1;
                } else {
                    size_t end = p;
                    while (end < h_.size() && !is_ws(h_[end]) && h_[end] != '>')
                        ++end;
                    std::string_view v = h_.substr(p, end - p);
                    if (v.empty() || v.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") != std::string_view::npos)
                        error(p, "quote this attribute value");
                    value = std::string(v);
                    p = end;
                }
            }
            if (std::any_of(attributes.begin(), attributes.end(), [&](const auto& a) { return a.first == attr; }))
                error(n, "duplicate attribute '" + attr + "': the browser keeps the first");
            attributes.emplace_back(std::move(attr), std::move(value));
        }
    }

    static auto attribute(const Attributes& a, std::string_view key) -> const std::string*
    {
        for (const auto& [k, v] : a) {
            if (k == key)
                return &v;
        }
        return nullptr;
    }

    // Content of raw-text/RCDATA elements; pos_ moves past the end tag.
    auto raw(const std::string& name, size_t at, bool references) -> void
    {
        size_t start = pos_;
        for (size_t p = h_.find("</", start); p != std::string_view::npos; p = h_.find("</", p + 2)) {
            if (lower(h_.substr(p + 2, name.size())) != name)
                continue;
            char after = p + 2 + name.size() < h_.size() ? h_[p + 2 + name.size()] : '>';
            if (!is_ws(after) && after != '>' && after != '/')
                continue;
            if (references)
                check_references(start, p);
            if (name == "script" && h_.substr(start, p - start).find("<!--") != std::string_view::npos)
                error(at, "'<!--' inside <script> changes where the browser ends the script");
            size_t gt = h_.find('>', p);
            pos_ = gt == std::string_view::npos ? h_.size() : gt + 1;
            return;
        }
        error(at, "<" + name + "> is never closed: the browser treats the rest of the page as its content");
        pos_ = h_.size();
    }

    auto block_boundary() -> void
    {
        if (indexed())
            block_pending_ = true;
    }

    auto start_tag(const std::string& name, const Attributes& attributes, bool self_closing, size_t at) -> void
    {
        // Document structure: html, head, body explicitly, in this order.
        if (name == "html" || name == "head" || name == "body") {
            Mode expected = name == "html" ? Mode::initial : name == "head" ? Mode::before_head : Mode::after_head;
            if (mode_ != expected) {
                error(at, "<" + name + "> out of place or repeated: the browser merges or ignores it");
                return;
            }
            mode_ = name == "html" ? Mode::before_head : name == "head" ? Mode::in_head : Mode::in_body;
            if (name == "html") {
                const std::string* lang = attribute(attributes, "lang");
                lang_ = lang ? *lang : "";
                has_lang_ = lang != nullptr;
            }
            push(name, attributes, at);
            return;
        }
        switch (mode_) {
        case Mode::initial:
            error(at, "start the page with <html>");
            mode_ = Mode::in_body;
            break;
        case Mode::before_head:
            error(at, "missing <head>");
            mode_ = Mode::in_body;
            break;
        case Mode::after_head:
            error(at, "<" + name + "> between </head> and <body>");
            mode_ = Mode::in_body;
            break;
        case Mode::after_body: case Mode::after_html:
            error(at, "<" + name + "> after </body>: the browser moves it into the body");
            break;
        default: break;
        }
        if (mode_ == Mode::in_head && !is_one_of(name, head_elements)) {
            error(at, "<" + name + "> is not allowed in <head>: the browser ends the head here");
            mode_ = Mode::in_body;
        }
        if (name == "meta")
            meta(attributes, at);
        if (name == "plaintext") {
            error(at, "<plaintext> turns the rest of the page into text");
            plaintext_ = true;
            return;
        }
        if (name == "image" || name == "frameset" || name == "frame" || name == "isindex")
            error(at, "<" + name + "> is rewritten or ignored by the browser");
        if (name == "listing") {
            error(at, "<listing> is an obsolete HTML element that the parser treats like <pre> and that closes an open <p> "
                      "(a custom element needs a hyphen, e.g. <x-listing>)");
            reported_next_ = true;
        }

        // Implied end tags and elements the parser moves.
        if (mode_ == Mode::in_body) {
            const Open* o = top();
            bool foreign = o && o->foreign;
            if (foreign && is_one_of(name, foreign_breakout))
                error(at, "<" + name + "> inside SVG/MathML: the browser closes the SVG/MathML here");
            if (!foreign) {
                if (is_one_of(name, p_closers) && in_scope("p", {"button"})) {
                    if (!reported_next_)
                        error(at, "<" + name + "> inside <p>: the browser closes the <p> here");
                    close_to("p");
                    implicitly_closed_.push_back("p");   // the matching </p> later is a consequence, not a new error
                }
                if (name == "li" && in_scope("li", {"ol", "ul"})) {
                    if (!reported_next_)
                        error(at, "<li> inside an open <li>: the browser closes the first one");
                    close_to("li");
                }
                if ((name == "dt" || name == "dd") && (in_scope("dt", {"dl"}) || in_scope("dd", {"dl"}))) {
                    if (!reported_next_)
                        error(at, "<" + name + "> inside an open <dt>/<dd>: the browser closes it");
                    close_to(in_scope("dd", {"dl"}) ? "dd" : "dt");
                }
                o = top();
                if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && o && o->name.size() == 2 &&
                    o->name[0] == 'h' && o->name[1] >= '1' && o->name[1] <= '6') {
                    error(at, "heading inside a heading: the browser closes the outer one");
                    stack_.pop_back();
                }
                o = top();
                if (name == "a" && in_scope("a", {}))
                    error(at, "<a> inside <a>: the browser closes the outer link");
                if (name == "button" && in_scope("button", {}))
                    error(at, "<button> inside <button>: the browser closes the outer one");
                if (name == "form" && in_scope("form", {}))
                    error(at, "<form> inside <form> is ignored by the browser");
                if ((name == "option" || name == "optgroup") && o && (o->name == "option"))
                    error(at, "<" + name + "> inside <option>: the browser closes the option");
                if (o && (o->name == "select" || o->name == "optgroup") && name != "option" && name != "optgroup" &&
                    name != "hr" && name != "script" && name != "template")
                    error(at, "<" + name + "> inside <" + o->name + ">: browsers parse select content differently");
                if (o && o->name == "option" && name != "script")
                    error(at, "<" + name + "> inside <option>: browsers parse option content differently");
                static constexpr std::string_view table_children[] = {"caption", "colgroup", "col", "tbody", "thead", "tfoot", "tr", "td", "th", "script", "style", "template"};
                static constexpr std::string_view section_children[] = {"tr", "td", "th", "script", "style", "template"};
                static constexpr std::string_view row_children[] = {"td", "th", "script", "style", "template"};
                if (o && ((o->name == "table" && !is_one_of(name, table_children)) ||
                          ((o->name == "tbody" || o->name == "thead" || o->name == "tfoot") && !is_one_of(name, section_children)) ||
                          (o->name == "tr" && !is_one_of(name, row_children)) || (o->name == "colgroup" && name != "col" && name != "template")))
                    error(at, "<" + name + "> directly inside <" + o->name + ">: the browser moves it in front of the table");
            }
        }

        // Raw text and RCDATA elements.
        bool in_root_now = indexed();
        if (name == "script" || name == "style") {
            raw(name, at, false);
            return;
        }
        if (name == "title" || name == "textarea") {
            size_t content = pos_;
            raw(name, at, true);
            if (name == "title" && mode_ == Mode::in_head) {
                ++titles_;
                std::string_view t = h_.substr(content, h_.rfind("</", pos_) - content);
                if (std::all_of(t.begin(), t.end(), is_ws))
                    warning(at, "empty <title>: results show the path");
            }
            if (name == "textarea" && in_root_now)
                warning(at, "<textarea> text is indexed, and editing it moves the highlights");
            return;
        }
        if (is_one_of(name, not_in_root)) {
            if (in_root_now && mode_ == Mode::in_body)
                error(at, "<" + name + "> inside the indexed root: its content is raw text in the DOM; mark it data-fs-skip");
            raw(name, at, false);
            return;
        }

        bool foreign_parent = top() && top()->foreign;
        bool is_void = is_one_of(name, void_elements);
        if (self_closing && !is_void && !foreign_parent && name != "svg" && name != "math")
            error(at, "<" + name + "/> is not self-closing in HTML: the element stays open; write <" + name + "></" + name + ">");
        if (attribute(attributes, "data-fs-skip") && is_void)
            warning(at, "data-fs-skip on a void element has no effect");
        if (in_root_now && !attribute(attributes, "data-fs-skip")) {
            const std::string* style = attribute(attributes, "style");
            std::string css = style ? lower(*style) : "";
            css.erase(std::remove_if(css.begin(), css.end(), is_ws), css.end());
            if (attribute(attributes, "hidden") || css.find("display:none") != std::string::npos ||
                css.find("visibility:hidden") != std::string::npos)
                warning(at, "hidden content is indexed and highlighted invisibly; mark it data-fs-skip");
        }
        if (is_one_of(name, block_elements))
            block_boundary();
        if (is_void || self_closing)   // a non-void "<x/>" was reported above; not opening it avoids follow-up errors
            return;
        push(name, attributes, at);
    }

    auto push(const std::string& name, const Attributes& attributes, size_t at) -> void
    {
        const Open* parent = top();
        bool skip = (parent && parent->skip) || attribute(attributes, "data-fs-skip") || name == "template";
        bool is_root = !root_seen_ && name == root_;
        if (is_root) {
            root_seen_ = true;
            if (skip)
                warning(at, "the indexed root <" + name + "> is inside or carries data-fs-skip: nothing is indexed");
        } else if (name == "main") {
            warning(at, "second <main>: only the first one is indexed");
        }
        bool foreign = name == "svg" || name == "math" ||
                       (parent && parent->foreign && name != "foreignobject" && name != "desc" && !(name == "title" && parent->name == "svg"));
        stack_.push_back(Open{name, at, skip, (parent && parent->in_root) || is_root, foreign, reported_next_});
        reported_next_ = false;
    }

    auto end_tag(const std::string& name, size_t at) -> void
    {
        if (is_one_of(name, void_elements)) {
            error(at, "</" + name + "> for a void element: the browser " + (name == "br" ? "inserts a <br>" : "ignores it"));
            return;
        }
        if (is_one_of(name, block_elements))
            block_boundary();
        const Open* o = top();
        if (o && o->name == name) {
            stack_.pop_back();
        } else {
            bool open = std::any_of(stack_.begin(), stack_.end(), [&](const Open& e) { return e.name == name; });
            if (!open) {
                auto consequence = std::find(implicitly_closed_.begin(), implicitly_closed_.end(), name);
                if (consequence != implicitly_closed_.end())
                    implicitly_closed_.erase(consequence);
                else
                    error(at, "</" + name + "> without an open <" + name + ">" + (name == "p" ? ": the browser inserts an empty <p>" : ""));
                return;
            }
            bool already = false;   // elements left open by an earlier reported problem
            for (size_t i = stack_.size(); i-- > 0;) {
                already = already || stack_[i].reported;
                if (stack_[i].name == name)
                    break;
            }
            if (!already)
                error(at, "</" + name + "> while <" + (o ? o->name : "") + "> (line " + line_of(o ? o->at : 0) +
                              ") is still open: close elements in reverse order");
            close_to(name);
        }
        if (name == "head" && mode_ == Mode::in_head)
            mode_ = Mode::after_head;
        else if (name == "body")
            mode_ = Mode::after_body;
        else if (name == "html")
            mode_ = Mode::after_html;
    }

    // Pops up to and including the innermost open element with this name, as the parser does.
    auto close_to(std::string_view name) -> void
    {
        for (size_t i = stack_.size(); i-- > 0;) {
            if (stack_[i].name == name) {
                for (size_t k = i + 1; k < stack_.size(); ++k)   // their own end tags come later and are consequences
                    implicitly_closed_.push_back(stack_[k].name);
                stack_.resize(i);
                return;
            }
        }
    }

    auto meta(const Attributes& attributes, size_t at) -> void
    {
        const std::string* charset = attribute(attributes, "charset");
        const std::string* equiv = attribute(attributes, "http-equiv");
        const std::string* content = attribute(attributes, "content");
        std::string cs = charset ? lower(*charset) : "";
        if (equiv && lower(*equiv) == "content-type" && content) {
            std::string c = lower(*content);
            size_t p = c.find("charset=");
            cs = p == std::string::npos ? "" : c.substr(p + 8);
        }
        if (charset || (equiv && lower(*equiv) == "content-type")) {
            if (cs != "utf-8" && cs != "utf8")
                error(at, "the page must be UTF-8, not '" + cs + "'");
            else if (at >= 1024)
                error(at, "<meta charset> must be within the first 1024 bytes for the browser to see it");
            else
                charset_ok_ = true;
        }
        const std::string* name = attribute(attributes, "name");
        if (name && lower(*name) == "search-prior") {
            char* end = nullptr;
            float v = content ? std::strtof(content->c_str(), &end) : -1.0f;
            if (!content || end == content->c_str() || *end != '\0' || v < 0.0f || v > 100.0f)
                error(at, "search-prior must be a number from 0 to 100");
        }
        if (name && lower(*name) == "keywords" && (!content || content->find_first_not_of(" ,") == std::string::npos))
            warning(at, "empty keywords");
    }

    auto finish() -> void
    {
        if (plaintext_)
            return;
        for (const Open& o : stack_) {
            if (!o.reported)
                error(o.at, "<" + o.name + "> is never closed");
        }
        if (!seen_doctype_)
            error(0, "missing <!DOCTYPE html>: the browser uses quirks mode");
        if (!charset_ok_)
            error(0, "missing <meta charset=\"utf-8\"> in the first 1024 bytes: the browser may guess another encoding");
        if (titles_ == 0)
            warning(0, "no <title>: results show the path");
        if (!has_lang_)
            warning(0, "no <html lang>: the browser does not know the page language");
        else if (!language_.empty() && lower(lang_).substr(0, 2) != lower(language_).substr(0, 2))
            error(0, "<html lang=\"" + lang_ + "\"> does not match the index language '" + std::string(language_) + "'");
        if (root_ == "body")
            warning(0, "no <main>: the whole <body> is indexed; give MarkIt document.body as root");
        if (root_.empty())
            error(0, "no <main> and no <body>");
        if (indexed_bytes_ == 0 && !root_.empty())
            warning(0, "no indexable text");
    }

    std::string_view h_;
    std::string_view language_;
    std::string root_;
    std::vector<size_t> line_starts_;
    std::vector<Html_Issue> issues_;
    std::vector<Open> stack_;
    Mode mode_ = Mode::initial;
    size_t pos_ = 0;
    size_t indexed_bytes_ = 0;
    int titles_ = 0;
    std::string lang_;
    bool has_lang_ = false;
    bool seen_doctype_ = false;
    bool charset_ok_ = false;
    bool root_seen_ = false;
    bool block_pending_ = false;
    bool last_ws_ = true;
    bool plaintext_ = false;
    bool reported_next_ = false;
    std::vector<std::string> implicitly_closed_;   // closed by the parser before their end tag
};

// ---------------------------------------------------------------------------------------------
// Index writer

auto put32(std::string& out, uint32_t v) -> void { out.append(reinterpret_cast<const char*>(&v), 4); }
auto put64(std::string& out, uint64_t v) -> void { out.append(reinterpret_cast<const char*>(&v), 8); }

auto utf8_path(const std::string& s) -> std::filesystem::path
{
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// The directory the relative input paths are resolved against. POSIX: an O_DIRECTORY descriptor
// for openat(), so all files come from the same directory even if the path is renamed meanwhile.
class Base_Directory {
public:
    explicit Base_Directory(const std::string& path);
    ~Base_Directory();
    Base_Directory(const Base_Directory&) = delete;
    auto operator=(const Base_Directory&) -> Base_Directory& = delete;

    auto ok() const -> bool { return ok_; }
    auto read(const std::string& path, std::string& out) const -> bool;

private:
    std::string path_;
    bool ok_ = true;
#ifndef _WIN32
    int fd_ = AT_FDCWD;
#endif
};

Base_Directory::Base_Directory(const std::string& path) : path_(path)
{
#ifndef _WIN32
    if (!path.empty()) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ok_ = fd_ >= 0;
    }
#else
    std::error_code ec;
    ok_ = path.empty() || std::filesystem::is_directory(utf8_path(path), ec);
#endif
}

Base_Directory::~Base_Directory()
{
#ifndef _WIN32
    if (fd_ >= 0)
        ::close(fd_);
#endif
}

// Relative paths are opened relative to the base directory, absolute paths as they are.
auto Base_Directory::read(const std::string& path, std::string& out) const -> bool
{
    out.clear();
#ifndef _WIN32
    int fd = ::openat(fd_, path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
    out.resize(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < out.size()) {
        ssize_t n = ::read(fd, out.data() + got, out.size() - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += static_cast<size_t>(n);
    }
    bool complete = got == out.size();
    ::close(fd);
    return complete;
#else
    std::filesystem::path file = utf8_path(path);
    if (file.is_relative() && !path_.empty())
        file = utf8_path(path_) / file;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
#endif
}

auto set_error(std::string* error, std::string message) -> bool
{
    if (error)
        *error = std::move(message);
    return false;
}

}  // namespace

auto build_index(const std::vector<std::string>& files, const std::string& base_directory, std::string_view language,
                 const std::string& output_path, Build_Report* report, std::string* error) -> bool
{
    Build_Report local;
    Build_Report& r = report ? *report : local;
    r = Build_Report{};
    if (language.empty() || language.size() > 15)
        return set_error(error, "language must be 1 to 15 characters");
    if (files.size() > std::numeric_limits<uint32_t>::max())
        return set_error(error, "too many files");

    Base_Directory base(base_directory);
    if (!base.ok())
        return set_error(error, "cannot open base directory " + base_directory);

    detail::Analyzer analyzer(language);
    std::string strings, records, text;
    std::string table;
    std::vector<detail::Token> tokens, segment;
    std::string html;
    auto add_string = [&strings](std::string_view s) -> std::pair<uint32_t, uint32_t> {
        uint32_t off = static_cast<uint32_t>(strings.size());
        strings += s;
        return {off, static_cast<uint32_t>(s.size())};
    };

    for (const std::string& path : files) {
        if (!base.read(path, html))
            return set_error(error, "cannot read " + path + (base_directory.empty() ? "" : " in " + base_directory));
        Page_Text page = Html_Extractor::extract(html, r.warnings, path);
        if (!page.lang.empty() && lower(page.lang).substr(0, 2) != lower(language).substr(0, 2))
            r.warnings.push_back(path + ": <html lang=\"" + page.lang + "\"> differs from the index language");
        if (page.text.empty())
            r.warnings.push_back(path + ": no indexable text");

        // Records: [advance][weight:2|length:6][stem]; fillers when a gap exceeds 255 bytes.
        tokens.clear();
        page.blocks.push_back(static_cast<uint32_t>(page.text.size()));
        for (uint32_t a = 0; uint32_t b : page.blocks) {   // analyze block by block
            if (b > a) {
                analyzer.analyze(std::string_view(page.text).substr(a, b - a), segment);
                for (detail::Token& t : segment) {
                    t.start += a;
                    tokens.push_back(std::move(t));
                }
            }
            a = std::max(a, b);
        }
        uint32_t record_start = static_cast<uint32_t>(records.size());
        uint32_t previous = 0, words = 0;
        for (const detail::Token& t : tokens) {
            uint32_t advance = t.start - previous;
            for (; advance > 255; advance -= 255) {
                records += static_cast<char>(255);
                records += static_cast<char>(detail::record_filler);
            }
            records += static_cast<char>(advance);
            uint8_t weight = t.term.empty() ? 0 : page.weight[t.start];
            records += static_cast<char>((weight << 6) | t.term.size());
            records += t.term;
            previous = t.start;
            ++words;
        }

        std::string keywords;
        for (const std::string& k : page.keywords) {
            std::string folded = analyzer.fold(k);
            if (folded.empty())
                continue;
            if (!keywords.empty())
                keywords += '\n';
            keywords += folded;
        }
        auto [path_off, path_len] = add_string(path);
        auto [title_off, title_len] = add_string(page.title);
        auto [ftitle_off, ftitle_len] = add_string(analyzer.fold(page.title));
        auto [terms_off, terms_len] = add_string(keywords);
        std::array<uint32_t, detail::page_fields> entry{};
        entry[detail::path_off] = path_off;
        entry[detail::path_len] = path_len;
        entry[detail::title_off] = title_off;
        entry[detail::title_len] = title_len;
        entry[detail::ftitle_off] = ftitle_off;
        entry[detail::ftitle_len] = ftitle_len;
        entry[detail::terms_off] = terms_off;
        entry[detail::terms_len] = terms_len;
        entry[detail::records_off] = record_start;
        entry[detail::records_len] = static_cast<uint32_t>(records.size()) - record_start;
        entry[detail::word_count] = words;
        entry[detail::text_off] = static_cast<uint32_t>(text.size());
        entry[detail::text_len] = static_cast<uint32_t>(page.text.size());
        entry[detail::prior_milli] = static_cast<uint32_t>(page.prior * 1000.0f + 0.5f);
        for (uint32_t v : entry)
            put32(table, v);
        text += page.text;
        r.words += words;
        if (strings.size() > UINT32_MAX || records.size() > UINT32_MAX || text.size() > UINT32_MAX)
            return set_error(error, "index section exceeds 4 GB; split the input");
    }

    std::string header;
    header.append(detail::file_magic, 8);
    put32(header, format_version);
    put32(header, tokenizer_version);
    char lang[16] = {};
    std::memcpy(lang, language.data(), language.size());
    header.append(lang, 16);
    put32(header, static_cast<uint32_t>(files.size()));
    put32(header, 0);
    uint64_t pages_offset = detail::header_size;
    uint64_t strings_offset = pages_offset + table.size();
    uint64_t records_offset = strings_offset + strings.size();
    uint64_t text_offset = records_offset + records.size();
    put64(header, pages_offset);
    put64(header, strings_offset);
    put64(header, strings.size());
    put64(header, records_offset);
    put64(header, records.size());
    put64(header, text_offset);
    put64(header, text.size());

    std::string temporary = output_path + ".tmp";
    {
        std::ofstream out(utf8_path(temporary), std::ios::binary | std::ios::trunc);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(table.data(), static_cast<std::streamsize>(table.size()));
        out.write(strings.data(), static_cast<std::streamsize>(strings.size()));
        out.write(records.data(), static_cast<std::streamsize>(records.size()));
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out.flush())
            return set_error(error, "cannot write " + temporary);
    }
    std::error_code ec;
    std::filesystem::rename(utf8_path(temporary), utf8_path(output_path), ec);
    if (ec)
        return set_error(error, "cannot rename " + temporary + ": " + ec.message());

    r.pages = static_cast<uint32_t>(files.size());
    r.text_bytes = text.size();
    r.record_bytes = records.size();
    r.file_bytes = header.size() + table.size() + strings.size() + records.size() + text.size();
    return true;
}

auto validate_page(std::string_view html, std::string_view language) -> std::vector<Html_Issue>
{
    return Html_Validator(html, language, Html_Extractor::root_of(html)).run();
}

}  // namespace page_index
