#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace gacha_mod {

//  TIER & RATE TYPE
// ─────────────────────────────────────────────────────────────

enum class GachaTier : int {
    B5 = 5,   // Legendary
    B4 = 4,   // Epic
    B3 = 3    // Rare (filler)
};

enum class RateType {
    RateOn,   // Hadiah utama banner aktif
    RateOff,  // Standard pool
    None      // Untuk B3 — tidak ada Rate On/Off
};

//  QUEST
// ─────────────────────────────────────────────────────────────

enum class QuestType {
    Normal,    // 100% chance — reward 10-15 SD
    Advance,   //  10% chance — reward 20-25 SD
    Special,   //   2% chance — reward 30-40 SD
    Monthly    // sekali per bulan — reward 10 ZI
};

enum class QuestCategory {
    Kill,     // Bunuh mob
    Collect   // Serahkan item ke NPC
};

struct QuestData {
    std::string questId;
    QuestType   type;
    QuestCategory category;
    std::string targetId;    // Mob ID atau Item ID
    int         targetMin;   // Target jumlah minimum
    int         targetMax;   // Target jumlah maksimum (random saat spawn quest)
    int         rewardMin;   // Reward SD/ZI minimum
    int         rewardMax;   // Reward SD/ZI maksimum
};

//  ENCHANTMENT
// ─────────────────────────────────────────────────────────────

struct Enchantment {
    std::string id;       // e.g. "minecraft:sharpness"
    int         level = 1;
};

//  HASIL GACHA
// ─────────────────────────────────────────────────────────────

struct GachaResult {
    GachaTier   tier;
    RateType    rateType;
    std::string itemId;              // "minecraft:netherite_sword" atau "currency:sheld_dust"
    std::string displayName;         // Nama untuk form UI / chat feedback
    std::string ingameDisplayName;   // Nama berwarna yang muncul di atas item in-game (opsional)
    int         count    = 1;
    std::vector<Enchantment> enchantments;
    std::string extraNbt;            // Untuk potion type, dll.
};

//  SHRINE ALTAR DATA
// ─────────────────────────────────────────────────────────────

struct AltarPos {
    int x;
    int y;
    int z;
};

} // namespace gacha_mod