#pragma once
#include "mod/QuestData.h"
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>

// Forward declare global Player & Actor
class Player;
class Actor;

namespace gacha_mod {

class PlayerProfile;

// ─────────────────────────────────────────────────────────────────────────────
//  QuestManager — singleton
//
//  Tanggung jawab:
//    1. Load quest_config.json saat mod startup
//    2. Generate daily/monthly quests untuk player (saat reset)
//    3. Track progress kill / collect quest
//    4. Auto-claim reward saat target tercapai → kredit wallet + notif chat
//    5. Reset daily (00:00 WIB) & monthly (tanggal 1)
//
//  Storage:
//    - Quest pool definitions: in-memory (loaded from JSON sekali)
//    - Player active quest state: di PlayerProfile.dailyQuestJson & monthlyQuestJson
//      (field existing, sudah di-persist ke LevelDB via Database)
// ─────────────────────────────────────────────────────────────────────────────

class QuestManager {
public:
    static QuestManager& getInstance() {
        static QuestManager instance;
        return instance;
    }

    // Load quest pool definitions dari JSON config
    bool loadConfig(const std::string& filePath);

    // ── Reset Logic ──────────────────────────────────────────
    // Cek tanggal saat ini vs lastQuestReset di profile.
    // Kalau tanggal beda (atau pertama kali), generate set quest baru.
    // Return true kalau ada reset (daily/monthly) yang terjadi.
    bool checkAndReset(PlayerProfile& profile);

    // ── Quest Generation (internal, exposed untuk admin/test) ─
    void generateDailyQuests(PlayerProfile& profile);
    void generateMonthlyQuest(PlayerProfile& profile);

    // ── Event Tracking ───────────────────────────────────────
    // Dipanggil dari MobDieEvent listener. Cek semua kill quest aktif player,
    // increment progress kalau target match. Auto-reward saat selesai.
    void recordKill(PlayerProfile& profile, Player& player,
                    const std::string& mobTypeName);

    // Dipanggil saat player submit item via NPC form.
    // Mengembalikan jumlah item yang berhasil di-submit (clamped ke remaining).
    int submitItems(PlayerProfile& profile, Player& player,
                    const std::string& questId, int requestedAmount);

    // ── Query / Reader ───────────────────────────────────────
    std::vector<ActiveQuest> getActiveDailyQuests(const PlayerProfile& profile) const;
    std::optional<ActiveQuest> getActiveMonthlyQuest(const PlayerProfile& profile) const;

    // Item ID match check (sama persis, no fuzzy match)
    bool isCollectTarget(const ActiveQuest& q, const std::string& itemId) const {
        return q.category == QuestCategory::Collect && q.targetId == itemId;
    }

    // ── Hit Tracker (untuk delayed kills) ────────────────────
    // Dipanggil dari ActorHurtEvent: rekam attacker player saat mob terkena
    // damage langsung. Dipakai untuk credit kill via fire aspect, drowning,
    // fall damage, dst — di mana MobDieEvent source bukan entity langsung.
    //
    // Key: int64_t ActorUniqueID (stable across pointer lifecycle, beda dengan
    // Actor* yang slot-nya bisa reused oleh allocator → salah credit kill).
    void trackHit(int64_t mobUniqueId, const std::string& killerXuid);

    // Dipanggil dari MobDieEvent kalau source bukan entity. Cari attacker
    // terakhir dari tracker (TTL 60 detik). Return xuid atau string kosong.
    std::string lookupRecentHitter(int64_t mobUniqueId) const;

    // Hapus entry tracker setelah mob mati.
    void clearHit(int64_t mobUniqueId);

    // Periodic cleanup (dipanggil dari tick loop).
    void pruneExpiredHits();

    // ── Helpers ──────────────────────────────────────────────
    static std::string todayYmd();    // "2026-05-15" — server local time (WIB)
    static std::string thisMonthYm(); // "2026-05"
    static std::string serializeQuests(const std::vector<ActiveQuest>& qs);
    static std::vector<ActiveQuest> deserializeQuests(const std::string& json);
    static std::string serializeMonthly(const std::optional<ActiveQuest>& q);
    static std::optional<ActiveQuest> deserializeMonthly(const std::string& json);

private:
    QuestManager() = default;

    // Quest pools (read-only setelah loadConfig)
    std::vector<QuestDefinition> mNormalPool;
    std::vector<QuestDefinition> mAdvancePool;
    std::vector<QuestDefinition> mSpecialPool;
    std::vector<QuestDefinition> mMonthlyPool;

    // Hit tracker — key = ActorUniqueID (stable int64), value = killer xuid + timestamp.
    // Sebelumnya pakai Actor const* sebagai key — bug: pointer slot bisa reused
    // oleh allocator setelah mob mati. Mob baru di address yang sama bakal match
    // stale entry → salah credit kill ke player random.
    struct HitEntry { std::string xuid; long long ts; };
    mutable std::unordered_map<int64_t, HitEntry> mHits;
    mutable std::mutex mHitsMu;            // guard all access to mHits
    static constexpr long long kHitTTL = 60; // seconds

    // Generation rules (dari config)
    int   mNormalCount    = 2;
    float mAdvanceChance  = 0.35f;
    float mSpecialChance  = 0.07f;

    // Convert QuestDefinition → ActiveQuest (fresh state)
    static ActiveQuest defToActive(const QuestDefinition& d);

    // Random helpers
    static float  rollChance();         // 0.0–1.0
    static size_t pickRandomIndex(size_t poolSize);

    // Internal: setelah recordKill/submitItems, kalau quest selesai → pay reward.
    void tryCompleteAndReward(PlayerProfile& profile, Player& player, ActiveQuest& q);

    // Helper: kirim chat notif reward
    static void sendRewardNotif(Player& player, const ActiveQuest& q);
};

} // namespace gacha_mod
