#!/usr/bin/env python3
"""Populate a Wine prefix with realistic artifacts so every collector module
has something to find. Test-only tool: creates fake but structurally valid
profiles (Chromium SQLite databases, Gecko profile, wallets, tdata, FileZilla,
Signal, Slack, SSH keys, git credentials, documents...).

Usage:
    python3 tools/wine_testenv.py [--prefix ~/.wine] [--user tester]
"""
import argparse
import base64
import json
import os
import sqlite3
import sys

APPDATA = "AppData/Roaming"
LOCALAPP = "AppData/Local"
USER = "drive_c/users"

created = []


def w(base, *parts):
    p = os.path.join(base, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def write(base, rel, data, mode=None):
    p = w(base, rel)
    if isinstance(data, str):
        data = data.encode("utf-8")
    with open(p, "wb") as f:
        f.write(data)
    if mode is not None:
        os.chmod(p, mode)
    created.append(rel)


def sqlite_db(base, rel, statements):
    p = w(base, rel)
    if os.path.exists(p):
        os.remove(p)
    con = sqlite3.connect(p)
    for s in statements:
        con.execute(s)
    con.commit()
    con.close()
    created.append(rel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default=os.path.expanduser("~/.wine"))
    ap.add_argument("--user", default="tester")
    args = ap.parse_args()

    root = os.path.join(args.prefix, USER, args.user)
    if not os.path.isdir(root):
        sys.exit(f"wine user dir not found: {root}")

    app = os.path.join(root, APPDATA)
    loc = os.path.join(root, LOCALAPP)
    desk = os.path.join(root, "Desktop")
    docs = os.path.join(root, "Documents")
    down = os.path.join(root, "Downloads")

    # ---------------- Chromium family ----------------
    for brand, sub in [("Google/Chrome", ""), ("Microsoft/Edge", ""), ("BraveSoftware/Brave-Browser", "")]:
        ud = os.path.join(loc, brand, "User Data")
        # a plausible (but meaningless) DPAPI-wrapped key blob
        fake_key = base64.b64encode(b"DPAPI" + os.urandom(32)).decode()
        write(args.prefix, os.path.relpath(os.path.join(ud, "Local State"), args.prefix),
              json.dumps({"os_crypt": {"encrypted_key": fake_key, "audit_enabled": True}}))
        prof = os.path.join(ud, "Default")
        sqlite_db(args.prefix, os.path.relpath(os.path.join(prof, "Login Data"), args.prefix), [
            "CREATE TABLE logins (origin_url TEXT, username_value TEXT, password_value BLOB, "
            "date_created INTEGER, blacklisted_by_user INTEGER)",
            "INSERT INTO logins VALUES ('https://accounts.snapchat.com/v2/password','jane.doe@example.com',"
            "X'76313000000000000000000000000000000000000000000000000000',13370000000000000,0)",
            "INSERT INTO logins VALUES ('https://ubereats.com/login','marc.dubois@orange.fr',"
            "X'76313000000000000000000000000000000000000000000000000000',13370000000000000,0)",
        ])
        sqlite_db(args.prefix, os.path.relpath(os.path.join(prof, "Network", "Cookies"), args.prefix), [
            "CREATE TABLE cookies (host_key TEXT, name TEXT, encrypted_value BLOB, path TEXT, "
            "expires_utc INTEGER, is_secure INTEGER, is_httponly INTEGER)",
            "INSERT INTO cookies VALUES ('.netflix.com','NetflixId',X'76313000000000000000000000000000000000',"
            "'/',13400000000000000,1,1)",
            "INSERT INTO cookies VALUES ('.spotify.com','sp_dc',X'76313000000000000000000000000000000000',"
            "'/',13400000000000000,1,1)",
        ])
        sqlite_db(args.prefix, os.path.relpath(os.path.join(prof, "Web Data"), args.prefix), [
            "CREATE TABLE autofill (name TEXT, value TEXT, count INTEGER)",
            "INSERT INTO autofill VALUES ('email','jane.doe@example.com',3)",
            "CREATE TABLE credit_cards (name_on_card TEXT, expiration_month INTEGER, expiration_year INTEGER, "
            "card_number_encrypted BLOB, nickname TEXT, guid TEXT)",
            "INSERT INTO credit_cards VALUES ('JANE DOE', 11, 2029, X'76313000000000000000000000000000000000000000000000000000000000', 'perso', 'g1')",
            "INSERT INTO credit_cards VALUES ('J DOE PRO', 4, 2028, X'76313000000000000000000000000000000000000000000000000000000000', 'travail', 'g2')",
        ])
        sqlite_db(args.prefix, os.path.relpath(os.path.join(prof, "History"), args.prefix), [
            "CREATE TABLE urls (id INTEGER PRIMARY KEY, url TEXT, title TEXT, visit_count INTEGER)",
            "INSERT INTO urls VALUES (1,'https://mail.example.com/','Webmail',14)",
            "CREATE TABLE downloads (id INTEGER PRIMARY KEY, target_path TEXT, tab_url TEXT, total_bytes INTEGER)",
            "INSERT INTO downloads VALUES (1,'C:\\Users\\tester\\Downloads\\statement.pdf',"
            "'https://bank.example.com/statement',204800)",
        ])
        write(args.prefix, os.path.relpath(os.path.join(prof, "Bookmarks"), args.prefix),
              json.dumps({"roots": {"bookmark_bar": {"children": [
                  {"type": "url", "name": "Webmail", "url": "https://mail.example.com/"},
                  {"type": "url", "name": "Bank", "url": "https://bank.example.com/"},
              ]}}}))

    # ---------------- Gecko ----------------
    prof = os.path.join(app, "Mozilla", "Firefox", "Profiles", "ab12cd34.default-release")
    write(args.prefix, os.path.relpath(os.path.join(prof, "logins.json"), args.prefix),
          json.dumps({"logins": [
              {"hostname": "https://accounts.firefox.com", "encryptedUsername": "MDoEEPgAAAAA",
               "encryptedPassword": "MDoEEPgAAAAA", "timeCreated": 1337000000000},
              {"hostname": "https://twitter.com", "encryptedUsername": "MDoEEPgAAAAA",
               "encryptedPassword": "MDoEEPgAAAAA", "timeCreated": 1337000000000},
          ]}))
    write(args.prefix, os.path.relpath(os.path.join(prof, "key4.db"), args.prefix), os.urandom(4096))
    write(args.prefix, os.path.relpath(os.path.join(prof, "cert9.db"), args.prefix), os.urandom(2048))
    sqlite_db(args.prefix, os.path.relpath(os.path.join(prof, "cookies.sqlite"), args.prefix), [
        "CREATE TABLE moz_cookies (id INTEGER PRIMARY KEY, host TEXT, name TEXT, value TEXT, "
        "path TEXT, expiry INTEGER, isSecure INTEGER, isHttpOnly INTEGER)",
        "INSERT INTO moz_cookies VALUES (1,'.github.com','logged_in','yes','/',1340000000,1,1)",
    ])
    sqlite_db(args.prefix, os.path.relpath(os.path.join(prof, "places.sqlite"), args.prefix), [
        "CREATE TABLE moz_places (id INTEGER PRIMARY KEY, url TEXT, title TEXT, visit_count INTEGER)",
        "INSERT INTO moz_places VALUES (1,'https://news.ycombinator.com/','Hacker News',41)",
    ])

    # ---------------- Telegram ----------------
    td = os.path.join(app, "Telegram Desktop", "tdata")
    write(args.prefix, os.path.relpath(os.path.join(td, "key_datas"), args.prefix), os.urandom(256))
    write(args.prefix, os.path.relpath(os.path.join(td, "D877F783D5D3EF8C"), args.prefix), os.urandom(512))
    write(args.prefix, os.path.relpath(os.path.join(td, "D877F783D5D3EF8C1"), args.prefix), os.urandom(512))
    write(args.prefix, os.path.relpath(os.path.join(td, "settings"), args.prefix),
          json.dumps({"theme": "dark"}))

    # ---------------- Discord (leveldb with a fake token) ----------------
    ldb = os.path.join(app, "discord", "Local Storage", "leveldb")
    fake_token = ("AAAAAAAAAAAAAAAAAAAAAAAA.Gh1jKl."
                  "BBBBBBBBBBBBBBBBBBBBBBBBBBB")
    write(args.prefix, os.path.relpath(os.path.join(ldb, "000005.ldb"), args.prefix),
          ("\x00_\x00token\x01" + fake_token + "\x02").encode("utf-8", "surrogateescape"))
    write(args.prefix, os.path.relpath(os.path.join(ldb, "000010.log"), args.prefix),
          f"2026/07/14 20:11:03 token={fake_token}\n")

    # ---------------- Wallets ----------------
    exo = os.path.join(app, "Exodus", "exodus.wallet")
    write(args.prefix, os.path.relpath(os.path.join(exo, "passphrase.json"), args.prefix),
          json.dumps({"version": 1, "data": base64.b64encode(os.urandom(64)).decode()}))
    write(args.prefix, os.path.relpath(os.path.join(exo, "seed.seco"), args.prefix), os.urandom(128))
    write(args.prefix, os.path.relpath(os.path.join(exo, "info.seco"), args.prefix), os.urandom(96))
    write(args.prefix, os.path.relpath(os.path.join(app, "Electrum", "wallets", "default_wallet"),
                                       args.prefix), os.urandom(1024))
    write(args.prefix, os.path.relpath(os.path.join(app, "Atomic", "Local Storage", "leveldb", "000003.log"),
                                       args.prefix), os.urandom(512))
    write(args.prefix, os.path.relpath(os.path.join(root, "Documents", "Bitcoin", "wallet.dat"),
                                       args.prefix), os.urandom(4096))
    write(args.prefix, os.path.relpath(os.path.join(app, ".ethereum", "keystore",
                                                    "UTC--2026-01-01T00-00-00.000Z--abcdef"),
                                       args.prefix),
          json.dumps({"address": "abcdef", "crypto": {"cipher": "aes-128-ctr",
                                                      "ciphertext": "00" * 32}}))
    write(args.prefix, os.path.relpath(os.path.join(app, "Ledger Live", "config.json"), args.prefix),
          json.dumps({"dataFolder": "C:\\Users\\tester\\AppData\\Roaming\\Ledger Live"}))

    # ---------------- Windows registries for Steam / PuTTY ----------------
    regs = [
        ('HKCU\\Software\\Valve\\Steam', 'SteamPath', 'C:\\Program Files (x86)\\Steam'),
        ('HKCU\\Software\\Valve\\Steam', 'SteamExe', 'C:\\Program Files (x86)\\Steam\\steam.exe'),
        ('HKCU\\Software\\Valve\\Steam', 'AutoLoginUser', 'tester'),
        ('HKCU\\Software\\SimonTatham\\PuTTY\\Sessions\\prod-db', 'HostName', 'db.example.com'),
        ('HKCU\\Software\\SimonTatham\\PuTTY\\Sessions\\prod-db', 'UserName', 'deploy'),
    ]
    print("registering:", " | ".join(f"{k}\\{n}" for k, n, _ in regs))

    # ---------------- FileZilla / WinSCP / PuTTY files ----------------
    write(args.prefix, os.path.relpath(os.path.join(app, "FileZilla", "sitemanager.xml"), args.prefix),
          """<?xml version="1.0"?><FileZilla3><Servers><Server>
<Host>ftp.example.com</Host><Port>21</Port><User>webmaster</User>
<Pass encoding="base64">c2VjcmV0cGFzcw==</Pass>
</Server></Servers></FileZilla3>""")
    write(args.prefix, os.path.relpath(os.path.join(app, "FileZilla", "recentservers.xml"), args.prefix),
          """<?xml version="1.0"?><FileZilla3><RecentServers><Server>
<Host>backup.example.net</Host><User>admin</User>
<Pass encoding="base64">YmFja3VwMTIz</Pass>
</Server></RecentServers></FileZilla3>""")
    write(args.prefix, os.path.relpath(os.path.join(app, "WinSCP.ini"), args.prefix),
          "[Sessions\\prod]\\nHostName=sftp.example.com\\nUserName=deploy\\n")
    write(args.prefix, os.path.relpath(os.path.join(app, "PuTTY", "keys", "id_ppk.ppk"), args.prefix),
          "PuTTY-User-Key-File-2: ssh-rsa\nEncryption: none\n")

    # ---------------- Mail ----------------
    tb = os.path.join(app, "Thunderbird", "Profiles", "mail.default")
    write(args.prefix, os.path.relpath(os.path.join(tb, "logins.json"), args.prefix),
          json.dumps({"logins": [{"hostname": "imaps://imap.example.com",
                                  "encryptedUsername": "MDoEEPgAAAAA",
                                  "encryptedPassword": "MDoEEPgAAAAA"}]}))
    write(args.prefix, os.path.relpath(os.path.join(tb, "key4.db"), args.prefix), os.urandom(2048))
    write(args.prefix, os.path.relpath(os.path.join(tb, "prefs.js"), args.prefix),
          'user_pref("mail.account.account1.identities", "id1");\n')

    # ---------------- VPN ----------------
    ovpn = os.path.join(app, "OpenVPN Connect", "profiles")
    write(args.prefix, os.path.relpath(os.path.join(ovpn, "corp.ovpn"), args.prefix),
          "client\ndev tun\nremote vpn.example.com 1194\n<auth-user-pass>\nuser1\n</auth-user-pass>\n")
    write(args.prefix, os.path.relpath(os.path.join(app, "ProtonVPN", "Settings.json"), args.prefix),
          json.dumps({"vpn": {"server": "auto"}}))
    write(args.prefix, os.path.relpath(os.path.join(app, "NordVPN", "NordVPN.exe"), args.prefix), "stub")

    # ---------------- Password managers ----------------
    write(args.prefix, os.path.relpath(os.path.join(app, "Bitwarden", "data.json"), args.prefix),
          json.dumps({"global_environment": "https://vault.bitwarden.com",
                      "user_email": "jane.doe@example.com"}))
    write(args.prefix, os.path.relpath(os.path.join(docs, "vault-export.kdbx"), args.prefix),
          os.urandom(2048))
    write(args.prefix, os.path.relpath(os.path.join(app, "1Password", "1Password.opvault", "default", "profile.js"),
                                       args.prefix), json.dumps({"uuid": "abc"}))

    # ---------------- Socials / dev credentials ----------------
    write(args.prefix, os.path.relpath(os.path.join(app, "Slack", "storage.json"), args.prefix),
          json.dumps({"teams": {"T0001": {"name": "acme", "token": "xoxc-0000-0000"}}}))
    write(args.prefix, os.path.relpath(os.path.join(app, "Slack", "Local Storage", "leveldb", "000005.ldb"),
                                       args.prefix), "workspace-token\x01xoxp-fake\x02")
    write(args.prefix, ".gitconfig",
          "[user]\n\tname = Jane Doe\n\temail = jane.doe@example.com\n")
    write(args.prefix, ".git-credentials",
          "https://jane:ghp_EXAMPLE_not_a_real_token_0000000000@github.com\n")
    write(args.prefix, os.path.relpath(os.path.join(app, "npm", ".npmrc"), args.prefix),
          "//registry.npmjs.org/:_authToken=npm_AAAAAAAAAAAAAAAAAAAAAAAAAAAA\n")
    write(args.prefix, os.path.relpath(os.path.join(app, ".aws", "credentials"), args.prefix),
          "[default]\naws_access_key_id = AWS_EXAMPLE_KEY_NOT_REAL\n"
          "aws_secret_access_key = wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY\n")

    # ---------------- Games ----------------
    mc = os.path.join(app, ".minecraft")
    write(args.prefix, os.path.relpath(os.path.join(mc, "launcher_accounts.json"), args.prefix),
          json.dumps({"accounts": {"abc": {"accessToken": "0" * 32,
                                           "minecraftProfile": {"name": "JanePlays"}}}}))
    write(args.prefix, os.path.relpath(os.path.join(mc, "options.txt"), args.prefix), "fov:0.5\n")
    write(args.prefix, os.path.relpath(os.path.join(mc, "servers.dat"), args.prefix), os.urandom(256))
    write(args.prefix, os.path.relpath(os.path.join(loc, "EpicGamesLauncher", "Saved", "Config",
                                                    "Windows", "GameUserSettings.ini"), args.prefix),
          "[/Script/Engine.GameUserSettings]\n")

    # ---------------- Steam files ----------------
    st = os.path.join(root, "Program Files (x86)", "Steam")
    write(args.prefix, os.path.relpath(os.path.join(st, "config", "loginusers.vdf"), args.prefix),
          '"users"\n{\n\t"76561198000000000"\n\t{\n\t\t"AccountName"\t\t"tester"\n'
          '\t\t"PersonaName"\t\t"JanePlays"\n\t\t"RememberPassword"\t\t"1"\n\t}\n}\n')
    write(args.prefix, os.path.relpath(os.path.join(st, "config", "config.vdf"), args.prefix),
          '"InstallConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
          '\t\t\t\t"AutoLoginUser"\t\t"tester"\n\t\t\t}\n\t\t}\n\t\t"ctest"\n\t\t{\n\t\t\t"SteamDefaultDialog"\t\t"#app_games"\n\t\t}\n\t}\n}\n')
    write(args.prefix, os.path.relpath(os.path.join(st, "ssfn123456789"), args.prefix), os.urandom(64))
    write(args.prefix, os.path.relpath(os.path.join(st, "userdata", "12345678", "config", "localconfig.vdf"), args.prefix),
          '"UserLocalConfigStore"\n{\n}\n')

    # ---------------- Messengers ----------------
    write(args.prefix, os.path.relpath(os.path.join(app, "Signal", "config.json"), args.prefix),
          json.dumps({"accountE164": "+33600000000"}))
    sqlite_db(args.prefix, os.path.relpath(os.path.join(app, "Signal", "sql", "db.sqlite"), args.prefix), [
        "CREATE TABLE conversations (id INTEGER PRIMARY KEY, serviceId TEXT, json TEXT)",
        "INSERT INTO conversations VALUES (1,'+33600000000','{}')",
    ])
    write(args.prefix, os.path.relpath(os.path.join(root, ".purple", "accounts.xml"), args.prefix),
          "<accounts><account><protocol>prpl-irc</protocol><name>jane</name></account></accounts>")
    write(args.prefix, os.path.relpath(os.path.join(app, "Tox", "tox_save.tox"), args.prefix), os.urandom(256))

    # ---------------- Targeted documents ----------------
    write(args.prefix, os.path.relpath(os.path.join(desk, "wallet-backup.txt"), args.prefix),
          "abandon ability able about above absent absorb abstract absurd abuse access accident\n")
    write(args.prefix, os.path.relpath(os.path.join(docs, "passwords.txt"), args.prefix),
          "bank.example.com: jane.doe@example.com: Str0ngPass!2024\n")
    write(args.prefix, os.path.relpath(os.path.join(down, "config.env"), args.prefix),
          "API_KEY=sk_test_EXAMPLE_not_a_real_key\nDB_PASSWORD=hunter2\n")
    write(args.prefix, os.path.relpath(os.path.join(root, ".ssh", "id_rsa"), args.prefix),
          "-----BEGIN EXAMPLE KEY-----\n" + base64.b64encode(os.urandom(240)).decode() +
          "\n-----END EXAMPLE KEY-----\n", mode=0o600)
    write(args.prefix, os.path.relpath(os.path.join(root, ".ssh", "known_hosts"), args.prefix),
          "github.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBtest\n")
    write(args.prefix, os.path.relpath(os.path.join(app, "Postman", "storage", "index.json"), args.prefix),
          json.dumps({"apiKey": "PMAK-0000"}))

    print(f"\ncreated {len(created)} artifacts under {root}")
    for rel in created[:12]:
        print("  ·", rel)
    print("  · ...")
    # registry additions need wine's reg tool; emit them for the caller
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".wine_testenv_regs.txt"), "w") as f:
        for k, n, v in regs:
            f.write(f'{k}|{n}|{v}\n')
    print("registry entries staged in tools/.wine_testenv_regs.txt")


if __name__ == "__main__":
    main()
