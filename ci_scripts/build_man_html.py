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
# A page whose whole body is `.so other` becomes a redirect to that page, and
# index.html lists every page by module with the line from its NAME section.
#
# Run it from the root of the repository; `make man_html` and CI both call it.
# MANDOC names the mandoc program.
import html
import os
import re
import shutil
import subprocess
import sys

MAN_DIR = "man"
STYLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "man_html.css")
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
            page["summary"] = plain_text(lines[i + 1])
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


def render(mandoc, pages, key, outdir):
    page = pages[key]
    module = page["module"]
    src = os.path.join(module, page["file"])
    with open(os.path.join(MAN_DIR, src), encoding="ascii") as f:
        source = nofill_lines_kept(f.read())
    r = subprocess.run([mandoc, "-man", "-T", "html", "-O",
                        "style=../man_html.css"], input=source,
                       cwd=MAN_DIR, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        fail("mandoc could not render %s" % src)
    doc = r.stdout
    marker = '<div class="manual-text">'
    cut = doc.find(marker)
    if cut < 0:
        fail("unexpected mandoc output for %s" % src)
    doc = doc[:cut] + add_links(doc[cut:], pages, key, module)
    nav = ('<nav class="site"><a href="../index.html">C Collections '
           'manual</a> / <a href="../index.html#%s">%s</a></nav>\n'
           % (module, module))
    doc = doc.replace("<body>\n", "<body>\n" + nav, 1)
    write(outdir, module, page["file"] + ".html", doc)


def redirect(pages, key, outdir):
    page = pages[key]
    target = href(pages, key, page["module"])
    name = html.escape("%s(%s)" % key)
    doc = ('<!DOCTYPE html>\n<html>\n<head>\n  <meta charset="utf-8"/>\n'
           '  <meta http-equiv="refresh" content="0; url=%s"/>\n'
           '  <link rel="canonical" href="%s"/>\n'
           '  <link rel="stylesheet" href="../man_html.css"/>\n'
           '  <title>%s</title>\n</head>\n<body>\n'
           '<p>%s is documented on <a href="%s">this page</a>.</p>\n'
           '</body>\n</html>\n' % (target, target, name, name, target))
    write(outdir, page["module"], page["file"] + ".html", doc)


def write(outdir, module, fname, text):
    d = os.path.join(outdir, module)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, fname), "w", encoding="utf-8") as f:
        f.write(text)


def index(pages, outdir):
    by_module = {}
    for key, page in pages.items():
        by_module.setdefault(page["module"], []).append(key)
    parts = ['<!DOCTYPE html>\n<html>\n<head>\n  <meta charset="utf-8"/>\n'
             '  <meta name="viewport" content="width=device-width, '
             'initial-scale=1.0"/>\n'
             '  <link rel="stylesheet" href="man_html.css"/>\n'
             '  <title>C Collections manual</title>\n</head>\n<body>\n'
             '<nav class="site"><a href="../">C Collections</a></nav>\n'
             '<h1 class="index">C Collections manual</h1>\n'
             '<p class="index">One page for each function and macro, '
             'and a section 7 page for each module.</p>\n'
             '<ul class="modules">\n']
    for module in sorted(by_module):
        parts.append('  <li><a href="#%s">%s</a></li>\n' % (module, module))
    parts.append("</ul>\n")
    for module in sorted(by_module):
        # The module overview (section 7) first, then the rest by name.
        keys = sorted(by_module[module], key=lambda k: (k[1] != "7", k[0]))
        parts.append('<h2 class="index" id="%s">%s</h2>\n<dl class="index">\n'
                     % (module, module))
        for key in keys:
            page = pages[key]
            if "alias_of" in page:
                tfile = page["alias_of"].split("/")[1]
                tm = NAME_RE.match(tfile)
                text = "see %s(%s)" % (tm.group(1), tm.group(2))
            else:
                text = page.get("summary", "")
                # The NAME line reads "names - what they do"; the names are
                # already the link.
                if " - " in text:
                    text = text.split(" - ", 1)[1]
            parts.append('  <dt><a href="%s/%s.html">%s(%s)</a></dt>'
                         '<dd>%s</dd>\n' % (module, page["file"], key[0],
                                            key[1], html.escape(text)))
        parts.append("</dl>\n")
    parts.append("</body>\n</html>\n")
    with open(os.path.join(outdir, "index.html"), "w", encoding="utf-8") as f:
        f.write("".join(parts))


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
    shutil.copyfile(STYLE, os.path.join(outdir, "man_html.css"))
    for key, page in pages.items():
        if "alias_of" in page:
            redirect(pages, key, outdir)
        else:
            render(mandoc, pages, key, outdir)
    index(pages, outdir)
    print("build_man_html: %d pages written to %s" % (len(pages), outdir))


if __name__ == "__main__":
    main()
