"""Send the GPL-3.0 §6 source request as a GitHub issue.

`docs/GPL_SOURCE_REQUEST.md` is the single source of truth: this script
parses the title and body out of it (the blocks tagged `<!-- ns:title -->`
and `<!-- ns:body -->`) and hands them to GitHub, so editing the document
edits the issue.

  python tools/send_gpl_request.py           open the prefilled form in the
                                             browser (you press Submit)
  python tools/send_gpl_request.py --print   print the prefilled URL
  python tools/send_gpl_request.py --body    print title + body only
  python tools/send_gpl_request.py --gh      create it via the gh CLI
                                             (asks for confirmation first)

Nothing is published without a human action: the browser path needs the
Submit click, the gh path needs a typed `yes`.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import urllib.parse
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "GPL_SOURCE_REQUEST.md"
REPO = "wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass"
NEW_ISSUE = f"https://github.com/{REPO}/issues/new"

# GitHub drops prefilled query strings that get too long; the body is ~3 KB
# encoded, so this only fires if someone grows the document a lot.
URL_WARN = 6000


def _utf8_stdout() -> None:
    """The console codepage here is not UTF-8; without this the pasteable
    body comes out with '?' where the section sign and the dashes were."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def _block(marker: str) -> str:
    """Text of the fenced block that follows `<!-- ns:<marker> -->`.

    The body block is fenced with four backticks because it contains a
    three-backtick block of its own, so the closing fence is matched
    against the opening one rather than assumed.
    """
    text = DOC.read_text(encoding="utf-8")
    idx = text.find(f"<!-- ns:{marker} -->")
    if idx < 0:
        raise SystemExit(f"{DOC}: marker ns:{marker} not found")
    rest = text[idx:]
    m = re.search(r"^(`{3,})[a-z]*\n", rest, re.M)
    if not m:
        raise SystemExit(f"{DOC}: no fenced block after ns:{marker}")
    fence = m.group(1)
    start = m.end()
    end = rest.find(f"\n{fence}\n", start)
    if end < 0:
        raise SystemExit(f"{DOC}: unterminated block after ns:{marker}")
    return rest[start:end].strip()


def _confirm(prompt: str) -> bool:
    try:
        return input(prompt).strip().lower() in ("y", "yes", "s", "si", "sí")
    except EOFError:
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--print", dest="print_url", action="store_true",
                    help="print the prefilled URL instead of opening it")
    ap.add_argument("--body", action="store_true",
                    help="print the title and body, for pasting by hand")
    ap.add_argument("--gh", action="store_true",
                    help="create the issue with the gh CLI (asks first)")
    args = ap.parse_args()
    _utf8_stdout()

    title = _block("title")
    body = _block("body")

    if args.body:
        print(f"TITLE: {title}\n\n{body}")
        return 0

    if args.gh:
        if not shutil.which("gh"):
            print("gh not found on PATH. Install it with:\n"
                  "  winget install --id GitHub.cli\n"
                  "then `gh auth login`, or just run this script without --gh.",
                  file=sys.stderr)
            return 2
        print(f"About to create a PUBLIC issue on {REPO} as your GitHub user:")
        print(f"  {title}")
        if not _confirm("Create it? [y/N] "):
            print("Nothing sent.")
            return 1
        out = subprocess.run(["gh", "issue", "create", "--repo", REPO,
                              "--title", title, "--body", body],
                             text=True, encoding="utf-8", errors="replace")
        return out.returncode

    url = (NEW_ISSUE + "?title=" + urllib.parse.quote(title)
           + "&body=" + urllib.parse.quote(body))
    if len(url) > URL_WARN:
        print(f"[warn] prefilled URL is {len(url)} chars; if GitHub opens the "
              f"form empty, use --body and paste by hand.", file=sys.stderr)

    if args.print_url:
        print(url)
        return 0

    print(f"Opening the prefilled issue form on {REPO}.")
    print("Review it and press 'Submit new issue' — this script does not post.")
    if not webbrowser.open(url):
        print("Could not open a browser; the URL is:\n" + url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
