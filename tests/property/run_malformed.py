"""Run the malformed-corpus property test.

Properties checked per file (each in its own sanitizer-instrumented process):
  1. zero crashes     - process must not die by signal / sanitizer abort
  2. zero out-of-bounds - no ASan/UBSan diagnostics
  3. zero leaks       - no LeakSanitizer report at exit
Every failure is copied to counterexamples/ together with the mutation
offsets from malformed/manifest.json.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(ROOT, "bin", "fuzz_one")
MAL = os.path.join(ROOT, "malformed")
CE = os.path.join(ROOT, "counterexamples")

ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = ("detect_leaks=1:allocator_may_return_null=1:"
                       "abort_on_error=1:exitcode=99:hard_rss_limit_mb=8192")
ENV["UBSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1:print_stacktrace=1"

SAN_KEYS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
            "runtime error:", "SUMMARY: AddressSanitizer",
            "SUMMARY: LeakSanitizer", "HARNESS-FAIL")


def classify(rc, stderr):
    if any(k in stderr for k in SAN_KEYS):
        if "LeakSanitizer" in stderr:
            return "leak"
        if "AddressSanitizer" in stderr:
            return "oob/crash(asan)"
        if "runtime error:" in stderr:
            return "undefined-behavior"
        return "harness-exception"
    if rc == 124:
        return "timeout"
    if rc != 0:
        return "crash(exit=%d)" % rc
    return None


def main():
    with open(os.path.join(MAL, "manifest.json")) as f:
        manifest = json.load(f)
    os.makedirs(CE, exist_ok=True)
    results = []
    failures = []
    for entry in manifest:
        path = os.path.join(MAL, entry["file"])
        try:
            p = subprocess.run([BIN, path], env=ENV, capture_output=True,
                               text=True, timeout=120)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            rc, out, err = 124, "", "timeout after 120s"
        fail = classify(rc, err)
        rec = dict(entry)
        rec["result"] = out.strip()
        rec["failure"] = fail
        results.append(rec)
        if fail:
            dst = os.path.join(CE, entry["file"])
            shutil.copy(path, dst)
            with open(dst + ".json", "w") as f:
                json.dump(dict(entry, failure=fail, stderr=err[-4000:]),
                          f, indent=1)
            failures.append(rec)
            print("FAIL %-18s %-22s %s" % (entry["file"], entry["mutation"],
                                           fail))
    with open(os.path.join(ROOT, "malformed_results.json"), "w") as f:
        json.dump(results, f, indent=1)

    total = len(results)
    print("\n=== malformed corpus summary ===")
    print("total: %d  clean: %d  failures: %d" %
          (total, total - len(failures), len(failures)))
    by_kind = {}
    for r in results:
        key = r["mutation"]
        by_kind.setdefault(key, [0, 0])
        by_kind[key][0] += 1
        by_kind[key][1] += 1 if r["failure"] else 0
    for k in sorted(by_kind):
        n, bad = by_kind[k]
        print("  %-24s %3d files, %d failures" % (k, n, bad))
    if failures:
        print("\ncounterexamples (with mutation offsets):")
        for r in failures:
            offs = ", ".join("0x%x(%s)" % (o["offset"], o.get("tag", "-"))
                             for o in r["offsets"])
            print("  %s [%s] %s offsets: %s" %
                  (r["file"], r["failure"], r["mutation"], offs))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
