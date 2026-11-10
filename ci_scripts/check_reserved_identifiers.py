#!/usr/bin/env python3
# Fails when the library declares, defines or uses as a macro name an
# identifier that ISO C reserves for the compiler and the C library, or one
# that the compiler predefines as a macro.
#
# C11 7.1.3 reserves every identifier that begins with two underscores or
# with an underscore and an uppercase letter for any use, and every other
# identifier that begins with an underscore for file scope and as a macro
# name. 7.31 (future library directions) reserves macro names such as
# E[0-9A-Z]..., SIG..., LC_..., PRI..., ATOMIC_... and function names such as
# is[a-z]..., to[a-z]..., str[a-z]..., mem[a-z]... for the headers that
# declare their family. A program that declares or defines such a name has
# undefined behavior: the compiler or the C library may already use it, a
# macro of theirs then rewrites our code, and a symbol of theirs can be bound
# in place of ours. Library code therefore spells none of them. A name that
# only marks something as internal uses the form <prefix>__<rest>
# (ccol__fatal_err, cvector__destroy), which C leaves to the program.
#
# The check covers every header under include/ (public, internal and the test
# helpers) and every source under src/. It reads the code with comments,
# string and character literals and #include lines taken out, and it skips an
# identifier that follows ##, which only ever ends a pasted name.
#
# A name that begins with an underscore passes only when it's a keyword of
# the language, a builtin of the compiler, a macro that a system predefines or
# a feature-test macro that POSIX lets a program define. That list is
# explicit: a new use of such a name in the library has to be added to it
# here, which keeps each one a deliberate choice.
#
# POSIX reserves two more patterns for the system: names that end in _t for
# every header, and names that end in _MAX for <limits.h>. A name the library
# defines takes neither form, so a type is cselectable and not cselectable_t,
# and a limit ends in _LIMIT or _SIZE. The check finds the library's own
# definitions in the code itself (typedef names, struct, union and enum tags,
# enumerators, macro names and macro parameters), so the system types that
# the library only uses, such as size_t or pthread_mutex_t, never count, and
# the answer is the same on every system.
#
# The last check asks the compiler (CC, default cc) for the macros it
# predefines in the library's dialect and fails when one of our identifiers
# has the same spelling. GNU C predefines linux and unix, for example, and an
# identifier with either name turns into the number 1.
#
# Run it from the root of the repository; `make check_namespace` and CI do.
import os
import re
import subprocess
import sys

ROOT_DIRS = ("include", "src")

# Keywords and special identifiers of C11 and GNU C.
KEYWORDS = {
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
    "_Imaginary", "_Noreturn", "_Pragma", "_Static_assert", "_Thread_local",
    "__VA_ARGS__", "__COUNTER__", "__FILE__", "__LINE__", "__func__",
    "__attribute__", "__typeof__", "__extension__", "__auto_type", "__thread",
    "__uint128_t", "__asm__", "__inline__", "__restrict", "__restrict__",
    "__volatile__", "__alignof__",
}
# Macros that a compiler or a system predefines, and feature-test macros.
PREDEFINED = {
    "__APPLE__", "__DragonFly__", "__FreeBSD__", "__NetBSD__", "__OpenBSD__",
    "__linux__", "__GLIBC__", "__GNUC__", "__clang__", "__clang_major__",
    "__CHAR_UNSIGNED__", "__SANITIZE_THREAD__", "__SANITIZE_ADDRESS__",
    "__STDC_VERSION__", "__BYTE_ORDER__", "__ORDER_BIG_ENDIAN__",
    "__ORDER_LITTLE_ENDIAN__", "_GNU_SOURCE", "_PC_NAME_MAX",
    "_SC_NPROCESSORS_ONLN",
}
# Families of compiler builtins and runtime interfaces.
BUILTIN_PREFIXES = (
    "__builtin_", "__atomic_", "__ATOMIC_", "__sync_", "__has_", "__tsan_",
    "__asan_", "__lsan_", "__msan_", "__ubsan_", "__sanitizer_", "__gcov_",
)

# C11 7.31 macro names, and the 7.31 function names, which an
# implementation may also define as macros (7.1.4).
FUTURE_MACRO = re.compile(
    r"^(E[0-9A-Z]|FE_[A-Z]|PRI[a-zX]|SCN[a-zX]|U?INT[0-9A-Z_]*_(MAX|MIN|C)$"
    r"|LC_[A-Z]|SIG_?[A-Z]|ATOMIC_[A-Z]|TIME_[A-Z]|MATH_[A-Z]"
    r"|(is|to|str|mem|wcs)[a-z]|(cnd|mtx|thrd|tss)_[a-z]|atomic_[a-z]"
    r"|memory_order_[a-z]|u?int[0-9a-z_]*_t$)"
)

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
POSIX_SUFFIX = re.compile(r"(_t|_MAX)$")
TAG_DEF = re.compile(r"\b(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{")
DEFINE_PARAMS = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]\w*\(([^)]*)\)", re.M)
FUNC_PTR_NAME = re.compile(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)", re.M)


def strip_code(src):
    """Return src with comments, literals and #include lines blanked out."""
    out = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, j))
            i = j
        elif src[i] in "\"'":
            q, j = src[i], i + 1
            while j < n and src[j] != q and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(" ")
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    text = "".join(out)
    return re.sub(r"^[ \t]*#[ \t]*include\b.*$", "", text, flags=re.M)


def defined_names(code):
    """Yield (name, offset) for every name that this code defines: typedef
    names, tags, enumerators and macro parameters. Macro names come from
    DEFINE. The scan works on code that strip_code() has cleaned."""
    for m in TAG_DEF.finditer(code):
        yield m.group(1), m.start(1)
    for m in DEFINE_PARAMS.finditer(code):
        for param in m.group(1).split(","):
            param = param.strip()
            if IDENT.fullmatch(param):
                yield param, m.start(1)
    # typedef statements: walk to the ';' at brace and paren depth 0.
    for m in re.finditer(r"\btypedef\b", code):
        depth, i = 0, m.end()
        while i < len(code):
            c = code[i]
            if c in "({[":
                depth += 1
            elif c in ")}]":
                depth -= 1
            elif c == ";" and depth == 0:
                break
            i += 1
        stmt = code[m.end():i]
        ptr = FUNC_PTR_NAME.search(stmt)
        if ptr and stmt.rstrip().endswith(")"):
            yield ptr.group(1), m.end() + ptr.start(1)
            continue
        tail = re.sub(r"\[[^\]]*\]\s*$", "", stmt.rstrip())
        last = re.search(r"([A-Za-z_]\w*)\s*$", tail)
        if last:
            yield last.group(1), m.end() + last.start(1)
    # enumerators: the first identifier of each item of an enum body.
    for m in re.finditer(r"\benum\b[^;{]*\{", code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            depth += {"{": 1, "}": -1}.get(code[i], 0)
            i += 1
        body = code[m.end():i - 1]
        pos = m.end()
        for item in body.split(","):
            e = re.match(r"\s*([A-Za-z_]\w*)", item)
            if e:
                yield e.group(1), pos + e.start(1)
            pos += len(item) + 1


def allowed(name):
    return (name in KEYWORDS or name in PREDEFINED
            or name.startswith(BUILTIN_PREFIXES))


def main():
    files = []
    for d in ROOT_DIRS:
        for dirpath, _dirs, names in os.walk(d):
            files += [os.path.join(dirpath, f) for f in sorted(names)
                      if f.endswith((".c", ".h"))]
    if not files:
        print("check_reserved_identifiers: no source found; run it from the "
              "root of the repository", file=sys.stderr)
        return 2

    problems = []
    identifiers = {}
    for path in sorted(files):
        with open(path, encoding="utf-8") as fh:
            code = strip_code(fh.read())
        for m in IDENT.finditer(code):
            name = m.group(0)
            if code[max(0, m.start() - 2):m.start()] == "##":
                continue
            line = code.count("\n", 0, m.start()) + 1
            identifiers.setdefault(name, f"{path}:{line}")
            if name.startswith("_") and not allowed(name):
                problems.append(f"{path}:{line}: {name} begins with an "
                                "underscore (C11 7.1.3)")
        for name, off in defined_names(code):
            if POSIX_SUFFIX.search(name):
                line = code.count("\n", 0, off) + 1
                problems.append(f"{path}:{line}: {name} ends in a suffix "
                                "that POSIX reserves for the system")
        for m in DEFINE.finditer(code):
            name = m.group(1)
            if POSIX_SUFFIX.search(name):
                line = code.count("\n", 0, m.start()) + 1
                problems.append(f"{path}:{line}: macro {name} ends in a "
                                "suffix that POSIX reserves for the system")
            if FUTURE_MACRO.match(name):
                line = code.count("\n", 0, m.start()) + 1
                problems.append(f"{path}:{line}: macro {name} matches a name "
                                "that C11 7.31 reserves")

    cc = os.environ.get("CC", "cc").split()
    try:
        predef = subprocess.run(cc + ["-std=gnu11", "-dM", "-E", "-x", "c",
                                      os.devnull],
                                capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"check_reserved_identifiers: cannot ask {' '.join(cc)} for its "
              f"predefined macros: {exc}", file=sys.stderr)
        return 2
    macros = {line.split()[1].split("(")[0]
              for line in predef.stdout.splitlines()
              if line.startswith("#define ")}
    if not macros:
        print("check_reserved_identifiers: the compiler listed no predefined "
              "macro", file=sys.stderr)
        return 2
    for name in sorted(macros):
        if not name.startswith("_") and name in identifiers:
            problems.append(f"{identifiers[name]}: {name} is a macro that "
                            f"{' '.join(cc)} predefines")

    if problems:
        print("check_reserved_identifiers: the library spells names that the "
              "implementation reserves:", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1
    print(f"check_reserved_identifiers: OK ({len(files)} files, "
          f"{len(identifiers)} identifiers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
