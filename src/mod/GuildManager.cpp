#include "mod/GuildManager.h"
#include "mod/Database.h"
#include "mod/MailManager.h"
#include "mod/PlayerProfile.h"
#include "mod/MyMod.h"

#include "ll/api/service/Bedrock.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/Player.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static long long nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string GuildManager::generateGuildId() {
    static std::atomic<uint64_t> counter{0};
    return "g" + std::to_string(nowUnix()) + "_" + std::to_string(counter.fetch_add(1));
}

std::string GuildManager::generateVoteId() {
    static std::atomic<uint64_t> counter{0};
    return "v" + std::to_string(nowUnix()) + "_" + std::to_string(counter.fetch_add(1));
}

std::string GuildManager::payloadGuildId(const std::string& payload) {
    try {
        auto j = json::parse(payload);
        return j.value("guildId", "");
    } catch (...) { return ""; }
}

std::string GuildManager::payloadString(const std::string& payload, const std::string& key) {
    try {
        auto j = json::parse(payload);
        return j.value(key, "");
    } catch (...) { return ""; }
}

std::unordered_map<std::string, std::string> GuildManager::parsePayload(const std::string& payload) {
    std::unordered_map<std::string, std::string> out;
    try {
        auto j = json::parse(payload);
        if (!j.is_object()) return out;
        for (auto it = j.begin(); it != j.end(); ++it) {
            // Hanya simpan string fields (yang dipakai handler form)
            if (it.value().is_string()) {
                out[it.key()] = it.value().get<std::string>();
            } else if (it.value().is_number()) {
                out[it.key()] = std::to_string(it.value().get<long long>());
            }
        }
    } catch (...) {}
    return out;
}

bool GuildManager::isUnanimous(const std::map<std::string, bool>& votes) {
    if (votes.empty()) return false;
    for (auto const& [_, v] : votes) {
        if (!v) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Serialisasi
// ─────────────────────────────────────────────────────────────────────────────

std::string GuildManager::serializeGuild(const GuildData& g) {
    json j;
    j["guild_id"]    = g.guildId;
    j["guild_name"]  = g.guildName;
    j["leader_xuid"] = g.leaderXuid;
    j["created_at"]  = g.createdAt;
    j["ff_protect"]   = g.friendlyFireProtection;
    j["last_renamed"] = g.lastRenamedAt;
    j["emblem"]       = g.emblem;
    json m = json::array();
    for (const auto& mem : g.members) {
        m.push_back({
            {"xuid",      mem.xuid},
            {"name",      mem.name},
            {"joined_at", mem.joinedAt},
            {"last_seen", mem.lastSeen},
        });
    }
    j["members"] = m;
    json logArr = json::array();
    for (const auto& e : g.log) {
        logArr.push_back({{"t", e.timestamp}, {"m", e.text}});
    }
    j["log"] = logArr;
    return j.dump();
}

std::optional<GuildData> GuildManager::deserializeGuild(const std::string& s) {
    try {
        auto j = json::parse(s);
        GuildData g;
        g.guildId    = j.value("guild_id",    "");
        g.guildName  = j.value("guild_name",  "");
        g.leaderXuid = j.value("leader_xuid", "");
        g.createdAt  = j.value("created_at",  0LL);
        g.friendlyFireProtection = j.value("ff_protect", true); // default ON kalau field tidak ada
        g.lastRenamedAt          = j.value("last_renamed", 0LL);
        g.emblem                 = j.value("emblem", std::string());
        if (j.contains("members") && j["members"].is_array()) {
            for (const auto& mj : j["members"]) {
                GuildMember m;
                m.xuid     = mj.value("xuid",      "");
                m.name     = mj.value("name",      "");
                m.joinedAt = mj.value("joined_at", 0LL);
                m.lastSeen = mj.value("last_seen", 0LL);
                g.members.push_back(std::move(m));
            }
        }
        if (j.contains("log") && j["log"].is_array()) {
            for (const auto& lj : j["log"]) {
                GuildLogEntry e;
                e.timestamp = lj.value("t", 0LL);
                e.text      = lj.value("m", "");
                g.log.push_back(std::move(e));
            }
        }
        return g;
    } catch (...) { return std::nullopt; }
}

static json votesToJson(const std::map<std::string, bool>& v) {
    json o = json::object();
    for (const auto& [k, val] : v) o[k] = val;
    return o;
}

static std::map<std::string, bool> jsonToVotes(const json& j) {
    std::map<std::string, bool> out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        // Type-safe parse — kalau entry corrupt atau migrasi schema lama
        // (mis. dulu pakai int 0/1), default ke false, jangan throw.
        const auto& v = it.value();
        if (v.is_boolean())       out[it.key()] = v.get<bool>();
        else if (v.is_number())   out[it.key()] = (v.get<int>() != 0);
        else                      out[it.key()] = false;
    }
    return out;
}

std::string GuildManager::serializeKick(const KickVote& v) {
    json j;
    j["vote_id"]        = v.voteId;
    j["guild_id"]       = v.guildId;
    j["target_xuid"]    = v.targetXuid;
    j["target_name"]    = v.targetName;
    j["initiator_xuid"] = v.initiatorXuid;
    j["votes"]          = votesToJson(v.votes);
    j["expires_at"]     = v.expiresAt;
    j["active"]         = v.active;
    return j.dump();
}

std::optional<KickVote> GuildManager::deserializeKick(const std::string& s) {
    try {
        auto j = json::parse(s);
        KickVote v;
        v.voteId        = j.value("vote_id",        "");
        v.guildId       = j.value("guild_id",       "");
        v.targetXuid    = j.value("target_xuid",    "");
        v.targetName    = j.value("target_name",    "");
        v.initiatorXuid = j.value("initiator_xuid", "");
        v.votes         = jsonToVotes(j.value("votes", json::object()));
        v.expiresAt     = j.value("expires_at",     0LL);
        v.active        = j.value("active",         false);
        return v;
    } catch (...) { return std::nullopt; }
}

std::string GuildManager::serializeMutiny(const MutinyVote& v) {
    json j;
    j["vote_id"]             = v.voteId;
    j["guild_id"]            = v.guildId;
    j["initiator_xuid"]      = v.initiatorXuid;
    j["initiator_name"]      = v.initiatorName;
    j["current_leader_xuid"] = v.currentLeaderXuid;
    j["votes"]               = votesToJson(v.votes);
    j["expires_at"]          = v.expiresAt;
    j["active"]              = v.active;
    return j.dump();
}

std::optional<MutinyVote> GuildManager::deserializeMutiny(const std::string& s) {
    try {
        auto j = json::parse(s);
        MutinyVote v;
        v.voteId             = j.value("vote_id",             "");
        v.guildId            = j.value("guild_id",            "");
        v.initiatorXuid      = j.value("initiator_xuid",      "");
        v.initiatorName      = j.value("initiator_name",      "");
        v.currentLeaderXuid  = j.value("current_leader_xuid", "");
        v.votes              = jsonToVotes(j.value("votes", json::object()));
        v.expiresAt          = j.value("expires_at",          0LL);
        v.active             = j.value("active",              false);
        return v;
    } catch (...) { return std::nullopt; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Storage
// ─────────────────────────────────────────────────────────────────────────────

bool GuildManager::saveGuild(const GuildData& g) {
    return Database::getInstance().kvSet(keyGuild(g.guildId), serializeGuild(g));
}

bool GuildManager::saveKick(const KickVote& v) {
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        if (v.active) mActiveKickGuilds[v.guildId] = true;
        else          mActiveKickGuilds.erase(v.guildId);
    }
    return Database::getInstance().kvSet(keyKick(v.guildId), serializeKick(v));
}

bool GuildManager::saveMutiny(const MutinyVote& v) {
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        if (v.active) mActiveMutinyGuilds[v.guildId] = true;
        else          mActiveMutinyGuilds.erase(v.guildId);
    }
    return Database::getInstance().kvSet(keyMutiny(v.guildId), serializeMutiny(v));
}

std::optional<GuildData> GuildManager::getGuild(const std::string& id) const {
    auto raw = Database::getInstance().kvGet(keyGuild(id));
    if (!raw || raw->empty()) return std::nullopt;
    return deserializeGuild(*raw);
}

std::optional<std::string> GuildManager::getGuildIdOfPlayer(const std::string& xuid) const {
    // Cache lookup dulu — empty string sentinel = player TIDAK di guild.
    {
        std::lock_guard<std::mutex> lock(mGuildCacheMu);
        auto it = mXuidToGuildId.find(xuid);
        if (it != mXuidToGuildId.end()) {
            if (it->second.empty()) return std::nullopt;
            return it->second;
        }
    }
    // Cache miss → DB lookup, lalu populate cache.
    auto raw = Database::getInstance().kvGet(keyMember(xuid));
    std::string val = (raw && !raw->empty()) ? *raw : "";
    {
        std::lock_guard<std::mutex> lock(mGuildCacheMu);
        mXuidToGuildId[xuid] = val;
    }
    if (val.empty()) return std::nullopt;
    return val;
}

std::optional<GuildData> GuildManager::getGuildOfPlayer(const std::string& xuid) const {
    auto gid = getGuildIdOfPlayer(xuid);
    if (!gid) return std::nullopt;
    return getGuild(*gid);
}

bool GuildManager::isInSameGuild(const std::string& a, const std::string& b) const {
    if (a == b) return false;
    auto ga = getGuildIdOfPlayer(a);
    auto gb = getGuildIdOfPlayer(b);
    return ga && gb && *ga == *gb;
}

bool GuildManager::isFriendlyFireBlocked(const std::string& a, const std::string& b) const {
    if (a == b) return false;
    auto ga = getGuildIdOfPlayer(a);
    auto gb = getGuildIdOfPlayer(b);
    if (!ga || !gb || *ga != *gb) return false;
    // Sama guild → cek apakah FF protection aktif
    auto g = getGuild(*ga);
    return g && g->friendlyFireProtection;
}

bool GuildManager::setFriendlyFireProtection(const std::string& guildId,
                                             const std::string& callerXuid,
                                             bool enabled) {
    auto g = getGuild(guildId);
    if (!g) return false;
    if (g->leaderXuid != callerXuid) return false; // leader-only
    g->friendlyFireProtection = enabled;
    g->log.insert(g->log.begin(), {nowUnix(),
        enabled
            ? "§3[⚙] §rFriendly Fire protection §aENABLED"
            : "§3[⚙] §rFriendly Fire protection §cDISABLED"});
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);
    return saveGuild(*g);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RENAME GUILD (cost 20 ZI + 14-day cooldown)
// ─────────────────────────────────────────────────────────────────────────────

long long GuildManager::renameCooldownRemaining(const std::string& guildId) const {
    auto g = getGuild(guildId);
    if (!g) return 0;
    if (g->lastRenamedAt == 0) return 0; // belum pernah rename → bebas
    long long elapsed = nowUnix() - g->lastRenamedAt;
    if (elapsed >= kGuildRenameCooldownSec) return 0;
    return kGuildRenameCooldownSec - elapsed;
}

// Trim spaces termasuk Unicode invisible (NBSP latin-1 byte).
static bool isAsciiWhitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Cek control chars (<0x20) dan color-code injection (§).
// Untuk emblem: simple — allow Unicode chars (★ ✦ dll).
static bool hasControlOrColorCode(const std::string& s) {
    for (char ch : s) {
        unsigned char uc = static_cast<unsigned char>(ch);
        if (uc < 0x20) return true;  // control chars (newline, tab, dll)
    }
    return s.find("§") != std::string::npos;
}

// Untuk nama guild: stricter — selain control + §, juga reject UTF-8 byte sequence
// untuk Unicode invisible whitespace yang bisa bypass length check / impersonation:
//   U+00A0 NBSP    = 0xC2 0xA0
//   U+200B ZWSP    = 0xE2 0x80 0x8B
//   U+3000 IDSP    = 0xE3 0x80 0x80
static bool nameHasForbiddenChars(const std::string& s) {
    if (hasControlOrColorCode(s)) return true;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if ((unsigned char)s[i] == 0xC2 && (unsigned char)s[i+1] == 0xA0) return true;
    }
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        unsigned char a = s[i], b = s[i+1], c = s[i+2];
        if (a == 0xE2 && b == 0x80 && c == 0x8B) return true; // ZWSP
        if (a == 0xE3 && b == 0x80 && c == 0x80) return true; // IDSP
    }
    return false;
}

int GuildManager::renameWithCost(const std::string& guildId,
                                 const std::string& callerXuid,
                                 const std::string& newName,
                                 long long& outRemainingCooldownSec) {
    outRemainingCooldownSec = 0;
    auto g = getGuild(guildId);
    if (!g) return 5;                                       // not found
    if (g->leaderXuid != callerXuid) return 1;              // not leader
    // Validate name (3-20 chars after trim, no invisibles, no color-code)
    std::string n = newName;
    while (!n.empty() && isAsciiWhitespace((unsigned char)n.front())) n.erase(n.begin());
    while (!n.empty() && isAsciiWhitespace((unsigned char)n.back()))  n.pop_back();
    if (n.size() < 3 || n.size() > 20) return 4;            // invalid length
    if (nameHasForbiddenChars(n)) return 4;                 // invisibles / color-code injection
    // Cooldown check
    long long remaining = renameCooldownRemaining(guildId);
    if (remaining > 0) {
        outRemainingCooldownSec = remaining;
        return 2; // cooldown active
    }
    // Pre-check saldo (jangan charge dulu — charge SETELAH saveGuild sukses).
    // Pattern lama: charge → save. Kalau save gagal, ZI leader hilang tanpa rename.
    // Pattern baru: cek saldo cukup → save → kalau sukses, baru charge.
    int currentZI = 0;
    if (PlayerCache::getInstance().isOnline(callerXuid)) {
        currentZI = PlayerCache::getInstance().get(callerXuid).getZenIngot();
    } else {
        auto data = Database::getInstance().getOrCreate(callerXuid);
        currentZI = data.zenIngot;
    }
    if (currentZI < kGuildRenameCostZI) return 3; // insufficient ZI

    std::string oldName = g->guildName;
    g->guildName     = n;
    g->lastRenamedAt = nowUnix();
    g->log.insert(g->log.begin(), {nowUnix(),
        std::string("§3[⚙] §rRenamed: §6") + oldName + " §7→ §6" + n});
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);
    if (!saveGuild(*g)) return 5; // save error — treat as not-found

    // Save sukses → baru charge ZI (idempotent kalau ada retry).
    chargeZI(callerXuid, kGuildRenameCostZI);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  GUILD EMBLEM (leader-only, no cost)
// ─────────────────────────────────────────────────────────────────────────────

bool GuildManager::setEmblem(const std::string& guildId,
                             const std::string& callerXuid,
                             const std::string& emblem) {
    auto g = getGuild(guildId);
    if (!g) return false;
    if (g->leaderXuid != callerXuid) return false;
    // Validasi defense-in-depth (caller di FormManager sudah pakai preset list, tapi
    // API publik harus tetap proteksi terhadap input arbitrary):
    //   - max 8 UTF-8 bytes (cukup untuk semua preset BMP)
    //   - no newline/control char (anti format-string injection di nametag/chat)
    //   - no '§' (anti color-code injection)
    if (emblem.size() > 8) return false;
    if (!emblem.empty() && hasControlOrColorCode(emblem)) return false;

    g->emblem = emblem;
    g->log.insert(g->log.begin(), {nowUnix(),
        emblem.empty()
            ? "§3[⚙] §rEmblem cleared"
            : "§3[⚙] §rEmblem set to §6" + emblem});
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);
    return saveGuild(*g);
}

// ─────────────────────────────────────────────────────────────────────────────
//  LAST SEEN TRACKING
// ─────────────────────────────────────────────────────────────────────────────

void GuildManager::updateMemberLastSeen(const std::string& xuid, long long timestamp) {
    auto gid = getGuildIdOfPlayer(xuid);
    if (!gid) return;
    auto g = getGuild(*gid);
    if (!g) return;
    for (auto& m : g->members) {
        if (m.xuid == xuid) {
            m.lastSeen = timestamp;
            saveGuild(*g);
            return;
        }
    }
}

void GuildManager::markLastSeen(const std::string& xuid) {
    // In-memory only — microseconds. Periodic flush atau disconnect persist ke DB.
    const long long ts = nowUnix();
    std::lock_guard<std::mutex> lock(mLastSeenMu);
    mLastSeenBuffer[xuid] = ts;
}

void GuildManager::flushLastSeenBuffer() {
    // Steal buffer di bawah lock, lalu process tanpa lock (DB IO).
    std::unordered_map<std::string, long long> snapshot;
    {
        std::lock_guard<std::mutex> lock(mLastSeenMu);
        snapshot = std::move(mLastSeenBuffer);
        mLastSeenBuffer.clear();
    }
    if (snapshot.empty()) return;

    // Batch by guildId — hanya 1 guild read+write per guild aktif.
    // Untuk 30 player di 5 guild = 5 guild operations bukan 30.
    std::unordered_map<std::string, std::vector<std::pair<std::string, long long>>> byGuild;
    for (const auto& [xuid, ts] : snapshot) {
        auto gid = getGuildIdOfPlayer(xuid);  // O(1) cache hit
        if (!gid) continue;                    // skip non-guild players
        byGuild[*gid].emplace_back(xuid, ts);
    }

    for (auto& [gid, updates] : byGuild) {
        auto g = getGuild(gid);
        if (!g) continue;
        bool changed = false;
        for (auto& [xuid, ts] : updates) {
            for (auto& m : g->members) {
                if (m.xuid == xuid) {
                    if (m.lastSeen != ts) {
                        m.lastSeen = ts;
                        changed = true;
                    }
                    break;
                }
            }
        }
        if (changed) saveGuild(*g);
    }
}

bool GuildManager::isLeader(const std::string& guildId, const std::string& xuid) const {
    auto g = getGuild(guildId);
    return g && g->leaderXuid == xuid;
}

bool GuildManager::isMember(const std::string& guildId, const std::string& xuid) const {
    auto g = getGuild(guildId);
    if (!g) return false;
    for (auto const& m : g->members) if (m.xuid == xuid) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Create / Add / Remove / Disband
// ─────────────────────────────────────────────────────────────────────────────

std::string GuildManager::createGuild(const std::string& founderXuid,
                                      const std::string& founderName,
                                      const std::string& guildName) {
    if (founderXuid.empty() || guildName.empty()) return "";
    if (getGuildIdOfPlayer(founderXuid)) return ""; // sudah punya guild

    GuildData g;
    g.guildId    = generateGuildId();
    g.guildName  = guildName;
    g.leaderXuid = founderXuid;
    g.createdAt  = nowUnix();
    g.members.push_back({founderXuid, founderName, g.createdAt});
    g.log.push_back({nowUnix(), "§a[+] §6" + founderName + " §rfounded the guild"});

    if (!saveGuild(g)) return "";
    Database::getInstance().kvSet(keyMember(founderXuid), g.guildId);
    {
        std::lock_guard<std::mutex> lock(mGuildCacheMu);
        mXuidToGuildId[founderXuid] = g.guildId;
    }
    refreshNametag(founderXuid);
    return g.guildId;
}

bool GuildManager::addMember(const std::string& guildId,
                             const std::string& xuid,
                             const std::string& name) {
    auto g = getGuild(guildId);
    if (!g) return false;
    if (g->members.size() >= kGuildMaxMembers) return false;
    if (getGuildIdOfPlayer(xuid)) return false; // sudah punya guild
    for (auto const& m : g->members) if (m.xuid == xuid) return false; // sudah anggota

    g->members.push_back({xuid, name, nowUnix()});
    g->log.insert(g->log.begin(), {nowUnix(), "§a[+] §6" + name + " §rjoined"});
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);

    if (!saveGuild(*g)) return false;
    Database::getInstance().kvSet(keyMember(xuid), g->guildId);
    {
        std::lock_guard<std::mutex> lock(mGuildCacheMu);
        mXuidToGuildId[xuid] = g->guildId;
    }
    refreshNametag(xuid);
    return true;
}

bool GuildManager::removeMember(const std::string& guildId, const std::string& xuid,
                                const std::string& customLogText) {
    auto g = getGuild(guildId);
    if (!g) return false;

    auto it = std::find_if(g->members.begin(), g->members.end(),
                           [&](const GuildMember& m) { return m.xuid == xuid; });
    if (it == g->members.end()) return false;

    bool wasLeader      = (g->leaderXuid == xuid);
    std::string oldName = it->name;
    g->members.erase(it);
    Database::getInstance().kvDel(keyMember(xuid));
    {
        std::lock_guard<std::mutex> lock(mGuildCacheMu);
        mXuidToGuildId[xuid] = ""; // empty sentinel = no guild
    }

    if (g->members.empty()) {
        bool ok = disband(guildId);
        refreshNametag(xuid);
        return ok;
    }

    std::string logText = customLogText.empty()
        ? ("§c[-] §6" + oldName + " §rleft")
        : customLogText;
    g->log.insert(g->log.begin(), {nowUnix(), logText});
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);

    if (wasLeader) {
        // pilih member tertua sebagai leader baru
        auto oldest = std::min_element(g->members.begin(), g->members.end(),
            [](const GuildMember& a, const GuildMember& b) { return a.joinedAt < b.joinedAt; });
        g->leaderXuid = oldest->xuid;
        g->log.insert(g->log.begin(), {nowUnix(),
            "§6[★] §6" + oldest->name + " §rauto-promoted to leader"});
        if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);
    }
    bool ok = saveGuild(*g);
    refreshNametag(xuid);
    return ok;
}

bool GuildManager::disband(const std::string& guildId) {
    auto g = getGuild(guildId);
    if (!g) return false;
    std::vector<std::string> formerMembers;
    for (auto const& m : g->members) {
        formerMembers.push_back(m.xuid);
        Database::getInstance().kvDel(keyMember(m.xuid));
        {
            std::lock_guard<std::mutex> lock(mGuildCacheMu);
            mXuidToGuildId[m.xuid] = ""; // empty sentinel
        }
    }
    Database::getInstance().kvDel(keyKick(guildId));
    Database::getInstance().kvDel(keyMutiny(guildId));
    Database::getInstance().kvDel(keyGuild(guildId));
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        mActiveKickGuilds.erase(guildId);
        mActiveMutinyGuilds.erase(guildId);
    }
    for (const auto& x : formerMembers) refreshNametag(x);
    return true;
}

bool GuildManager::transferLeader(const std::string& guildId, const std::string& newLeader) {
    auto g = getGuild(guildId);
    if (!g) return false;
    std::string newLeaderName;
    bool found = false;
    for (auto const& m : g->members) {
        if (m.xuid == newLeader) { found = true; newLeaderName = m.name; break; }
    }
    if (!found) return false;
    g->leaderXuid = newLeader;
    g->log.insert(g->log.begin(), {nowUnix(),
        "§6[★] §rLeadership transferred to §6" + newLeaderName});
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);
    return saveGuild(*g);
}

bool GuildManager::rename(const std::string& guildId, const std::string& newName) {
    auto g = getGuild(guildId);
    if (!g || newName.empty()) return false;
    g->guildName = newName;
    return saveGuild(*g);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZI Charge / Refund
// ─────────────────────────────────────────────────────────────────────────────

bool GuildManager::chargeZI(const std::string& xuid, int amount) {
    if (amount <= 0) return true;
    if (PlayerCache::getInstance().isOnline(xuid)) {
        auto& p = PlayerCache::getInstance().get(xuid);
        return p.spendZenIngot(amount);
    }
    if (!Database::getInstance().exists(xuid)) return false;
    auto data = Database::getInstance().getOrCreate(xuid);
    if (data.zenIngot < amount) return false;
    data.zenIngot -= amount;
    Database::getInstance().save(data);
    return true;
}

void GuildManager::refundZI(const std::string& xuid, int amount) {
    if (amount <= 0) return;
    if (PlayerCache::getInstance().isOnline(xuid)) {
        PlayerCache::getInstance().get(xuid).addZenIngot(amount);
        return;
    }
    if (!Database::getInstance().exists(xuid)) return;
    auto data = Database::getInstance().getOrCreate(xuid);
    data.zenIngot += amount;
    Database::getInstance().save(data);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Broadcast Notice
// ─────────────────────────────────────────────────────────────────────────────

void GuildManager::noticeAllMembers(const GuildData& g,
                                    const std::string& subject,
                                    const std::string& body,
                                    const std::string& excludeXuid) {
    json payload;
    payload["guildId"]   = g.guildId;
    payload["guildName"] = g.guildName;
    for (auto const& m : g.members) {
        if (m.xuid == excludeXuid) continue;
        MailManager::getInstance().sendSystem(
            m.xuid, subject, body, MailType::GuildNotice, payload.dump());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  KICK VOTE
// ─────────────────────────────────────────────────────────────────────────────

std::string GuildManager::startKickVote(const std::string& guildId,
                                        const std::string& leaderXuid,
                                        const std::string& targetXuid) {
    auto g = getGuild(guildId);
    if (!g) return "";
    if (g->leaderXuid != leaderXuid) return "";
    if (leaderXuid == targetXuid) return "";
    if (!isMember(guildId, targetXuid)) return "";

    // Vote sudah ada?
    if (auto existing = getKickVote(guildId); existing && existing->active &&
        existing->expiresAt > nowUnix()) {
        return "";
    }

    // Charge ZI dulu
    if (!chargeZI(leaderXuid, kKickCostZI)) return "";

    KickVote v;
    v.voteId        = generateVoteId();
    v.guildId       = guildId;
    v.targetXuid    = targetXuid;
    v.initiatorXuid = leaderXuid;
    v.expiresAt     = nowUnix() + kVoteTimeoutSec;
    v.active        = true;

    // Cari nama target
    for (auto const& m : g->members) {
        if (m.xuid == targetXuid) { v.targetName = m.name; break; }
    }

    // Voters: semua member kecuali leader & target
    for (auto const& m : g->members) {
        if (m.xuid == leaderXuid || m.xuid == targetXuid) continue;
        v.votes[m.xuid] = false;
    }

    // Edge case: hanya leader + target (2 anggota) → tidak ada voter →
    // langsung approve. Jarang, tapi handle anyway.
    if (v.votes.empty()) {
        saveKick(v);
        executeKick(*g, v);
        return v.voteId;
    }

    if (!saveKick(v)) { refundZI(leaderXuid, kKickCostZI); return ""; }

    // Kirim mail vote ke semua voter
    json payload;
    payload["guildId"]    = guildId;
    payload["voteId"]     = v.voteId;
    payload["targetXuid"] = targetXuid;
    payload["targetName"] = v.targetName;
    payload["expiresAt"]  = v.expiresAt;

    for (auto const& [voterXuid, _] : v.votes) {
        MailManager::getInstance().sendSystem(
            voterXuid,
            "§cKick Vote: " + v.targetName,
            "§fThe guild leader requests to kick §c" + v.targetName + "§f from the guild.\n"
            "§7Open this mail to vote. The vote expires in 1 hour.",
            MailType::KickVote, payload.dump()
        );
    }
    return v.voteId;
}

std::optional<KickVote> GuildManager::getKickVote(const std::string& guildId) const {
    auto raw = Database::getInstance().kvGet(keyKick(guildId));
    if (!raw || raw->empty()) return std::nullopt;
    return deserializeKick(*raw);
}

bool GuildManager::castKickApprove(const std::string& guildId, const std::string& voter) {
    auto v = getKickVote(guildId);
    if (!v || !v->active) return false;
    if (v->expiresAt <= nowUnix()) return false;
    auto it = v->votes.find(voter);
    if (it == v->votes.end()) return false;
    it->second = true;
    saveKick(*v);

    if (isUnanimous(v->votes)) {
        auto g = getGuild(guildId);
        if (g) executeKick(*g, *v);
        return true;
    }
    return false;
}

bool GuildManager::castKickReject(const std::string& guildId, const std::string& voter) {
    auto v = getKickVote(guildId);
    if (!v || !v->active) return false;
    auto it = v->votes.find(voter);
    if (it == v->votes.end()) return false;

    v->active = false;
    saveKick(*v);
    refundZI(v->initiatorXuid, kKickCostZI);

    // Notif ke leader
    json payload; payload["guildId"] = guildId;
    MailManager::getInstance().sendSystem(
        v->initiatorXuid,
        "§eKick Vote Rejected",
        "§fThe kick vote for §c" + v->targetName + "§f was rejected by a guild member.\n"
        "§a10 ZI has been refunded to your wallet.",
        MailType::GuildNotice, payload.dump()
    );
    Database::getInstance().kvDel(keyKick(guildId));
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        mActiveKickGuilds.erase(guildId);
    }
    return true;
}

bool GuildManager::executeKick(const GuildData& gIn, const KickVote& v) {
    auto g = gIn; // copy
    // Custom log text di removeMember → tidak perlu re-fetch + re-save untuk patch log
    removeMember(g.guildId, v.targetXuid, "§c[!] §6" + v.targetName + " §rwas kicked");
    auto fresh = getGuild(g.guildId);

    // Notif ke target
    json p; p["guildName"] = g.guildName;
    MailManager::getInstance().sendSystem(
        v.targetXuid,
        "§cYou were kicked from the guild",
        "§fYou have been removed from guild §6" + g.guildName + "§f by unanimous vote.",
        MailType::GuildNotice, p.dump()
    );

    // Notif ke sisa anggota
    if (fresh) {
        noticeAllMembers(*fresh,
            "§eMember Kicked",
            "§f" + v.targetName + " §7has been removed from the guild.",
            v.targetXuid);
    }

    Database::getInstance().kvDel(keyKick(g.guildId));
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        mActiveKickGuilds.erase(g.guildId);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  MUTINY VOTE
// ─────────────────────────────────────────────────────────────────────────────

std::string GuildManager::startMutinyVote(const std::string& guildId,
                                          const std::string& initiatorXuid,
                                          const std::string& initiatorName) {
    auto g = getGuild(guildId);
    if (!g) return "";
    if (g->leaderXuid == initiatorXuid) return "";        // leader tidak bisa mutiny diri sendiri
    if (!isMember(guildId, initiatorXuid)) return "";

    if (auto existing = getMutinyVote(guildId); existing && existing->active &&
        existing->expiresAt > nowUnix()) {
        return "";
    }

    if (!chargeZI(initiatorXuid, kMutinyCostZI)) return "";

    MutinyVote v;
    v.voteId             = generateVoteId();
    v.guildId            = guildId;
    v.initiatorXuid      = initiatorXuid;
    v.initiatorName      = initiatorName;
    v.currentLeaderXuid  = g->leaderXuid;
    v.expiresAt          = nowUnix() + kVoteTimeoutSec;
    v.active             = true;

    // Voters: semua member selain leader & initiator
    for (auto const& m : g->members) {
        if (m.xuid == g->leaderXuid || m.xuid == initiatorXuid) continue;
        v.votes[m.xuid] = false;
    }

    // Edge case: 2-member guild (leader + initiator) → tidak ada voter lain →
    // initiator otomatis menjadi leader.
    if (v.votes.empty()) {
        saveMutiny(v);
        executeMutiny(*g, v);
        return v.voteId;
    }

    if (!saveMutiny(v)) { refundZI(initiatorXuid, kMutinyCostZI); return ""; }

    json payload;
    payload["guildId"]        = guildId;
    payload["voteId"]         = v.voteId;
    payload["initiatorXuid"]  = initiatorXuid;
    payload["initiatorName"]  = initiatorName;
    payload["expiresAt"]      = v.expiresAt;

    for (auto const& [voter, _] : v.votes) {
        MailManager::getInstance().sendSystem(
            voter,
            "§4Mutiny Vote",
            "§f" + initiatorName + " §fwants to replace the current guild leader.\n"
            "§7Open this mail to vote. The vote expires in 1 hour.",
            MailType::MutinyVote, payload.dump()
        );
    }
    return v.voteId;
}

std::optional<MutinyVote> GuildManager::getMutinyVote(const std::string& guildId) const {
    auto raw = Database::getInstance().kvGet(keyMutiny(guildId));
    if (!raw || raw->empty()) return std::nullopt;
    return deserializeMutiny(*raw);
}

bool GuildManager::castMutinyApprove(const std::string& guildId, const std::string& voter) {
    auto v = getMutinyVote(guildId);
    if (!v || !v->active) return false;
    if (v->expiresAt <= nowUnix()) return false;
    auto it = v->votes.find(voter);
    if (it == v->votes.end()) return false;
    it->second = true;
    saveMutiny(*v);

    if (isUnanimous(v->votes)) {
        auto g = getGuild(guildId);
        if (g) executeMutiny(*g, *v);
        return true;
    }
    return false;
}

bool GuildManager::castMutinyReject(const std::string& guildId, const std::string& voter) {
    auto v = getMutinyVote(guildId);
    if (!v || !v->active) return false;
    auto it = v->votes.find(voter);
    if (it == v->votes.end()) return false;

    v->active = false;
    saveMutiny(*v);
    refundZI(v->initiatorXuid, kMutinyCostZI);

    json p; p["guildId"] = guildId;
    MailManager::getInstance().sendSystem(
        v->initiatorXuid,
        "§eMutiny Rejected",
        "§fA guild member voted against your leadership challenge.\n"
        "§a10 ZI has been refunded to your wallet.",
        MailType::GuildNotice, p.dump()
    );
    Database::getInstance().kvDel(keyMutiny(guildId));
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        mActiveMutinyGuilds.erase(guildId);
    }
    return true;
}

bool GuildManager::executeMutiny(const GuildData& gIn, const MutinyVote& v) {
    auto g = gIn;
    g.leaderXuid = v.initiatorXuid;
    g.log.insert(g.log.begin(), {nowUnix(),
        "§4[⚡] §6" + v.initiatorName + " §rseized leadership via mutiny"});
    if ((int)g.log.size() > kGuildLogMax) g.log.resize(kGuildLogMax);
    if (!saveGuild(g)) return false;

    Database::getInstance().kvDel(keyMutiny(g.guildId));
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        mActiveMutinyGuilds.erase(g.guildId);
    }

    // Mail ke mantan leader
    json p; p["guildName"] = g.guildName;
    MailManager::getInstance().sendSystem(
        v.currentLeaderXuid,
        "§4You have been deposed",
        "§fGuild §6" + g.guildName + "§f has chosen new leadership. You remain a member.",
        MailType::GuildNotice, p.dump()
    );

    // Mail ke seluruh anggota (notif transfer)
    noticeAllMembers(g,
        "§eNew Guild Leader",
        "§f" + v.initiatorName + " §ais now the leader of §6" + g.guildName + "§a.",
        "");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ACTIVITY LOG
// ─────────────────────────────────────────────────────────────────────────────

void GuildManager::appendLog(const std::string& guildId, const std::string& text) {
    auto g = getGuild(guildId);
    if (!g) return;
    GuildLogEntry e;
    e.timestamp = nowUnix();
    e.text      = text;
    g->log.insert(g->log.begin(), std::move(e));
    if ((int)g->log.size() > kGuildLogMax) g->log.resize(kGuildLogMax);
    saveGuild(*g);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SEARCH BY NAME — substring case-insensitive
// ─────────────────────────────────────────────────────────────────────────────

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::vector<GuildData> GuildManager::searchByName(const std::string& query, int maxResults) const {
    std::vector<GuildData> out;
    const std::string q = toLower(query);
    auto& db = Database::getInstance();
    // kvScanPrefix("guild:") hanya match key dengan colon — tidak akan inklusi
    // "guild_by_member:", "guild_kick:", "guild_mutiny:" yang pakai underscore.
    db.kvScanPrefix("guild:", [&](const std::string&, const std::string& val) {
        auto g = deserializeGuild(val);
        if (!g) return true;
        if (q.empty() || toLower(g->guildName).find(q) != std::string::npos) {
            out.push_back(std::move(*g));
            if ((int)out.size() >= maxResults) return false;
        }
        return true;
    });
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  JOIN REQUEST
// ─────────────────────────────────────────────────────────────────────────────

bool GuildManager::sendJoinRequest(const std::string& guildId,
                                   const std::string& requesterXuid,
                                   const std::string& requesterName) {
    auto g = getGuild(guildId);
    if (!g) return false;
    if ((int)g->members.size() >= kGuildMaxMembers) return false;
    if (getGuildIdOfPlayer(requesterXuid)) return false; // requester sudah punya guild

    json payload;
    payload["guildId"]        = guildId;
    payload["guildName"]      = g->guildName;
    payload["requesterXuid"]  = requesterXuid;
    payload["requesterName"]  = requesterName;

    MailManager::getInstance().sendSystem(
        g->leaderXuid,
        "§2Join Request: " + requesterName,
        "§f" + requesterName + " §fwants to join your guild §6" + g->guildName + "§f.\n"
        "§7Open this mail to Accept or Decline.",
        MailType::GuildJoinRequest, payload.dump()
    );
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  NAMETAG REFRESH
// ─────────────────────────────────────────────────────────────────────────────

void GuildManager::refreshNametag(const std::string& xuid) {
    // ALWAYS set nametag — guaranteed correct state regardless of prior nametag.
    // Dipanggil dari removeMember/disband/rename/setEmblem yang butuh RESET
    // nametag kalau player keluar dari guild.
    auto* p = PlayerCache::getInstance().getOnlinePlayer(xuid);
    if (!p) return;
    auto g = getInstance().getGuildOfPlayer(xuid);
    std::string realName = std::string(p->getRealName());
    if (g) {
        std::string emblemPart = g->emblem.empty() ? "" : (g->emblem + " ");
        p->setNameTag("§6[" + emblemPart + g->guildName + "]§r\n§f" + realName);
    } else {
        p->setNameTag(realName);
    }
}

void GuildManager::refreshNametagAtJoin(const std::string& xuid) {
    // OPTIMIZED PATH untuk PlayerJoin handler — early exit kalau player gak
    // di guild. Bedrock sudah set nametag default ke real name saat join,
    // jadi gak perlu override kalau gak ada guild prefix.
    //
    // Tidak boleh dipakai di remove/disband/rename — itu butuh full refresh
    // untuk RESET nametag dari prefix lama.
    auto* p = PlayerCache::getInstance().getOnlinePlayer(xuid);
    if (!p) return;
    auto gid = getInstance().getGuildIdOfPlayer(xuid);  // O(1) cache
    if (!gid) return;  // No override needed
    auto g = getInstance().getGuild(*gid);
    if (!g) return;
    std::string realName = std::string(p->getRealName());
    std::string emblemPart = g->emblem.empty() ? "" : (g->emblem + " ");
    p->setNameTag("§6[" + emblemPart + g->guildName + "]§r\n§f" + realName);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CLEANUP — dipanggil dari tick loop
// ─────────────────────────────────────────────────────────────────────────────

void GuildManager::tickCleanup() {
    const long long now = nowUnix();
    auto& db = Database::getInstance();

    // ── Kick votes ─────────────────────────────────────────────
    // Snapshot guild-id list di bawah lock, lalu proses tanpa lock supaya
    // DB call + mail send tidak block form callback yang touch map.
    std::vector<std::string> activeKickIds;
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        activeKickIds.reserve(mActiveKickGuilds.size());
        for (const auto& [gid, _] : mActiveKickGuilds) activeKickIds.push_back(gid);
    }

    std::vector<std::pair<std::string, KickVote>> expiredKicks;
    for (const auto& gid : activeKickIds) {
        auto v = getKickVote(gid);
        if (!v || !v->active) {
            std::lock_guard<std::mutex> lock(mActiveVotesMu);
            mActiveKickGuilds.erase(gid); // stale entry — bersihkan
            continue;
        }
        if (v->expiresAt <= now) {
            expiredKicks.emplace_back(gid, *v);
        }
    }
    for (auto& [gid, v] : expiredKicks) {
        v.active = false;
        saveKick(v);                // saveKick juga akan erase dari mActiveKickGuilds (under lock)
        refundZI(v.initiatorXuid, kKickCostZI);

        json p; p["guildId"] = gid;
        MailManager::getInstance().sendSystem(
            v.initiatorXuid,
            "§eKick Vote Expired",
            "§fThe kick vote for §c" + v.targetName + "§f expired without unanimous approval.\n"
            "§a10 ZI has been refunded to your wallet.",
            MailType::GuildNotice, p.dump()
        );
        db.kvDel(keyKick(gid));
        {
            std::lock_guard<std::mutex> lock(mActiveVotesMu);
            mActiveKickGuilds.erase(gid);
        }
    }

    // ── Mutiny votes ───────────────────────────────────────────
    std::vector<std::string> activeMutinyIds;
    {
        std::lock_guard<std::mutex> lock(mActiveVotesMu);
        activeMutinyIds.reserve(mActiveMutinyGuilds.size());
        for (const auto& [gid, _] : mActiveMutinyGuilds) activeMutinyIds.push_back(gid);
    }

    std::vector<std::pair<std::string, MutinyVote>> expiredMutinies;
    for (const auto& gid : activeMutinyIds) {
        auto v = getMutinyVote(gid);
        if (!v || !v->active) {
            std::lock_guard<std::mutex> lock(mActiveVotesMu);
            mActiveMutinyGuilds.erase(gid);
            continue;
        }
        if (v->expiresAt <= now) {
            expiredMutinies.emplace_back(gid, *v);
        }
    }
    for (auto& [gid, v] : expiredMutinies) {
        v.active = false;
        saveMutiny(v);
        refundZI(v.initiatorXuid, kMutinyCostZI);

        json p; p["guildId"] = gid;
        MailManager::getInstance().sendSystem(
            v.initiatorXuid,
            "§eMutiny Expired",
            "§fYour leadership challenge expired without unanimous approval.\n"
            "§a10 ZI has been refunded to your wallet.",
            MailType::GuildNotice, p.dump()
        );
        db.kvDel(keyMutiny(gid));
        {
            std::lock_guard<std::mutex> lock(mActiveVotesMu);
            mActiveMutinyGuilds.erase(gid);
        }
    }
}

} // namespace gacha_mod
