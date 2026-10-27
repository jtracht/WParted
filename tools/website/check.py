#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
"""Checks the generated website (CI gate for WP-090).

Per page: well-formed nesting, exactly one <h1>, <title> 10-65 characters,
meta description 50-170 characters, lang attribute, valid JSON-LD, no external
scripts or stylesheets, all relative links resolve. With absolute URLs
(--base-url build): canonical + hreflang for every language + x-default, and
sitemap.xml listing every page. Also requires robots.txt and llms.txt.

Usage: python tools/website/check.py build/site [--expect-base]
"""
import json
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlparse

VOID = {"meta", "link", "br", "img", "input", "hr", "source", "wbr"}


class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.errors = [], []
        self.h1 = 0
        self.title = ""
        self.in_title = self.in_ld = False
        self.ld = []
        self.meta, self.links, self.hrefs, self.scripts = {}, [], [], []
        self.lang = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "html":
            self.lang = a.get("lang")
        if tag not in VOID:
            self.stack.append(tag)
        if tag == "h1":
            self.h1 += 1
        elif tag == "title":
            self.in_title = True
        elif tag == "meta" and "name" in a:
            self.meta[a["name"]] = a.get("content", "")
        elif tag == "link":
            self.links.append(a)
        elif tag == "a" and "href" in a:
            self.hrefs.append(a["href"])
        elif tag == "script":
            self.scripts.append(a)
            self.in_ld = a.get("type") == "application/ld+json"
            if self.in_ld:
                self.ld.append("")

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.errors.append(f"unexpected </{tag}> (open: {self.stack[-3:]})")
            if tag in self.stack:
                while self.stack and self.stack.pop() != tag:
                    pass
            return
        self.stack.pop()
        if tag == "title":
            self.in_title = False
        if tag == "script":
            self.in_ld = False

    def handle_data(self, data):
        if self.in_title:
            self.title += data
        if self.in_ld:
            self.ld[-1] += data


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    expect_base = "--expect-base" in sys.argv
    pages = sorted(root.rglob("index.html"))
    problems = []
    langs_seen = set()
    for f in pages:
        p = Page()
        p.feed(f.read_text(encoding="utf-8"))
        name = f.relative_to(root).as_posix()
        err = lambda msg: problems.append(f"{name}: {msg}")
        for e in p.errors:
            err(e)
        if p.stack:
            err(f"unclosed tags {p.stack}")
        if p.h1 != 1:
            err(f"{p.h1} <h1> elements (expected 1)")
        if not 10 <= len(p.title) <= 65:
            err(f"title length {len(p.title)}: {p.title!r}")
        d = p.meta.get("description", "")
        if not 50 <= len(d) <= 170:
            err(f"description length {len(d)}")
        if not p.lang:
            err("missing <html lang>")
        langs_seen.add(p.lang)
        for s in p.scripts:
            if s.get("type") != "application/ld+json":
                err("script other than JSON-LD (the site must not run scripts)")
        for ld in p.ld:
            try:
                json.loads(ld)
            except ValueError as e:
                err(f"invalid JSON-LD: {e}")
        for l in p.links:
            if l.get("rel") == "stylesheet" and urlparse(l.get("href", "")).scheme:
                err("external stylesheet")
        for h in p.hrefs:
            u = urlparse(h)
            if u.scheme or h.startswith("#"):
                continue
            target = (f.parent / u.path).resolve()
            if target.is_dir():
                target = target / "index.html"
            if not target.exists():
                err(f"broken link {h}")
        if expect_base:
            rels = [l for l in p.links if l.get("rel") == "canonical"]
            alts = {l.get("hreflang") for l in p.links if l.get("rel") == "alternate"}
            if len(rels) != 1:
                err("missing canonical")
            if not {"en", "de", "x-default"} <= alts:
                err(f"hreflang alternates incomplete: {sorted(a for a in alts if a)}")
    for required in ("robots.txt", "llms.txt", "assets/style.css"):
        if not (root / required).exists():
            problems.append(f"missing {required}")
    if expect_base:
        sm = root / "sitemap.xml"
        if not sm.exists():
            problems.append("missing sitemap.xml")
        elif sm.read_text(encoding="utf-8").count("<loc>") != len(pages):
            problems.append("sitemap.xml does not list every page")
    if langs_seen != {"en", "de"}:
        problems.append(f"languages {sorted(x for x in langs_seen if x)} (expected en, de)")
    for pr in problems:
        print("ERROR", pr)
    print(f"website check: {len(pages)} page(s), {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
