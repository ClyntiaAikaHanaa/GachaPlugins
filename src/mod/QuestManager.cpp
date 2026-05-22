#include "mod/QuestManager.h"
#include "mod/PlayerProfile.h"
#include "mod/Database.h"
#include "mod/MyMod.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <format>

using json = nlohmann::json;

namespace gacha_mod {

// Helper untuk timestamp unix detik (server local time).
static long long nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ─────────────────────────────────────────────────────────────────────────────
//  RNG helpers (thread-local untuk safety)
// ─────────────────────────────────────────────────────────────────────────────

float QuestManager::rollChance() {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    return dist(gen);
}

size_t QuestManager::pickRandomIndex(size_t poolSize) {
    if (poolSize == 0) return 0;
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, poolSize - 1);
    return dist(gen);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Date/time helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string QuestManager::todayYmd() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

std::string QuestManager::thisMonthYm() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[12];
    std::strftime(buf, sizeof(buf), "%Y-%m", &tm);
    return std::string(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Config Loading
// ─────────────────────────────────────────────────────────────────────────────

static QuestDefinition parseDef(const json& j, QuestType kindFallback) {
    QuestDefinition d;
    d.id        = j.value("id", "");
    d.kind      = questTypeFromString(j.value("type", std::string()));
    if (d.id.empty()) d.kind = kindFallback;
    d.category  = questCategoryFromString(j.value("category", "Kill"));
    d.targetId  = j.value("target_id", "");
    d.target    = j.value("target", 1);
    d.rewardSD  = j.value("reward_sd", 0);
    d.rewardZI  = j.value("reward_zi", 0);
    d.display   = j.value("display", d.id);
    return d;
}

bool QuestManager::loadConfig(const std::string& filePath) {
    std::ifstream in(filePath);
    if (!in.is_open()) return false;
    try {
        json root;
        in >> root;

        mNormalPool.clear();
        mAdvancePool.clear();
        mSpecialPool.clear();
        mMonthlyPool.clear();

        for (const auto& q : root.value("normal_pool",   json::array())) mNormalPool .push_back(parseDef(q, QuestType::Normal));
        for (const auto& q : root.value("advance_pool",  json::array())) mAdvancePool.push_back(parseDef(q, QuestType::Advance));
        for (const auto& q : root.value("special_pool",  json::array())) mSpecialPool.push_back(parseDef(q, QuestType::Special));
        for (const auto& q : root.value("monthly_pool",  json::array())) mMonthlyPool.push_back(parseDef(q, QuestType::Monthly));

        if (root.contains("generation_rules")) {
            const auto& gr = root["generation_rules"];
            mNormalCount   = gr.value("normal_count",   2);
            mAdvanceChance = gr.value("advance_chance", 0.35f);
            mSpecialChance = gr.value("special_chance", 0.07f);
        }
        MyMod::getInstance().getSelf().getLogger().info(
            "[Quest] Pools loaded — Normal:{} Advance:{} Special:{} Monthly:{} (advChance:{:.2f}, spChance:{:.2f})",
            (int)mNormalPool.size(), (int)mAdvancePool.size(),
            (int)mSpecialPool.size(), (int)mMonthlyPool.size(),
            mAdvanceChance, mSpecialChance);
        return true;
    } catch (const std::exception& e) {
        MyMod::getInstance().getSelf().getLogger().error(
            "[Quest] Failed to parse quest_config.json: {}", e.what());
        return false;
    } catch (...) {
        MyMod::getInstance().getSelf().getLogger().error(
            "[Quest] Unknown error while parsing quest_config.json");
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Active Quest creation + serialization
// ─────────────────────────────────────────────────────────────────────────────

ActiveQuest QuestManager::defToActive(const QuestDefinition& d) {
    ActiveQuest q;
    q.id        = d.id;
    q.kind      = d.kind;
    q.category  = d.category;
    q.targetId  = d.targetId;
    q.target    = d.target;
    q.progress  = 0;
    q.rewardSD  = d.rewardSD;
    q.rewardZI  = d.rewardZI;
    q.display   = d.display;
    q.completed = false;
    return q;
}

static json questToJson(const ActiveQuest& q) {
    return json{
        {"id",        q.id},
        {"kind",      questTypeToString(q.kind)},
        {"category",  questCategoryToString(q.category)},
        {"target_id", q.targetId},
        {"target",    q.target},
        {"progress",  q.progress},
        {"reward_sd", q.rewardSD},
        {"reward_zi", q.rewardZI},
        {"display",   q.display},
        {"completed", q.completed},
    };
}

static ActiveQuest jsonToQuest(const json& j) {
    ActiveQuest q;
    q.id        = j.value("id",        "");
    q.kind      = questTypeFromString(j.value("kind", "Normal"));
    q.category  = questCategoryFromString(j.value("category", "Kill"));
    q.targetId  = j.value("target_id", "");
    q.target    = j.value("target",    1);
    q.progress  = j.value("progress",  0);
    q.rewardSD  = j.value("reward_sd", 0);
    q.rewardZI  = j.value("reward_zi", 0);
    q.display   = j.value("display",   "");
    q.completed = j.value("completed", false);
    return q;
}

std::string QuestManager::serializeQuests(const std::vector<ActiveQuest>& qs) {
    json arr = json::array();
    for (const auto& q : qs) arr.push_back(questToJson(q));
    return arr.dump();
}

std::vector<ActiveQuest> QuestManager::deserializeQuests(const std::string& s) {
    std::vector<ActiveQuest> out;
    try {
        auto j = json::parse(s);
        if (!j.is_array()) return out;
        for (const auto& jq : j) out.push_back(jsonToQuest(jq));
    } catch (...) {}
    return out;
}

std::string QuestManager::serializeMonthly(const std::optional<ActiveQuest>& q) {
    if (!q) return "{}";
    return questToJson(*q).dump();
}

std::optional<ActiveQuest> QuestManager::deserializeMonthly(const std::string& s) {
    try {
        auto j = json::parse(s);
        if (!j.is_object() || !j.contains("id") || j["id"].get<std::string>().empty())
            return std::nullopt;
        return jsonToQuest(j);
    } catch (...) {
        return std::nullopt;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Generation
// ─────────────────────────────────────────────────────────────────────────────

void QuestManager::generateDailyQuests(PlayerProfile& profile) {
    auto& logger = MyMod::getInstance().getSelf().getLogger();
    if (mNormalPool.empty()) {
        logger.warn("[Quest] generateDailyQuests called but Normal pool is empty! "
                    "Check quest_config.json exists at <data_dir>/quest_config.json");
    }
    std::vector<ActiveQuest> generated;

    // 1. Pick N normal quests (no duplicates)
    if (!mNormalPool.empty()) {
        std::vector<size_t> indices;
        for (size_t i = 0; i < mNormalPool.size(); ++i) indices.push_back(i);
        // Shuffle indices
        thread_local std::random_device rd;
        thread_local std::mt19937 gen(rd());
        std::shuffle(indices.begin(), indices.end(), gen);

        int pick = std::min(mNormalCount, static_cast<int>(indices.size()));
        for (int i = 0; i < pick; ++i) {
            generated.push_back(defToActive(mNormalPool[indices[i]]));
        }
    }

    // 2. Roll Advance chance
    if (!mAdvancePool.empty() && rollChance() < mAdvanceChance) {
        generated.push_back(defToActive(mAdvancePool[pickRandomIndex(mAdvancePool.size())]));
    }

    // 3. Roll Special chance
    if (!mSpecialPool.empty() && rollChance() < mSpecialChance) {
        generated.push_back(defToActive(mSpecialPool[pickRandomIndex(mSpecialPool.size())]));
    }

    profile.setDailyQuestJson(serializeQuests(generated));
    profile.setLastQuestReset(todayYmd());
    logger.info("[Quest] Generated {} daily quest(s) for {}", (int)generated.size(), profile.getXuid());
}

void QuestManager::generateMonthlyQuest(PlayerProfile& profile) {
    if (mMonthlyPool.empty()) {
        profile.setMonthlyQuestJson("{}");
        return;
    }
    auto q = defToActive(mMonthlyPool[pickRandomIndex(mMonthlyPool.size())]);
    // Stamp month langsung saat generate biar tidak race dengan recordKill
    // yang fire sebelum checkAndReset sempat stamp di iterasi terpisah.
    json j = questToJson(q);
    j["month"] = thisMonthYm();
    profile.setMonthlyQuestJson(j.dump());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Reset Logic
// ─────────────────────────────────────────────────────────────────────────────

bool QuestManager::checkAndReset(PlayerProfile& profile) {
    bool didReset = false;

    // Daily check — compare YYYY-MM-DD (cheap string compare)
    const std::string today = todayYmd();
    if (profile.getLastQuestReset() != today) {
        generateDailyQuests(profile);
        didReset = true;
    }

    // Monthly check — OPTIMIZED: pakai field lastMonthlyReset (YYYY-MM string).
    // Dulu parse JSON mMonthlyQuestJson tiap join untuk extract "month" field
    // (lambat, dipanggil di hot path). Sekarang cukup string compare → 100%
    // kasus same-month skip 0 JSON parse.
    const std::string thisMonth = thisMonthYm();
    if (profile.getLastMonthlyReset() != thisMonth) {
        // generateMonthlyQuest sudah stamp "month" di JSON sendiri (race-safe).
        generateMonthlyQuest(profile);
        profile.setLastMonthlyReset(thisMonth);
        didReset = true;
    }

    if (didReset) {
        // saveQuests partial write — tidak include lastMonthlyReset field.
        // Pakai full save() supaya lastMonthlyReset ter-persist juga.
        profile.save();
    }
    return didReset;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Active Quest readers
// ─────────────────────────────────────────────────────────────────────────────

std::vector<ActiveQuest> QuestManager::getActiveDailyQuests(const PlayerProfile& profile) const {
    return deserializeQuests(profile.getDailyQuestJson());
}

std::optional<ActiveQuest> QuestManager::getActiveMonthlyQuest(const PlayerProfile& profile) const {
    return deserializeMonthly(profile.getMonthlyQuestJson());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Reward Delivery
// ─────────────────────────────────────────────────────────────────────────────

void QuestManager::sendRewardNotif(Player& player, const ActiveQuest& q) {
    std::string rewardText;
    if (q.rewardZI > 0)
        rewardText = std::format("§e+{} Zen Ingot", q.rewardZI);
    else if (q.rewardSD > 0)
        rewardText = std::format("§3+{} SheldDust", q.rewardSD);
    else
        rewardText = "§7(no reward)";

    std::string kindLabel;
    switch (q.kind) {
        case QuestType::Normal:  kindLabel = "§a[Daily]";   break;
        case QuestType::Advance: kindLabel = "§b[Advance]"; break;
        case QuestType::Special: kindLabel = "§d[Special]"; break;
        case QuestType::Monthly: kindLabel = "§6[Monthly]"; break;
    }

    player.sendMessage(std::format(
        "§6[§eAetheria§6] §8» {} §fQuest complete: §f{} §8| {} §fadded to your wallet!",
        kindLabel, q.display, rewardText));
}

void QuestManager::tryCompleteAndReward(PlayerProfile& profile, Player& player, ActiveQuest& q) {
    if (q.completed) return;
    if (q.progress < q.target) return;

    // Cap progress
    q.progress  = q.target;
    q.completed = true;

    // Reward delivery
    if (q.rewardSD > 0) profile.addSheldDust(q.rewardSD);
    if (q.rewardZI > 0) profile.addZenIngot(q.rewardZI);

    sendRewardNotif(player, q);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Kill Tracking
// ─────────────────────────────────────────────────────────────────────────────

void QuestManager::recordKill(PlayerProfile& profile, Player& player,
                              const std::string& mobTypeName) {
    bool anyUpdate = false;

    // Daily quests
    auto daily = getActiveDailyQuests(profile);
    for (auto& q : daily) {
        if (q.completed) continue;
        if (q.category != QuestCategory::Kill) continue;
        if (q.targetId != mobTypeName) continue;
        ++q.progress;
        anyUpdate = true;
        tryCompleteAndReward(profile, player, q);
    }
    if (anyUpdate) profile.setDailyQuestJson(serializeQuests(daily));

    // Monthly quest
    auto monthly = getActiveMonthlyQuest(profile);
    if (monthly && !monthly->completed
        && monthly->category == QuestCategory::Kill
        && monthly->targetId == mobTypeName) {
        ++monthly->progress;
        tryCompleteAndReward(profile, player, *monthly);

        // Re-serialize, preserving month stamp
        json j = questToJson(*monthly);
        try {
            auto orig = json::parse(profile.getMonthlyQuestJson());
            if (orig.is_object() && orig.contains("month")) j["month"] = orig["month"];
        } catch (...) {}
        profile.setMonthlyQuestJson(j.dump());
        anyUpdate = true;
    }

    // Partial write — hemat I/O daripada full profile.save() yang re-write semua field.
    if (anyUpdate) {
        Database::getInstance().saveQuests(
            profile.getXuid(),
            profile.getDailyQuestJson(),
            profile.getMonthlyQuestJson(),
            profile.getLastQuestReset());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Item Submission
//
//  requestedAmount: jumlah yang player minta untuk submit.
//  Mengembalikan: jumlah aktual yang berhasil di-deduct dari inventory & dicatat.
// ─────────────────────────────────────────────────────────────────────────────

int QuestManager::submitItems(PlayerProfile& profile, Player& player,
                              const std::string& questId, int requestedAmount) {
    if (requestedAmount <= 0) return 0;

    // Cari quest di daily quests (collect quest tidak ada di monthly per spec)
    auto daily = getActiveDailyQuests(profile);
    int submitted = 0;
    bool anyUpdate = false;

    for (auto& q : daily) {
        if (q.id != questId) continue;
        if (q.completed) return 0;
        if (q.category != QuestCategory::Collect) return 0;

        // Single pass: deduct up to min(requested, remaining target).
        // Tidak perlu count terpisah — kita stop saat sudah cukup.
        const int cap = std::min(requestedAmount, q.target - q.progress);
        if (cap <= 0) return 0;

        auto& inv = player.getInventory();
        const int slots = inv.getContainerSize();
        int taken = 0;
        for (int i = 0; i < slots && taken < cap; ++i) {
            const auto& item = inv.getItem(i);
            if (item.getTypeName() != q.targetId) continue;
            // SKIP items dengan NBT — melindungi:
            //   • Shulker box / bundle yang berisi item (kalau quest minta wadah)
            //   • Item enchanted / renamed / dengan damage NBT
            //   • Item dengan custom data lainnya
            // Tanpa skip ini, player bisa kehilangan isi shulker / item langka
            // yang kebetulan punya typeName sama dengan target quest.
            if (item.mUserData != nullptr) continue;
            int take = std::min(cap - taken, static_cast<int>(item.mCount));
            inv.removeItem(i, take);
            taken += take;
        }
        if (taken <= 0) return 0;
        player.sendInventory(false);

        // Update progress
        q.progress += taken;
        submitted = taken;
        anyUpdate = true;
        tryCompleteAndReward(profile, player, q);
        break;
    }

    if (anyUpdate) {
        profile.setDailyQuestJson(serializeQuests(daily));
        // Partial write
        Database::getInstance().saveQuests(
            profile.getXuid(),
            profile.getDailyQuestJson(),
            profile.getMonthlyQuestJson(),
            profile.getLastQuestReset());
    }
    return submitted;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hit Tracker — untuk credit kill yang delayed (fire aspect, drowning, fall)
// ─────────────────────────────────────────────────────────────────────────────

void QuestManager::trackHit(int64_t mobUniqueId, const std::string& killerXuid) {
    if (mobUniqueId == 0 || killerXuid.empty()) return;
    std::lock_guard<std::mutex> lock(mHitsMu);
    mHits[mobUniqueId] = HitEntry{killerXuid, nowUnix()};
}

std::string QuestManager::lookupRecentHitter(int64_t mobUniqueId) const {
    if (mobUniqueId == 0) return "";
    std::lock_guard<std::mutex> lock(mHitsMu);
    auto it = mHits.find(mobUniqueId);
    if (it == mHits.end()) return "";
    const long long age = nowUnix() - it->second.ts;
    if (age > kHitTTL) return ""; // sudah expired
    return it->second.xuid;
}

void QuestManager::clearHit(int64_t mobUniqueId) {
    if (mobUniqueId == 0) return;
    std::lock_guard<std::mutex> lock(mHitsMu);
    mHits.erase(mobUniqueId);
}

void QuestManager::pruneExpiredHits() {
    std::lock_guard<std::mutex> lock(mHitsMu);
    const long long now = nowUnix();
    for (auto it = mHits.begin(); it != mHits.end(); ) {
        if (now - it->second.ts > kHitTTL) it = mHits.erase(it);
        else                                ++it;
    }
}

} // namespace gacha_mod
