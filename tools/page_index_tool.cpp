// Test and benchmark tool for page_index (not part of the library).
//   page_index_tool build <index> <language> [--base <dir>] <file>...   (or @list.txt)
//   page_index_tool query <index> <query>                   results as JSON
//   page_index_tool bench <index> <queries.txt> [repeat]    timing per query
//   page_index_tool texts <index>                           page paths and texts as JSON
//   page_index_tool check [--lang xx] <file>...             strict prepared-HTML check, exit 1 on errors
#include "../page_index_build.h"
#include "../page_index_query.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace {

auto json(std::string_view s) -> std::string
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

auto print_results(const std::vector<page_index::Page_Hit>& hits, double ms) -> void
{
    std::printf("{\"ms\": %.3f, \"results\": [\n", ms);
    for (size_t i = 0; i < hits.size(); ++i) {
        const auto& h = hits[i];
        std::printf("  {\"page\": %u, \"path\": %s, \"title\": %s, \"tier\": %d, \"score\": %.3f,\n   \"summary\": %s,\n   \"snippet\": %s,\n   \"contexts\": [",
                    h.page, json(h.path).c_str(), json(h.title).c_str(), h.tier, h.score, json(h.summary).c_str(), json(h.snippet_html).c_str());
        for (size_t c = 0; c < h.contexts_html.size(); ++c)
            std::printf("%s%s", c ? ", " : "", json(h.contexts_html[c]).c_str());
        std::printf("],\n   \"ranges\": [");
        for (size_t r = 0; r < h.ranges.size(); ++r)
            std::printf("%s[%u,%u]", r ? "," : "", h.ranges[r].start, h.ranges[r].length);
        std::printf("]}%s\n", i + 1 < hits.size() ? "," : "");
    }
    std::printf("]}\n");
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: see source\n");
        return 2;
    }
    std::string command = argv[1], index_path = argv[2];
    if (command == "check") {
        std::string language;
        int first = 2;
        if (std::string(argv[2]) == "--lang" && argc >= 4) {
            language = argv[3];
            first = 4;
        }
        int errors = 0, warnings = 0, clean = 0;
        for (int i = first; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary);
            std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!in && !in.eof()) {
                std::fprintf(stderr, "%s: cannot read\n", argv[i]);
                ++errors;
                continue;
            }
            auto issues = page_index::validate_page(html, language);
            bool page_error = false;
            for (const auto& is : issues) {
                bool err = is.level == page_index::Issue_Level::error;
                std::printf("%s:%u:%u: %s: %s\n", argv[i], is.line, is.column, err ? "error" : "warning", is.message.c_str());
                errors += err;
                warnings += !err;
                page_error = page_error || err;
            }
            clean += !page_error;
        }
        std::fprintf(stderr, "%d of %d pages without errors; %d errors, %d warnings\n", clean, argc - first, errors, warnings);
        return errors ? 1 : 0;
    }
    if (command == "build" && argc >= 5) {
        std::vector<std::string> files;
        std::string base;
        int first = 4;
        if (argc >= 7 && std::string(argv[4]) == "--base") {
            base = argv[5];
            first = 6;
        }
        for (int i = first; i < argc; ++i) {
            if (argv[i][0] == '@') {
                std::ifstream list(argv[i] + 1);
                for (std::string line; std::getline(list, line);)
                    if (!line.empty())
                        files.push_back(line);
            } else {
                files.emplace_back(argv[i]);
            }
        }
        page_index::Build_Report report;
        std::string error;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = page_index::build_index(files, base, argv[3], index_path, &report, &error);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (const auto& w : report.warnings)
            std::fprintf(stderr, "warning: %s\n", w.c_str());
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        std::printf("pages %u, words %llu, text %llu B, records %llu B, file %llu B, %.1f ms\n", report.pages,
                    (unsigned long long)report.words, (unsigned long long)report.text_bytes,
                    (unsigned long long)report.record_bytes, (unsigned long long)report.file_bytes, ms);
        return 0;
    }
    std::string error;
    auto index = page_index::Page_Index::open(index_path, &error);
    if (!index) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (command == "query" && argc >= 4) {
        index->search(argv[3]);   // warm up the thread's analyzer
        auto t0 = std::chrono::steady_clock::now();
        page_index::Search_Options options;
        options.context_lines = 3;
        auto hits = index->search(argv[3], options);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        print_results(hits, ms);
        return 0;
    }
    if (command == "bench" && argc >= 4) {
        int repeat = argc >= 5 ? std::atoi(argv[4]) : 200;
        std::ifstream list(argv[3]);
        for (std::string q; std::getline(list, q);) {
            if (q.empty())
                continue;
            size_t n = index->search(q).size();
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < repeat; ++i)
                index->search(q);
            double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / repeat;
            std::printf("%8.1f us  %3zu results  %s\n", us, n, q.c_str());
        }
        return 0;
    }
    if (command == "texts") {
        std::printf("[\n");
        for (uint32_t n = 0; n < index->page_count(); ++n)
            std::printf("{\"path\": %s, \"text\": %s}%s\n", json(index->page_path(n)).c_str(), json(index->page_text(n)).c_str(),
                        n + 1 < index->page_count() ? "," : "");
        std::printf("]\n");
        return 0;
    }
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
