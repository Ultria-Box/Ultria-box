#!/usr/bin/env python3
"""
VirusTotal check for Ultria builds.
Usage:
    python3 tools/vt_check.py dist/ultria.exe            # lookup by SHA256
    python3 tools/vt_check.py dist/ultria.exe --submit   # upload if unknown
    python3 tools/vt_check.py dist/ultria.exe --watch    # re-check every 30s until analysis done
    VT_API_KEY env var (or --key) required.
Free API key: https://www.virustotal.com/gui/my-apikey (4 req/min limit on free tier)
"""
import argparse
import hashlib
import json
import os
import sys
import time
import urllib.request
import urllib.error

API = "https://www.virustotal.com/api/v3"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def api_get(url, key):
    req = urllib.request.Request(url, headers={"x-apikey": key})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def api_upload(path, key):
    boundary = "----ultriaform"
    with open(path, "rb") as f:
        data = f.read()
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{os.path.basename(path)}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + data + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request(
        f"{API}/files", data=body,
        headers={"x-apikey": key, "Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)


def report(hash_, key):
    j = api_get(f"{API}/files/{hash_}", key)
    if j is None:
        return None
    attrs = j["data"]["attributes"]
    stats = attrs.get("last_analysis_stats", {})
    results = attrs.get("last_analysis_results", {})
    flagged = {name: r["result"] for name, r in results.items()
               if r.get("category") in ("malicious", "suspicious")}
    return {
        "stats": stats,
        "flagged": flagged,
        "names": (attrs.get("popular_threat_classification") or {}).get("suggested_threat_label"),
        "link": f"https://www.virustotal.com/gui/file/{hash_}",
        "status": attrs.get("last_analysis_stats") is not None,
    }


def print_report(rep):
    s = rep["stats"]
    total = sum(s.values()) if s else 0
    print(f"  malicious:  {s.get('malicious', 0)}/{total}")
    print(f"  suspicious: {s.get('suspicious', 0)}/{total}")
    print(f"  undetected: {s.get('undetected', 0)}/{total}")
    if rep.get("names"):
        print(f"  label: {rep['names']}")
    if rep["flagged"]:
        print("  engines flagging:")
        for name, sig in sorted(rep["flagged"].items()):
            print(f"    - {name}: {sig}")
    print(f"  link: {rep['link']}")
    clean = s.get("malicious", 0) == 0 and s.get("suspicious", 0) == 0
    print(f"  verdict: {'CLEAN' if clean else 'DETECTED'}")
    return clean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--key", default=os.environ.get("VT_API_KEY", ""))
    ap.add_argument("--submit", action="store_true", help="upload if hash unknown")
    ap.add_argument("--watch", action="store_true", help="poll until analysis completes")
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        sys.exit(f"no such file: {args.binary}")
    if not args.key:
        sys.exit("VT_API_KEY not set (get one free: https://www.virustotal.com/gui/my-apikey)")

    h = sha256_of(args.binary)
    size = os.path.getsize(args.binary)
    print(f"[vt] {args.binary} ({size} bytes)")
    print(f"[vt] sha256: {h}")

    rep = report(h, args.key)
    if rep is None:
        if not args.submit:
            print("[vt] hash unknown to VirusTotal (run with --submit to upload)")
            sys.exit(2)
        print("[vt] uploading...")
        up = api_upload(args.binary, args.key)
        analysis_id = up["data"]["id"]
        print(f"[vt] analysis queued: {analysis_id}")
        if args.watch:
            for _ in range(40):
                time.sleep(30)
                j = api_get(f"{API}/analyses/{analysis_id}", args.key)
                if j and j["data"]["attributes"]["status"] == "completed":
                    break
            rep = report(h, args.key)

    if rep is None:
        print("[vt] still processing; re-run later with the same command")
        sys.exit(3)

    if args.watch and not rep["status"]:
        for _ in range(20):
            time.sleep(30)
            rep = report(h, args.key)
            if rep and rep["status"]:
                break

    clean = print_report(rep)
    sys.exit(0 if clean else 1)


if __name__ == "__main__":
    main()
