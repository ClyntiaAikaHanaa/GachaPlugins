#pragma once
#include "mod/GachaTypes.h"  // QuestType, QuestCategory (existing)
#include <string>
#include <vector>

namespace gacha_mod {

// QuestType & QuestCategory sudah didefinisikan di GachaTypes.h:
//   enum class QuestType    { Normal, Advance, Special, Monthly };
//   enum class QuestCategory{ Kill, Collect };
// File ini hanya menambahkan helper + struct baru untuk Quest System.

inline const char* questTypeToString(QuestType k) {
    switch (k) {
        case QuestType::Normal:  return "Normal";
        case QuestType::Advance: return "Advance";
        case QuestType::Special: return "Special";
        case QuestType::Monthly: return "Monthly";
    }
    return "Normal";
}

inline QuestType questTypeFromString(const std::string& s) {
    if (s == "Advance") return QuestType::Advance;
    if (s == "Special") return QuestType::Special;
    if (s == "Monthly") return QuestType::Monthly;
    return QuestType::Normal;
}

inline const char* questCategoryToString(QuestCategory c) {
    return (c == QuestCategory::Kill) ? "Kill" : "Collect";
}

inline QuestCategory questCategoryFromString(const std::string& s) {
    return (s == "Collect") ? QuestCategory::Collect : QuestCategory::Kill;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Quest Definition (dari quest_config.json — read-only)
// ─────────────────────────────────────────────────────────────────────────────

struct QuestDefinition {
    std::string   id;          // "kill_zombie_10"
    QuestType     kind;
    QuestCategory category;
    std::string   targetId;    // "minecraft:zombie" atau "minecraft:wheat"
    int           target;      // jumlah yang harus dicapai
    int           rewardSD;    // SheldDust (Normal/Advance/Special)
    int           rewardZI;    // Zen Ingot (Monthly)
    std::string   display;     // teks UI: "Bunuh 10 Zombie"
};

// ─────────────────────────────────────────────────────────────────────────────
//  Active Quest (state per-player, di-serialisasi di PlayerData JSON fields)
// ─────────────────────────────────────────────────────────────────────────────

struct ActiveQuest {
    std::string   id;
    QuestType     kind;
    QuestCategory category;
    std::string   targetId;
    int           target   = 0;
    int           progress = 0;
    int           rewardSD = 0;
    int           rewardZI = 0;
    std::string   display;
    bool          completed = false;
};

} // namespace gacha_mod
