#!/usr/bin/env python3
"""Differential test of cyaml against PyYAML.

It feeds the same input to cyaml_to_json (a small helper that
`make build_differential` builds in the parent directory) and to PyYAML,
compares the two values and reports every disagreement.

This isn't part of `make test` or `make memtest`. It's a tool you run by hand
or on a schedule, and a mismatch needs a person to judge it, so it never
fails a build. Its inputs are the vendored YAML Test Suite snapshot
(../yaml-test-suite/cases/*/in.yaml) and the fuzz seed corpus
(../fuzz/corpus/*), which the tree already carries for other reasons.

PyYAML implements YAML 1.1, while this library targets YAML 1.2, so a plain
yaml.safe_load() comparison would drown in the well-known differences
between the two core schemas (yes/no/on/off as booleans, sexagesimal
numbers, octal without 0o, automatic timestamps, ...) instead of showing
cyaml bugs. Cyaml12CoreLoader below narrows PyYAML's implicit (untagged)
scalar resolution to exactly the core schema that cyaml.h documents, so a
mismatch it reports is usually worth a look.

Explicit tags (!!str, !!binary, ...) are left alone: cyaml resolves and
applies tags itself (see cyaml.h), so a tag on a value should agree with
PyYAML. The one documented exception is a tag on a dictionary KEY. cyaml
checks that it is valid and then drops it, because a key in this DOM is a
plain string and has nowhere to keep a tag, while PyYAML resolves the
tagged key (a bare "!!null" key becomes the key "null"). That case is
reported as a known deviation, not as a bug.
"""

import glob
import json
import math
import os
import re
import subprocess
import sys

import yaml

CYAML_TO_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "cyaml_to_json")


class Cyaml12CoreLoader(yaml.SafeLoader):
    """A SafeLoader whose implicit scalar resolution is narrowed to the YAML
    1.2 core schema that cyaml.h documents (its "Implicit typing" section),
    plus the YAML 1.2 rule for the non-specific tag."""

    def compose_scalar_node(self, anchor):
        # PyYAML reads a bare "!" as no tag at all, so "! 12" turns into
        # the integer 12 and a lone "!" into None. YAML 1.2 section 6.9.1
        # (Example 6.28) makes a scalar under the non-specific tag a string,
        # and that's what cyaml does. Without this, every input with a bare
        # "!" on a scalar would report a mismatch that is PyYAML's fault.
        event = self.peek_event()
        if event.tag == "!":
            event.tag = "tag:yaml.org,2002:str"
        return super().compose_scalar_node(anchor)


Cyaml12CoreLoader.yaml_implicit_resolvers = {}

Cyaml12CoreLoader.add_implicit_resolver(
    "tag:yaml.org,2002:null",
    re.compile(r"^(?:~|null|Null|NULL|)$"),
    ["~", "n", "N", ""])

Cyaml12CoreLoader.add_implicit_resolver(
    "tag:yaml.org,2002:bool",
    re.compile(r"^(?:true|True|TRUE|false|False|FALSE)$"),
    list("tTfF"))

Cyaml12CoreLoader.add_implicit_resolver(
    "tag:yaml.org,2002:int",
    re.compile(r"^(?:[-+]?[0-9]+|0x[0-9a-fA-F]+|0o[0-7]+)$"),
    list("-+0123456789"))

Cyaml12CoreLoader.add_implicit_resolver(
    "tag:yaml.org,2002:float",
    re.compile(r"""^(?:
        [-+]?(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?
      | [-+]?[0-9]+[eE][-+]?[0-9]+
      | [-+]?\.(?:inf|Inf|INF)
      | \.(?:nan|NaN|NAN)
    )$""", re.X),
    list("-+.0123456789"))

_LLONG_MAX = 2**63 - 1


def _construct_core_int(loader, node):
    """cyaml keeps an integer in a signed 64-bit long long. A decimal literal
    outside that range becomes a float of the same magnitude, and an
    unsigned 0x or 0o literal above LLONG_MAX stays a string, since the core
    schema gives hex and octal no float reading (cyaml.h, "Integer range").
    PyYAML builds an int of any size, so this maps both cases onto what
    cyaml documents."""
    text = loader.construct_scalar(node)
    if text[:2] in ("0x", "0o"):
        value = int(text[2:], 16 if text[1] == "x" else 8)
        return text if value > _LLONG_MAX else value
    value = int(text, 10)
    if value > _LLONG_MAX or value < -_LLONG_MAX - 1:
        return float(text)
    return value


Cyaml12CoreLoader.add_constructor("tag:yaml.org,2002:int", _construct_core_int)

# cyaml supports merge keys (<<:), see cyaml.h, so we keep PyYAML's
# recognition of that key and both sides expand it the same way.
Cyaml12CoreLoader.add_implicit_resolver(
    "tag:yaml.org,2002:merge",
    re.compile(r"^(?:<<)$"),
    ["<"])


def to_jsonable(obj):
    """Encodes a value exactly as cyaml_to_json.c does, NaN and Infinity
    sentinel strings included, so the two sides compare with a plain ==."""
    if obj is None or isinstance(obj, (bool, str)):
        return obj
    if isinstance(obj, int):
        return obj
    if isinstance(obj, float):
        if math.isnan(obj):
            return "__cyaml_nan__"
        if math.isinf(obj):
            return "__cyaml_inf__" if obj > 0 else "__cyaml_neg_inf__"
        return obj
    if isinstance(obj, list):
        return [to_jsonable(x) for x in obj]
    if isinstance(obj, dict):
        return {to_key_string(k): to_jsonable(v) for k, v in obj.items()}
    raise TypeError(f"unexpected pyyaml value type: {type(obj).__name__}")


def to_key_string(key):
    """cyaml always keys a dictionary by a char *: every scalar key is
    stored as its text, and a YAML null key as the text "null" (cyaml's
    canonical text for null, which is also what cyaml_serialize writes),
    not as Python's str(None). A PyYAML key that isn't a string, such as an
    integer or a bool, is turned into its text to match. A key that isn't a
    scalar never gets here, because Python can't use one as a dict key at
    all; run_pyyaml reports that case as an expected accept/reject
    difference."""
    if key is None:
        return "null"
    if isinstance(key, str):
        return key
    if isinstance(key, bool):
        return "true" if key else "false"
    return str(key)


def run_cyaml(data: bytes):
    """Returns (ok, value_or_error_message)."""
    proc = subprocess.run([CYAML_TO_JSON], input=data,
                           capture_output=True)
    if proc.returncode != 0:
        return False, proc.stderr.decode("utf-8", "replace").strip()
    # cyaml refuses input that isn't well-formed UTF-8 (see cyaml.h), so
    # cyaml_to_json shouldn't ever print bytes that aren't UTF-8. We decode
    # the output explicitly anyway and report that case on its own, because
    # an uncaught UnicodeDecodeError would end the whole comparison run.
    try:
        stdout_text = proc.stdout.decode("utf-8")
    except UnicodeDecodeError as e:
        return False, f"cyaml_to_json emitted non-UTF-8 output: {e}"
    try:
        return True, json.loads(stdout_text)
    except json.JSONDecodeError as e:
        return False, f"cyaml_to_json produced invalid JSON: {e}"


def run_pyyaml(data: bytes):
    """Returns (ok, value_or_error_message), following cyaml_parse's
    documented rule for several documents: one document comes back as it
    is, more than one comes back wrapped in a list. A stream with no
    documents at all (empty or only comments) counts as one empty document,
    just like an explicit empty one. PyYAML's load_all() yields nothing
    there, but that's a side effect of it being a generator (yaml.load() on
    the same input returns None), so we turn it into None and not into an
    empty list."""
    try:
        docs = list(yaml.load_all(data, Loader=Cyaml12CoreLoader))
    except yaml.YAMLError as e:
        return False, str(e)
    except Exception as e:
        # Malformed input can hit a real bug inside PyYAML instead of a
        # clean yaml.YAMLError. A \U escape that names a code point of
        # 0x110000 or more makes its scanner call chr() out of range
        # (ValueError), and its regex resolver can take a value for a bool
        # that the construct_yaml_bool table then doesn't know (KeyError).
        # A larger fuzz corpus finds more of these. We report each one like
        # any other PyYAML rejection, with the exception type in the message,
        # rather than let it end the run. Catching Exception and not a list
        # of types that would keep growing still lets KeyboardInterrupt,
        # SystemExit and GeneratorExit through, since none of them
        # subclasses Exception.
        return False, f"pyyaml raised {type(e).__name__}: {e}"
    if not docs:
        value = None
    elif len(docs) == 1:
        value = docs[0]
    else:
        value = docs
    try:
        return True, to_jsonable(value)
    except TypeError as e:
        return False, str(e)


# Value mismatches that are not cyaml bugs. Each one is checked against a
# second reference parser, Ruby's Psych, and its entry says why cyaml's
# reading is the right one. The keys come from case_id() below.
# KNOWN_DEVIATIONS in tests_spec_suite.c is about accept/reject differences
# against the vendored suite, while this dict is about VALUE differences
# against PyYAML, so the two lists don't overlap.
KNOWN_VALUE_DEVIATIONS = {
    "652Z": "reference-parser leniency: cyaml correctly treats a bare '?' "
            "glued to non-whitespace content as a plain scalar, not an "
            "explicit-key indicator (ns-plain-first(c) permits this; see "
            "at_valid_flow_plain_scalar_start()'s own doc comment, already "
            "verified against a reference parser). PyYAML/Psych are both "
            "more lenient than the strict grammar here.",
    "HM87-1": "same glued-'?' leniency as 652Z, sequence-element form.",
    "Y2GN": "anchor name containing ':' (\"&an:chor\"): PyYAML and Psych "
            "disagree with EACH OTHER here, not just with cyaml, so there "
            "is no reference-parser consensus to judge cyaml against; "
            "cyaml's own reading (the whole 'an:chor' is one anchor name, "
            "per ns-anchor-char excluding only c-flow-indicator "
            "characters, not ':') is spec-defensible.",
    "from_tests_fe6d54f84464": "cyaml correctly combines a UTF-16 "
                               "surrogate pair (\\uD83D\\uDE00) into one "
                               "codepoint; PyYAML keeps the two lone "
                               "surrogates unpaired and Psych rejects the "
                               "input outright, both narrower than "
                               "cyaml's own, more complete handling.",
}

def case_id(path):
    """The case directory name for a yaml-test-suite input, or the file
    name without its extension for a fuzz corpus input, which is how
    KNOWN_VALUE_DEVIATIONS above is keyed."""
    base = os.path.basename(os.path.dirname(path))
    if base and base != "corpus":
        return base
    return os.path.splitext(os.path.basename(path))[0]


def compare_file(path):
    with open(path, "rb") as f:
        data = f.read()
    cyaml_ok, cyaml_val = run_cyaml(data)
    py_ok, py_val = run_pyyaml(data)
    if not cyaml_ok and not py_ok:
        return "both_reject", None
    if cyaml_ok != py_ok:
        return "accept_reject_mismatch", (cyaml_ok, cyaml_val, py_ok, py_val)
    if cyaml_val != py_val:
        if case_id(path) in KNOWN_VALUE_DEVIATIONS:
            return "known_value_deviation", (cyaml_val, py_val)
        return "value_mismatch", (cyaml_val, py_val)
    return "match", None


def collect_inputs():
    base = os.path.dirname(os.path.abspath(__file__))
    paths = []
    suite_dir = os.path.join(base, "..", "yaml-test-suite", "cases")
    for case_dir in sorted(glob.glob(os.path.join(suite_dir, "*"))):
        in_yaml = os.path.join(case_dir, "in.yaml")
        if os.path.isfile(in_yaml):
            paths.append(in_yaml)
    corpus_dir = os.path.join(base, "..", "fuzz", "corpus")
    for p in sorted(glob.glob(os.path.join(corpus_dir, "*"))):
        if os.path.isfile(p):
            paths.append(p)
    return paths


def main():
    if not os.path.isfile(CYAML_TO_JSON):
        print(f"error: {CYAML_TO_JSON} not found; "
              f"run `make build_differential` in the parent directory first",
              file=sys.stderr)
        return 2

    paths = collect_inputs()
    if not paths:
        print("error: no input files found", file=sys.stderr)
        return 2

    counts = {"match": 0, "both_reject": 0, "known_value_deviation": 0,
              "accept_reject_mismatch": 0, "value_mismatch": 0}
    mismatches = []
    known = []
    for path in paths:
        outcome, detail = compare_file(path)
        counts[outcome] += 1
        if outcome in ("accept_reject_mismatch", "value_mismatch"):
            mismatches.append((path, outcome, detail))
        elif outcome == "known_value_deviation":
            known.append(path)

    print(f"Checked {len(paths)} inputs: {counts['match']} value-matched, "
          f"{counts['both_reject']} both-rejected, "
          f"{counts['known_value_deviation']} known value deviations, "
          f"{counts['accept_reject_mismatch']} accept/reject mismatches, "
          f"{counts['value_mismatch']} unexpected value mismatches.")

    unused = set(KNOWN_VALUE_DEVIATIONS) - {case_id(p) for p in known}
    if unused:
        print(f"\nerror: {len(unused)} stale KNOWN_VALUE_DEVIATIONS "
              f"entries no longer reproduce and must be removed: "
              f"{', '.join(sorted(unused))}", file=sys.stderr)

    for path, outcome, detail in mismatches:
        rel = os.path.relpath(path, os.path.dirname(os.path.abspath(__file__)))
        print(f"\n[{outcome}] {rel}")
        if outcome == "accept_reject_mismatch":
            cyaml_ok, cyaml_val, py_ok, py_val = detail
            cyaml_desc = f"accepted -> {cyaml_val!r}" if cyaml_ok else f"rejected: {cyaml_val}"
            py_desc = f"accepted -> {py_val!r}" if py_ok else f"rejected: {py_val}"
            print(f"  cyaml:  {cyaml_desc}")
            print(f"  pyyaml: {py_desc}")
        else:
            cyaml_val, py_val = detail
            print(f"  cyaml:  {cyaml_val!r}")
            print(f"  pyyaml: {py_val!r}")

    return 1 if (mismatches or unused) else 0


if __name__ == "__main__":
    sys.exit(main())
