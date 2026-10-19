#!/usr/bin/env python3
# Checks the links from the Markdown documentation to the man pages.
#
# The guides link to the HTML pages that `make man_html` renders and that the
# Pages site publishes under SITE_MAN, never to the troff files under man/,
# which GitHub shows as raw source. GitHub cannot check an absolute link for
# us, so this script does: every link under SITE_MAN must name a page that
# exists in man/ (or a module of the index), and a link to a troff file is
# refused with the URL that should replace it. Links to man/README and to the
# man/ directory itself are fine; they are about the source tree.
#
# Run it from the root of the repository; `make check_doc_links` and CI both
# call it. It needs nothing but python3.
import glob
import os
import re
import sys

SITE = "https://abunchofnerds-25.github.io/ccollections/"
SITE_MAN = SITE + "man/"
DOCS = (["README.md", "SECURITY.md", "CHANGELOG.md"] + sorted(glob.glob("doc/*.md"))
        + sorted(glob.glob("bench/*.md")))
PAGE_RE = re.compile(r"^([a-z0-9_]+)/([A-Za-z_][A-Za-z0-9_]*\.[0-9])$")
INLINE_RE = re.compile(r"\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFDEF_RE = re.compile(r"^\s*\[[^\]]+\]:\s*<?(\S+?)>?(?:\s|$)", re.M)


def main():
    errors = []
    checked = 0
    for doc in DOCS:
        if not os.path.exists(doc):
            continue
        text = open(doc, encoding="utf-8").read()
        targets = [(m.start(), m.group(1)) for m in INLINE_RE.finditer(text)]
        targets += [(m.start(), m.group(1)) for m in REFDEF_RE.finditer(text)]
        for pos, url in targets:
            line = text.count("\n", 0, pos) + 1
            where = "%s:%d" % (doc, line)
            if url.startswith(SITE_MAN):
                checked += 1
                rest = url[len(SITE_MAN):]
                path, _, anchor = rest.partition("#")
                if path in ("", "index.html"):
                    if anchor and not os.path.isdir(os.path.join("man", anchor)):
                        errors.append("%s: %s names no module of man/" % (where, url))
                    continue
                if not path.endswith(".html") or not PAGE_RE.match(path[:-5]):
                    errors.append("%s: %s is not the URL of a rendered page "
                                  "(%s<module>/<page>.<n>.html)" % (where, url, SITE_MAN))
                    continue
                if not os.path.isfile(os.path.join("man", path[:-5])):
                    errors.append("%s: %s names no page under man/" % (where, url))
                continue
            if "://" in url or url.startswith(("#", "mailto:")):
                continue
            local = os.path.normpath(os.path.join(os.path.dirname(doc),
                                                  url.split("#")[0]))
            parts = local.split(os.sep)
            if len(parts) == 3 and parts[0] == "man" and \
                    PAGE_RE.match(parts[1] + "/" + parts[2]):
                errors.append("%s: %s links the troff source; use %s%s/%s.html"
                              % (where, url, SITE_MAN, parts[1], parts[2]))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print("check_doc_links: %d bad link(s)" % len(errors), file=sys.stderr)
        sys.exit(1)
    if checked == 0:
        print("check_doc_links: no link to the man pages found, so nothing "
              "was checked", file=sys.stderr)
        sys.exit(1)
    print("check_doc_links: OK (%d links to the man pages)" % checked)


if __name__ == "__main__":
    main()
