#!/usr/bin/env python3
"""lcsc_check.py — read stock, price and package of LCSC parts by their C-number.

Used for every board in hardware/ARTIST_BOARDS.md before a schematic is drawn or an order is made.
Reads only the public product-detail page data (no login, no search, no order), one part per second.

  python3 -I hardware/tools/lcsc_check.py C8963 C6186
  python3 -I hardware/tools/lcsc_check.py --from parts.md      # every C-number found in a file
  python3 -I hardware/tools/lcsc_check.py --json --from parts.md

Exit code 1 when a part is unknown or has less stock than --min-stock (default 1000): fails loudly.
Standard library only. LCSC's search endpoint refuses scripts; this tool never calls it.
"""
import argparse
import json
import re
import sys
import time
import urllib.request

DETAIL = "https://wmsc.lcsc.com/ftps/wm/product/detail?productCode={code}"
UA = "vizzz-parts-check/1 (+https://github.com/dob-0/vizzz.di)"


def fetch(code: str) -> dict:
    req = urllib.request.Request(DETAIL.format(code=code), headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=20) as resp:
        data = json.load(resp)
    r = data.get("result") or {}
    if not r:
        return {"code": code, "error": f"no result (code {data.get('code')})"}
    ladder = [(p.get("ladder"), p.get("usdPrice") or p.get("productPrice"))
              for p in (r.get("productPriceList") or [])]
    at10 = None
    for qty, price in ladder:  # the price that applies when buying 10
        if qty is not None and qty <= 10:
            at10 = price
    if at10 is None and ladder:
        at10 = ladder[0][1]
    return {
        "code": code,
        "model": r.get("productModel"),
        "brand": r.get("brandNameEn"),
        "stock": r.get("stockNumber"),
        "usd_at_10": at10,
        "min_buy": r.get("minBuyNumber"),
        "package": r.get("encapStandard"),
        "datasheet": r.get("pdfUrl"),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("codes", nargs="*", help="LCSC C-numbers")
    ap.add_argument("--from", dest="src", help="read every C-number found in this file")
    ap.add_argument("--min-stock", type=int, default=1000)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--all", action="store_true", help="with --from, also check lines that say 'rejected'")
    a = ap.parse_args()
    codes = list(a.codes)
    if a.src:
        with open(a.src, encoding="utf-8") as fh:
            for line in fh:
                # a design file keeps the parts it turned down on lines that say "rejected";
                # those are history, not the build, unless --all asks for them
                if a.all or "rejected" not in line.lower():
                    codes += re.findall(r"\bC\d{3,9}\b", line)
    codes = list(dict.fromkeys(codes))  # keep order, drop repeats
    if not codes:
        ap.error("no C-numbers given")
    rows, bad = [], 0
    for i, code in enumerate(codes):
        if i:
            time.sleep(1.0)
        try:
            row = fetch(code)
        except Exception as exc:  # network or JSON trouble is a failure, never a silent skip
            row = {"code": code, "error": str(exc)}
        if row.get("error") or (row.get("stock") or 0) < a.min_stock:
            bad += 1
            row["flag"] = "FAIL"
        rows.append(row)
    if a.json:
        print(json.dumps(rows, indent=1))
    else:
        for r in rows:
            if r.get("error"):
                print(f"FAIL {r['code']}: {r['error']}")
                continue
            print(f"{r.get('flag', 'ok  ')} {r['code']:>9} | {r['model']} | {r['brand']} | stock {r['stock']}"
                  f" | ${r['usd_at_10']} @10 | {r['package']}")
        print(f"{len(rows)} parts, {bad} FAIL (min stock {a.min_stock})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
