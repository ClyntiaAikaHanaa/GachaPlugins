#pragma once
#include <string>
#include <vector>
#include <map>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
//  Konstanta
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr int  kGuildMaxMembers       = 4;
inline constexpr int  kKickCostZI            = 10;     // biaya leader untuk kick
inline constexpr int  kMutinyCostZI          = 10;     // biaya inisiator mutiny
inline constexpr long long kVoteTimeoutSec   = 3600;   // 1 jam
inline constexpr long long kInviteTimeoutSec = 86400;  // 24 jam

// ─────────────────────────────────────────────────────────────────────────────
//  Guild Member
// ─────────────────────────────────────────────────────────────────────────────

struct GuildMember {
    std::string xuid;
    std::string name;       // nama saat join (snapshot)
    long long   joinedAt = 0;
    long long   lastSeen = 0;   // unix timestamp — updated on join/disconnect
};

// ─────────────────────────────────────────────────────────────────────────────
//  Rename constants
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr int       kGuildRenameCostZI       = 15;
inline constexpr long long kGuildRenameCooldownSec  = 14LL * 24 * 60 * 60; // 14 hari

// Preset emblem characters — hanya pakai char yang well-rendered di MCBE font.
// Emoji (🛡 🔱 ⛏ 🏹 ⚜ ☠ ❤ ⚡) tidak di-support font default Bedrock → tampil tofu.
// Karena itu kita pakai standard Unicode dari blok BMP yang dijamin render:
//   - Stars: ★ ☆ ✦ ✪
//   - Chess pieces: ♛ ♚ ♜ ♝ ♞ ♟
//   - Cards: ♠ ♥ ♦ ♣
//   - Geometric: ◆ ● ▲
inline const std::vector<std::string>& guildEmblemPresets() {
    static const std::vector<std::string> kPresets = {
        "★", "☆", "✦", "✪",
        "♛", "♚", "♜",
        "♠", "♥", "♦", "♣",
        "◆"
    };
    return kPresets;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Guild Data — entri utama yang disimpan di LevelDB
// ─────────────────────────────────────────────────────────────────────────────

// Log entry untuk activity log guild (max 20 entries, terbaru di depan)
struct GuildLogEntry {
    long long   timestamp = 0;
    std::string text;       // sudah pre-formatted dengan warna code (e.g., "§a[+] §6Jane §rjoined")
};

inline constexpr int kGuildLogMax = 20;

struct GuildData {
    std::string                 guildId;        // UUID-ish
    std::string                 guildName;
    std::string                 leaderXuid;
    std::vector<GuildMember>    members;        // termasuk leader; max kGuildMaxMembers
    long long                   createdAt = 0;
    std::vector<GuildLogEntry>  log;            // activity log, max kGuildLogMax
    bool                        friendlyFireProtection = true; // default: ON
    long long                   lastRenamedAt = 0;  // unix ts; 0 = never renamed
    std::string                 emblem;             // empty = no emblem
};

// ─────────────────────────────────────────────────────────────────────────────
//  Kick Vote — leader memilih kick salah satu member
//   • initiator = leader
//   • voters    = semua member non-target non-leader
//   • butuh approval semua voter
// ─────────────────────────────────────────────────────────────────────────────

struct KickVote {
    std::string                  voteId;
    std::string                  guildId;
    std::string                  targetXuid;
    std::string                  targetName;
    std::string                  initiatorXuid;     // leader
    std::map<std::string, bool>  votes;             // xuid -> approved
    long long                    expiresAt = 0;
    bool                         active    = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Mutiny Vote — member menginisiasi pelengseran leader
//   • initiator = calon leader baru
//   • voters    = semua member non-leader non-initiator
//   • butuh approval semua voter
// ─────────────────────────────────────────────────────────────────────────────

struct MutinyVote {
    std::string                  voteId;
    std::string                  guildId;
    std::string                  initiatorXuid;     // calon leader baru
    std::string                  initiatorName;
    std::string                  currentLeaderXuid; // yang akan dilengserkan
    std::map<std::string, bool>  votes;
    long long                    expiresAt = 0;
    bool                         active    = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Mail Message — inbox entry
// ─────────────────────────────────────────────────────────────────────────────

enum class MailType {
    System            = 0,
    GuildInvite       = 1,
    KickVote          = 2,
    MutinyVote        = 3,
    GuildNotice       = 4,    // info-only (kicked, dissolved, promoted, dst)
    GuildJoinRequest  = 5,    // outsider request join — diterima leader
};

inline const char* mailTypeToString(MailType t) {
    switch (t) {
        case MailType::GuildInvite:      return "guild_invite";
        case MailType::KickVote:         return "kick_vote";
        case MailType::MutinyVote:       return "mutiny_vote";
        case MailType::GuildNotice:      return "guild_notice";
        case MailType::GuildJoinRequest: return "guild_join_request";
        case MailType::System:           return "system";
    }
    return "system";
}

inline MailType mailTypeFromString(const std::string& s) {
    if (s == "guild_invite")        return MailType::GuildInvite;
    if (s == "kick_vote")           return MailType::KickVote;
    if (s == "mutiny_vote")         return MailType::MutinyVote;
    if (s == "guild_notice")        return MailType::GuildNotice;
    if (s == "guild_join_request")  return MailType::GuildJoinRequest;
    return MailType::System;
}

struct MailMessage {
    std::string mailId;
    std::string toXuid;
    std::string fromXuid;     // boleh kosong (system)
    std::string fromName;     // display
    std::string subject;
    std::string body;
    MailType    type = MailType::System;
    std::string payload;      // JSON serialisasi data tambahan (guildId, voteId, dst)
    bool        read   = false;
    long long   sentAt = 0;
};

} // namespace gacha_mod
