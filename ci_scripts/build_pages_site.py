#!/usr/bin/env python3
# Puts the GitHub Pages site together in one directory, ready to deploy:
#
#   build_pages_site.py <output> --man <man_html> [--coverage <html>]
#
# The site holds the front page, the rendered man pages (man/, from
# build_man_html.py), the merged coverage report (coverage/, from genhtml)
# when there is one, the stylesheet and logo (assets/), a 404 page, the
# sitemap that Google Search Console reads and the file that proves to
# Search Console that we own the site.
#
# The front page takes the module table, the table of guides and the first
# example from README.md, so it says what README says. The sitemap lists the
# front page, the manual index and every man page that isn't a redirect. When
# the checkout has the history, each entry carries the date of the last
# commit that changed its source; a shallow clone gets no dates, because a
# wrong date is worse than none. Every coverage page gets a noindex tag: the
# report is thousands of pages of annotated source that would compete in
# search results with the man pages of the same functions.
#
# Run it from the root of the repository; `make pages_site` and the Coverage
# workflow call it.
import argparse
import os
import re
import shutil
import subprocess
import sys

import site_layout as L

HERE = os.path.dirname(os.path.abspath(__file__))
VERIFY_FILE = "googlee0970a6a6d6890ca.html"


def fail(msg):
    print("build_pages_site: " + msg, file=sys.stderr)
    sys.exit(1)


# Turns the inline Markdown that README uses in table cells (code spans and
# links) into HTML.
def md_inline(text):
    out = []
    for part in re.split(r"(`[^`]*`)", text):
        if part.startswith("`") and part.endswith("`") and len(part) > 1:
            out.append("<code>%s</code>" % L.esc(part[1:-1]))
        else:
            part = L.esc(part)
            part = re.sub(r"\[([^\]]+)\]\(([^)]+)\)",
                          lambda m: '<a href="%s">%s</a>'
                          % (doc_url(m.group(2)), m.group(1)), part)
            out.append(part)
    return "".join(out)


# A link of README points into the repository; on the site it points at the
# file on GitHub.
def doc_url(target):
    if re.match(r"[a-z]+:", target) or target.startswith("#"):
        return target
    return "%s/blob/main/%s" % (L.REPO_URL, target)


# Gives the rows of the first Markdown table under the README heading
# `heading`, each as a list of cell texts.
def readme_table(readme, heading):
    m = re.search(r"^## %s\n(.*?)(?=^## |\Z)" % re.escape(heading), readme,
                  re.S | re.M)
    if not m:
        fail("README.md has no section '%s'" % heading)
    rows = []
    for line in m.group(1).splitlines():
        if not line.startswith("|") or re.match(r"\|\s*-", line):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        rows.append(cells)
    if len(rows) < 2:
        fail("README.md section '%s' has no table" % heading)
    return rows[1:]


def readme_example(readme):
    m = re.search(r"```c\n(.*?)```", readme, re.S)
    if not m:
        fail("README.md has no C example")
    return m.group(1)


def front_page(readme, man_dir, has_coverage, version):
    modules = readme_table(readme, "What is inside")
    cards = []
    for name, what, guide in modules:
        mod = name.strip("`")
        overview = os.path.join(man_dir, mod, mod + ".7.html")
        if not os.path.exists(overview):
            fail("README lists module %s, which has no overview page" % mod)
        gm = re.search(r"\(([^)]+)\)", guide)
        links = ['<a href="man/%s/%s.7.html">Manual</a>' % (mod, mod)]
        if gm:
            links.append('<a href="%s">Guide</a>' % doc_url(gm.group(1)))
        cards.append('<div class="card"><h3><a href="man/%s/%s.7.html">%s'
                     '</a></h3><p>%s</p><div class="links">%s</div></div>'
                     % (mod, mod, L.esc(mod), md_inline(what),
                        "".join(links)))

    guides = []
    for title, when in readme_table(readme, "Learning the library"):
        guides.append("<li>%s<span>%s</span></li>"
                      % (md_inline(title), md_inline(when)))

    example = L.highlight_c(readme_example(readme))
    coverage = ""
    coverage_button = ""
    if has_coverage:
        coverage_button = ('<a class="button" href="coverage/">Coverage '
                           'Report</a>')
        coverage = ('<li><a href="coverage/">Test coverage</a><span>Line '
                    'coverage of every source file, merged from all test '
                    'suites on the main branch.</span></li>')

    body = """
<div class="home">
<section class="hero">
  <h1><img src="assets/logo.png" alt="" width="76" height="76">C
  Collections</h1>
  <p>The pieces that other languages put in their standard library and C
  doesn't have: vectors, hash maps, ordered maps, strings, memory pools,
  thread-safe queues, a thread pool, a structured logger, JSON, YAML and an
  HTTP client and server. The compiler deduces your element types, with no
  <code>void *</code> casts.</p>
  <div class="actions">
    <a class="button primary" href="man/">Read the manual</a>
    <a class="button" href="%(repo)s">View on GitHub</a>
    %(coverage_button)s
  </div>
</section>

<ul class="facts">
  <li><strong>Typed macros</strong>C11 <code>_Generic</code> sees the types
  you declared, so a map from <code>char *</code> to <code>int</code> stays
  one at every call site.</li>
  <li><strong>One style everywhere</strong>Create with
  <code>*_construct</code>, release with <code>*_destroy</code>, or let a
  <code>*_scoped</code> form do it for you.</li>
  <li><strong>Multithreading where it helps</strong>Queues, cache, logger, thread
  pool and HTTP are thread-safe. The containers leave locking to you, since
  a lock inside each call can't cover a lookup followed by an insert.</li>
  <li><strong>Tested hard</strong>Valgrind, ASan, UBSan and TSan on several
  architectures and systems, plus fuzzing of every parser.</li>
</ul>

<h2>A first program</h2>
<p class="section-lede">A vector and a hash map, with the types deduced by
the compiler and both released at the end of the scope.</p>
<pre><code>%(example)s</code></pre>
<p>Build and install the library, then compile against it with
<code>pkg-config</code>. The install puts the man pages in place too, so
<code>man cvec_push</code> works right after it. It runs on Linux (glibc or
musl), FreeBSD, OpenBSD and macOS, with GCC 10 or Clang 11 and later.</p>
<pre><code>make
sudo make install
gcc -o hello hello.c $(pkg-config --cflags --libs ccollections)
./hello</code></pre>
<p>You don't have to build what you won't use. JSON, YAML, the logger and
the HTTP client and server are each one switch on the <code>make</code>
command line, and turning one off also drops what it needs: OpenSSL goes
with the two HTTP modules, zlib with the logger.</p>
<pre><code>make WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0     # no OpenSSL
make WITH_CJSON=0 WITH_CYAML=0 WITH_CLOGGER=0 \\
     WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0     # pthread and libm only</code></pre>
<p>The HTTP modules log through the logger, so it stays in while either of
them does. <a href="%(repo)s/blob/main/doc/building.md#leaving-modules-out">Leaving
modules out</a> covers the rest, such as installing a reduced build.</p>

<h2>Modules</h2>
<p class="section-lede">Each module has an overview page in the manual
and a guide with complete examples.</p>
<div class="modules">
%(cards)s
</div>

<h2>Documentation</h2>
<div class="split">
  <ul class="doclist">
    <li><a href="man/">Manual</a><span>One page for each public function and
    macro, with every parameter, return value and edge case.</span></li>
    %(coverage)s
    <li><a href="%(repo)s/blob/main/CHANGELOG.md">Changelog</a><span>What
    changed in each release.</span></li>
  </ul>
  <ul class="doclist">
%(guides)s
  </ul>
</div>
</div>
""" % {"repo": L.REPO_URL, "example": example, "cards": "\n".join(cards),
       "guides": "\n".join(guides), "coverage": coverage,
       "coverage_button": coverage_button}

    desc = ("C Collections is a GNU C11 library with type-inferred vectors, hash "
            "maps, ordered maps, strings, memory pools, thread-safe queues, "
            "a thread pool, a logger, JSON, YAML and an HTTP client and "
            "server.")
    ld = [
        {"@context": "https://schema.org", "@type": "WebSite",
         "name": L.SITE_NAME, "url": L.SITE_URL, "description": desc},
        {"@context": "https://schema.org", "@type": "SoftwareSourceCode",
         "name": L.SITE_NAME, "description": desc, "url": L.SITE_URL,
         "codeRepository": L.REPO_URL, "programmingLanguage": "C",
         "license": "https://opensource.org/licenses/MIT",
         "version": version, "image": L.SITE_URL + L.LOGO,
         "runtimePlatform": ["Linux", "FreeBSD", "OpenBSD", "macOS"]},
    ]
    return L.page("", 0, "C Collections: type-inferred containers and more "
                  "for GNU C11", desc, body, ld=ld, version=version)


def not_found_page(version):
    body = ('<section class="notfound"><h1>Page not found</h1>'
            '<p>There is no page at this address. It may have moved when '
            'a function was renamed.</p><div class="actions">'
            '<a class="button primary" href="%sman/">Browse the manual</a>'
            '<a class="button" href="%s">Front page</a></div></section>'
            % (L.SITE_URL, L.SITE_URL))
    return L.page("404.html", 0, "Page not found | %s" % L.SITE_NAME,
                  "The page you asked for doesn't exist.", body,
                  extra_head='  <meta name="robots" content="noindex">',
                  version=version, absolute=True)


# Gives the date of the last commit that changed each file, from one walk of
# the history, or an empty mapping when the clone is shallow or git is
# missing.
def commit_dates():
    try:
        shallow = subprocess.run(
            ["git", "rev-parse", "--is-shallow-repository"],
            capture_output=True, text=True, check=True).stdout.strip()
        if shallow != "false":
            print("build_pages_site: shallow clone, the sitemap has no dates")
            return {}
        log = subprocess.run(
            ["git", "log", "--format=@%cs", "--name-only", "--", "man",
             "README.md", "ci_scripts"],
            capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        print("build_pages_site: no git history, the sitemap has no dates")
        return {}
    dates = {}
    current = None
    for line in log.splitlines():
        if line.startswith("@"):
            current = line[1:]
        elif line and current:
            dates.setdefault(line, current)
    return dates


def sitemap(man_dir, dates):
    def newest(*paths):
        found = [dates[p] for p in paths if p in dates]
        return max(found) if found else None

    frame = ["ci_scripts/site_layout.py", "ci_scripts/site/site.css"]
    entries = [("", newest("README.md", "ci_scripts/build_pages_site.py",
                           *frame))]
    renderer = "ci_scripts/build_man_html.py"
    pages = []
    for module in sorted(os.listdir(man_dir)):
        mdir = os.path.join(man_dir, module)
        if not os.path.isdir(mdir):
            continue
        for fname in sorted(os.listdir(mdir)):
            if not fname.endswith(".html"):
                continue
            with open(os.path.join(mdir, fname), encoding="utf-8") as f:
                if 'content="noindex' in f.read(4096):
                    continue
            src = "man/%s/%s" % (module, fname[:-5])
            pages.append(src)
            entries.append(("man/%s/%s" % (module, fname),
                            newest(src, renderer, *frame)))
    entries.insert(1, ("man/", newest(renderer, *frame, *pages)))
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">']
    for path, date in entries:
        line = "  <url><loc>%s</loc>" % L.esc(L.SITE_URL + path)
        if date:
            line += "<lastmod>%s</lastmod>" % date
        out.append(line + "</url>")
    out.append("</urlset>")
    return "\n".join(out) + "\n", len(entries)


def noindex_coverage(cov_dir):
    tag = '<meta name="robots" content="noindex, follow">'
    count = 0
    for root, _, files in os.walk(cov_dir):
        for fname in files:
            if not fname.endswith(".html"):
                continue
            path = os.path.join(root, fname)
            with open(path, encoding="utf-8", errors="surrogateescape") as f:
                text = f.read()
            new, n = re.subn(r"(<head[^>]*>)", r"\1\n  " + tag, text, count=1,
                             flags=re.I)
            if n != 1:
                fail("%s has no <head>" % path)
            with open(path, "w", encoding="utf-8",
                      errors="surrogateescape") as f:
                f.write(new)
            count += 1
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--man", required=True)
    ap.add_argument("--coverage")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.man, "index.html")):
        fail("%s holds no rendered manual (run make man_html)" % args.man)
    has_coverage = bool(args.coverage) and os.path.isfile(
        os.path.join(args.coverage, "index.html"))
    if args.coverage and not has_coverage:
        print("build_pages_site: no coverage report in %s, the site goes "
              "without one" % args.coverage)

    version = L.library_version()
    with open("README.md", encoding="ascii") as f:
        readme = f.read()

    out = args.output
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "assets"))
    shutil.copytree(args.man, os.path.join(out, "man"))
    for asset in ("site.css", "logo.png"):
        shutil.copyfile(os.path.join(HERE, "site", asset),
                        os.path.join(out, "assets", asset))
    # Search Console proves that we own the site by fetching this file from
    # the root of it, so it must stay there under this name.
    shutil.copyfile(os.path.join(HERE, VERIFY_FILE),
                    os.path.join(out, VERIFY_FILE))
    if has_coverage:
        shutil.copytree(args.coverage, os.path.join(out, "coverage"))
        n = noindex_coverage(os.path.join(out, "coverage"))
        print("build_pages_site: %d coverage pages marked noindex" % n)

    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
        f.write(front_page(readme, args.man, has_coverage, version))
    with open(os.path.join(out, "404.html"), "w", encoding="utf-8") as f:
        f.write(not_found_page(version))
    text, n = sitemap(os.path.join(out, "man"), commit_dates())
    with open(os.path.join(out, "sitemap.xml"), "w", encoding="utf-8") as f:
        f.write(text)
    print("build_pages_site: site written to %s, %d addresses in the "
          "sitemap" % (out, n))


if __name__ == "__main__":
    main()
