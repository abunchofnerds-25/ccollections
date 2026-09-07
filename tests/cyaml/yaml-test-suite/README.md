This directory contains a pinned snapshot of the test cases from the
[YAML Test Suite](https://github.com/yaml/yaml-test-suite), tag
`v2022-01-17` (commit `45db50aecf9b1520f8258938c88f396e96f30831`), used by
`tests_spec_suite.c` to check cyaml's accept/reject behaviour against the
YAML 1.2 reference test corpus.

The upstream `src/*.yaml` meta-files (each a YAML document describing one or
more test cases, with fields such as `yaml`, `fail`, and `tree`) have been
pre-processed once, offline, into `cases/<id>/in.yaml` (the raw test input,
verbatim) and `cases/<id>/meta.txt` (`fail=0`/`fail=1` plus a `name=` line),
using PyYAML rather than cyaml itself, specifically so this test suite has no
dependency on cyaml's own parser to read its own input corpus. See
`LICENSE` for the upstream license (MIT).

Source files tagged `1.3-mod` or `1.3-err` (upstream's own convention for
cases specific to the still-draft YAML 1.3 spec, not YAML 1.2) are excluded
entirely at pre-processing time: cyaml implements YAML 1.2 only, so
comparing it against 1.3-specific expected behaviour is the wrong
comparison, not a real compliance gap. This excludes 52 of the upstream
source files, leaving 354 cases.
