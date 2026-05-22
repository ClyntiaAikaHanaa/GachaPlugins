#include "mod/PlayerProfile.h"
#include "mod/Database.h"
#include "mod/MyMod.h"
#include <cmath>
#include <ctime>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <format>
 
namespace gacha_mod {
 
auto& logger() {
    return MyMod::getInstance().getSelf().getLogger();
}


// Pop up welcome message untuk player baru

void PlayerProfile::flushSettings() const {
    auto data = toData();
    Database::getInstance().save(data); 
}


//  PLAYER PROFILE
 
PlayerProfile::PlayerProfile(const std::string& xuid) : mXuid(xuid) {
    loadFromData(Database::getInstance().getOrCreate(xuid));
}
 

//  CURRENCY
 
void PlayerProfile::addSheldDust(int amount) {
    if (amount <= 0) return;
    mSheldDust += amount;
    flushCurrency();
}
 
void PlayerProfile::addZenIngot(int amount) {
    if (amount <= 0) return;
    mZenIngot += amount;
    flushCurrency();
}
 
bool PlayerProfile::spendSheldDust(int amount) {
    if (amount <= 0 || mSheldDust < amount) return false;
    mSheldDust -= amount;
    flushCurrency();
    return true;
}
 
bool PlayerProfile::spendZenIngot(int amount) {
    if (amount <= 0 || mZenIngot < amount) return false;
    mZenIngot -= amount;
    flushCurrency();
    return true;
}
 
bool PlayerProfile::convertDiamondToSD(int diamonds) {
    // Pengurangan item di inventory dilakukan oleh caller (hook layer)
    if (diamonds < kDiaToSD) return false;
    int gained = diamonds / kDiaToSD;
    addSheldDust(gained);
    return true;
}
 
bool PlayerProfile::convertSDtoZI(int sdAmount) {
    if (sdAmount < kSDtoZI) return false;
    if (!spendSheldDust(sdAmount)) return false;
    addZenIngot(sdAmount / kSDtoZI);
    return true;
}
 

//  PITY B5 — Dynamic Guarantee Window

void PlayerProfile::incrementPityB5() {
    ++mPityB5;
    flushPity();
}
 
void PlayerProfile::resetPityB5() {
    // Menang 50/50 atau dapat Rate On garansi — full reset
    mPityB5            = 0;
    mGuaranteeWindowB5 = 70;
    mGuaranteedFlagB5  = false;
    flushPity();
}
 
void PlayerProfile::handleB5RateOff() {
    // Kalah 50/50:
    // → Guaranteed flag aktif
    // → Window dinamis = pity saat ini + 30
    // → Counter TIDAK direset (state persistence)
    mGuaranteedFlagB5  = true;
    mGuaranteeWindowB5 = mPityB5 + kGuaranteeAdd;
 
    logger().info(
        "[GachaMod] {} kalah 50/50 B5. Guarantee window: pull ke-{}",
        mXuid, mGuaranteeWindowB5);
 
    flushPity();
}
 

//  PITY B4
 
void PlayerProfile::incrementPityB4() {
    ++mPityB4;
    flushPity();
}
 
void PlayerProfile::resetPityB4() {
    mPityB4           = 0;
    mGuaranteedFlagB4 = false;
    flushPity();
}
 
void PlayerProfile::handleB4RateOff() {
    mGuaranteedFlagB4 = true;
    mPityB4 = 0; // <--- Tambahkan baris ini agar sinkron dengan prototipe
    flushPity();
}
 
// ─────────────────────────────────────────────────────────────
//  RATE CALCULATION
// ─────────────────────────────────────────────────────────────
 
double PlayerProfile::calcRateB5() const {
    if (!mGuaranteedFlagB5) {
        // ── PHASE 1 — Normal ─────────────────────────────────
        // Pull 1-40: rate statis 0.6%
        // Pull 41-70: eksponensial n=4
        // Rate_B5 = 0.6% + 99.4% × ((pity - 40) / 30)^4
        if (mPityB5 <= kSoftPityB5)
            return kRateB5Base;
        double t = std::min(static_cast<double>(mPityB5 - kSoftPityB5) / 30.0, 1.0);
        return std::min(kRateB5Base + 0.994 * std::pow(t, 4.0), 1.0);
    } else {
        // ── PHASE 2 — Jendela Garansi Dinamis ────────────────
        // LossPity = guaranteeWindowB5 - kGuaranteeAdd
        // Rate_B5 = 0.6% + 99.4% × ((pity - LossPity) / 30)^4
        // Menyentuh 100% tepat di LossPity + 30
        int lossPity = mGuaranteeWindowB5 - kGuaranteeAdd;
        if (mPityB5 <= lossPity)
            return kRateB5Base;
        double t = std::min(static_cast<double>(mPityB5 - lossPity) / 30.0, 1.0);
        return std::min(kRateB5Base + 0.994 * std::pow(t, 4.0), 1.0);
    }
}
 
double PlayerProfile::calcRateB4() const {
    // Eksponensial n=5: 5.1% (pity 0) → 100% (pity 20)
    // Rate_B4 = 5.1% + 94.9% × (pity / 20)^5
    // Kurva tajam — hampir flat di awal, melonjak drastis mendekati pity ke-20
    double t = std::min(static_cast<double>(mPityB4) / static_cast<double>(kHardPityB4), 1.0);
    return std::min(kRateB4Base + 0.949 * std::pow(t, 5.0), 1.0);
}
 
// ─────────────────────────────────────────────────────────────
//  PERSISTENCE
// ─────────────────────────────────────────────────────────────
 
void PlayerProfile::save() const {
    Database::getInstance().save(toData());
}
 
void PlayerProfile::reload() {
    loadFromData(Database::getInstance().getOrCreate(mXuid));
}
 
std::string PlayerProfile::getPitySummary() const {
    // ── Time-based greeting ───────────────────────────────────
    // Pakai localtime_s (thread-safe per-call buffer) bukan std::localtime
    // yang share static buffer — race condition kalau 2 form callback paralel.
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    int hour = 12;
#if defined(_WIN32)
    if (localtime_s(&lt, &now) == 0) hour = lt.tm_hour;
#else
    if (localtime_r(&now, &lt))      hour = lt.tm_hour;
#endif
 
    std::string greeting, greetIcon;
    if (hour >= 5 && hour < 12) {
        greeting  = "Good Morning";
        greetIcon = "§e☀ ";
    } else if (hour >= 12 && hour < 17) {
        greeting  = "Good Afternoon";
        greetIcon = "§6☀ ";
    } else if (hour >= 17 && hour < 21) {
        greeting  = "Good Evening";
        greetIcon = "§5✦ ";
    } else {
        greeting  = "Good Night";
        greetIcon = "§9✦ ";
    }
 
    // ── Pity B5 label ─────────────────────────────────────────
    std::string b5Status = mGuaranteedFlagB5
        ? "§c⚡ GUARANTEED Rate On"
        : "§7⚖ 50/50";
 
    // ── Pity B4 label ─────────────────────────────────────────
    std::string b4Status = mGuaranteedFlagB4
        ? "§c⚡ GUARANTEED Rate On"
        : "§7⚖ 50/50";
 
    // ── Pull cost hint ────────────────────────────────────────
    std::string pullHint = mZenIngot > 0
        ? std::format("§aReady to pull! §f({} pulls available)", mZenIngot)
        : "§7Earn more ZI from quests to pull!";
 
    return std::format(
        "{}§f{}!\n"
        "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
        "§b  GACHA SERVER §8| §fWelcome Back\n"
        "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
        "§e  ★ PITY STATUS\n"
        "§f  B5 §8» §a{}/{}  {}\n"
        "§f     §8└ Rate: §b{:.2f}%\n"
        "§f  B4 §8» §a{}/20  {}\n"
        "§f     §8└ Rate: §b{:.2f}%\n"
        "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
        "§6  ✦ WALLET\n"
        "§f  Zen Ingot §8» §e{} ZI\n"
        "§f  SheldDust §8» §3{} SD\n"
        "§f  {}\n"
        "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
        greetIcon, greeting,
        mPityB5, mGuaranteeWindowB5, b5Status,
        calcRateB5() * 100.0,
        mPityB4, b4Status,
        calcRateB4() * 100.0,
        mZenIngot,
        mSheldDust,
        pullHint
    );
}
 
// ─────────────────────────────────────────────────────────────
//  PRIVATE HELPERS
// ─────────────────────────────────────────────────────────────
 
void PlayerProfile::loadFromData(const PlayerData& d) {
    mPityB5            = d.pityB5;
    mGuaranteeWindowB5 = d.guaranteeWindowB5;
    mGuaranteedFlagB5  = d.guaranteedFlagB5;
    mPityB4            = d.pityB4;
    mGuaranteedFlagB4  = d.guaranteedFlagB4;
    mSheldDust         = d.sheldDust;
    mZenIngot          = d.zenIngot;
    mDailyQuestJson    = d.dailyQuestJson;
    mMonthlyQuestJson  = d.monthlyQuestJson;
    mLastQuestReset    = d.lastQuestReset;
    mLastMonthlyReset  = d.lastMonthlyReset;
    mActiveBannerWeek  = d.activeBannerWeek;
    mShowWelcomePopup  = d.showWelcomePopup;
}
 
PlayerData PlayerProfile::toData() const {
    PlayerData d;
    d.xuid              = mXuid;
    d.pityB5            = mPityB5;
    d.guaranteeWindowB5 = mGuaranteeWindowB5;
    d.guaranteedFlagB5  = mGuaranteedFlagB5;
    d.pityB4            = mPityB4;
    d.guaranteedFlagB4  = mGuaranteedFlagB4;
    d.sheldDust         = mSheldDust;
    d.zenIngot          = mZenIngot;
    d.dailyQuestJson    = mDailyQuestJson;
    d.monthlyQuestJson  = mMonthlyQuestJson;
    d.lastQuestReset    = mLastQuestReset;
    d.lastMonthlyReset  = mLastMonthlyReset;
    d.activeBannerWeek  = mActiveBannerWeek;
    d.showWelcomePopup  = mShowWelcomePopup;
    return d;
}
 
// Update parsial — lebih hemat I/O dari pada save() penuh
void PlayerProfile::flushPity() const {
    Database::getInstance().savePity(
        mXuid, mPityB5, mGuaranteeWindowB5, mGuaranteedFlagB5,
        mPityB4, mGuaranteedFlagB4);
}
 
void PlayerProfile::flushCurrency() const {
    Database::getInstance().saveCurrency(mXuid, mSheldDust, mZenIngot);
}
 
// ═════════════════════════════════════════════════════════════
//  PLAYER CACHE
// ═════════════════════════════════════════════════════════════
 
PlayerCache& PlayerCache::getInstance() {
    static PlayerCache instance;
    return instance;
}
 
PlayerProfile& PlayerCache::onPlayerJoin(const std::string& xuid, ::Player* p) {
    std::lock_guard lock(mMutex);

    // Register/refresh Player* untuk lookup O(1). Pointer di-update tiap join
    // sehingga reconnect (Player* baru) selalu terdaftar dengan pointer terkini.
    if (p) mOnlinePlayers[xuid] = p;

    // Jika sudah ada (reconnect) — reload dari DB
    if (mCache.count(xuid)) {
        mCache[xuid]->reload();
        return *mCache[xuid];
    }

    mCache[xuid] = std::make_unique<PlayerProfile>(xuid);
    logger().info("[GachaMod] Profile loaded: {}", xuid);
    return *mCache[xuid];
}

void PlayerCache::onPlayerLeave(const std::string& xuid) {
    std::lock_guard lock(mMutex);
    mOnlinePlayers.erase(xuid);
    auto it = mCache.find(xuid);
    if (it != mCache.end()) {
        it->second->save();
        mCache.erase(it);
        logger().info("[GachaMod] Profile saved & unloaded: {}", xuid);
    }
}

PlayerProfile& PlayerCache::get(const std::string& xuid) {
    std::lock_guard lock(mMutex);
    auto it = mCache.find(xuid);
    if (it == mCache.end())
        throw std::runtime_error("Player tidak online di cache: " + xuid);
    return *it->second;
}

bool PlayerCache::isOnline(const std::string& xuid) const {
    std::lock_guard lock(mMutex);
    return mCache.count(xuid) > 0;
}

::Player* PlayerCache::getOnlinePlayer(const std::string& xuid) const {
    std::lock_guard lock(mMutex);
    auto it = mOnlinePlayers.find(xuid);
    return (it == mOnlinePlayers.end()) ? nullptr : it->second;
}
 
void PlayerCache::saveAll() {
    std::lock_guard lock(mMutex);
    for (auto& [xuid, profile] : mCache)
        profile->save();
    logger().info("[GachaMod] Flush selesai — {} player disimpan.", mCache.size());
}
 
size_t PlayerCache::count() const {
    std::lock_guard lock(mMutex);
    return mCache.size();
}
 
} // namespace gacha_mod