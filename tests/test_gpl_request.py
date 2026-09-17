"""The GPL-3.0 source request stays sendable, no network needed.

The request in `docs/GPL_SOURCE_REQUEST.md` is what unblocks Phase 2b, and
`tools/send_gpl_request.py` parses that document rather than carrying its
own copy. So the thing that can silently break is the seam: a renamed
marker, a fence the body outgrew, an ask dropped in an edit. This checks
the seam and the prefilled URL; it never opens a browser and never posts.
"""
import sys
import urllib.parse
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE / "tools"))

import send_gpl_request as sender

failures = []


def check(cond, label):
    if not cond:
        failures.append(label)


def main() -> int:
    title = sender._block("title")
    body = sender._block("body")

    check(title.count("\n") == 0 and len(title) > 20, "title is one real line")
    check("GPL-3.0" in title, "title names the licence")

    # The five asks: without any one of them the request is not actionable.
    for ask in ("HIP kernel sources", "graph definition", "weight converter",
                "host glue", "Build instructions"):
        check(ask.lower() in body.lower(), f"body still asks for: {ask}")

    # The facts that make it verifiable rather than a vague complaint.
    for fact in ("DLSSNRW1", "153", "gfx1100", "amdhip64_7.dll", "§6",
                 "dlssnr_amd_pass0.dll", "RX 9070 XT"):
        check(fact in body, f"body keeps the evidence: {fact}")

    check("TODO" not in body and "XXX" not in body, "no placeholder left in body")

    # The body carries a fenced block of its own, so the outer fence must be
    # longer - if someone flattens it, the parser truncates the request.
    check("```" in body, "inner code block survived the parse")
    check(body.strip().endswith("."), "body ends whole, not mid-sentence")

    url = (sender.NEW_ISSUE + "?title=" + urllib.parse.quote(title)
           + "&body=" + urllib.parse.quote(body))
    parsed = urllib.parse.urlparse(url)
    query = urllib.parse.parse_qs(parsed.query)
    check(parsed.netloc == "github.com", "url points at github.com")
    check(parsed.path == "/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/new",
          "url points at the pack author's repo")
    check(query.get("title", [""])[0] == title, "title survives the round trip")
    check(query.get("body", [""])[0] == body, "body survives the round trip")
    check(len(url) < sender.URL_WARN,
          f"prefilled url still short enough ({len(url)} chars)")

    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print(f"OK: GPL request parses, {len(body)} chars, prefilled url "
          f"{len(url)} chars, target {parsed.path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
