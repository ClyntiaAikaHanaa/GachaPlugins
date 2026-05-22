#include "mod/Database.h"
#include "mod/MyMod.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────
//  SINGLETON
// ─────────────────────────────────────────────────────────────

Database& Database::getInstance() {
    static Database instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────
//  INIT & CLOSE
// ─────────────────────────────────────────────────────────────

bool Database::init(const std::string& dbPath) {
    try {
        mDb = std::make_unique<ll::data::KeyValueDB>(dbPath);
        mInitialized = true;
        MyMod::getInstance().getSelf().getLogger().info(
            "Database berhasil diinisialisasi: {}", dbPath);
        return true;
    } catch (const std::exception& e) {
        MyMod::getInstance().getSelf().getLogger().error(
            "Gagal membuka database: {}", e.what());
        return false;
    }
}

void Database::close() {
    if (mInitialized) {
        mDb.reset();
        mInitialized = false;
        MyMod::getInstance().getSelf().getLogger().info("Database ditutup.");
    }
}

// ─────────────────────────────────────────────────────────────
//  GET OR CREATE
// ─────────────────────────────────────────────────────────────

PlayerData Database::getOrCreate(const std::string& xuid) {
    auto key = makeKey(xuid);
    auto val = mDb->get(key);

    if (val.has_value() && !val->empty()) {
        return fromJson(xuid, *val);
    }

    // Player baru
    PlayerData data = makeDefault(xuid);
    save(data);
    MyMod::getInstance().getSelf().getLogger().info(
        "Player baru terdaftar: {}", xuid);
    return data;
}

// ─────────────────────────────────────────────────────────────
//  SAVE (full)
// ─────────────────────────────────────────────────────────────

void Database::save(const PlayerData& data) {
    mDb->set(makeKey(data.xuid), toJson(data));
}

bool Database::exists(const std::string& xuid) {
    auto val = mDb->get(makeKey(xuid));
    return val.has_value() && !val->empty();
}

// ─────────────────────────────────────────────────────────────
//  FOR EACH PLAYER
// ─────────────────────────────────────────────────────────────

void Database::forEachPlayer(std::function<void(PlayerData&)> callback) {
    for (auto const& [key, value] : mDb->iter()) {
        if (key.starts_with("player:")) {
            std::string xuid(key.substr(7));
            auto data = fromJson(xuid, std::string(value));
            callback(data);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  UPDATE PARSIAL
// ─────────────────────────────────────────────────────────────

void Database::savePity(const std::string& xuid,
                         int pityB5, int guaranteeWindowB5, bool guaranteedFlagB5,
                         int pityB4, bool guaranteedFlagB4) {
    auto data = getOrCreate(xuid);
    data.pityB5            = pityB5;
    data.guaranteeWindowB5 = guaranteeWindowB5;
    data.guaranteedFlagB5  = guaranteedFlagB5;
    data.pityB4            = pityB4;
    data.guaranteedFlagB4  = guaranteedFlagB4;
    save(data);
}

void Database::saveCurrency(const std::string& xuid,
                              int sheldDust, int zenIngot) {
    auto data = getOrCreate(xuid);
    data.sheldDust = sheldDust;
    data.zenIngot  = zenIngot;
    save(data);
}

void Database::saveQuests(const std::string& xuid,
                           const std::string& dailyJson,
                           const std::string& monthlyJson,
                           const std::string& lastReset) {
    auto data = getOrCreate(xuid);
    data.dailyQuestJson   = dailyJson;
    data.monthlyQuestJson = monthlyJson;
    data.lastQuestReset   = lastReset;
    save(data);
}

// ─────────────────────────────────────────────────────────────
//  RAW KV ACCESS (untuk GuildManager / MailManager)
// ─────────────────────────────────────────────────────────────

bool Database::kvSet(const std::string& key, const std::string& value) {
    if (!mInitialized || !mDb) return false;
    try { mDb->set(key, value); return true; }
    catch (...) { return false; }
}

std::optional<std::string> Database::kvGet(const std::string& key) const {
    if (!mInitialized || !mDb) return std::nullopt;
    try { return mDb->get(key); }
    catch (...) { return std::nullopt; }
}

bool Database::kvDel(const std::string& key) {
    if (!mInitialized || !mDb) return false;
    try { mDb->del(key); return true; }
    catch (...) { return false; }
}

bool Database::kvHas(const std::string& key) const {
    auto v = kvGet(key);
    return v.has_value() && !v->empty();
}

void Database::kvScanPrefix(const std::string& prefix,
                            std::function<bool(const std::string&, const std::string&)> cb) const {
    if (!mInitialized || !mDb) return;
    for (auto const& [k, v] : mDb->iter()) {
        if (k.starts_with(prefix)) {
            if (!cb(std::string(k), std::string(v))) break;
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  SERIALISASI JSON
// ─────────────────────────────────────────────────────────────

std::string Database::toJson(const PlayerData& d) {
    json j;
    j["pity_b5"]             = d.pityB5;
    j["guarantee_window_b5"] = d.guaranteeWindowB5;
    j["guaranteed_flag_b5"]  = d.guaranteedFlagB5;
    j["pity_b4"]             = d.pityB4;
    j["guaranteed_flag_b4"]  = d.guaranteedFlagB4;
    j["sheld_dust"]          = d.sheldDust;
    j["zen_ingot"]           = d.zenIngot;
    j["daily_quest_json"]    = d.dailyQuestJson;
    j["monthly_quest_json"]  = d.monthlyQuestJson;
    j["last_quest_reset"]    = d.lastQuestReset;
    j["last_monthly_reset"]  = d.lastMonthlyReset;
    j["active_banner_week"]  = d.activeBannerWeek;
    j["show_welcome_popup"]  = d.showWelcomePopup;
    return j.dump();
}

PlayerData Database::fromJson(const std::string& xuid, const std::string& jsonStr) {
    PlayerData d;
    d.xuid = xuid;
    try {
        auto j = json::parse(jsonStr);
        d.pityB5            = j.value("pity_b5",             0);
        d.guaranteeWindowB5 = j.value("guarantee_window_b5", 70);
        d.guaranteedFlagB5  = j.value("guaranteed_flag_b5",  false);
        d.pityB4            = j.value("pity_b4",             0);
        d.guaranteedFlagB4  = j.value("guaranteed_flag_b4",  false);
        d.sheldDust         = j.value("sheld_dust",          0);
        d.zenIngot          = j.value("zen_ingot",           0);
        d.dailyQuestJson    = j.value("daily_quest_json",    std::string("[]"));
        d.monthlyQuestJson  = j.value("monthly_quest_json",  std::string("{}"));
        d.lastQuestReset    = j.value("last_quest_reset",    std::string(""));
        d.lastMonthlyReset  = j.value("last_monthly_reset",  std::string(""));
        d.activeBannerWeek  = j.value("active_banner_week",  1);
        d.showWelcomePopup  = j.value("show_welcome_popup", true);
    } catch (...) {
        // JSON corrupt — return default agar tidak crash
        d = makeDefault(xuid);
    }
    return d;
}

PlayerData Database::makeDefault(const std::string& xuid) {
    PlayerData d;
    d.xuid              = xuid;
    d.pityB5            = 0;
    d.guaranteeWindowB5 = 70;
    d.guaranteedFlagB5  = false;
    d.pityB4            = 0;
    d.guaranteedFlagB4  = false;
    d.sheldDust         = 0;
    d.zenIngot          = 0;
    d.dailyQuestJson    = "[]";
    d.monthlyQuestJson  = "{}";
    d.lastQuestReset    = "";
    d.lastMonthlyReset  = "";
    d.activeBannerWeek  = 1;
    d.showWelcomePopup  = true;
    return d;
}

} // namespace gacha_mod
