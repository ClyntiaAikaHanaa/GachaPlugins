#include "mod/MailManager.h"
#include "mod/Database.h"
#include "mod/MyMod.h"
#include "mod/PlayerProfile.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/Player.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <atomic>
#include <algorithm>

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

// Kirim chat notif ke target jika online. O(1) lookup via PlayerCache —
// dulu pakai level->forEachPlayer (O(N) tiap mail), yang jadi hot path saat
// mass-send (kick vote, guild notice, dst).
static void notifyOnline(const std::string& xuid, const std::string& message) {
    if (auto* p = PlayerCache::getInstance().getOnlinePlayer(xuid)) {
        p->sendMessage(message);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Serialisasi
// ─────────────────────────────────────────────────────────────────────────────

std::string MailManager::serialize(const MailMessage& m) {
    json j;
    j["mail_id"]   = m.mailId;
    j["to_xuid"]   = m.toXuid;
    j["from_xuid"] = m.fromXuid;
    j["from_name"] = m.fromName;
    j["subject"]   = m.subject;
    j["body"]      = m.body;
    j["type"]      = mailTypeToString(m.type);
    j["payload"]   = m.payload;
    j["read"]      = m.read;
    j["sent_at"]   = m.sentAt;
    return j.dump();
}

std::optional<MailMessage> MailManager::deserialize(const std::string& s) {
    try {
        auto j = json::parse(s);
        MailMessage m;
        m.mailId   = j.value("mail_id",   "");
        m.toXuid   = j.value("to_xuid",   "");
        m.fromXuid = j.value("from_xuid", "");
        m.fromName = j.value("from_name", "");
        m.subject  = j.value("subject",   "");
        m.body     = j.value("body",      "");
        m.type     = mailTypeFromString(j.value("type", "system"));
        m.payload  = j.value("payload",   "");
        m.read     = j.value("read",      false);
        m.sentAt   = j.value("sent_at",   0LL);
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ID Generator
// ─────────────────────────────────────────────────────────────────────────────

std::string MailManager::generateMailId() {
    static std::atomic<uint64_t> counter{0};
    long long t = nowUnix();
    uint64_t c  = counter.fetch_add(1);
    return "m" + std::to_string(t) + "_" + std::to_string(c);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Index Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Index format v2: {"v":2, "ids":[...], "unread": N}
// Old format (v1): [...]  (array of ids)
//
// readIndex() backwards-compatible: parse array as v1, object as v2.
// readIndexFull() returns both ids + unread count from v2 (or computes from v1).

std::vector<std::string> MailManager::readIndex(const std::string& xuid) const {
    auto raw = Database::getInstance().kvGet(indexKey(xuid));
    if (!raw || raw->empty()) return {};
    try {
        auto j = json::parse(*raw);
        if (j.is_array()) {
            // v1 format
            return j.get<std::vector<std::string>>();
        }
        if (j.is_object()) {
            // v2 format
            return j.value("ids", std::vector<std::string>{});
        }
        return {};
    } catch (...) {
        return {};
    }
}

// Read both ids and unread count. Computes unread by scanning kalau v1 format
// (lazy migration: dipersist saat next writeIndexFull).
MailManager::IndexData MailManager::readIndexFull(const std::string& xuid) const {
    IndexData out;
    auto raw = Database::getInstance().kvGet(indexKey(xuid));
    if (!raw || raw->empty()) return out;
    try {
        auto j = json::parse(*raw);
        if (j.is_object()) {
            out.ids    = j.value("ids", std::vector<std::string>{});
            out.unread = j.value("unread", 0);
            out.fromV1 = false;
            return out;
        }
        if (j.is_array()) {
            // v1: compute unread by scanning (one-time cost, then save as v2)
            out.ids    = j.get<std::vector<std::string>>();
            out.unread = 0;
            for (const auto& id : out.ids) {
                if (auto m = get(xuid, id); m && !m->read) ++out.unread;
            }
            out.fromV1 = true;
            return out;
        }
    } catch (...) {}
    return out;
}

bool MailManager::writeIndex(const std::string& xuid, const std::vector<std::string>& ids) {
    // Legacy-style — pakai writeIndexFull untuk maintain unread count.
    // Kalau dipanggil tanpa unread info, default ke 0 (caller harus update).
    IndexData d;
    d.ids = ids;
    d.unread = 0;
    return writeIndexFull(xuid, d);
}

bool MailManager::writeIndexFull(const std::string& xuid, const IndexData& data) {
    json j;
    j["v"]      = 2;
    j["ids"]    = data.ids;
    j["unread"] = std::max(0, data.unread);
    return Database::getInstance().kvSet(indexKey(xuid), j.dump());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Send
// ─────────────────────────────────────────────────────────────────────────────

std::string MailManager::send(const MailMessage& msgIn) {
    MailMessage m = msgIn;
    if (m.mailId.empty()) m.mailId = generateMailId();
    if (m.sentAt == 0)    m.sentAt = nowUnix();

    if (m.toXuid.empty()) return "";

    // Tulis mail
    if (!Database::getInstance().kvSet(mailKey(m.toXuid, m.mailId), serialize(m)))
        return "";

    // Update index full (ids + unread count) — mail baru = unread = +1.
    auto data = readIndexFull(m.toXuid);
    data.ids.insert(data.ids.begin(), m.mailId);
    data.unread += 1;
    pruneToCapacity(m.toXuid, data.ids);
    writeIndexFull(m.toXuid, data);

    // Sync in-memory cache supaya konsisten
    {
        std::lock_guard<std::mutex> lock(mUnreadMu);
        mUnreadCache[m.toXuid] = data.unread;
    }

    // Notifikasi chat kalau target online
    std::string notif = "§6[§eAetheria§6] §8» §fNew mail: §e" + m.subject
                      + " §7— open §a/account §7→ Inbox.";
    notifyOnline(m.toXuid, notif);

    return m.mailId;
}

std::string MailManager::sendSystem(const std::string& toXuid,
                                    const std::string& subject,
                                    const std::string& body,
                                    MailType type,
                                    const std::string& payload) {
    MailMessage m;
    m.toXuid   = toXuid;
    m.fromXuid = "";
    m.fromName = "System";
    m.subject  = subject;
    m.body     = body;
    m.type     = type;
    m.payload  = payload;
    return send(m);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Read / List / Modify
// ─────────────────────────────────────────────────────────────────────────────

std::optional<MailMessage> MailManager::get(const std::string& xuid,
                                            const std::string& mailId) const {
    auto raw = Database::getInstance().kvGet(mailKey(xuid, mailId));
    if (!raw || raw->empty()) return std::nullopt;
    return deserialize(*raw);
}

std::vector<std::string> MailManager::listIds(const std::string& xuid) const {
    return readIndex(xuid);
}

std::vector<MailMessage> MailManager::listAll(const std::string& xuid) const {
    std::vector<MailMessage> out;
    auto ids = readIndex(xuid);
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto m = get(xuid, id)) out.push_back(std::move(*m));
    }
    return out;
}

int MailManager::unreadCount(const std::string& xuid) const {
    // Fast path: in-memory cache hit (subsequent calls per session)
    {
        std::lock_guard<std::mutex> lock(mUnreadMu);
        auto it = mUnreadCache.find(xuid);
        if (it != mUnreadCache.end() && it->second >= 0) {
            return it->second;
        }
    }

    // Cache miss: pakai persisted count dari index (O(1) DB read).
    // OPTIMIZATION: dulu lazy populate dengan N+1 DB scan (mahal saat first
    // call). Sekarang index v2 simpan unread count langsung — first call
    // = 1 DB read instead of N+1.
    auto data = readIndexFull(xuid);

    // Lazy migrate dari v1 ke v2 (kalau perlu) — write back dengan computed unread.
    // Aman karena readIndexFull sudah scan saat fromV1=true.
    if (data.fromV1) {
        const_cast<MailManager*>(this)->writeIndexFull(xuid, data);
    }

    {
        std::lock_guard<std::mutex> lock(mUnreadMu);
        mUnreadCache[xuid] = data.unread;
    }
    return data.unread;
}

bool MailManager::markRead(const std::string& xuid, const std::string& mailId) {
    auto m = get(xuid, mailId);
    if (!m) return false;
    if (m->read) return true;
    m->read = true;
    bool ok = Database::getInstance().kvSet(mailKey(xuid, mailId), serialize(*m));
    if (ok) {
        // Persist decrement ke index (v2 format)
        auto data = readIndexFull(xuid);
        if (data.unread > 0) data.unread -= 1;
        writeIndexFull(xuid, data);

        // Sync cache
        std::lock_guard<std::mutex> lock(mUnreadMu);
        mUnreadCache[xuid] = data.unread;
    }
    return ok;
}

bool MailManager::remove(const std::string& xuid, const std::string& mailId) {
    // Cek apakah mail yang akan di-hapus masih unread → harus decrement count.
    bool wasUnread = false;
    if (auto m = get(xuid, mailId)) wasUnread = !m->read;

    bool ok = Database::getInstance().kvDel(mailKey(xuid, mailId));

    // Update index full (ids + unread) — atomically dalam satu DB write
    auto data = readIndexFull(xuid);
    data.ids.erase(std::remove(data.ids.begin(), data.ids.end(), mailId), data.ids.end());
    if (wasUnread && data.unread > 0) data.unread -= 1;
    writeIndexFull(xuid, data);

    // Sync cache
    {
        std::lock_guard<std::mutex> lock(mUnreadMu);
        mUnreadCache[xuid] = data.unread;
    }
    return ok;
}

void MailManager::clearAll(const std::string& xuid) {
    for (const auto& id : readIndex(xuid)) {
        Database::getInstance().kvDel(mailKey(xuid, id));
    }
    Database::getInstance().kvDel(indexKey(xuid));
    invalidateUnreadCache(xuid);
}

void MailManager::invalidateUnreadCache(const std::string& xuid) {
    std::lock_guard<std::mutex> lock(mUnreadMu);
    mUnreadCache.erase(xuid);
}

void MailManager::pruneToCapacity(const std::string& xuid, std::vector<std::string>& ids) {
    if ((int)ids.size() <= kMailMaxPerPlayer) return;
    // FIFO eviction — ids urut terbaru di depan, jadi yang dihapus dari belakang.
    auto& db = Database::getInstance();
    for (size_t i = kMailMaxPerPlayer; i < ids.size(); ++i) {
        db.kvDel(mailKey(xuid, ids[i]));
    }
    ids.resize(kMailMaxPerPlayer);
}

void MailManager::tickPurgeOldMail() {
    // Purge mail yang lebih lama dari retention untuk online players saja.
    // Untuk offline players, purge happens saat mereka login → trigger
    // unreadCount() → tidak fully necessary tapi cleanup di sini lebih agresif.
    const long long now    = nowUnix();
    const long long cutoff = now - (long long)kMailRetentionDays * 86400;

    auto level = ll::service::getLevel();
    if (!level) return;

    auto& db = Database::getInstance();

    // Kumpulkan xuid online dulu — hindari iterate sambil modifikasi.
    std::vector<std::string> onlineXuids;
    level->forEachPlayer([&onlineXuids](Player const& p) {
        onlineXuids.push_back(p.getXuid());
        return true;
    });

    for (const auto& xuid : onlineXuids) {
        auto data = readIndexFull(xuid);
        if (data.ids.empty()) continue;

        std::vector<std::string> kept;
        kept.reserve(data.ids.size());
        bool anyRemoved = false;
        int unreadRemoved = 0;
        for (const auto& id : data.ids) {
            auto m = get(xuid, id);
            if (m && m->sentAt < cutoff) {
                // Expired — purge
                db.kvDel(mailKey(xuid, id));
                if (!m->read) ++unreadRemoved;
                anyRemoved = true;
            } else {
                kept.push_back(id);
            }
        }
        if (anyRemoved) {
            data.ids    = std::move(kept);
            data.unread = std::max(0, data.unread - unreadRemoved);
            writeIndexFull(xuid, data);

            // Sync cache
            std::lock_guard<std::mutex> lock(mUnreadMu);
            mUnreadCache[xuid] = data.unread;
        }
    }
}

} // namespace gacha_mod
