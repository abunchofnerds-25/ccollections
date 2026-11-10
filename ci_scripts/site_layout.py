# The page frame of the GitHub Pages site, shared by build_man_html.py and
# build_pages_site.py so every page has the same head, header and footer.
#
# CCOL_SITE_URL in the environment overrides the public address of the site
# (a fork publishes under its own account). It must end with a slash.
import html
import json
import os
import re

SITE_URL = os.environ.get("CCOL_SITE_URL",
                          "https://abunchofnerds-25.github.io/ccollections/")
if not SITE_URL.endswith("/"):
    SITE_URL += "/"
REPO_URL = "https://github.com/abunchofnerds-25/ccollections"
SITE_NAME = "C Collections"
# The logo, from the site root: a 176 x 176 PNG with a transparent
# background, so it sits on the light and the dark theme alike. It's the
# header mark, the browser icon and the picture a shared link shows.
LOGO = "assets/logo.png"
LOGO_SIZE = 176
SITE_TAGLINE = ("Type-inferred containers, strings, memory pools, thread-safe "
                "queues, a thread pool, a logger, JSON, YAML and HTTP for C11.")

# Shared links in the header of every page: label, path from the site root,
# or an absolute address.
NAV = [
    ("Manual", "man/"),
    ("Coverage", "coverage/"),
    ("GitHub", REPO_URL),
]


def esc(text):
    return html.escape(text, quote=True)


# Marks up C source with <span> classes that site.css colours: comments,
# strings and characters, numbers, preprocessor lines, keywords, standard
# types and every name that's called, which picks out the library's macros.
# The front page and the EXAMPLES sections of the man pages use it. It's a
# small tokenizer of our own, so the site needs no highlighting package and
# no script in the browser. src is plain C text, the result is escaped HTML
# for a <pre> block.
C_KEYWORDS = frozenset("""
    auto break case const continue default do else enum extern for goto if
    inline register restrict return sizeof static struct switch typedef union
    volatile while _Alignas _Alignof _Atomic _Generic _Noreturn
    _Static_assert _Thread_local""".split())
C_TYPES = frozenset("""
    void char short int long float double signed unsigned bool _Bool size_t
    ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t
    uint8_t uint16_t uint32_t uint64_t FILE""".split())
C_TOKEN = re.compile(r"""
    (?P<include>^[ \t]*\#[ \t]*include)(?P<header>[ \t]*(?:<[^>\n]*>|"[^"\n]*"))
  | (?P<pre>^[ \t]*\#[ \t]*\w+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
  | (?P<number>\b(?:0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][+-]?\d+)?)[uUlLfF]*\b)
  | (?P<name>[A-Za-z_]\w*)(?P<call>(?=[ \t]*\())?
""", re.M | re.S | re.X)


def highlight_c(src):
    out = []
    pos = 0

    def span(cls, text):
        out.append('<span class="tok-%s">%s</span>' % (cls, esc(text)))

    for m in C_TOKEN.finditer(src):
        out.append(esc(src[pos:m.start()]))
        pos = m.end()
        if m.group("include"):
            span("p", m.group("include"))
            span("s", m.group("header"))
        elif m.group("pre"):
            span("p", m.group("pre"))
        elif m.group("comment"):
            span("c", m.group("comment"))
        elif m.group("string"):
            span("s", m.group("string"))
        elif m.group("number"):
            span("n", m.group("number"))
        else:
            word = m.group("name")
            if word in C_KEYWORDS:
                span("k", word)
            elif word in C_TYPES:
                span("t", word)
            elif m.group("call") is not None:
                span("f", word)
            else:
                out.append(esc(word))
    out.append(esc(src[pos:]))
    return "".join(out)


# Reads the release version from include/common.h, the one place where it's
# written, so the footer always names the version the site was built from.
def library_version():
    parts = {}
    with open(os.path.join("include", "common.h"), encoding="ascii") as f:
        for line in f:
            m = re.match(r"#define CCOL_VERSION_(MAJOR|MINOR|PATCH) (\d+)", line)
            if m:
                parts[m.group(1)] = m.group(2)
    if len(parts) != 3:
        raise SystemExit("site_layout: no version macros in include/common.h")
    return "%s.%s.%s" % (parts["MAJOR"], parts["MINOR"], parts["PATCH"])


# Gives the relative path from a page `depth` directories below the site root
# to `path`, or `path` itself when it's an absolute address.
def rel(depth, path):
    if re.match(r"[a-z]+:", path):
        return path
    return "../" * depth + path if depth else (path or "./")


# Builds a whole HTML page. path is the page's location from the site root
# (for the canonical address), depth how many directories it sits below the
# root, title and description the text a search result shows, body the HTML
# inside <main>, crumbs a list of (label, path or None) for the breadcrumb
# line and its structured data, ld more structured-data objects, and extra
# head lines such as a robots tag. absolute makes every link absolute, for
# the 404 page, which GitHub serves at any address.
def page(path, depth, title, description, body, crumbs=None, ld=None,
         extra_head="", current=None, version="", absolute=False,
         body_class=""):
    def link(p):
        if absolute and not re.match(r"[a-z]+:", p):
            return SITE_URL + p
        return rel(depth, p)

    canonical = SITE_URL + path
    head = [
        '<!DOCTYPE html>',
        '<html lang="en">',
        '<head>',
        '  <meta charset="utf-8">',
        '  <meta name="viewport" content="width=device-width, '
        'initial-scale=1">',
        '  <title>%s</title>' % esc(title),
        '  <meta name="description" content="%s">' % esc(description),
    ]
    if not absolute:
        head.append('  <link rel="canonical" href="%s">' % esc(canonical))
    head += [
        '  <meta property="og:type" content="website">',
        '  <meta property="og:site_name" content="%s">' % SITE_NAME,
        '  <meta property="og:title" content="%s">' % esc(title),
        '  <meta property="og:description" content="%s">' % esc(description),
        '  <meta property="og:url" content="%s">' % esc(canonical),
        '  <meta property="og:image" content="%s">' % esc(SITE_URL + LOGO),
        '  <meta property="og:image:width" content="%d">' % LOGO_SIZE,
        '  <meta property="og:image:height" content="%d">' % LOGO_SIZE,
        '  <meta property="og:image:alt" content="%s logo">' % SITE_NAME,
        '  <meta name="twitter:card" content="summary">',
        '  <meta name="color-scheme" content="light dark">',
        '  <link rel="icon" href="%s" type="image/png" sizes="%dx%d">'
        % (link(LOGO), LOGO_SIZE, LOGO_SIZE),
        '  <link rel="apple-touch-icon" href="%s">' % link(LOGO),
        '  <link rel="stylesheet" href="%s">' % link("assets/site.css"),
    ]
    if extra_head:
        head.append(extra_head)
    objects = list(ld or [])
    if crumbs:
        items = []
        for i, (label, p) in enumerate(crumbs, 1):
            item = {"@type": "ListItem", "position": i, "name": label}
            if p is not None:
                item["item"] = SITE_URL + p
            items.append(item)
        objects.append({"@context": "https://schema.org",
                        "@type": "BreadcrumbList", "itemListElement": items})
    for obj in objects:
        text = json.dumps(obj, separators=(",", ":")).replace("</", "<\\/")
        head.append('  <script type="application/ld+json">%s</script>' % text)
    head.append('</head>')

    nav = []
    for label, p in NAV:
        cur = ' aria-current="page"' if label == current else ""
        nav.append('<a href="%s"%s>%s</a>' % (esc(link(p)), cur, label))
    header = (
        '<header class="top"><div class="bar">'
        '<a class="brand" href="%s" aria-label="%s home"><img src="%s" '
        'alt="" width="30" height="30"><span>%s</span></a>'
        '<nav aria-label="Site">%s</nav></div></header>'
        % (esc(link("")), SITE_NAME, esc(link(LOGO)), SITE_NAME,
           "".join(nav)))

    crumb_html = ""
    if crumbs:
        parts = []
        for label, p in crumbs:
            if p is None:
                parts.append('<span aria-current="page">%s</span>' % esc(label))
            else:
                parts.append('<a href="%s">%s</a>' % (esc(link(p)), esc(label)))
        crumb_html = ('<nav class="crumbs" aria-label="Breadcrumb">%s</nav>'
                      % '<span class="sep">/</span>'.join(parts))

    footer = (
        '<footer class="bottom"><div class="bar">'
        '<span>%s %s</span><span>MIT License</span>'
        '<a href="%s">Source on GitHub</a></div></footer>'
        % (SITE_NAME, esc(version), esc(REPO_URL)))

    cls = ' class="%s"' % body_class if body_class else ""
    return "\n".join(head) + (
        "\n<body%s>\n%s\n<main>\n%s%s\n</main>\n%s\n</body>\n</html>\n"
        % (cls, header, crumb_html, body, footer))
