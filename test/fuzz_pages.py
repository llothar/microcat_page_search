#!/usr/bin/env python3
# Random prepared-HTML pages with valid and invalid constructs. Usage: fuzz_pages.py <outdir> <count> <seed>
import os, sys, random
out, count, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rnd = random.Random(seed)
os.makedirs(out, exist_ok=True)
words = ["printer", "Drucker", "système", "naïve", "café", "プリンター", "打印机", "😀", "𝔘nicode", "AT", "x", "gtk_widget_queue_draw", "über"]
def w(n=4): return " ".join(rnd.choice(words) for _ in range(n))
valid = [
    lambda: f"<p>{w()} &amp; {w(2)} &lt;tag&gt; &copy; &#x1F600; &#228;&nbsp;{w(1)}</p>\n",
    lambda: f"<p>{w()} <em>{w(2)}</em> and <strong>{w(1)}</strong>.</p>\n",
    lambda: f"<h2>{w(3)}</h2>\n",
    lambda: f"<pre>\n{w(3)}\n  indented</pre>\n",
    lambda: f"<pre>{w(2)}</pre>\n",
    lambda: f"<ul>\n<li>{w(2)}</li>\n<li><code>{w(1)}</code></li>\n</ul>\n",
    lambda: f"<table>\n<tr><th>{w(1)}</th><td>{w(2)}</td></tr>\n</table>\n",
    lambda: f"<table><tbody><tr><td>{w(1)}</td></tr></tbody></table>\n",
    lambda: f"<figure><img src=\"a.png\" alt=\"{w(1)}\"><figcaption>{w(2)}</figcaption></figure>\n",
    lambda: f"<div data-fs-skip=\"\"><p>{w(2)}</p></div>\n",
    lambda: f"<svg width=\"10\" height=\"10\"><text x=\"0\" y=\"9\">{w(1)}</text><rect width=\"1\" height=\"1\"/></svg>\n",
    lambda: f"<p>{w(2)}<br>{w(2)}</p>\n",
    lambda: f"<!-- comment with <main> and <p> inside -->\n",
    lambda: f"<p>{w(2)}</p>\r\n<p>{w(2)}\r{w(1)}</p>\r\n",
    lambda: f"<textarea>\n{w(2)}</textarea>\n",
    lambda: f"<select><option>{w(1)}</option><option>{w(1)}</option></select>\n",
    lambda: f"<x-note class=\"n\">{w(2)}</x-note>\n",
    lambda: f"<template><p>{w(2)}</p></template>\n",
    lambda: f"<dl><dt>{w(1)}</dt><dd>{w(2)}</dd></dl>\n",
    lambda: f"<blockquote><p>{w(2)}</p></blockquote>\n",
    lambda: f"<p>a b {w(1)}</p>\n",
    lambda: f"<script>var s = \"</p><p>\" + 1 < 2;</script>\n",
    lambda: f"<details><summary>{w(1)}</summary><p>{w(2)}</p></details>\n",
    lambda: f"<p><a href=\"x.html?a=1&amp;b=2\">{w(2)}</a></p>\n",
]
invalid = [
    lambda: f"<p>{w(2)}<div>{w(2)}</div></p>\n",
    lambda: f"<p>{w(2)}<p>{w(2)}\n",
    lambda: f"<p>{w(1)} &vert; {w(1)}</p>\n",
    lambda: f"<p>&copy 2026 {w(1)}</p>\n",
    lambda: f"<p>a < b {w(1)}</p>\n",
    lambda: f"<p>AT&T {w(1)}</p>\n",
    lambda: f"<div/>{w(2)}\n",
    lambda: f"<table>oops {w(1)}<tr><td>{w(1)}</td></tr></table>\n",
    lambda: f"<table><div>{w(1)}</div><tr><td>{w(1)}</td></tr></table>\n",
    lambda: f"<noscript>{w(2)}</noscript>\n",
    lambda: f"<p><b><i>{w(1)}</b></i></p>\n",
    lambda: f"<p>{w(1)}</br>{w(1)}</p>\n",
    lambda: f"<listing>\n{w(2)}</listing>\n",
    lambda: f"<p><a href=\"#\">{w(1)} <a href=\"#\">{w(1)}</a></a></p>\n",
    lambda: f"<ul><li>{w(1)}<li>{w(1)}</ul>\n",
    lambda: f"<h1><h2>{w(1)}</h2></h1>\n",
    lambda: f"<script>/* <!-- */ var a = 1;</script>\n<p>{w(1)}</p>\n",
    lambda: f"<p>{w(1)} &#128; {w(1)}</p>\n",
    lambda: f"<p>{w(1)} \x01 {w(1)}</p>\n",
    lambda: f"<p>{w(1)} BAD_UTF8 {w(1)}</p>\n",
    lambda: f"<p>Vec<T> {w(1)}</p>\n",
    lambda: f"<p data-x=\"1\" data-x=\"2\">{w(1)}</p>\n",
    lambda: f"<p>{w(1)}</p>\n<p data-fs-skip=\"\"><div>{w(1)}</div></p>\n",
    lambda: f"<select><b>{w(1)}</b><option>{w(1)}</option></select>\n",
    lambda: f"<svg><p>{w(1)}</p></svg>\n",
    lambda: f"<p hidden>{w(1)}</p>\n",
]
for i in range(count):
    bad = rnd.random() < 0.6
    parts = [rnd.choice(valid)() for _ in range(rnd.randint(3, 7))]
    if bad:
        for _ in range(rnd.randint(1, 2)):
            parts.insert(rnd.randrange(len(parts) + 1), rnd.choice(invalid)())
    charset = '<meta charset="utf-8">' if rnd.random() > 0.08 else ''
    use_main = rnd.random() > 0.15
    body = "".join(parts)
    main = f"<main>\n{body}</main>\n" if use_main else body
    page = (f"<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n{charset}\n<title>{w(2)}</title>\n</head>\n"
            f"<body>\n<nav data-fs-skip=\"\"><a href=\"index.html\">Home</a></nav>\n{main}"
            f"<footer data-fs-skip=\"\">footer</footer>\n</body>\n</html>\n")
    data = page.encode("utf-8").replace(b"BAD_UTF8", b"\xff\xc3(")
    open(os.path.join(out, f"page{i:03d}.html"), "wb").write(data)
