#pragma once
// The 80 check modules shown in the Ultria Checker UI.
// These are presentation-only entries (fake checkers) — no network activity.

namespace guimod {

struct Module {
    const char* name;
    const char* category;
    unsigned color;   // 0xRRGGBB badge color
};

inline const Module kModules[] = {
    // streaming
    {"Netflix", "Streaming", 0xE50914},
    {"Disney+", "Streaming", 0x113CCF},
    {"Prime Video", "Streaming", 0x1F9DEB},
    {"Hulu", "Streaming", 0x1CE783},
    {"HBO Max", "Streaming", 0x5B21B6},
    {"Spotify", "Streaming", 0x1DB954},
    {"SoundCloud", "Streaming", 0xFF7700},
    {"Tidal", "Streaming", 0x00FFFF},
    {"Crunchyroll", "Streaming", 0xF47521},
    {"YouTube Premium", "Streaming", 0xFF0000},
    // social
    {"Instagram", "Social", 0xE1306C},
    {"Facebook", "Social", 0x1877F2},
    {"Twitter/X", "Social", 0x1DA1F2},
    {"TikTok", "Social", 0xFE2C55},
    {"Snapchat", "Social", 0xFFFC00},
    {"Pinterest", "Social", 0xE60023},
    {"LinkedIn", "Social", 0x0A66C2},
    {"Reddit", "Social", 0xFF4500},
    // gaming
    {"Steam", "Gaming", 0x1B2838},
    {"Epic Games", "Gaming", 0x2A2A2A},
    {"Riot Games", "Gaming", 0xD13639},
    {"Battle.net", "Gaming", 0x148EFF},
    {"PlayStation", "Gaming", 0x0070D1},
    {"Xbox", "Gaming", 0x107C10},
    {"Roblox", "Gaming", 0xE2231A},
    {"Twitch", "Gaming", 0x9146FF},
    {"Discord", "Gaming", 0x5865F2},
    // finance
    {"PayPal", "Finance", 0x003087},
    {"Stripe", "Finance", 0x635BFF},
    {"Wise", "Finance", 0x9FE870},
    {"Revolut", "Finance", 0x0666EB},
    {"N26", "Finance", 0x36A18B},
    {"Monzo", "Finance", 0xFF4A55},
    // food
    {"Uber Eats", "Food", 0x06C167},
    {"Deliveroo", "Food", 0x00CCBC},
    {"DoorDash", "Food", 0xFF3008},
    {"Grubhub", "Food", 0xF63400},
    {"Just Eat", "Food", 0xFF8000},
    {"Zomato", "Food", 0xE23744},
    {"Swiggy", "Food", 0xFC8019},
    {"Instacart", "Food", 0x43B02A},
    // travel
    {"Airbnb", "Travel", 0xFF5A5F},
    {"Booking.com", "Travel", 0x003580},
    {"Expedia", "Travel", 0x1D5CFF},
    {"TripAdvisor", "Travel", 0x34E0A1},
    {"Trivago", "Travel", 0xEC2059},
    {"Uber", "Travel", 0x0A0A0A},
    {"Lyft", "Travel", 0xFF00BF},
    // shopping
    {"Amazon", "Shopping", 0xFF9900},
    {"eBay", "Shopping", 0xE53238},
    {"AliExpress", "Shopping", 0xFF4747},
    {"Walmart", "Shopping", 0x0071CE},
    {"Etsy", "Shopping", 0xF1641E},
    {"Zalando", "Shopping", 0xFF6900},
    // cloud
    {"Dropbox", "Cloud", 0x0061FF},
    {"Google Drive", "Cloud", 0x0F9D58},
    {"OneDrive", "Cloud", 0x0078D4},
    {"iCloud", "Cloud", 0x3693F3},
    {"Mega", "Cloud", 0xD9272C},
    // crypto
    {"Binance", "Crypto", 0xF0B90B},
    {"Coinbase", "Crypto", 0x0052FF},
    {"Kraken", "Crypto", 0x5741D9},
    {"Crypto.com", "Crypto", 0x1199FA},
    {"KuCoin", "Crypto", 0x24AE8F},
    {"Bybit", "Crypto", 0xF7A600},
    {"OKX", "Crypto", 0x0A0A0A},
    // vpn
    {"NordVPN", "VPN", 0x4687FF},
    {"ExpressVPN", "VPN", 0xDA3940},
    {"ProtonVPN", "VPN", 0x6D4AFF},
    {"Mullvad", "VPN", 0xE3E3E3},
    // adult
    {"Pornhub", "Adult", 0xFF9000},
    {"OnlyFans", "Adult", 0x00AFF0},
    // dating
    {"Tinder", "Dating", 0xFE3C72},
    {"Bumble", "Dating", 0xFEB000},
    {"Grindr", "Dating", 0xFFCF00},
    // education
    {"Coursera", "Education", 0x0056D2},
    {"Udemy", "Education", 0xA435FE},
    // mail
    {"Gmail", "Mail", 0xEA4335},
    {"Outlook", "Mail", 0x0078D4},
    // dev
    {"GitHub", "Dev", 0x8B5CF6},
};

inline constexpr int kModuleCount = (int)(sizeof(kModules) / sizeof(kModules[0]));

} // namespace guimod
