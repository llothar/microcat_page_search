#!/usr/bin/env python3
# Rust book Markdown -> prepared HTML pages (test corpus). Usage: make_corpus.py <book/src> <outdir>
import sys, os, re, html, glob, markdown
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
for path in sorted(glob.glob(os.path.join(src, "*.md"))):
    md = open(path, encoding="utf-8").read()
    md = re.sub(r"\{\{#\w+[^}]*\}\}", "", md)                      # mdBook includes
    m = re.search(r"^#{1,6}\s+(.+)$", md, re.M)
    title = m.group(1).strip() if m else os.path.basename(path)
    body = markdown.markdown(md, extensions=["fenced_code", "tables"])
    name = os.path.splitext(os.path.basename(path))[0]
    keywords = "ownership, borrowing" if name.startswith("ch04") else ""
    page = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>{html.escape(title)}</title>
<meta name="keywords" content="{keywords}"></head>
<body><nav data-fs-skip=""><a href="index.html">Contents</a> · <a href="#">Next</a></nav>
<main>{body}</main>
<footer data-fs-skip="">Rust book test page</footer><script>var x = "<b>not text</b>";</script>
</body></html>
"""
    open(os.path.join(out, name + ".html"), "w", encoding="utf-8").write(page)
