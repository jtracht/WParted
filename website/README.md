<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted website

Static, bilingual (EN/DE) project website. Sources are HTML fragments in `pages/<lang>/` and the
stylesheet in `assets/`; `tools/website/build.py` adds the layout and generates the site.

```sh
python tools/website/build.py --out build/site                          # relative links only
python tools/website/build.py --out build/site --base-url https://…/   # + canonical, hreflang, sitemap
python tools/website/check.py build/site [--expect-base]
```

What the generator provides:

- **SEO:** unique title and meta description per page (length-checked), canonical URL, `hreflang`
  alternates for EN/DE plus `x-default`, `sitemap.xml` with language alternates, `robots.txt`,
  Open Graph tags, one `<h1>` per page.
- **AEO:** JSON-LD `FAQPage` generated from the FAQ questions and `SoftwareApplication` on the home
  pages; short, direct answers in "In short" blocks.
- **GEO:** `llms.txt` with a factual summary and links for AI assistants.
- **Honest figures:** version from `CHANGELOG.md`, progress from `status/progress.json` (same
  calculation as `tools/project-management/progress.py`). Pages make no claims beyond the support
  matrix.
- **Privacy:** no scripts (only JSON-LD data), no external fonts, no cookies, no analytics.

CI (`ci.yml`, job *static checks*) builds and checks the site on every push and uploads it as the
artefact `wparted-website`. **Hosting is not set up yet**: the domain and the host are a maintainer
decision, so the site is not deployed anywhere and the absolute URLs are only used in the check.
