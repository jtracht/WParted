#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
"""Static website generator for WParted (EN/DE), no dependencies.

Pages are HTML fragments in website/pages/<lang>/<name>.html with a front
matter comment (title, description, optional schema). The generator adds the
layout, navigation, language alternates, canonical URLs, Open Graph tags and
JSON-LD (SoftwareApplication, FAQPage), and writes sitemap.xml, robots.txt and
llms.txt. Figures (version, progress) come from the repository, so the site
never claims more than CHANGELOG.md and status/progress.json say.

The site has no scripts, no external fonts and no analytics.

Usage: python tools/website/build.py [--out build/site] [--base-url https://example.org/]
Without --base-url, absolute URLs (canonical, hreflang, sitemap) are omitted.
"""
import argparse
import html
import importlib.util
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "website"
REPO_URL = "https://git.tracht-digital.de/jtracht/WParted"
RELEASES_URL = REPO_URL + "/releases"
LANGS = ("en", "de")
PAGES = ("index", "features", "download", "faq")
NAV = {
    "en": {"index": "Home", "features": "Features", "download": "Download", "faq": "FAQ", "source": "Source code"},
    "de": {"index": "Start", "features": "Funktionen", "download": "Download", "faq": "FAQ", "source": "Quellcode"},
}
TEXT = {
    "en": {"skip": "Skip to content", "lang": "Deutsch", "footer": "Free software under GPL-2.0-or-later. "
           "This site uses no cookies, no scripts and no tracking.", "area": "Area", "progress": "Progress",
           "overall": "Overall"},
    "de": {"skip": "Zum Inhalt", "lang": "English", "footer": "Freie Software unter GPL-2.0-or-later. "
           "Diese Seite verwendet keine Cookies, keine Skripte und kein Tracking.", "area": "Bereich",
           "progress": "Fortschritt", "overall": "Gesamt"},
}
AREA_DE = {"Architecture": "Architektur", "CI/CD": "CI/CD", "Partition Engine": "Partitions-Engine",
           "Windows Core": "Windows-Kern", "Filesystem Support": "Dateisysteme", "User Interface": "Oberfläche",
           "Testing": "Tests", "Documentation": "Dokumentation"}


def load_progress():
    spec = importlib.util.spec_from_file_location("progress", ROOT / "tools" / "project-management" / "progress.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    data = json.loads((ROOT / "status" / "progress.json").read_text(encoding="utf-8"))
    return mod.compute(data)


def latest_release():
    """First released version in CHANGELOG.md (skips [Unreleased])."""
    for line in (ROOT / "CHANGELOG.md").read_text(encoding="utf-8").splitlines():
        m = re.match(r"## \[(\d+\.\d+\.\d+[^\]]*)\]", line)
        if m:
            return m.group(1)
    raise SystemExit("no released version in CHANGELOG.md")


def parse_page(path):
    text = path.read_text(encoding="utf-8")
    m = re.match(r"\s*<!--(.*?)-->\s*", text, re.S)
    if not m:
        raise SystemExit(f"{path}: missing front matter")
    meta = {}
    for line in m.group(1).strip().splitlines():
        key, _, value = line.partition(":")
        meta[key.strip()] = value.strip()
    for key in ("title", "description"):
        if not meta.get(key):
            raise SystemExit(f"{path}: front matter needs '{key}'")
    return meta, text[m.end():]


def url_path(lang, page):
    prefix = "" if lang == "en" else lang + "/"
    return prefix + ("" if page == "index" else page + "/")


def rel(from_path, to_path):
    depth = from_path.count("/")
    return ("../" * depth if depth else "./") + to_path


def areas_table(lang, overall, areas):
    t = TEXT[lang]
    rows = "".join(
        f'<tr><td>{html.escape(AREA_DE.get(a, a) if lang == "de" else a)}</td>'
        f'<td><span class="bar" style="--p:{p:.0f}%"></span> {p:.0f} %</td></tr>' for a, p in areas.items())
    return (f'<table class="progress"><thead><tr><th>{t["area"]}</th><th>{t["progress"]}</th></tr></thead>'
            f'<tbody>{rows}<tr class="total"><td>{t["overall"]}</td><td>'
            f'<span class="bar" style="--p:{overall:.0f}%"></span> {overall:.0f} %</td></tr></tbody></table>')


def faq_schema(body):
    items = re.findall(r"<summary>(.*?)</summary>\s*<p>(.*?)</p>", body, re.S)
    clean = lambda s: re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", s)).strip()
    return {"@context": "https://schema.org", "@type": "FAQPage", "mainEntity": [
        {"@type": "Question", "name": clean(q), "acceptedAnswer": {"@type": "Answer", "text": clean(a)}}
        for q, a in items]}


def app_schema(lang, version, description):
    return {"@context": "https://schema.org", "@type": "SoftwareApplication", "name": "WParted",
            "applicationCategory": "UtilitiesApplication", "applicationSubCategory": "Partition manager",
            "operatingSystem": "Windows 7 SP1, Windows 8.1, Windows 10, Windows 11",
            "softwareVersion": version, "license": "https://spdx.org/licenses/GPL-2.0-or-later.html",
            "isAccessibleForFree": True, "offers": {"@type": "Offer", "price": "0", "priceCurrency": "EUR"},
            "downloadUrl": RELEASES_URL, "codeRepository": REPO_URL, "inLanguage": lang,
            "description": description}


def render(lang, page, meta, body, ctx, base):
    here = url_path(lang, page)
    other = "de" if lang == "en" else "en"
    t = TEXT[lang]
    body = body.replace("{{areas_table}}", areas_table(lang, ctx["overall"], ctx["areas"]))
    title = meta["title"]
    for k, v in (("version", ctx["version"]), ("progress", f'{ctx["overall"]:.0f}'), ("release_url", RELEASES_URL),
                 ("repo_url", REPO_URL)):
        body = body.replace("{{" + k + "}}", v)
        title = title.replace("{{" + k + "}}", v)
    body = re.sub(r"<table(.*?)</table>", r'<div class="table-wrap"><table\1</table></div>', body, flags=re.S)
    body = re.sub(r'href="@/([^"]*)"', lambda m: f'href="{rel(here, url_path(lang, m.group(1).strip("/") or "index"))}"', body)
    head = [f"<title>{html.escape(title)}</title>",
            f'<meta name="description" content="{html.escape(meta["description"])}">',
            f'<meta property="og:title" content="{html.escape(title)}">',
            f'<meta property="og:description" content="{html.escape(meta["description"])}">',
            '<meta property="og:type" content="website">',
            f'<meta property="og:locale" content="{"de_DE" if lang == "de" else "en_US"}">']
    if base:
        head.append(f'<link rel="canonical" href="{base}{here}">')
        head.append(f'<meta property="og:url" content="{base}{here}">')
        for l in LANGS:
            head.append(f'<link rel="alternate" hreflang="{l}" href="{base}{url_path(l, page)}">')
        head.append(f'<link rel="alternate" hreflang="x-default" href="{base}{url_path("en", page)}">')
    schema = meta.get("schema")
    if schema == "FAQPage":
        data = faq_schema(body)
    elif schema == "SoftwareApplication":
        data = app_schema(lang, ctx["version"], meta["description"])
    else:
        data = None
    if data:
        head.append('<script type="application/ld+json">' + json.dumps(data, ensure_ascii=False).replace("</", "<\\/")
                    + "</script>")
    current = ' aria-current="page"'
    nav = "".join(
        f'<a href="{rel(here, url_path(lang, p))}"{current if p == page else ""}>{NAV[lang][p]}</a>'
        for p in PAGES) + f'<a href="{REPO_URL}">{NAV[lang]["source"]}</a>'
    return f"""<!doctype html>
<html lang="{lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
{chr(10).join(head)}
<link rel="stylesheet" href="{rel(here, 'assets/style.css')}">
</head>
<body>
<a class="skip" href="#main">{t["skip"]}</a>
<header class="site"><div class="wrap">
<a class="brand" href="{rel(here, url_path(lang, 'index'))}">WParted</a>
<nav aria-label="{NAV[lang]['index']}">{nav}</nav>
<a class="lang" hreflang="{other}" lang="{other}" href="{rel(here, url_path(other, page))}">{t["lang"]}</a>
</div></header>
<main id="main" class="wrap">
{body.strip()}
</main>
<footer class="site"><div class="wrap"><p>{t["footer"]} <a href="{REPO_URL}">{REPO_URL.split("//")[1]}</a></p></div></footer>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build" / "site"))
    ap.add_argument("--base-url", default="")
    a = ap.parse_args()
    base = a.base_url.rstrip("/") + "/" if a.base_url else ""
    out = Path(a.out)
    if out.exists():
        shutil.rmtree(out)
    overall, areas, _ = load_progress()
    ctx = {"version": latest_release(), "overall": overall, "areas": areas}
    shutil.copytree(SITE / "assets", out / "assets")
    descriptions = {}
    for lang in LANGS:
        for page in PAGES:
            meta, body = parse_page(SITE / "pages" / lang / f"{page}.html")
            target = out / url_path(lang, page) / "index.html"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(render(lang, page, meta, body, ctx, base), encoding="utf-8", newline="\n")
            descriptions[(lang, page)] = meta["description"].replace("{{version}}", ctx["version"])
    robots = "User-agent: *\nAllow: /\n"
    if base:
        urls = []
        for lang in LANGS:
            for page in PAGES:
                alts = "".join(f'\n    <xhtml:link rel="alternate" hreflang="{l}" href="{base}{url_path(l, page)}"/>'
                               for l in LANGS)
                urls.append(f"  <url>\n    <loc>{base}{url_path(lang, page)}</loc>{alts}\n  </url>")
        (out / "sitemap.xml").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" '
            'xmlns:xhtml="http://www.w3.org/1999/xhtml">\n' + "\n".join(urls) + "\n</urlset>\n", encoding="utf-8",
            newline="\n")
        robots += f"Sitemap: {base}sitemap.xml\n"
    (out / "robots.txt").write_text(robots, encoding="utf-8", newline="\n")
    # llms.txt (https://llmstxt.org): a short, factual summary for AI assistants.
    link = lambda lang, page: (base + url_path(lang, page)) if base else url_path(lang, page) or "./"
    llms = [f"# WParted", "", "> Free, open-source (GPL-2.0-or-later) partition manager for Windows 7 SP1 to 11 "
            "that follows the GParted workflow. Native Win32 program, portable, no telemetry.", "",
            f"- Latest release: {ctx['version']} (pre-release): plans partition operations for every disk and applies them to disk image files only.",
            "- Physical disks are never written by this version; image files are written after two confirmations, every step is read back.",
            "- Verified on Windows 11 x64; Windows 7 to 10 are build targets, not yet verified.",
            f"- Progress towards 1.0: {overall:.0f} % (weighted, from status/progress.json).",
            f"- Source, issues and releases: {REPO_URL}", "", "## Pages", ""]
    for page in PAGES:
        llms.append(f"- [{NAV['en'][page]}]({link('en', page)}): {descriptions[('en', page)]}")
    llms += ["", "## Deutsch", ""]
    for page in PAGES:
        llms.append(f"- [{NAV['de'][page]}]({link('de', page)}): {descriptions[('de', page)]}")
    (out / "llms.txt").write_text("\n".join(llms) + "\n", encoding="utf-8", newline="\n")
    print(f"website: {len(LANGS) * len(PAGES)} pages written to {out}")


if __name__ == "__main__":
    main()
