#include "mod/LootPoolManager.h"
#include "mod/MyMod.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <stdexcept>

using json = nlohmann::json;

namespace gacha_mod {

auto& loggerLoot() {
    return MyMod::getInstance().getSelf().getLogger();
}

bool LootPoolManager::loadConfig(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        loggerLoot().error("[GachaMod] Gagal membuka konfigurasi JSON: {}", filePath);
        return false;
    }

    try {
        json j;
        file >> j;

        // Bersihkan memori sebelum memuat ulang (berguna untuk fitur /gachaadmin reload)
        mB5RateOn.clear();
        mB4RateOn.clear();
        mB5Standard.clear();
        mB4Standard.clear();
        mB3Pool.clear();

        // Helper lambda untuk parsing JSON array ke vector C++ (schema v3)
        auto parsePool = [](const json& jsonArray, std::vector<PoolItem>& targetPool) {
            for (const auto& item : jsonArray) {
                PoolItem p;
                // Support both old "id" and new "item_id" field
                p.itemId  = item.contains("item_id") ? item.value("item_id", "minecraft:air")
                                                     : item.value("id",      "minecraft:air");
                p.name    = item.value("name", "Unknown Item");
                p.ingameDisplayName = item.value("display_name", "");
                p.count   = item.value("count",  1);
                p.weight  = item.value("weight", 1);
                p.extraNbt = item.value("extra_nbt", "");

                // Parse structured enchantments array
                if (item.contains("enchantments")) {
                    for (const auto& e : item.at("enchantments")) {
                        Enchantment ench;
                        ench.id    = e.value("id",    "");
                        ench.level = e.value("level", 1);
                        if (!ench.id.empty())
                            p.enchantments.push_back(std::move(ench));
                    }
                }
                // Fallback: old "nbt" string field — store in extraNbt so it isn't lost
                else if (item.contains("nbt") && !item.value("nbt", "").empty()) {
                    p.extraNbt = item.value("nbt", "");
                }

                targetPool.push_back(std::move(p));
            }
        };

        // 1. Load B5 & B4 Rate On (Per Minggu 1-4)
        for (int week = 1; week <= 4; ++week) {
            std::string wStr = std::to_string(week);
            if (j["b5_rate_on"].contains(wStr)) {
                parsePool(j["b5_rate_on"][wStr], mB5RateOn[week]);
            }
            if (j["b4_rate_on"].contains(wStr)) {
                parsePool(j["b4_rate_on"][wStr], mB4RateOn[week]);
            }
        }

        // 2. Load Standard & B3 Pools
        if (j.contains("b5_standard")) parsePool(j["b5_standard"], mB5Standard);
        if (j.contains("b4_standard")) parsePool(j["b4_standard"], mB4Standard);
        if (j.contains("b3_pool"))     parsePool(j["b3_pool"], mB3Pool);

        // 3. Load banner display info (untuk ditampilkan di Form UI)
        mBannerInfo.clear();
        if (j.contains("banner_info")) {
            for (const auto& bi : j["banner_info"]) {
                BannerDisplayInfo info;
                info.week  = bi.value("week", 1);
                info.name  = bi.value("name", "Unknown Banner");
                info.theme = bi.value("theme", "");
                for (const auto& it : bi.value("featured_b5", json::array()))
                    info.featuredB5.push_back(it.get<std::string>());
                for (const auto& it : bi.value("featured_b4", json::array()))
                    info.featuredB4.push_back(it.get<std::string>());
                mBannerInfo[info.week] = std::move(info);
            }
        }

        loggerLoot().info("[GachaMod] Berhasil memuat gacha_pool.json!");
        return true;

    } catch (const std::exception& e) {
        loggerLoot().error("[GachaMod] Error parsing JSON: {}", e.what());
        return false;
    }
}

// Mengacak item dari pool dengan weighted random
const PoolItem& LootPoolManager::pickRandom(const std::vector<PoolItem>& pool) const {
    if (pool.empty()) {
        throw std::runtime_error("Loot pool kosong! Cek gacha_pool.json");
    }

    static std::random_device rd;
    static std::mt19937 gen(rd());

    // Hitung total weight
    int totalWeight = 0;
    for (const auto& item : pool) totalWeight += std::max(item.weight, 1);

    std::uniform_int_distribution<int> dist(1, totalWeight);
    int roll = dist(gen);

    int cumulative = 0;
    for (const auto& item : pool) {
        cumulative += std::max(item.weight, 1);
        if (roll <= cumulative) return item;
    }
    return pool.back(); // fallback (tidak seharusnya tercapai)
}

GachaResult LootPoolManager::getRandomItem(GachaTier tier, RateType type, int bannerWeek) const {
    const PoolItem* selectedItem = nullptr;

    switch (tier) {
        case GachaTier::B5:
            if (type == RateType::RateOn) {
                // Pastikan minggu valid, jika tidak lempar ke minggu 1
                int w = mB5RateOn.count(bannerWeek) ? bannerWeek : 1;
                selectedItem = &pickRandom(mB5RateOn.at(w));
            } else {
                selectedItem = &pickRandom(mB5Standard);
            }
            break;
            
        case GachaTier::B4:
            if (type == RateType::RateOn) {
                int w = mB4RateOn.count(bannerWeek) ? bannerWeek : 1;
                selectedItem = &pickRandom(mB4RateOn.at(w));
            } else {
                selectedItem = &pickRandom(mB4Standard);
            }
            break;
            
        case GachaTier::B3:
        default:
            selectedItem = &pickRandom(mB3Pool);
            break;
    }

    // Ubah PoolItem → GachaResult
    return GachaResult{
        tier,
        type,
        selectedItem->itemId,
        selectedItem->name,
        selectedItem->ingameDisplayName,
        selectedItem->count,
        selectedItem->enchantments,
        selectedItem->extraNbt
    };
}

const BannerDisplayInfo* LootPoolManager::getBannerDisplayInfo(int week) const {
    auto it = mBannerInfo.find(week);
    return (it != mBannerInfo.end()) ? &it->second : nullptr;
}

} // namespace gacha_mod