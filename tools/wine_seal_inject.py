#!/usr/bin/env python3
"""Plant the sealed test material (from wine_seal_test.exe / seal_out.txt) into
the Wine browser profiles, so the collector has genuinely encrypted passwords
and card numbers to decrypt.

Usage: python3 tools/wine_seal_inject.py [--prefix ~/.wine] [--user USER] [--seal seal_out.txt]
"""
import argparse
import base64
import json
import os
import sqlite3
import sys

BRANDS = ["Google/Chrome", "Microsoft/Edge", "BraveSoftware/Brave-Browser"]
BRANDS_LOCAL = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default=os.path.expanduser("~/.wine"))
    ap.add_argument("--user", default=os.environ.get("USER", "user"))
    ap.add_argument("--seal", default="seal_out.txt")
    args = ap.parse_args()

    if not os.path.isfile(args.seal):
        sys.exit(f"seal file not found: {args.seal} (run wine seal_test.exe first)")
    lines = open(args.seal).read().split()
    if len(lines) < 3:
        sys.exit("seal file malformed")
    key_b64, pass_b64, card_b64 = lines[0], lines[1], lines[2]
    pass_blob = base64.b64decode(pass_b64)
    card_blob = base64.b64decode(card_b64)

    root = os.path.join(args.prefix, "drive_c/users", args.user)
    local = os.path.join(root, "AppData/Local")
    changed = []

    for brand in BRANDS:
        ud = os.path.join(local, brand, "User Data")
        if not os.path.isdir(ud):
            continue
        # 1) Local State: the DPAPI-wrapped key
        ls_path = os.path.join(ud, "Local State")
        try:
            state = json.load(open(ls_path))
        except Exception:
            state = {}
        state.setdefault("os_crypt", {})
        state["os_crypt"]["encrypted_key"] = key_b64
        json.dump(state, open(ls_path, "w"))
        changed.append(f"{brand}: Local State key planted")

        # 2) Login Data: sealed passwords
        ldb = os.path.join(ud, "Default", "Login Data")
        if os.path.exists(ldb):
            con = sqlite3.connect(ldb)
            rows = con.execute("SELECT rowid FROM logins").fetchall()
            for (rowid,) in rows[:2]:
                con.execute("UPDATE logins SET password_value=? WHERE rowid=?",
                            (sqlite3.Binary(pass_blob), rowid))
            con.commit()
            con.close()
            changed.append(f"{brand}: {min(2, len(rows))} sealed passwords planted")

        # 3) Web Data: sealed card number (full PAN)
        wd = os.path.join(ud, "Default", "Web Data")
        if os.path.exists(wd):
            con = sqlite3.connect(wd)
            con.execute("UPDATE credit_cards SET card_number_encrypted=?",
                        (sqlite3.Binary(card_blob),))
            con.commit()
            con.close()
            changed.append(f"{brand}: sealed card number planted")

    if not changed:
        sys.exit("no browser profile found under the wine prefix")
    for c in changed:
        print("  ·", c)
    print(f"done — {len(changed)} updates")


if __name__ == "__main__":
    main()
