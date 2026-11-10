#!/usr/bin/env python3
# Renders every page under man/ to HTML for the GitHub Pages site, which is
# where the guides under doc/ and README.md link to. The troff sources stay
# exactly as `make install` copies them; this only reads them.
#
#   build_man_html.py <output directory>
#
# It first runs `mandoc -T lint -W error` over every page and stops when that
# finds anything. An error there is a defect of the installed page too: a
# line of text that starts with ' or . is a control line in troff, and `man`
# drops it without a word.
#
# mandoc renders the man(7) pages but doesn't link `.BR name (3)` or a
# name(3) in the text, so this script adds the links, for our own pages only.
# Every page goes into the frame of site_layout.py, with a title and a
# description taken from its NAME line and a list of the other pages of its
# module beside it. A page whose whole body is `.so other` becomes a redirect
# to that page that search engines don't index, and index.html lists every
# page by module. The output is the man/ directory of the site; the
# stylesheet it refers to comes from build_pages_site.py.
#
# Run it from the root of the repository; `make man_html` and CI both call it.
# MANDOC names the mandoc program.
import html
import os
import re
import shutil
import subprocess
import sys

import site_layout as L

MAN_DIR = "man"
NAME_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\.([0-9])$")
# A name(3) in running text. The look-behind keeps it off the middle of a
# longer word and off a path such as man/x.3.
REF_RE = re.compile(r"(?<![A-Za-z0-9_./])([A-Za-z_][A-Za-z0-9_]*)\(([0-9])\)")
# What mandoc writes for `.BR name (3)`.
BOLD_REF_RE = re.compile(r"<b>([A-Za-z_][A-Za-z0-9_]*)</b>\(([0-9])\)")


def fail(msg):
    print("build_man_html: " + msg, file=sys.stderr)
    sys.exit(1)


def find_pages():
    pages = {}
    for module in sorted(os.listdir(MAN_DIR)):
        mdir = os.path.join(MAN_DIR, module)
        if not os.path.isdir(mdir):
            continue
        for fname in sorted(os.listdir(mdir)):
            m = NAME_RE.match(fname)
            if not m:
                continue
            key = (m.group(1), m.group(2))
            if key in pages:
                fail("two pages are named %s(%s): %s and %s" %
                     (key[0], key[1], pages[key]["module"], module))
            pages[key] = {"module": module, "file": fname}
    if not pages:
        fail("no pages found under man/")
    return pages


def plain_text(line):
    """Turns one troff text line into plain text for the index."""
    line = re.sub(r"\\f[BIRP]|\\f\([A-Z]{2}|\\&", "", line)
    return line.replace("\\-", "-").replace("\\e", "\\").strip()


def read_source(page):
    path = os.path.join(MAN_DIR, page["module"], page["file"])
    with open(path, encoding="ascii") as f:
        lines = f.read().splitlines()
    body = [l for l in lines if l.strip() and not l.startswith('.\\"')]
    if len(body) == 1 and body[0].startswith(".so "):
        target = os.path.normpath(body[0][4:].strip())
        page["alias_of"] = target
        return
    for i, l in enumerate(lines):
        if l.strip().upper() == ".SH NAME" and i + 1 < len(lines):
            text = []
            for nl in lines[i + 1:]:
                if nl.startswith(".") or not nl.strip():
                    break
                text.append(plain_text(nl))
            page["summary"] = " ".join(text)
            return
    fail("%s has no NAME section" % path)


def lint(mandoc, pages):
    files = [os.path.join(p["module"], p["file"]) for p in pages.values()]
    r = subprocess.run([mandoc, "-T", "lint", "-W", "error"] + files,
                       cwd=MAN_DIR, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or out:
        print(out, file=sys.stderr)
        fail("mandoc reports errors in the pages above (a text line that "
             "starts with ' or . needs \\& in front of it)")


def href(pages, key, from_module):
    target = pages[key]
    while "alias_of" in target:
        tmod, tfile = target["alias_of"].split("/")
        m = NAME_RE.match(tfile)
        target = pages[(m.group(1), m.group(2))]
    rel = target["file"] + ".html"
    if target["module"] != from_module:
        rel = "../%s/%s" % (target["module"], rel)
    return rel


def add_links(body, pages, own_key, module):
    def bold(m):
        key = (m.group(1), m.group(2))
        if key not in pages or key == own_key:
            return m.group(0)
        return '<a class="Xr" href="%s">%s</a>' % (href(pages, key, module),
                                                  m.group(0))

    body = BOLD_REF_RE.sub(bold, body)

    def plain(m):
        key = (m.group(1), m.group(2))
        if key not in pages or key == own_key:
            return m.group(0)
        return '<a class="Xr" href="%s">%s</a>' % (href(pages, key, module),
                                                  m.group(0))

    out = []
    in_link = 0
    for tok in re.split(r"(<[^>]*>)", body):
        if tok.startswith("<"):
            if re.match(r"<a[\s>]", tok):
                in_link += 1
            elif tok == "</a>":
                in_link -= 1
            out.append(tok)
        elif in_link:
            out.append(tok)
        else:
            out.append(REF_RE.sub(plain, tok))
    return "".join(out)


def nofill_lines_kept(source):
    """Returns the page source as mandoc's HTML output needs it.

    Inside a .nf block mandoc's HTML output drops an empty line and puts an
    extra line break in front of a line that starts with a space, so the
    paragraphs of a block run together and an aligned continuation line gets
    a blank line above it. An empty line becomes one unpaddable space (\\ ),
    which mandoc keeps as a line, and a line that starts with a space gets a
    \\& (the zero-width character) in front; neither changes what troff
    prints. Only the copy that goes to mandoc changes; the page on disk
    stays the source that `make install` copies."""
    out = []
    nofill = False
    for line in source.splitlines():
        if line.startswith(".nf"):
            nofill = True
        elif line.startswith(".fi"):
            nofill = False
        elif nofill and line == "":
            line = "\\ "
        elif nofill and line.startswith(" "):
            line = "\\&" + line
        out.append(line)
    return "\n".join(out) + "\n"


# The text after the dash of a NAME line ("names - what they do").
def summary_of(page):
    text = page.get("summary", "")
    return text.split(" - ", 1)[1] if " - " in text else text


# The names before the dash of a NAME line, for the page heading.
def names_of(page, key):
    text = page.get("summary", "")
    if " - " in text:
        return text.split(" - ", 1)[0].strip()
    return key[0]


def module_title(module):
    return "ccollections" if module == "common" else module


def overview_key(pages, module):
    for key, page in pages.items():
        if page["module"] == module and key[1] == "7":
            return key
    return None


# The list of every page of a module, beside each of its pages, so a reader
# can move between related functions and every page links its neighbours.
def sidebar(pages, module, own_key):
    keys = sorted((k for k, p in pages.items() if p["module"] == module),
                  key=lambda k: (k[1] != "7", k[0]))
    items = []
    for key in keys:
        label = "Overview" if key[1] == "7" else key[0]
        if key == own_key:
            items.append('<li><span aria-current="page">%s</span></li>'
                         % L.esc(label))
        else:
            items.append('<li><a href="%s">%s</a></li>'
                         % (href(pages, key, module), L.esc(label)))
    return ('<aside class="man-side" aria-label="Pages of %s"><h2>%s</h2>'
            '<ul>%s</ul></aside>' % (L.esc(module_title(module)),
                                     L.esc(module_title(module)),
                                     "".join(items)))


def describe(text, limit=158):
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) <= limit:
        return text
    return text[:limit - 3].rsplit(" ", 1)[0] + "..."


# The pages keep their prose in no-fill blocks so that `man` shows it with
# the line breaks and the alignment of the source, and mandoc writes each
# such block as a <pre>. Only SYNOPSIS and EXAMPLES hold code; every other
# <pre> gets the class "prose", which the stylesheet shows as text, without
# the frame of a code block.
CODE_SECTIONS = ("SYNOPSIS", "EXAMPLES", "EXAMPLE")
EXAMPLE_SECTIONS = ("EXAMPLES", "EXAMPLE")


def mark_prose_blocks(body):
    out = []
    for part in re.split(r'(?=<section class="Sh">)', body):
        m = re.search(r'<h2 class="Sh" id="([^"]*)"', part)
        if m and m.group(1) not in CODE_SECTIONS:
            part = part.replace("<pre>", '<pre class="prose">')
        elif m and m.group(1) in EXAMPLE_SECTIONS:
            part = re.sub(r"(<pre>)(.*?)(</pre>)", highlight_block, part,
                          flags=re.S)
        out.append(part)
    return "".join(out)


# Colours one <pre> block of an EXAMPLES section. mandoc writes the example
# as escaped text with no markup inside, so the text is decoded, highlighted
# and escaped again. A no-break space, which a blank line of the source
# becomes, goes back out as an entity so the page stays ASCII.
def highlight_block(m):
    text = html.unescape(m.group(2))
    code = L.highlight_c(text).replace("\xa0", "&#x00A0;")
    return m.group(1) + code + m.group(3)


def render(mandoc, pages, key, outdir, version):
    page = pages[key]
    module = page["module"]
    src = os.path.join(module, page["file"])
    with open(os.path.join(MAN_DIR, src), encoding="ascii") as f:
        source = nofill_lines_kept(f.read())
    r = subprocess.run([mandoc, "-man", "-T", "html", "-O", "fragment"],
                       input=source, cwd=MAN_DIR, capture_output=True,
                       text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        fail("mandoc could not render %s" % src)
    doc = r.stdout
    marker = '<div class="manual-text">'
    cut = doc.find(marker)
    end = doc.find('<table class="foot">')
    if cut < 0 or end < cut:
        fail("unexpected mandoc output for %s" % src)
    body = add_links(doc[cut:end].rstrip(), pages, key, module)
    # mandoc makes every section an h1; the page has its own h1, so sections
    # move one level down.
    body = re.sub(r"<(/?)h2\b", r"<\1h3", body)
    body = re.sub(r"<(/?)h1\b", r"<\1h2", body)
    body = mark_prose_blocks(body)

    ref = "%s(%s)" % key
    what = summary_of(page)
    mod = module_title(module)
    if key[1] == "7":
        title = "%s: %s | %s" % (mod, what, L.SITE_NAME)
        heading = mod
    else:
        title = "%s - %s | %s" % (ref, what, L.SITE_NAME)
        heading = names_of(page, key)
    if key[1] == "7":
        desc = describe("%s. The %s module of %s, a C11 library: %s." %
                        (what[:1].upper() + what[1:].rstrip("."), mod,
                         L.SITE_NAME, "concepts, rules and the list of its "
                         "functions"))
    else:
        desc = describe("%s: %s. Manual page from the %s module of %s, a "
                        "C11 library." % (ref, what.rstrip("."), mod,
                                          L.SITE_NAME))
    crumbs = [("Manual", "man/")]
    ov = overview_key(pages, module)
    if key[1] == "7" or ov is None:
        crumbs.append((mod, None))
    else:
        crumbs.append((mod, "man/%s/%s.html" % (module, pages[ov]["file"])))
        crumbs.append((ref, None))
    path = "man/%s/%s.html" % (module, page["file"])
    article = {"@context": "https://schema.org", "@type": "TechArticle",
               "headline": "%s: %s" % (ref, what), "description": desc,
               "url": L.SITE_URL + path,
               "isPartOf": {"@type": "WebSite", "name": L.SITE_NAME,
                            "url": L.SITE_URL},
               "about": {"@type": "SoftwareSourceCode",
                         "name": L.SITE_NAME, "codeRepository": L.REPO_URL,
                         "programmingLanguage": "C"}}
    section = "Overview" if key[1] == "7" else "Section %s" % key[1]
    main = ('<div class="man-layout"><article class="man">'
            '<p class="kicker">%s &middot; %s</p><h1 class="page-title">%s</h1>'
            '%s</article>%s</div>'
            % (L.esc(mod), section, L.esc(heading), body,
               sidebar(pages, module, key)))
    out = L.page(path, 2, title, desc, main, crumbs=crumbs, ld=[article],
                 current="Manual", version=version)
    write(outdir, module, page["file"] + ".html", out)


def redirect(pages, key, outdir, version):
    page = pages[key]
    target = href(pages, key, page["module"])
    tmod, tfile = target.split("/")[-2:] if "/" in target else \
        (page["module"], target)
    absolute = L.SITE_URL + "man/%s/%s" % (tmod, tfile)
    name = L.esc("%s(%s)" % key)
    body = ('<p>%s is documented on <a href="%s">%s</a>.</p>'
            % (name, L.esc(target), L.esc(tfile[:-5])))
    head = ('  <meta name="robots" content="noindex, follow">\n'
            '  <meta http-equiv="refresh" content="0; url=%s">'
            % L.esc(target))
    out = L.page("man/%s/%s.html" % (page["module"], page["file"]), 2,
                 "%s | %s" % ("%s(%s)" % key, L.SITE_NAME),
                 "%s is documented on the %s page." % ("%s(%s)" % key,
                                                       tfile[:-5]),
                 body, extra_head=head, version=version, current="Manual")
    # The canonical link of an alias names the page it redirects to.
    out = re.sub(r'<link rel="canonical" href="[^"]*">',
                 '<link rel="canonical" href="%s">' % L.esc(absolute), out)
    out = re.sub(r'<meta property="og:url" content="[^"]*">',
                 '<meta property="og:url" content="%s">' % L.esc(absolute),
                 out)
    write(outdir, page["module"], page["file"] + ".html", out)


def write(outdir, module, fname, text):
    d = os.path.join(outdir, module)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, fname), "w", encoding="utf-8") as f:
        f.write(text)


FILTER_SCRIPT = """<script>
(function () {
  var box = document.getElementById("filter");
  box.hidden = false;
  box.addEventListener("input", function () {
    var q = box.value.trim().toLowerCase();
    document.querySelectorAll("section.mod").forEach(function (sec) {
      var shown = 0;
      sec.querySelectorAll("dt").forEach(function (dt) {
        var dd = dt.nextElementSibling;
        var hit = !q || (dt.textContent + " " + dd.textContent)
          .toLowerCase().indexOf(q) >= 0;
        dt.hidden = dd.hidden = !hit;
        if (hit) shown++;
      });
      sec.hidden = shown === 0;
    });
  });
})();
</script>"""


def index(pages, outdir, version):
    by_module = {}
    for key, page in pages.items():
        by_module.setdefault(page["module"], []).append(key)
    toc = "".join('<a href="#%s">%s</a>' % (m, L.esc(module_title(m)))
                  for m in sorted(by_module))
    parts = ['<h1 class="page-title">Manual</h1>'
             '<p class="lede">One page for each public function and macro, '
             'and an overview page for each module. These are the pages that '
             '<code>man</code> shows after <code>make install</code>.</p>'
             '<nav class="toc" aria-label="Modules">%s</nav>'
             '<input class="filter" type="search" id="filter" '
             'placeholder="Filter pages by name or description" '
             'aria-label="Filter pages" hidden>' % toc]
    for module in sorted(by_module):
        # The module overview (section 7) first, then the rest by name.
        keys = sorted(by_module[module], key=lambda k: (k[1] != "7", k[0]))
        parts.append('<section class="mod" id="%s"><h2>%s</h2>'
                     '<dl class="index">' % (module,
                                             L.esc(module_title(module))))
        for key in keys:
            page = pages[key]
            if "alias_of" in page:
                tfile = page["alias_of"].split("/")[1]
                tm = NAME_RE.match(tfile)
                text = "see %s(%s)" % (tm.group(1), tm.group(2))
            else:
                text = summary_of(page)
            parts.append('<dt><a href="%s/%s.html">%s(%s)</a></dt>'
                         '<dd>%s</dd>' % (module, page["file"], key[0],
                                          key[1], L.esc(text)))
        parts.append("</dl></section>")
    # The filter box is hidden until this runs, so a reader without
    # JavaScript sees the plain list and no box that does nothing.
    parts.append(FILTER_SCRIPT)
    n = sum(1 for p in pages.values() if "alias_of" not in p)
    desc = ("Reference manual of %s: %d pages covering every public "
            "function and macro of its modules, from cvector and chashmap "
            "to the HTTP client and server." % (L.SITE_NAME, n))
    out = L.page("man/", 1, "Manual | %s" % L.SITE_NAME, desc,
                 "".join(parts), crumbs=[("Manual", None)],
                 current="Manual", version=version)
    with open(os.path.join(outdir, "index.html"), "w", encoding="utf-8") as f:
        f.write(out)


def main():
    if len(sys.argv) != 2:
        fail("usage: build_man_html.py <output directory>")
    outdir = os.path.abspath(sys.argv[1])
    mandoc = os.environ.get("MANDOC", "mandoc")
    if shutil.which(mandoc) is None:
        fail("%s not found (Debian and Ubuntu: apt-get install mandoc)"
             % mandoc)
    pages = find_pages()
    for page in pages.values():
        read_source(page)
    for key, page in pages.items():
        if "alias_of" in page:
            tfile = page["alias_of"].split("/")
            m = NAME_RE.match(tfile[-1]) if len(tfile) == 2 else None
            if not m or (m.group(1), m.group(2)) not in pages:
                fail("%s/%s points at %s, which is not a page" %
                     (page["module"], page["file"], page["alias_of"]))
    lint(mandoc, pages)
    if os.path.exists(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    version = L.library_version()
    for key, page in pages.items():
        if "alias_of" in page:
            redirect(pages, key, outdir, version)
        else:
            render(mandoc, pages, key, outdir, version)
    index(pages, outdir, version)
    print("build_man_html: %d pages written to %s" % (len(pages), outdir))


if __name__ == "__main__":
    main()
