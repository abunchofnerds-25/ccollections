#!/usr/bin/env python3
"""Differential testing harness: cyaml vs. PyYAML.

Runs cyaml_to_json (a standalone helper built from `make build_differential`
in this directory's parent) and PyYAML side by side against the same input,
compares the resulting values, and reports any disagreement.

This is not part of `make test`/`make memtest`; it is a manual/scheduled
tool, not a blocking pass/fail gate. Its inputs are the vendored YAML Test
Suite snapshot (../yaml-test-suite/cases/*/in.yaml) and the fuzz seed corpus
(../fuzz/corpus/*), both already checked into this tree for other purposes.

PyYAML implements YAML 1.1, not YAML 1.2 (this library's target), so a plain
yaml.safe_load() comparison would be dominated by well-known 1.1-vs-1.2
core-schema noise (yes/no/on/off as booleans, sexagesimal numbers, octal
without a 0o prefix, timestamp auto-resolution, ...) rather than genuine
cyaml bugs. Cyaml12CoreLoader below narrows PyYAML's *implicit* (untagged)
scalar resolution down to exactly the core schema cyaml.h itself documents,
so a reported mismatch is far more likely to be a real, actionable finding.
Explicit tags (!!str, !!binary, ...) are untouched here: cyaml resolves and
applies tags itself now (see cyaml.h), so a value-position tag is expected
to match PyYAML directly rather than diverge. The one documented exception
is a tag decorating a dictionary KEY, which cyaml parses for validity but
always discards (this DOM's keys are plain strings, not nodes, so a key has
nowhere to store a tag); PyYAML instead resolves a tagged key's own value
(e.g. a bare "!!null" key becomes the stringified key "null"), which is
reported as a known, deliberate deviation rather than a bug.
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
    """SafeLoader with implicit scalar resolution narrowed to match this
    library's own documented YAML 1.2 core schema (see cyaml.h's "Implicit
    typing" section)."""


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

# Merge keys (<<:) are a documented cyaml feature (see cyaml.h); keep
# PyYAML's own recognition of the key so both sides expand it the same way.
Cyaml12CoreLoader.add_implicit_resolver(
    "tag:yaml.org,2002:merge",
    re.compile(r"^(?:<<)$"),
    ["<"])


def to_jsonable(obj):
    """Mirrors cyaml_to_json.c's own JSON encoding exactly, including its
    NaN/Infinity sentinel strings, so both sides can be compared with a
    plain ==."""
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
    """cyaml's own dictionary storage is always char*-keyed: every scalar
    key is stored as its raw text, including a YAML-null key, stored as
    the literal text "null" (cyaml's own canonical text for a null value,
    matching what cyaml_serialize would emit for one), not Python's str(None).
    A non-string PyYAML key (e.g. an integer or bool scalar used as a
    mapping key) is stringified to match; a non-scalar key never reaches
    here, since PyYAML/Python cannot construct one as a dict key at all
    (see run_pyyaml's own handling of that case as an expected
    accept/reject divergence)."""
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
    # cyaml treats input as an opaque byte string and does not validate
    # UTF-8 well-formedness (see cyaml.h), so a non-UTF-8 input byte
    # sequence can make cyaml_to_json emit non-UTF-8 bytes in what is
    # otherwise meant to be UTF-8-encoded JSON; decode explicitly and
    # report that distinctly rather than letting UnicodeDecodeError
    # propagate uncaught and abort the whole comparison run.
    try:
        stdout_text = proc.stdout.decode("utf-8")
    except UnicodeDecodeError as e:
        return False, f"cyaml_to_json emitted non-UTF-8 output: {e}"
    try:
        return True, json.loads(stdout_text)
    except json.JSONDecodeError as e:
        return False, f"cyaml_to_json produced invalid JSON: {e}"


def run_pyyaml(data: bytes):
    """Returns (ok, value_or_error_message). Mirrors cyaml_parse's own
    documented multi-document convention: a single document is returned
    directly, more than one is wrapped in a list. A stream with no
    documents at all (empty input, or comment-only input) is the same
    "one empty document" case as an explicit empty document; PyYAML's
    own load_all() quirk of yielding zero items there is a generator
    artifact, not a real zero-documents result (yaml.load() on the same
    input returns None, not an error), so it is normalized to None rather
    than an empty list."""
    try:
        docs = list(yaml.load_all(data, Loader=Cyaml12CoreLoader))
    except yaml.YAMLError as e:
        return False, str(e)
    except Exception as e:
        # A malformed input can hit a genuine PyYAML-internal bug rather
        # than a normal, cleanly-reported yaml.YAMLError - e.g. a \U escape
        # naming a codepoint >= 0x110000 makes PyYAML's own scanner call
        # chr() on an out-of-range value (ValueError), or its regex-based
        # scalar resolver can match a value as boolean-looking while its
        # own construct_yaml_bool value-lookup table then rejects it
        # (KeyError); a larger fuzz corpus keeps finding further internal
        # PyYAML failure modes of this shape. Every one is reported the
        # same way as an ordinary pyyaml-side rejection, naming the actual
        # exception type for diagnostic purposes, rather than letting it
        # abort the whole comparison run; catching Exception rather than a
        # specific, growing list of exception types still lets
        # KeyboardInterrupt/SystemExit/GeneratorExit (none of which
        # subclass Exception) propagate normally.
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


# Value-level mismatches individually investigated (checked against a
# second reference parser, Ruby's Psych, before being accepted) and judged
# NOT to be cyaml bugs; see this project's own internal history notes for
# the full reasoning behind each one. Keyed by case_id() below. Unlike
# tests_spec_suite.c's own KNOWN_DEVIATIONS (which only covers
# accept/reject-level disagreement
# against the vendored suite itself), this dict is specifically about
# VALUE-level disagreement against PyYAML, so the two lists are not
# expected to overlap.
KNOWN_VALUE_DEVIATIONS = {
    "652Z": "reference-parser leniency: cyaml correctly treats a bare '?' "
            "glued to non-whitespace content as a plain scalar, not an "
            "explicit-key indicator (ns-plain-first(c) permits this; see "
            "at_valid_flow_plain_scalar_start()'s own doc comment, already "
            "verified against a reference parser). PyYAML/Psych are both "
            "more lenient than the strict grammar here.",
    "HM87-1": "same glued-'?' leniency as 652Z, sequence-element form.",
    "FH7J": "tag-on-key-discarded policy: cyaml parses a tag decorating a "
            "dictionary key for validity but always discards it (keys are "
            "plain strings, not nodes, so there is nowhere to store one), "
            "while PyYAML resolves a tagged key's own value (e.g. a bare "
            "'!!null' key becomes the stringified key 'null' rather than "
            "cyaml's raw, tag-stripped empty-string key).",
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
    """The suite's own case directory name for a yaml-test-suite input, or
    the filename (no extension) for a fuzz-corpus input; matches the keys
    in KNOWN_VALUE_DEVIATIONS above."""
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
