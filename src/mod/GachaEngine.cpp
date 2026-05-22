#include "mod/GachaEngine.h"
#include "mod/LootPoolManager.h"
#include "mod/BannerManager.h"
#include <random>
#include <algorithm>
#include <cmath>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────
//  THREAD-SAFE RNG GENERATORS
// ─────────────────────────────────────────────────────────────

float GachaEngine::rollRNG() {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, 100.0f);
    return dist(gen);
}

float GachaEngine::rollCoin() {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    return dist(gen);
}

// ─────────────────────────────────────────────────────────────
//  CORE GACHA LOGIC
// ─────────────────────────────────────────────────────────────

GachaResult GachaEngine::performPull(PlayerProfile& profile) {
    // 1. Increment Pity
    profile.incrementPityB5();
    profile.incrementPityB4();

    // 2. Ambil Rate dari Profile (Sudah termasuk perhitungan LossPity / Guarantee Window)
    float r5 = static_cast<float>(profile.calcRateB5() * 100.0);
    float r4 = static_cast<float>(profile.calcRateB4() * 100.0);

    // 3. Tentukan Prioritas (Dynamic Priority Stacking)
    bool isB5First = (r5 >= r4);
    float eff5, eff4, eff3;

    if (isB5First) {
        eff5 = r5;
        eff4 = std::min(r4, std::max(0.0f, 100.0f - r5));
        eff3 = std::max(0.0f, 100.0f - eff5 - eff4);
    } else {
        eff4 = r4;
        eff5 = std::min(r5, std::max(0.0f, 100.0f - r4));
        eff3 = std::max(0.0f, 100.0f - eff4 - eff5);
    }

    // 4. Roll Dadu Probabilitas
    float roll = rollRNG();
    GachaTier tierGot;

    if (isB5First) {
        if      (roll < eff5)          tierGot = GachaTier::B5;
        else if (roll < eff5 + eff4)   tierGot = GachaTier::B4;
        else                           tierGot = GachaTier::B3;
    } else {
        if      (roll < eff4)          tierGot = GachaTier::B4;
        else if (roll < eff4 + eff5)   tierGot = GachaTier::B5;
        else                           tierGot = GachaTier::B3;
    }

    // 5. Roll 50/50 (Hidden 40/60) & Penyesuaian State Pity
    RateType typeGot = RateType::None;

    if (tierGot == GachaTier::B5) {
        // Hidden mechanic: 40% Rate On, 60% Rate Off
        bool winRateOn = profile.isGuaranteedB5() || (rollCoin() < 0.40f);
        typeGot = winRateOn ? RateType::RateOn : RateType::RateOff;

        if (winRateOn) {
            profile.resetPityB5(); // Full reset jika dapat Rate On
        } else {
            profile.handleB5RateOff(); // Kalah 50/50 (40/60), simpan state LossPity
        }
    } 
    else if (tierGot == GachaTier::B4) {
        bool winRateOn = profile.isGuaranteedB4() || (rollCoin() < 0.40f);
        typeGot = winRateOn ? RateType::RateOn : RateType::RateOff;
        
        if (winRateOn) {
            profile.resetPityB4(); // Menang
        } else {
            profile.handleB4RateOff(); // Kalah, B4 berikutnya pasti Rate On
        }
    }

    // 6. Ambil Item Fisik dari LootPoolManager
    // Gunakan BannerManager (global) bukan profile.getBannerWeek() (per-player)
    int activeWeek = BannerManager::getInstance().getCurrentWeek();
    return LootPoolManager::getInstance().getRandomItem(tierGot, typeGot, activeWeek);
}

} // namespace gacha_mod