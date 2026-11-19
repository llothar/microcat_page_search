# page_index

Small full-text search for help pages and small static websites: one C++20 library made of two header and
two source files, one pair to build an index and one to query it. Searching runs in the app
or behind FastCGI; highlighting in the page is done by MarkIt (or mark.js) with the exact
ranges the query returns.

| File | Purpose |
| --- | --- |
| `page_index_build.h/.cpp` | `build_index()`: prepared HTML files of one language → one index file. `validate_page()`: strict check that a page works with the index |
| `page_index_query.h/.cpp` | `Page_Index`: maps the index file, `search()` returns pages, UTF-16 ranges and an HTML snippet. Also the analyzer both sides share |
| `tools/page_index_tool.cpp` | Test and benchmark tool (not part of the library) |
| `test/` | Corpus script, browser verification, validator soundness test, sample pages |

Dependencies: ICU's common library (`icu-uc`, tested with 78.3) and Snowball `libstemmer_c`
(`libstemmer`, tested with 3.2.0), both found with pkg-config.

License: MIT (see `LICENSE`).

## Building and installing

```sh
PKG_CONFIG_PATH=<icu>/lib/pkgconfig:<libstemmer>/lib/pkgconfig \
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<prefix> [-DPAGE_INDEX_TOOLS=OFF]
cmake --build build
cmake --install build
```

Installs the static library (`lib/libpage_index.a`, on Windows `lib/page_index.lib`), the two
headers into `include/page_index/` and `lib/pkgconfig/page_index.pc`, which requires `icu-uc` and
`libstemmer`. Consumers include `<page_index/page_index_query.h>` and
`<page_index/page_index_build.h>`. `page_index_tool` (on by default) goes into `bin/`.

The index and every program that searches it must use the same ICU version and data: word
boundaries come from ICU's rules and dictionaries, and a different ICU can split words
differently than the index did. ICU needs only this part of its data: the word break rules and
dictionaries (`brkitr/word*.brk`, `brkitr/*.dict`, `brkitr/*.res`) and `nfkc_cf.nrm`.

Releases: tag `v<version>` and attach the archive `microcat_page_search-<version>.tar.gz` (top
directory `microcat_page_search-<version>/`) to the GitHub release; package builds pin its SHA-256.

## Usage

```cpp
// Build, e.g. in the help build or the website deploy step
page_index::Build_Report report;
std::string error;
// files relative to the site root, e.g. "help/printing.html": opened with openat() in the
// base directory and stored as given, so they are the URLs search() returns
if (!page_index::build_index(files, "/srv/site", "en", "site.en.idx", &report, &error))
    fail(error);
for (const auto& w : report.warnings) log(w);       // read these: they point at offset problems

// Query, e.g. in the FastCGI worker: open once, search per request (thread-safe)
auto index = page_index::Page_Index::open("site.en.idx", &error);
for (const page_index::Page_Hit& hit : index->search(query_text)) {
    // hit.path, hit.title, hit.snippet_html (escaped, <mark> around matches)
    // hit.ranges: {start, length} in UTF-16 units → put into the result link
}
```

In the result page:

```js
import { markit } from "./third_party/markit/index.js";   // @markitjs/core, vendored
// link: page.html#fs-hl=1124.7,3045.10
const ranges = location.hash.slice(7).split(",").map(r => {
  const [start, length] = r.split(".").map(Number);
  return { start, length };
});
markit(document.querySelector("main")).markRanges(ranges, {
  exclude: ["[data-fs-skip]", "script", "style"],   // must match what the build skips
});
```

Checking pages before they are indexed (also `page_index_tool check --lang en pages/*.html`,
which prints `file:line:column: error: …` and exits with 1 on errors):

```cpp
for (const page_index::Html_Issue& issue : page_index::validate_page(html, "en"))
    report(issue.level, issue.line, issue.column, issue.message);
```

Rebuilding: `build_index()` writes `<output>.tmp` and renames it over the old file. Workers that
still map the old file keep a valid mapping; reopen when the file's inode or mtime changes.

## Prepared HTML contract

The index stores, per page, exactly the text the browser's DOM holds under the root element,
so the ranges are valid for `markRanges()` without any rules on the JavaScript side.

- **Root:** the first `<main>`, otherwise `<body>`.
- **Skipped:** elements with a `data-fs-skip` attribute (whole subtree), `script`, `style`,
  `template`. Give MarkIt the same `exclude` list.
- **Entities:** write text as UTF-8. Decoded: `&amp; &lt; &gt; &quot; &apos; &nbsp;`, numeric
  references and about 40 common names (`&copy;`, `&mdash;`, `&hellip;` …). Any other name
  gives a build warning, because the browser would decode it and the offsets on that page
  would shift. Always end entities with `;`.
- **Block boundaries:** put whitespace (a newline) between block elements. `</h1><p>` without it
  makes "TitleFirst" one string in the DOM. The build still splits words there, so search and
  highlighting work, but snippets show the words glued, and the build warns.
- **Parser details handled:** CR/LF → LF, the newline right after `<pre>` dropped, raw-text
  elements. Not handled (keep out of the root or mark `data-fs-skip`): `noscript`, `iframe`
  content, mis-nested tags, text directly inside `<table>`.
- **Weights:** `h1`–`h6` heading, `figcaption`/`caption` caption, `em strong b i mark` emphasis.
- **Page data:** `<title>`, `<meta name="keywords" content="a, b">` (index terms),
  `<meta name="search-prior" content="0.5">` (ranking factor, default 1).

## Validator

`validate_page()` accepts only pages whose DOM the indexer reproduces exactly. **Errors** mean
the browser's DOM may differ from what the indexer reads, or the page is parsed in a way the
indexer does not model. **Warnings** mean the page works, but search or snippets are worse.

Errors:

- Encoding: invalid UTF-8, control characters, missing `<meta charset="utf-8">` in the first
  1024 bytes, a charset other than UTF-8.
- Document: missing or non-standard `<!DOCTYPE html>` (quirks mode), `html`/`head`/`body`
  missing, out of order or repeated, elements or text that the parser moves out of `<head>` or
  into the body after `</body>`.
- Nesting: every non-void element closed explicitly and in reverse order. Start tags that make
  the parser close elements (`<div>` or `<table>` inside `<p>`, `<li>` in `<li>`, heading in
  heading, `<a>` in `<a>`, nested `<button>`/`<form>`), `<x/>` on non-void elements, end tags for
  void elements (`</br>`).
- Elements the parser moves or rewrites: text or non-table elements directly inside
  `table`/`tbody`/`tr` (foster parenting), HTML elements inside SVG/MathML, non-option content in
  `<select>`/`<option>`, `<image>`, `<frameset>`, `<plaintext>`, the obsolete `<listing>`.
- Unknown element names without a hyphen, which are usually unescaped `<` in text (`Vec<T>`).
- Raw-text elements in the indexed root (`noscript`, `iframe`, `xmp`, `noembed`, `noframes`),
  `<!--` inside `<script>`, unclosed `script`/`style`/`title`/`textarea`.
- Character references: bare `&`, references without `;`, names the indexer does not decode,
  invalid or remapped numbers (`&#128;`).
- Syntax: unescaped `<`, malformed or unclosed comments, CDATA and `<?…>` in HTML, unquoted
  attribute values with special characters, duplicate attributes, unclosed tags.
- `<html lang>` not matching the index language, an invalid `search-prior`.

Warnings: no whitespace between adjacent blocks, hidden content (`hidden`, `display:none`)
not marked `data-fs-skip`, indexed `<textarea>`, no `<main>`, a second `<main>`, missing or
empty `<title>`, no `<html lang>`, empty keywords, obsolete elements, noncharacters, no
indexable text.

After an error, the validator recovers the way the browser does and does not report
follow-up errors (for example the stray `</p>` that follows a `<div>` inside `<p>`).

Soundness test (`test/fuzz_pages.py`, `test/fuzz_compare.cjs`): 1,000 random pages mixing
valid and invalid constructs, compared with headless Chrome. Every page the validator
accepted (390) has exactly the DOM text the indexer reads. All 141 pages whose text really
differed from the DOM were rejected. The test found one indexer bug, now fixed: the parser
moves whitespace after `</body>` and `</html>` into the body. The validator and indexer also
ran under AddressSanitizer and UBSan on 400 truncated and corrupted pages without findings.
Checking the 112 Rust book pages (1.5 MB of HTML) takes 35 ms.

## Query syntax and ranking

Words must all occur (AND). If none of the pages has all of them, pages with some of them are
returned with a penalty. `"quoted phrases"` must occur as an exact sequence; stop words keep
their position. At most 31 distinct terms.

1. **Tier** from title and index terms (folded words, not stemmed): 3 exact, 2 starts with the
   query, 1 all query words present. Any tier ranks above any full-text-only result.
2. **BM25** per term with weighted term frequency: body 1, emphasis 2, caption 3, heading 5;
   match quality: exact stem 1.0, stem starts with the term 0.7 (terms ≥ 3 bytes), term inside a
   longer stem 0.3 (terms ≥ 4 bytes; German compounds, identifiers).
3. **Proximity:** × (1 + matched terms / smallest window holding all of them).
4. **Prior** from `search-prior`.

## Analyzer

Shared by build and query (`detail::Analyzer`): ICU word boundaries (dictionary segmentation
for Chinese, Japanese, Thai), NFKC case folding, Snowball stemmer for the index language,
Latin accents removed after stemming. Words with `_`, `.`, digits or inner capitals are not
stemmed. English: possessive `'s` removed, Lucene's 33 stop words (kept as placeholders).
Other languages: no stop words yet.

## Index file format (version 1)

Little-endian; uncompressed so it can be memory-mapped.

```
header 96 bytes   magic "PGINDEX1", format version, tokenizer version, language[16],
                  page count, offsets and sizes of the four sections
page table        per page 14 × u32: path, title, folded title, folded index terms (offset,
                  length into strings), records offset/length, word count, text offset/length,
                  prior × 1000
strings           paths, titles, folded titles, index terms (one per line)
records           per page: [advance u8][weight:2 | stem length:6][stem bytes]
                  advance = UTF-8 bytes from the previous record's start
                  length 0, weight 0 = stop word;  length 0, weight 3 = filler (advance only)
text              per page: the DOM text, UTF-8
```

A word's end is not stored: it is found with ICU's word break in the page text and capped
at the next record's start.

## Measurements

Rust book (112 pages, 1.0 MB text, 171,218 words), one core of a 2-core VM:

| | |
| --- | --: |
| Build | 175 ms |
| Index file | 1.93 MB (records 0.91 MB, text 1.0 MB) |
| Query, rare word / no match | 0.13 ms |
| Query, common words (20 results with snippets) | 0.8–2.2 ms |

Every query walks the records of every page that contains the query's bytes, so query time
grows linearly with the text: about 1–2 ms per MB for common words.

Verification (`test/verify_offsets.cjs`, headless Chrome, MarkIt 1.0.4 with the CSS Custom
Highlight API): 111 of 112 pages produce exactly the DOM text, and 1,328 of 1,336 result
ranges select exactly the indexed word. The one failing page uses `&vert;`, which the build
reported as a warning. The library was also run under AddressSanitizer and UBSan, and damaged
or truncated index files are rejected by `open()`.

## Not done yet

- Identifier parts (`queue_draw` inside `gtk_widget_queue_draw`) are found only through infix
  matching, not indexed as separate words.
- Index terms point to pages, not to anchors.
- Phrase queries return all matched words as ranges, not only the phrase occurrences.
- Stop-word lists for languages other than English.
- The Windows memory-mapping code compiles in principle but has not been built or run.
