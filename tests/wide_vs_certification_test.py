"""Gate for the wide-varying VS certification record.

``tools/vs_wide_certify.py`` proves, per emitted-verified VS that reads
TEXCOORD1..4, whether a paired runtime PS exists and samples the same
channels.  The committed record must stay honest: no program with a non-``ok``
verdict may be draw-enabled, and every entry must carry a 256-sample numeric
proof.
"""

from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
REPORT = os.path.join(REPO, "source", "shader_packs",
                      "wide_vs_certification.json")


def main() -> int:
    checks = 0
    failures = []

    def check(condition, text):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(text)

    report = json.load(open(REPORT, encoding="utf-8"))
    entries = report.get("entries", [])
    check(report.get("schema") == "wide-vs-certification/1", "schema")
    check(len(entries) == 15, "certified %d programs, expected 15"
          % len(entries))
    check(report.get("ok") == sum(1 for e in entries
                                  if e.get("verdict") == "ok"),
          "ok counter disagrees with the entries")

    for entry in entries:
        label = entry.get("id")
        check(entry.get("verdict"), "%s has no verdict" % label)
        check(bool(entry.get("reason")), "%s has no reason" % label)
        check((entry.get("numeric") or {}).get("samples", 0) ==
              report.get("samples"),
              "%s numeric sample count" % label)
        numeric = entry.get("numeric") or {}
        check(numeric.get("samples") == 256, "%s is not a 256-sample proof"
              % label)
        check(numeric.get("status") == "pass", "%s numeric status" % label)
        check(numeric.get("max_abs_position", 1.0) <= 1e-4,
              "%s position differential" % label)
        check(numeric.get("max_abs_varying", 1.0) <= 1e-4,
              "%s varying differential" % label)
        if entry.get("draw_enabled") == 1:
            check(entry.get("verdict") == "ok",
                  "%s is draw-enabled with verdict %r"
                  % (label, entry.get("verdict")))
    # The current evidence says none of the 15 has a provable pair.  If a
    # future change flips one, this test forces a review of that claim.
    check(report.get("ok") == 0,
          "%d programs are now promotable; review and promote them"
          % report.get("ok"))

    print("wide_vs_certification_test: %d checks, %d failures (%d programs, "
          "%d promotable)" % (checks, len(failures), len(entries),
                              report.get("ok")))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
