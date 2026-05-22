#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "mod/GachaTypes.h"

namespace gacha_mod {

// Struct untuk menampung data item dari JSON (schema v3)
struct PoolItem {
    std::string itemId;          // "minecraft:netherite_sword" atau "currency:sheld_dust"
    std::string name;            // Nama yang tampil di form UI
    std::string ingameDisplayName; // Nama berwarna di atas item in-game (opsional)
    int         count  = 1;
    int         weight = 1;
    std::vector<Enchantment> enchantments; // array enchantment terstruktur
    std::string extraNbt;        // untuk potion type, dll.
};

// Informasi banner yang ditampilkan ke player di Form UI
struct BannerDisplayInfo {
    int                      week        = 1;
    std::string              name;         // Nama banner, cth: "Blade of Nether"
    std::string              theme;        // Keterangan singkat banner
    std::vector<std::string> featuredB5;  // Display name item B5 Rate On minggu ini
    std::vector<std::string> featuredB4;  // Display name item B4 Rate On minggu ini
};

class LootPoolManager {
public:
    static LootPoolManager& getInstance() {
        static LootPoolManager instance;
        return instance;
    }

    // Membaca dan mem-parsing gacha_pool.json
    bool loadConfig(const std::string& filePath);

    // Mengambil item acak berdasarkan Tier, Rate Type, dan Minggu berjalan
    GachaResult getRandomItem(GachaTier tier, RateType type, int bannerWeek) const;

    // Mengambil info display banner untuk minggu tertentu (untuk Form UI)
    const BannerDisplayInfo* getBannerDisplayInfo(int week) const;

    // Getter pool standar untuk ditampilkan di Banner Details
    const std::vector<PoolItem>& getB5Standard() const { return mB5Standard; }
    const std::vector<PoolItem>& getB4Standard() const { return mB4Standard; }
    const std::vector<PoolItem>& getB3Pool()     const { return mB3Pool; }

    // Getter pool Rate-On per minggu (dipakai admin debug command).
    // Mengembalikan nullptr kalau week tidak terdaftar.
    const std::vector<PoolItem>* getB5RateOn(int week) const {
        auto it = mB5RateOn.find(week);
        return it == mB5RateOn.end() ? nullptr : &it->second;
    }
    const std::vector<PoolItem>* getB4RateOn(int week) const {
        auto it = mB4RateOn.find(week);
        return it == mB4RateOn.end() ? nullptr : &it->second;
    }

private:
    LootPoolManager() = default;

    // Penyimpanan di memori (RAM)
    std::unordered_map<int, std::vector<PoolItem>> mB5RateOn;
    std::unordered_map<int, std::vector<PoolItem>> mB4RateOn;

    std::vector<PoolItem> mB5Standard;
    std::vector<PoolItem> mB4Standard;
    std::vector<PoolItem> mB3Pool;

    // Info banner per minggu untuk ditampilkan di UI
    std::unordered_map<int, BannerDisplayInfo> mBannerInfo;

    // Helper untuk mengambil elemen acak dari sebuah vector
    const PoolItem& pickRandom(const std::vector<PoolItem>& pool) const;
};

} // namespace gacha_mod