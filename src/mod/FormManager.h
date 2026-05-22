#pragma once
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/form/ModalForm.h"
#include "mod/PlayerProfile.h"
#include "mod/GachaEngine.h"
#include "mod/LootPoolManager.h"
#include "mod/BannerManager.h"
#include "mod/ExchangeManager.h"
#include "mod/GuildManager.h"
#include "mod/MailManager.h"
#include "mod/QuestManager.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/safety/RedactableString.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <cctype>
#include <format>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────────────────────────

namespace form_util {

inline std::string buildWalletLine(const PlayerProfile& profile) {
    return std::format(
        "§f  Zen Ingot §8» §e{} ZI\n§f  SheldDust §8» §3{} SD",
        profile.getZenIngot(),
        profile.getSheldDust()
    );
}

inline std::string buildPityLines(const PlayerProfile& profile) {
    constexpr int kBarWidth = 16;

    auto makeBar = [&](int val, int max, const std::string& color) {
        int filled = std::min(kBarWidth, val * kBarWidth / std::max(max, 1));
        return color + std::string(filled, '|') + "§8" + std::string(kBarWidth - filled, '|');
    };

    std::string b5Rate = profile.isGuaranteedB5()
        ? "§c⚡ GUARANTEED"
        : std::format("§a{:.2f}%", profile.calcRateB5() * 100.0);

    std::string b4Rate = profile.isGuaranteedB4()
        ? "§c⚡ GUARANTEED"
        : std::format("§b{:.2f}%", profile.calcRateB4() * 100.0);

    return std::format(
        "§6  ★ §fLegendary B5 §8» §a{}/{}  {}\n"
        "§8    [{}§8]\n"
        "§5  ★ §fEpic B4      §8» §b{}/20  {}\n"
        "§8    [{}§8]",
        profile.getPityB5(), profile.getGuaranteeWindowB5(), b5Rate,
        makeBar(profile.getPityB5(), profile.getGuaranteeWindowB5(), "§a"),
        profile.getPityB4(), b4Rate,
        makeBar(profile.getPityB4(), 20, "§b")
    );
}

inline std::string formatPullLine(int index, const GachaResult& res) {
    std::string tierTag;
    switch (res.tier) {
        case GachaTier::B5: tierTag = "§6✦ Legendary"; break;
        case GachaTier::B4: tierTag = "§5✦ Epic";      break;
        default:            tierTag = "§b✦ Rare";      break;
    }
    std::string rateTag;
    if (res.tier != GachaTier::B3)
        rateTag = (res.rateType == RateType::RateOn) ? " §a[Rate On]" : " §7[Rate Off]";
    return std::format("§7{:>2}. {}§f {}{}", index + 1, tierTag, res.displayName, rateTag);
}

// Count diamonds in the player's main inventory.
// Lewati item dengan NBT (renamed / shulker-contained / dst.) supaya hitungan
// konsisten dengan takeDiamonds() — kalau countDiamonds bilang 10 tapi
// takeDiamonds cuma bisa ambil 3, UX-nya buruk.
inline int countDiamonds(Player& player) {
    auto& inv = player.getInventory();
    int total = 0;
    int size  = inv.getContainerSize();
    for (int i = 0; i < size; ++i) {
        const auto& item = inv.getItem(i);
        if (item.getTypeName() != "minecraft:diamond") continue;
        if (item.mUserData != nullptr) continue;
        total += static_cast<int>(item.mCount);
    }
    return total;
}

// Remove `amount` diamonds from player's inventory; returns false if not enough.
// Calls player.sendInventory() after removal to force-sync server state to client,
// preventing the client's stale view from overwriting the removal on next interaction.
inline bool takeDiamonds(Player& player, int amount) {
    auto& inv     = player.getInventory();
    int remaining = amount;
    int size      = inv.getContainerSize();
    for (int i = 0; i < size && remaining > 0; ++i) {
        const auto& item = inv.getItem(i);
        if (item.getTypeName() != "minecraft:diamond") continue;
        // SKIP diamond dengan NBT (renamed, enchanted, dari shulker/bundle).
        // Vanilla diamond yang ditambang tidak punya NBT, jadi flow normal aman.
        // Tanpa skip ini, diamond gacha bernama bisa hilang tanpa sengaja.
        if (item.mUserData != nullptr) continue;
        int take = std::min(remaining, static_cast<int>(item.mCount));
        inv.removeItem(i, take);
        remaining -= take;
    }
    bool success = (remaining == 0);
    if (success) {
        // Force-push the updated server inventory to the client so the diamond
        // reduction is visible immediately and cannot be overwritten by a
        // subsequent client-side inventory packet.
        player.sendInventory(false);
    }
    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CLAIM SYSTEM — item delivery setelah pull berhasil
// ─────────────────────────────────────────────────────────────────────────────

// Mapping nama enchantment (minecraft:xxx) → numeric ID Bedrock Edition
inline int enchantToBedrockId(const std::string& name) {
    static const std::unordered_map<std::string, int> kMap = {
        {"minecraft:protection",            0},
        {"minecraft:fire_protection",       1},
        {"minecraft:feather_falling",       2},
        {"minecraft:blast_protection",      3},
        {"minecraft:projectile_protection", 4},
        {"minecraft:thorns",                5},
        {"minecraft:respiration",           6},
        {"minecraft:depth_strider",         7},
        {"minecraft:aqua_affinity",         8},
        {"minecraft:sharpness",             9},
        {"minecraft:smite",                10},
        {"minecraft:bane_of_arthropods",   11},
        {"minecraft:knockback",            12},
        {"minecraft:fire_aspect",          13},
        {"minecraft:looting",              14},
        {"minecraft:efficiency",           15},
        {"minecraft:silk_touch",           16},
        {"minecraft:unbreaking",           17},
        {"minecraft:fortune",              18},
        {"minecraft:power",                19},
        {"minecraft:punch",                20},
        {"minecraft:flame",                21},
        {"minecraft:infinity",             22},
        {"minecraft:luck_of_the_sea",      23},
        {"minecraft:lure",                 24},
        {"minecraft:frost_walker",         25},
        {"minecraft:mending",              26},
        {"minecraft:binding",              27},
        {"minecraft:vanishing",            28},
        {"minecraft:impaling",             29},
        {"minecraft:riptide",              30},
        {"minecraft:loyalty",              31},
        {"minecraft:channeling",           32},
        {"minecraft:multishot",            33},
        {"minecraft:piercing",             34},
        {"minecraft:quick_charge",         35},
        {"minecraft:soul_speed",           36},
        {"minecraft:swift_sneak",          37},
        {"minecraft:wind_burst",           38},
        {"minecraft:density",              39},
        {"minecraft:breach",               40},
    };
    auto it = kMap.find(name);
    return it != kMap.end() ? it->second : -1;
}

// Bangun SNBT enchantment string untuk ItemStack::setUserData
// MCBE menggunakan tag "ench" untuk SEMUA item termasuk enchanted_book.
// "StoredEnchantments" adalah konvensi Java Edition — tidak dikenali oleh Bedrock engine.
// isBook disimpan sebagai parameter agar signature tidak berubah.
inline std::string buildEnchantSnbt(const std::vector<Enchantment>& enchants, bool /*isBook*/) {
    if (enchants.empty()) return "";
    // Selalu gunakan "ench" — di MCBE ini berlaku untuk senjata, armor, alat, dan buku
    std::string snbt = "{ench:[";
    bool first = true;
    for (const auto& e : enchants) {
        int id = enchantToBedrockId(e.id);
        if (id < 0) continue; // enchant tidak dikenal di Bedrock, lewati
        if (!first) snbt += ",";
        snbt += "{id:" + std::to_string(id) + "s,lvl:" + std::to_string(e.level) + "s}";
        first = false;
    }
    snbt += "]}";
    // Kembalikan string kosong jika tidak ada enchant valid
    if (snbt == "{ench:[]}") return "";
    return snbt;
}

// Berikan satu GachaResult ke player.
// Currency ("currency:xxx") → langsung ke database.
// Item biasa → player.addAndRefresh(); jika inventory penuh → drop di kaki.
// Mengembalikan: 0=gagal/unknown, 1=masuk inventory, 2=di-drop, 3=currency credited
inline int giveGachaItem(Player& player, PlayerProfile& profile, const GachaResult& res) {
    // ── Currency: langsung ke database ──────────────────────────────────────
    if (res.itemId.rfind("currency:", 0) == 0) {
        const std::string curr = res.itemId.substr(9);
        if (curr == "sheld_dust") {
            profile.addSheldDust(res.count);
            return 3;
        } else if (curr == "zen_ingot") {
            profile.addZenIngot(res.count);
            return 3;
        }
        return 0; // currency tidak dikenal
    }

    // ── Physical item ────────────────────────────────────────────────────────

    // Item dengan max stack size = 1 di MCBE — count > 1 dalam satu ItemStack akan
    // menyebabkan item tidak bisa dipindah di inventory. Solusi: kalau res.count > 1,
    // BUKAN clamp ke 1, tapi LOOP dan kirim N stack terpisah masing-masing berisi 1 item.
    static const std::unordered_set<std::string> kMaxStack1 = {
        "minecraft:totem_of_undying",
        "minecraft:elytra",
        "minecraft:trident",
        "minecraft:bow",
        "minecraft:crossbow",
        "minecraft:fishing_rod",
        "minecraft:flint_and_steel",
        "minecraft:shield",
        "minecraft:enchanted_book",
        "minecraft:netherite_sword",   "minecraft:diamond_sword",   "minecraft:iron_sword",
        "minecraft:netherite_axe",     "minecraft:diamond_axe",     "minecraft:iron_axe",
        "minecraft:netherite_pickaxe", "minecraft:diamond_pickaxe", "minecraft:iron_pickaxe",
        "minecraft:netherite_shovel",  "minecraft:diamond_shovel",  "minecraft:iron_shovel",
        "minecraft:netherite_hoe",     "minecraft:diamond_hoe",     "minecraft:iron_hoe",
        "minecraft:netherite_helmet",  "minecraft:diamond_helmet",  "minecraft:iron_helmet",
        "minecraft:netherite_chestplate","minecraft:diamond_chestplate","minecraft:iron_chestplate",
        "minecraft:netherite_leggings","minecraft:diamond_leggings","minecraft:iron_leggings",
        "minecraft:netherite_boots",   "minecraft:diamond_boots",   "minecraft:iron_boots",
        "minecraft:mace",
    };
    const bool maxStack1   = (kMaxStack1.count(res.itemId) > 0);
    const int  perStack    = maxStack1 ? 1 : res.count;       // ukuran per ItemStack
    const int  numStacks   = maxStack1 ? res.count : 1;       // jumlah ItemStack yang dibuat

    // Di MCBE, jenis potion ditentukan oleh auxValue (parameter ke-3 $reinit),
    // bukan oleh NBT tag. extraNbt untuk potion berisi angka integer auxValue.
    const bool isPotion = (res.itemId == "minecraft:potion"         ||
                           res.itemId == "minecraft:splash_potion"   ||
                           res.itemId == "minecraft:lingering_potion");

    int auxVal = 0;
    if (isPotion && !res.extraNbt.empty()) {
        try { auxVal = std::stoi(res.extraNbt); } catch (...) {}
    }

    // Build NBT/SNBT sekali (sama untuk semua stack)
    std::string snbt;
    if (!isPotion) {
        const bool isBook = (res.itemId == "minecraft:enchanted_book");
        if (!res.extraNbt.empty()) snbt = res.extraNbt;
        else                       snbt = buildEnchantSnbt(res.enchantments, isBook);
    }

    int claimedCount = 0, droppedCount = 0;

    for (int s = 0; s < numStacks; ++s) {
        ItemStack item;
        item.$reinit(res.itemId, perStack, isPotion ? auxVal : 0);
        if (item.isNull()) return 0; // item ID tidak valid

        // Apply NBT (enchants etc.) — potion sudah dapat type via auxValue
        if (!snbt.empty()) {
            auto tagResult = CompoundTag::fromSnbt(snbt);
            if (tagResult) {
                item.setUserData(std::make_unique<CompoundTag>(std::move(*tagResult)));
            }
        }

        // Set nama berwarna di atas item — HANYA kalau item memang sudah
        // non-stackable dengan vanilla secara natural. Kalau diterapkan ke item
        // plain stackable (cooked_beef, iron_ingot, arrow, dll), customName
        // akan menambah NBT → item gak bisa stack dengan vanilla counterpart.
        //
        // Item dianggap "sudah non-stackable" kalau salah satu kondisi terpenuhi:
        //   • Punya enchantments / extra NBT (snbt non-empty)
        //   • Potion (auxValue bikin unique per type, vanilla potion stack=1)
        //   • Max stack 1 (weapon, armor, tool — stack tidak relevan)
        const bool itemAlreadyNonStackable = !snbt.empty() || isPotion || maxStack1;
        if (itemAlreadyNonStackable && !res.ingameDisplayName.empty()) {
            Bedrock::Safety::RedactableString rs{
                std::string(res.ingameDisplayName), std::nullopt};
            item.setCustomName(rs);
        }

        // Berikan ke player (addAndRefresh = add + sync inventory ke client)
        if (player.addAndRefresh(item)) {
            ++claimedCount;
        } else {
            // Inventory penuh → drop di kaki player
            player.drop(item, false);
            ++droppedCount;
        }
    }

    if (claimedCount > 0) return 1;
    if (droppedCount > 0) return 2;
    return 0;
}

// Claim semua hasil pull sekaligus.
// Mengembalikan tuple {claimed, dropped, currency}
inline std::tuple<int,int,int> claimGachaResults(
    Player& player, PlayerProfile& profile,
    const std::vector<GachaResult>& results)
{
    int claimed = 0, dropped = 0, currency = 0;
    for (const auto& res : results) {
        int r = giveGachaItem(player, profile, res);
        if (r == 1) ++claimed;
        else if (r == 2) ++dropped;
        else if (r == 3) ++currency;
    }
    return {claimed, dropped, currency};
}

} // namespace form_util


// ─────────────────────────────────────────────────────────────────────────────
//  FORM MANAGER
// ─────────────────────────────────────────────────────────────────────────────

class FormManager {
public:

    // =========================================================================
    //  1.  WELCOME MENU
    // =========================================================================

    static void showWelcomeMenu(Player& player, PlayerProfile& profile) {
        ll::form::CustomForm form;
        form.setTitle("§l§8─── §6AETHERIA §8───");
        form.appendLabel(
            "§8────────────────────\n"
            "§l§eWelcome to the World of Aetheria!§r\n"
            "§8────────────────────\n"
            "\n"
            "§7Ancient magical energy crystallizes into §3SheldDust "
            "§8(§3SD§8)§7. Collect it at the §aAdventure Guild§7, "
            "exchange for §eZen Ingots §8(§eZI§8)§7, then spend them "
            "at the §6Ancient Shrine§7 for Gacha pulls!\n"
            "\n"
            "§8────────────────────\n"
            "§l§6GETTING STARTED§r\n"
            "§8────────────────────\n"
            "\n"
            "§6 1. §fExplore a Village → §aAdventure Guild\n"
            "§6 2. §fComplete Daily Quests → earn §3SheldDust\n"
            "§6 3. §fExchange §3SD §f→ §eZI §fat the Shrine Maiden\n"
            "§6 4. §fLeft-click §6Shrine Maiden §f— Currency Exchange\n"
            "§6 5. §fRight-click §6Gacha Altar §f— Pull Gacha\n"
            "\n"
            "§8────────────────────\n"
            "§l§dGUILD SYSTEM§r\n"
            "§8────────────────────\n"
            "\n"
            "§7Adventure is better with allies! Form a §dGuild "
            "§7with fellow players to unlock:\n"
            "§d ✦ §fFriendly Fire Protection §8— §7no accidental PvP\n"
            "§d ✦ §fGuild Chat Prefix §8— §7display your guild name\n"
            "§d ✦ §fShared Base Protection §8— §7anti-grief on guild land\n"
            "§d ✦ §fGuild Vote System §8— §7kick, mutiny, leadership\n"
            "\n"
            "§7Type §a/guild §7to create or join one. Search by name, "
            "request to join, or accept invitations from leaders.\n"
            "\n"
            "§8────────────────────\n"
            "§7§oMay fortune smile upon your journey!"
        );
        form.appendToggle("dont_show", "§7Don't show again", !profile.shouldShowWelcome());
        std::string xuid = player.getXuid();
        form.sendTo(player,
            [xuid](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
                if (!result) return;
                // Capture xuid by value + re-lookup di body callback — aman kalau
                // player disconnect sebelum click confirm. Pattern konsisten dengan
                // form handler lainnya (showGachaMenu, showBannerDetails, dll).
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                bool dontShow = std::get<uint64_t>(result->at("dont_show"));
                pr.setWelcomePopup(!dontShow);
                p.sendMessage("§6[§eAetheria§6] §7Your adventure begins! §6★");
            });
    }


    // =========================================================================
    //  2.  GACHA MENU
    // =========================================================================

    static void showGachaMenu(Player& player, PlayerProfile& profile) {
        int week = BannerManager::getInstance().getCurrentWeek(); // global, bukan per-player
        const BannerDisplayInfo* bannerInfo = LootPoolManager::getInstance().getBannerDisplayInfo(week);
        std::string bannerName  = bannerInfo ? bannerInfo->name  : "Unknown Banner";
        std::string bannerTheme = bannerInfo ? bannerInfo->theme : "";

        // Split item name + enchants ke 2 baris untuk mencegah wrap di tengah
        auto formatShortItem = [](const std::string& name, char starColor) -> std::string {
            std::string star = std::string("§") + starColor;
            auto pos = name.find(" [");
            if (pos == std::string::npos) {
                return "§f  " + star + "★ §f" + name + "\n";
            }
            std::string itemName = name.substr(0, pos);
            std::string enchPart = name.substr(pos + 2);
            auto closePos = enchPart.rfind(']');
            if (closePos != std::string::npos) enchPart = enchPart.substr(0, closePos);
            return "§f  " + star + "★ §f" + itemName + "\n"
                 + "§8       └ §7" + enchPart + "\n";
        };

        std::string featuredSection;
        if (bannerInfo && (!bannerInfo->featuredB5.empty() || !bannerInfo->featuredB4.empty())) {
            featuredSection += "§6  ✦ FEATURED THIS WEEK\n";
            for (const auto& item : bannerInfo->featuredB5)
                featuredSection += formatShortItem(item, '6');
            for (const auto& item : bannerInfo->featuredB4)
                featuredSection += formatShortItem(item, '5');
            featuredSection += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";
        }

        std::string body =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ ACTIVE BANNER §8— §eWeek " + std::to_string(week) + "\n"
            "§f  " + bannerName + "\n"
            "§7  " + bannerTheme + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            + featuredSection +
            "§6  ✦ YOUR WALLET\n"
            + form_util::buildWalletLine(profile) + "\n";

        ll::form::SimpleForm form;
        form.setTitle("§l✦ §6Ancient Shrine §8─ §0Week " + std::to_string(week) + " §r§l✦");
        form.setContent(body);
        form.appendButton("§l§2  1x Pull\n§8Cost: §01 Zen Ingot");
        form.appendButton("§l§6  10+1 Pull\n§8Cost: §010 Zen Ingot");
        form.appendButton("§l§1  ◎ Banner Details\n§8View all obtainable items");
        form.appendButton("§l§5  ✦ Pity Status\n§8Counter & probability info");
        form.appendButton("§4  ✕ Close");

        std::string xuid = player.getXuid();
        form.sendTo(player,
            [xuid, week](Player& p, int selected, ll::form::FormCancelReason) {
                if (selected < 0 || selected == 4) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                switch (selected) {
                    case 0: showPullConfirm(p, pr, /*cost=*/1,  /*pulls=*/1);  break;
                    case 1: showPullConfirm(p, pr, /*cost=*/10, /*pulls=*/11); break;
                    case 2: showBannerDetails(p, pr); break;
                    case 3: showPityStatus(p, pr);    break;
                    default: break;
                }
            });
    }


    // =========================================================================
    //  3.  BANNER DETAILS
    // =========================================================================

    static void showBannerDetails(Player& player, PlayerProfile& profile) {
        int week = BannerManager::getInstance().getCurrentWeek(); // global, bukan per-player
        const BannerDisplayInfo* info = LootPoolManager::getInstance().getBannerDisplayInfo(week);

        // Format item dengan split nama + enchants (mencegah auto-wrap di tengah)
        // "Diamond Chestplate [Protection IV + Unbreaking III]"
        // →
        // • §<color>Diamond Chestplate
        //   §8└ Protection IV + Unbreaking III
        auto formatItem = [](const std::string& name, char colorCode) -> std::string {
            std::string color = std::string("§") + colorCode;
            auto pos = name.find(" [");
            if (pos == std::string::npos) {
                return "§f  • " + color + name + "\n";
            }
            std::string itemName = name.substr(0, pos);
            // Cari closing bracket
            std::string enchPart = name.substr(pos + 2); // skip " ["
            auto closePos = enchPart.rfind(']');
            if (closePos != std::string::npos) enchPart = enchPart.substr(0, closePos);
            return "§f  • " + color + itemName + "\n"
                 + "§8     └ §7" + enchPart + "\n";
        };

        std::string body = "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";

        if (info) {
            body += "§6§l✦ FEATURED B5 §r§7— Legendary (Rate On)\n";
            for (const auto& item : info->featuredB5)
                body += formatItem(item, '6');
            body += "\n§5§l✦ FEATURED B4 §r§7— Epic (Rate On)\n";
            for (const auto& item : info->featuredB4)
                body += formatItem(item, '5');
        } else {
            body += "§cNo banner data available for this week.\n";
        }

        // Standard B5 pool
        const auto& b5Std = LootPoolManager::getInstance().getB5Standard();
        body += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";
        body += "§6§l✦ B5 STANDARD §r§7— Legendary (Rate Off)\n";
        for (const auto& item : b5Std)
            body += formatItem(item.name, '6');

        // Standard B4 pool
        const auto& b4Std = LootPoolManager::getInstance().getB4Standard();
        body += "\n§5§l✦ B4 STANDARD §r§7— Epic (Rate Off)\n";
        for (const auto& item : b4Std)
            body += formatItem(item.name, '5');

        // B3 pool
        const auto& b3Pool = LootPoolManager::getInstance().getB3Pool();
        body += "\n§b§l✦ B3 POOL §r§7— Rare\n";
        for (const auto& item : b3Pool)
            body += formatItem(item.name, 'b');

        body +=
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ 40/60 SYSTEM\n"
            "§f  Every B5/B4 pull has a §a40%§f Rate On chance.\n"
            "§f  If you lose 40/60, your next B5/B4 is §cGUARANTEED §fRate On!\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        ll::form::SimpleForm form;
        std::string bannerName = info ? info->name : "Unknown";
        form.setTitle("§l✦ §6Banner Details §8─ §0" + bannerName + " §r§l✦");
        form.setContent(body);
        form.appendButton("§l§0← Return to Shrine");

        std::string xuid = player.getXuid();
        form.sendTo(player,
            [xuid](Player& p, int selected, ll::form::FormCancelReason) {
                if (selected != 0) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                showGachaMenu(p, pr);
            });
    }


    // =========================================================================
    //  4.  PITY STATUS
    // =========================================================================

    static void showPityStatus(Player& player, PlayerProfile& profile) {
        std::string body =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§d  ✦ PITY STATUS\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            + form_util::buildPityLines(profile) + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ HOW IT WORKS\n"
            "§f  B5 soft-pity starts at pull §a41§f (exponential rate increase).\n"
            "§f  B5 is guaranteed by pull §c" + std::to_string(profile.getGuaranteeWindowB5()) + "§f (dynamic hard cap).\n"
            "§f  B4 is guaranteed by pull §c20§f (fixed hard cap).\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        ll::form::SimpleForm form;
        form.setTitle("§l✦ §5Pity Status §r§l✦");
        form.setContent(body);
        form.appendButton("§l§0← Return to Shrine");

        std::string xuid = player.getXuid();
        form.sendTo(player,
            [xuid](Player& p, int selected, ll::form::FormCancelReason) {
                if (selected != 0) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                showGachaMenu(p, pr);
            });
    }


    // =========================================================================
    //  5.  PULL CONFIRM
    // =========================================================================

    static void showPullConfirm(Player& player, PlayerProfile& profile, int cost, int pullCount) {
        int currentZI = profile.getZenIngot();

        if (currentZI < cost) {
            std::string xuid = player.getXuid();
            ll::form::SimpleForm notif;
            notif.setTitle("§4✗ Insufficient Balance");
            notif.setContent(std::format(
                "§fYou need §e{} Zen Ingot§f for {}x pull{}.\n\n"
                "§f  Current balance §8» §c{} ZI\n\n"
                "§7Visit the §aShrine Maiden§7 to exchange §3SheldDust §7for §eZen Ingots§7.",
                cost, pullCount, pullCount == 1 ? "" : "s", currentZI
            ));
            notif.appendButton("§l§0← Back to Shrine");
            notif.sendTo(player, [xuid](Player& p, int, ll::form::FormCancelReason) {
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                showGachaMenu(p, pr);
            });
            return;
        }

        ll::form::ModalForm confirm;
        confirm.setTitle("§6✦ Confirm Pull");
        confirm.setContent(std::format(
            "§fSpend §e{} Zen Ingot§f for §a{}x Pull§f?\n\n"
            "§f  Current balance  §8» §e{} ZI\n"
            "§f  After pull       §8» §e{} ZI\n\n"
            "§7Proceed?",
            cost, pullCount, currentZI, currentZI - cost
        ));
        confirm.setUpperButton("§l§2✔ Pull Now!");
        confirm.setLowerButton("§0✗ Cancel");

        std::string xuid = player.getXuid();
        confirm.sendTo(player,
            [xuid, cost, pullCount](Player& p, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
                if (result != ll::form::ModalFormSelectedButton::Upper) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);

                if (!pr.spendZenIngot(cost)) {
                    p.sendMessage("§c[Shrine] §fInsufficient Zen Ingot balance.");
                    return;
                }

                std::vector<GachaResult> results;
                results.reserve(pullCount);
                for (int i = 0; i < pullCount; ++i)
                    results.push_back(GachaEngine::performPull(pr));

                showPullResults(p, pr, results);
            });
    }


    // =========================================================================
    //  6.  PULL RESULTS
    // =========================================================================

    static void showPullResults(Player& player, PlayerProfile& profile,
                                const std::vector<GachaResult>& results) {
        int countB5 = 0, countB4 = 0, countB3 = 0;
        std::string resultLines;
        for (int i = 0; i < static_cast<int>(results.size()); ++i) {
            const auto& r = results[i];
            resultLines += form_util::formatPullLine(i, r) + "\n";
            if (r.tier == GachaTier::B5)      ++countB5;
            else if (r.tier == GachaTier::B4) ++countB4;
            else                              ++countB3;
        }

        std::string summary =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ SUMMARY\n"
            "§f  §6★ Legendary (B5) §8» §a" + std::to_string(countB5) + "x\n"
            "§f  §5★ Epic (B4)      §8» §b" + std::to_string(countB4) + "x\n"
            "§f  §9★ Rare (B3)      §8» §7" + std::to_string(countB3) + "x\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";

        std::string body =
            summary +
            "§6  ✦ PULL DETAILS\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            + resultLines +
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7§oItems will be delivered to your inventory.";

        ll::form::SimpleForm form;
        form.setTitle("§l✦ §6Pull Results §r§l✦");
        form.setContent(body);
        form.appendButton("§l§2  ✔ Claim & Close");
        form.appendButton("§l§6  ↺ Pull Again\n§8Return to Shrine");

        std::string xuid = player.getXuid();
        // Move-capture results — hindari copy vector<GachaResult> (~3KB) per
        // form sent. Saat server ramai pull, copy heap-alloc bisa accumulate.
        form.sendTo(player,
            [xuid, results = std::move(results)](Player& p, int selected, ll::form::FormCancelReason) {
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);

                if (selected == 0) {
                    // ── Claim & Close ──────────────────────────────────────
                    auto [claimed, dropped, currency] = form_util::claimGachaResults(p, pr, results);

                    // Feedback chat
                    std::string msg = "§a[Shrine] §f";
                    if (claimed > 0)
                        msg += std::format("§a✔ {}x §fitem added to inventory. ", claimed);
                    if (currency > 0)
                        msg += std::format("§e✔ {}x §fcurrency credited. ", currency);
                    if (dropped > 0)
                        msg += std::format("§e⚠ {}x §fitem dropped §7(inventory full).", dropped);

                    p.sendMessage(msg);
                    p.sendMessage(std::format("§7  ZI: §e{}  §8|  SD: §3{}", pr.getZenIngot(), pr.getSheldDust()));

                } else if (selected == 1) {
                    // ── Pull Again ─────────────────────────────────────────
                    // Claim dulu sebelum pull lagi
                    form_util::claimGachaResults(p, pr, results);
                    showGachaMenu(p, pr);
                }
            });
    }


    // =========================================================================
    //  7.  EXCHANGE MENU
    // =========================================================================

    static void showExchangeMenu(Player& player, PlayerProfile& profile) {
        // Wallet info
        std::string body =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ YOUR WALLET\n"
            "§f  Zen Ingot §8» §e" + std::to_string(profile.getZenIngot())  + " ZI\n"
            "§f  SheldDust §8» §3" + std::to_string(profile.getSheldDust()) + " SD\n"
            "§f  Diamond   §8» §7" + std::to_string(form_util::countDiamonds(player)) + " ◆\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7Select an exchange option below.\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        ll::form::SimpleForm form;
        form.setTitle("§l⚗ §6Shrine Maiden §8─ §0Currency Exchange §r§l⚗");
        form.setContent(body);

        const auto& options = ExchangeManager::getInstance().getOptions();

        for (const auto& opt : options) {
            int fromTotal = opt.fromAmount * opt.bulkMultiplier;
            int toTotal   = opt.toAmount   * opt.bulkMultiplier;
            std::string label, subtitle;

            if (opt.fromCurrency == "diamond") {
                label    = std::format("§l§1◆ {}  Diamond  §0→  §3{}  SheldDust", fromTotal, toTotal);
                subtitle = "§8  ◆ Taken from your inventory";
            } else if (opt.fromCurrency == "sd" && opt.toCurrency == "zi") {
                if (opt.bulkMultiplier > 1)
                    label = std::format("§l§3{}  SheldDust  §0→  §6{}  Zen Ingot  §8(×{})", fromTotal, toTotal, opt.bulkMultiplier);
                else
                    label = std::format("§l§3{}  SheldDust  §0→  §6{}  Zen Ingot", fromTotal, toTotal);
                subtitle = "§8  " + (opt.description.empty() ? "Standard exchange" : opt.description);
            } else {
                label    = std::format("§l§1{}  {}  §0→  §6{}  {}", fromTotal, opt.fromCurrency, toTotal, opt.toCurrency);
                subtitle = "§8  " + opt.description;
            }
            form.appendButton(label + "\n" + subtitle);
        }
        form.appendButton("§4  ✕ Close");

        std::string xuid = player.getXuid();
        int optCount = static_cast<int>(options.size());
        form.sendTo(player,
            [xuid, optCount](Player& p, int selected, ll::form::FormCancelReason) {
                if (selected < 0 || selected >= optCount) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                const auto& opts = ExchangeManager::getInstance().getOptions();
                showExchangeConfirm(p, pr, opts[selected]);
            });
    }


    // =========================================================================
    //  8.  EXCHANGE CONFIRM
    // =========================================================================

    static void showExchangeConfirm(Player& player, PlayerProfile& profile,
                                    const ExchangeOption& opt) {
        int fromTotal = opt.fromAmount * opt.bulkMultiplier;
        int toTotal   = opt.toAmount   * opt.bulkMultiplier;

        std::string fromLabel = (opt.fromCurrency == "sd")      ? "SheldDust"
                              : (opt.fromCurrency == "zi")      ? "Zen Ingot"
                                                                : "Diamond";
        std::string toLabel   = (opt.toCurrency   == "sd")      ? "SheldDust" : "Zen Ingot";

        // Pre-check affordability
        bool canAfford = true;
        int sdAfter = profile.getSheldDust();
        int ziAfter = profile.getZenIngot();
        int diaAvail = 0;

        if (opt.fromCurrency == "sd") {
            canAfford = sdAfter >= fromTotal;
            sdAfter  -= fromTotal;
            if (opt.toCurrency == "zi") ziAfter += toTotal;
        } else if (opt.fromCurrency == "zi") {
            canAfford = ziAfter >= fromTotal;
            ziAfter  -= fromTotal;
            if (opt.toCurrency == "sd") sdAfter += toTotal;
        } else if (opt.fromCurrency == "diamond") {
            diaAvail  = form_util::countDiamonds(player);
            canAfford = diaAvail >= fromTotal;
            sdAfter  += toTotal;
        }

        if (!canAfford) {
            std::string shortage;
            if (opt.fromCurrency == "diamond")
                shortage = std::format("§fYou need §b{}§f ◆ Diamond but only have §c{}§f.", fromTotal, diaAvail);
            else
                shortage = std::format("§fYou need §b{}§f {} but your balance is insufficient.", fromTotal, fromLabel);

            std::string xuid = player.getXuid();
            ll::form::SimpleForm notif;
            notif.setTitle("§4✗ Insufficient Balance");
            notif.setContent(
                shortage + "\n\n"
                "§f  Zen Ingot §8» §e" + std::to_string(profile.getZenIngot()) + "\n"
                "§f  SheldDust §8» §3" + std::to_string(profile.getSheldDust()) + "\n"
                "§f  Diamond   §8» §b" + std::to_string(diaAvail) + " ◆"
            );
            notif.appendButton("§l§0← Back to Exchange");
            notif.sendTo(player, [xuid](Player& p, int, ll::form::FormCancelReason) {
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                showExchangeMenu(p, pr);
            });
            return;
        }

        ll::form::ModalForm confirm;
        confirm.setTitle("§6⚗ Confirm Exchange");
        confirm.setContent(std::format(
            "§fYou will exchange:\n"
            "§f  §e{} {} §8→ §3{} {}\n\n"
            "§f  ZI after  §8» §e{}\n"
            "§f  SD after  §8» §3{}\n\n"
            "§7Proceed?",
            fromTotal, fromLabel,
            toTotal, toLabel,
            ziAfter, sdAfter
        ));
        confirm.setUpperButton("§l§2✔ Exchange Now!");
        confirm.setLowerButton("§0✗ Cancel");

        std::string xuid = player.getXuid();
        ExchangeOption optCopy = opt;
        confirm.sendTo(player,
            [xuid, optCopy, fromTotal, toTotal](Player& p, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
                if (result != ll::form::ModalFormSelectedButton::Upper) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);

                bool success = false;

                if (optCopy.fromCurrency == "sd" && optCopy.toCurrency == "zi") {
                    success = pr.spendSheldDust(fromTotal);
                    if (success) pr.addZenIngot(toTotal);

                } else if (optCopy.fromCurrency == "zi" && optCopy.toCurrency == "sd") {
                    success = pr.spendZenIngot(fromTotal);
                    if (success) pr.addSheldDust(toTotal);

                } else if (optCopy.fromCurrency == "diamond" && optCopy.toCurrency == "sd") {
                    // Take diamonds from inventory, then credit SD
                    if (form_util::takeDiamonds(p, fromTotal)) {
                        pr.convertDiamondToSD(fromTotal);
                        success = true;
                    }
                }

                if (success) {
                    std::string fromUnit = (optCopy.fromCurrency == "sd")      ? "SD"
                                        : (optCopy.fromCurrency == "zi")       ? "ZI"
                                                                               : "Diamond";
                    std::string toUnit   = (optCopy.toCurrency   == "sd")      ? "SD" : "ZI";
                    p.sendMessage(std::format(
                        "§a[Shrine Maiden] §fExchanged §e{} {} §f→ §b{} {}!",
                        fromTotal, fromUnit, toTotal, toUnit
                    ));
                    p.sendMessage(std::format("§7  ZI: §e{}  §8|  §7SD: §3{}", pr.getZenIngot(), pr.getSheldDust()));
                } else {
                    p.sendMessage("§c[Shrine Maiden] §fExchange failed — balance may have changed.");
                }
            });
    }


    // =========================================================================
    //  9.  ACCOUNT INFO WINDOW  (called by /account command)
    // =========================================================================

    static void showAccountInfo(Player& player, PlayerProfile& profile) {
        // ── Pity bars (16 chars wide) ─────────────────────────────────────
        constexpr int kBar = 16;
        auto makeBar = [&](int val, int max, const std::string& col) {
            int f = std::min(kBar, val * kBar / std::max(max, 1));
            return col + std::string(f, '|') + "§8" + std::string(kBar - f, '|');
        };

        // ── Rate labels ───────────────────────────────────────────────────
        std::string b5Rate = profile.isGuaranteedB5()
            ? "§c⚡ Guaranteed!"
            : std::format("§a{:.2f}%", profile.calcRateB5() * 100.0);

        std::string b4Rate = profile.isGuaranteedB4()
            ? "§c⚡ Guaranteed!"
            : std::format("§b{:.2f}%", profile.calcRateB4() * 100.0);

        // ── Wallet ────────────────────────────────────────────────────────
        int zi = profile.getZenIngot();
        int sd = profile.getSheldDust();

        // ── Banner label (global) ─────────────────────────────────────────
        int week = BannerManager::getInstance().getCurrentWeek();
        std::string bannerLabel = std::format("Week {} — {}", week, BannerManager::bannerName(week));

        // ── Guild label ───────────────────────────────────────────────────
        auto guildOpt = GuildManager::getInstance().getGuildOfPlayer(player.getXuid());
        std::string guildLabel = guildOpt
            ? std::format("§6{} §7({} members)", guildOpt->guildName, (int)guildOpt->members.size())
            : "§7Not in any guild — use §a/guild";

        // ── Inbox unread ─────────────────────────────────────────────────
        int unread = MailManager::getInstance().unreadCount(player.getXuid());
        std::string inboxBtn = unread > 0
            ? std::format("§l§1✉ §0Inbox §4({} unread)", unread)
            : "§l§0✉ §0Inbox";

        // ── Active Missions panel ────────────────────────────────────────
        auto& qm = QuestManager::getInstance();
        // Make sure daily/monthly are reset for the current day before reading
        try { qm.checkAndReset(profile); } catch (...) {}
        auto activeDaily   = qm.getActiveDailyQuests(profile);
        auto activeMonthly = qm.getActiveMonthlyQuest(profile);

        // Compact label (3 chars each) untuk hindari line wrap di MCBE form
        auto questLabel = [](QuestType k) -> std::string {
            switch (k) {
                case QuestType::Normal:  return "§a[D]"; // Daily
                case QuestType::Advance: return "§b[A]"; // Advance
                case QuestType::Special: return "§d[S]"; // Special
                case QuestType::Monthly: return "§6[M]"; // Monthly
            }
            return "§7[?]";
        };
        auto progressStr = [](const ActiveQuest& q) -> std::string {
            if (q.completed) return "§a✔";
            return std::format("§7[{}/{}]", q.progress, q.target);
        };
        // Trim trailing whitespace dari display (config sengaja padded untuk NPC menu)
        auto trim = [](std::string s) -> std::string {
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };

        std::string missionsSection = "§3  ✦ ACTIVE MISSIONS\n"
                                      "§8  [D]aily  [A]dvance  [S]pecial  [M]onthly\n";
        bool hasAny = false;

        // Daily / Advance / Special quests
        for (const auto& q : activeDaily) {
            missionsSection += std::format("§f  {} §f{} §8{}\n",
                questLabel(q.kind), trim(q.display), progressStr(q));
            hasAny = true;
        }
        // Monthly quest
        if (activeMonthly) {
            missionsSection += std::format("§f  {} §f{} §8{}\n",
                questLabel(activeMonthly->kind),
                trim(activeMonthly->display),
                progressStr(*activeMonthly));
            hasAny = true;
        }
        if (!hasAny) {
            missionsSection += "§7  (No active missions — visit the Adventure Guild NPC)\n";
        }

        std::string body =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§f  Name  §8»  §a" + player.getRealName() + "\n"
            "§f  XUID  §8»  §7" + player.getXuid() + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ WALLET\n"
            "§f  Zen Ingot  §8»  §e" + std::to_string(zi) + " ZI\n"
            "§f  SheldDust  §8»  §3" + std::to_string(sd) + " SD\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§d  ✦ GACHA PITY\n"
            "§f  B5  §8»  §a" + std::to_string(profile.getPityB5())
                + "§7/" + std::to_string(profile.getGuaranteeWindowB5())
                + "  " + b5Rate + "\n"
            "§8      [" + makeBar(profile.getPityB5(), profile.getGuaranteeWindowB5(), "§a") + "§8]\n"
            "§f  B4  §8»  §b" + std::to_string(profile.getPityB4())
                + "§7/20  " + b4Rate + "\n"
            "§8      [" + makeBar(profile.getPityB4(), 20, "§b") + "§8]\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§5  ✦ ACTIVE BANNER\n"
            "§f  " + bannerLabel + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§6  ✦ GUILD\n"
            "§f  " + guildLabel + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            + missionsSection +
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        ll::form::SimpleForm form;
        form.setTitle("§l§6✦ §0Account Info §6✦");
        form.setContent(body);
        form.appendButton(inboxBtn);
        form.appendButton("§l§0  ✕ Close");

        std::string xuid = player.getXuid();
        form.sendTo(player,
            [xuid](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel == 0) {
                    showInbox(p);
                }
            });
    }


    // =========================================================================
    //  10.  INBOX  (mail list)
    // =========================================================================

    static void showInbox(Player& player) {
        const std::string xuid = player.getXuid();
        auto mails = MailManager::getInstance().listAll(xuid);

        ll::form::SimpleForm form;
        form.setTitle("§l§6✉ §0Inbox §6✦");

        if (mails.empty()) {
            form.setContent("§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                            "§7  Your inbox is empty.\n"
                            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
            form.appendButton("§l§0  ✕ Close");
            form.sendTo(player, [](Player&, int, ll::form::FormCancelReason) {});
            return;
        }

        form.setContent(std::format(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7  {} message{} — select to open\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
            (int)mails.size(), mails.size() == 1 ? "" : "s"));

        std::vector<std::string> mailIds;
        for (const auto& m : mails) {
            std::string tag;
            switch (m.type) {
                case MailType::GuildInvite:      tag = "§2[Invite]";  break;
                case MailType::KickVote:         tag = "§4[Vote]";    break;
                case MailType::MutinyVote:       tag = "§4[Mutiny]";  break;
                case MailType::GuildNotice:      tag = "§6[Notice]";  break;
                case MailType::GuildJoinRequest: tag = "§2[Request]"; break;
                default:                         tag = "§8[System]";  break;
            }
            std::string unread = m.read ? "§8" : "§l§0";
            std::string label = std::format("{} {} {}\n§8  from §0{}",
                unread, tag, m.subject, m.fromName.empty() ? "System" : m.fromName);
            form.appendButton(label);
            mailIds.push_back(m.mailId);
        }
        form.appendButton("§l§0  ✕ Close");

        form.sendTo(player,
            [xuid, mailIds](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0 || sel >= (int)mailIds.size()) return;
                showMail(p, mailIds[sel]);
            });
    }


    // =========================================================================
    //  11.  READ MAIL  (with action buttons)
    // =========================================================================

    static void showMail(Player& player, const std::string& mailId) {
        const std::string xuid = player.getXuid();
        auto mailOpt = MailManager::getInstance().get(xuid, mailId);
        if (!mailOpt) { showInbox(player); return; }
        MailMessage m = *mailOpt;

        // Mark as read
        if (!m.read) MailManager::getInstance().markRead(xuid, mailId);

        // Body header
        auto fmtTime = [](long long t) -> std::string {
            std::time_t tt = static_cast<std::time_t>(t);
            std::tm tm{};
        #if defined(_WIN32)
            localtime_s(&tm, &tt);
        #else
            localtime_r(&tt, &tm);
        #endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            return std::string(buf);
        };

        std::string body =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§f  From: §7" + (m.fromName.empty() ? std::string("System") : m.fromName) + "\n"
            "§f  Sent: §7" + fmtTime(m.sentAt) + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            + m.body + "\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        // Add live stopwatch for vote types
        if (m.type == MailType::KickVote || m.type == MailType::MutinyVote) {
            try {
                auto j = nlohmann::json::parse(m.payload);
                long long expiresAt = j.value("expiresAt", 0LL);
                long long now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                long long remaining = expiresAt - now;
                if (remaining > 0) {
                    long long mins = remaining / 60;
                    long long secs = remaining % 60;
                    body += std::format("\n§e⏱ Expires in: §f{}m {}s", mins, secs);
                } else {
                    body += "\n§cThis vote has expired.";
                }
            } catch (...) {}
        }

        ll::form::SimpleForm form;
        form.setTitle("§l§6✉ §0" + m.subject);
        form.setContent(body);

        // Action buttons per type
        switch (m.type) {
            case MailType::GuildInvite:
                form.appendButton("§l§2  ✔ Accept");
                form.appendButton("§l§4  ✕ Decline");
                break;
            case MailType::KickVote:
            case MailType::MutinyVote:
                form.appendButton("§l§2  ✔ Approve");
                form.appendButton("§l§4  ✕ Reject");
                break;
            case MailType::GuildJoinRequest:
                form.appendButton("§l§2  ✔ Accept Request");
                form.appendButton("§l§4  ✕ Decline Request");
                break;
            default:
                form.appendButton("§l§0  🗑 Delete");
                break;
        }
        form.appendButton("§l§0  ← Back to Inbox");

        // Capture necessary fields by value (form is async)
        MailType mtype     = m.type;
        std::string payload = m.payload;
        std::string subject = m.subject;

        form.sendTo(player,
            [xuid, mailId, mtype, payload, subject](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;

                // ── Last button = back to inbox ─────────────────────────
                int actionCount =
                    (mtype == MailType::GuildInvite || mtype == MailType::KickVote ||
                     mtype == MailType::MutinyVote || mtype == MailType::GuildJoinRequest) ? 2 : 1;

                if (sel == actionCount) { showInbox(p); return; }

                // ── Handle actions ──────────────────────────────────────
                std::string guildId = GuildManager::payloadGuildId(payload);

                if (mtype == MailType::GuildInvite) {
                    if (sel == 0) handleAcceptInvite(p, xuid, mailId, guildId);
                    else          handleDeclineInvite(p, xuid, mailId);
                }
                else if (mtype == MailType::KickVote) {
                    if (sel == 0) handleKickApprove(p, xuid, mailId, guildId);
                    else          handleKickReject(p, xuid, mailId, guildId);
                }
                else if (mtype == MailType::MutinyVote) {
                    if (sel == 0) handleMutinyApprove(p, xuid, mailId, guildId);
                    else          handleMutinyReject(p, xuid, mailId, guildId);
                }
                else if (mtype == MailType::GuildJoinRequest) {
                    if (sel == 0) handleJoinRequestAccept(p, xuid, mailId, payload);
                    else          handleJoinRequestDecline(p, xuid, mailId, payload);
                }
                else {
                    MailManager::getInstance().remove(xuid, mailId);
                    p.sendMessage("§7[Inbox] §fMessage deleted.");
                    showInbox(p);
                }
            });
    }

    // ── Mail action handlers (private helpers, static so form lambdas can call) ──

    static void handleAcceptInvite(Player& p, const std::string& xuid,
                                   const std::string& mailId, const std::string& guildId) {
        auto& gm = GuildManager::getInstance();

        // Sudah punya guild?
        if (gm.getGuildIdOfPlayer(xuid)) {
            p.sendMessage("§c[Guild] §fYou are already in a guild. Leave first to join another.");
            MailManager::getInstance().remove(xuid, mailId);
            return;
        }

        auto g = gm.getGuild(guildId);
        if (!g) {
            p.sendMessage("§c[Guild] §fThis guild no longer exists.");
            MailManager::getInstance().remove(xuid, mailId);
            return;
        }
        if ((int)g->members.size() >= kGuildMaxMembers) {
            p.sendMessage("§c[Guild] §fThis guild is already full.");
            MailManager::getInstance().remove(xuid, mailId);
            return;
        }

        if (gm.addMember(guildId, xuid, p.getRealName())) {
            p.sendMessage(std::format("§a[Guild] §fYou joined §6{}§f!", g->guildName));
            // Notif ke leader
            MailManager::getInstance().sendSystem(
                g->leaderXuid,
                "§eNew Member",
                "§f" + p.getRealName() + " §ahas joined the guild!",
                MailType::GuildNotice, "");
        } else {
            p.sendMessage("§c[Guild] §fFailed to join guild.");
        }
        MailManager::getInstance().remove(xuid, mailId);
    }

    static void handleDeclineInvite(Player& p, const std::string& xuid,
                                    const std::string& mailId) {
        MailManager::getInstance().remove(xuid, mailId);
        p.sendMessage("§7[Guild] §fInvitation declined.");
    }

    static void handleKickApprove(Player& p, const std::string& xuid,
                                  const std::string& mailId, const std::string& guildId) {
        bool executed = GuildManager::getInstance().castKickApprove(guildId, xuid);
        MailManager::getInstance().remove(xuid, mailId);
        p.sendMessage(executed
            ? "§a[Guild] §fYour approval was the final vote — kick executed."
            : "§a[Guild] §fApproval recorded. Awaiting other members.");
    }

    static void handleKickReject(Player& p, const std::string& xuid,
                                 const std::string& mailId, const std::string& guildId) {
        GuildManager::getInstance().castKickReject(guildId, xuid);
        MailManager::getInstance().remove(xuid, mailId);
        p.sendMessage("§7[Guild] §fYou rejected the kick — vote cancelled.");
    }

    static void handleMutinyApprove(Player& p, const std::string& xuid,
                                    const std::string& mailId, const std::string& guildId) {
        bool executed = GuildManager::getInstance().castMutinyApprove(guildId, xuid);
        MailManager::getInstance().remove(xuid, mailId);
        p.sendMessage(executed
            ? "§a[Guild] §fYour approval was the final vote — leadership transferred."
            : "§a[Guild] §fApproval recorded. Awaiting other members.");
    }

    static void handleMutinyReject(Player& p, const std::string& xuid,
                                   const std::string& mailId, const std::string& guildId) {
        GuildManager::getInstance().castMutinyReject(guildId, xuid);
        MailManager::getInstance().remove(xuid, mailId);
        p.sendMessage("§7[Guild] §fYou rejected the mutiny — vote cancelled.");
    }

    static void handleJoinRequestAccept(Player& p, const std::string& leaderXuid,
                                        const std::string& mailId, const std::string& payload) {
        auto& gm = GuildManager::getInstance();
        // Single-parse payload (cache fields untuk akses berikutnya — hemat 2x JSON parse)
        auto fields = GuildManager::parsePayload(payload);
        std::string guildId       = fields["guildId"];
        std::string requesterXuid = fields["requesterXuid"];
        std::string requesterName = fields["requesterName"];

        auto g = gm.getGuild(guildId);
        if (!g) {
            p.sendMessage("§c[Guild] §fThis guild no longer exists.");
            MailManager::getInstance().remove(leaderXuid, mailId);
            return;
        }

        // Hanya leader yang bisa accept
        if (g->leaderXuid != leaderXuid) {
            p.sendMessage("§c[Guild] §fOnly the guild leader can accept join requests.");
            return;
        }

        if ((int)g->members.size() >= kGuildMaxMembers) {
            p.sendMessage("§c[Guild] §fYour guild is full.");
            MailManager::getInstance().remove(leaderXuid, mailId);
            return;
        }

        // Requester sudah punya guild?
        if (gm.getGuildIdOfPlayer(requesterXuid)) {
            p.sendMessage(std::format(
                "§c[Guild] §f{} §fhas already joined another guild.", requesterName));
            MailManager::getInstance().remove(leaderXuid, mailId);
            return;
        }

        if (gm.addMember(guildId, requesterXuid, requesterName)) {
            p.sendMessage(std::format(
                "§a[Guild] §fAccepted §6{}§f's request — they have joined the guild.", requesterName));
            // Notif ke requester
            nlohmann::json pl;
            pl["guildId"]   = guildId;
            pl["guildName"] = g->guildName;
            MailManager::getInstance().sendSystem(
                requesterXuid,
                "§aJoin Request Accepted",
                "§fYour request to join §6" + g->guildName + "§f has been accepted!",
                MailType::GuildNotice, pl.dump());
        } else {
            p.sendMessage("§c[Guild] §fFailed to add member.");
        }
        MailManager::getInstance().remove(leaderXuid, mailId);
    }

    static void handleJoinRequestDecline(Player& p, const std::string& leaderXuid,
                                         const std::string& mailId, const std::string& payload) {
        // Single-parse payload
        auto fields = GuildManager::parsePayload(payload);
        std::string requesterXuid = fields["requesterXuid"];
        std::string requesterName = fields["requesterName"];
        std::string guildName     = fields["guildName"];

        MailManager::getInstance().remove(leaderXuid, mailId);
        p.sendMessage(std::format("§7[Guild] §fDeclined §6{}§f's join request.", requesterName));

        // Notif ringan ke requester
        MailManager::getInstance().sendSystem(
            requesterXuid,
            "§eJoin Request Declined",
            "§fYour request to join §6" + guildName + "§f was declined by the leader.",
            MailType::GuildNotice, "");
    }


    // =========================================================================
    //  12.  MAKE GUILD  (flow: name input → invite menu)
    // =========================================================================

    static void showMakeGuild(Player& player) {
        const std::string xuid = player.getXuid();

        // Sudah punya guild?
        if (GuildManager::getInstance().getGuildIdOfPlayer(xuid)) {
            ll::form::SimpleForm err;
            err.setTitle("§4✗ Already in a Guild");
            err.setContent("§fYou are already a member of a guild.\n\n"
                           "§7Use §a/guild §7to manage your current guild,\n"
                           "§7or leave it first to create a new one.");
            err.appendButton("§l§0  ✕ Close");
            err.sendTo(player, [](Player&, int, ll::form::FormCancelReason) {});
            return;
        }

        ll::form::CustomForm form;
        form.setTitle("§l§6✦ §0Create New Guild §6✦");
        form.appendLabel(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7  You will become the §6leader §7of this guild.\n"
            "§7  Max members: §f4\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
        form.appendInput("name", "§fGuild name (3–20 chars)", "Enter name...");
        form.sendTo(player,
            [xuid](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
                if (!result) return;
                std::string name = std::get<std::string>(result->at("name"));
                // Trim
                while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
                while (!name.empty() && std::isspace((unsigned char)name.back()))  name.pop_back();

                if (name.size() < 3 || name.size() > 20) {
                    p.sendMessage("§c[Guild] §fName must be 3–20 characters.");
                    return;
                }

                std::string gid = GuildManager::getInstance()
                    .createGuild(xuid, p.getRealName(), name);
                if (gid.empty()) {
                    p.sendMessage("§c[Guild] §fFailed to create guild.");
                    return;
                }

                p.sendMessage(std::format("§a[Guild] §6{} §fhas been created! You are the leader.", name));
                showInvitePlayer(p);
            });
    }


    // =========================================================================
    //  13.  INVITE PLAYER  (search + list)
    // =========================================================================

    static void showInvitePlayer(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g) { player.sendMessage("§c[Guild] §fYou are not in a guild."); return; }
        if (g->leaderXuid != xuid) {
            player.sendMessage("§c[Guild] §fOnly the leader can invite players.");
            return;
        }
        if ((int)g->members.size() >= kGuildMaxMembers) {
            player.sendMessage("§c[Guild] §fYour guild is already full.");
            return;
        }

        ll::form::CustomForm form;
        form.setTitle("§l§6✦ §0Invite Player §6✦");
        form.appendLabel(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7  Search by XUID, or leave empty\n"
            "§7  to see all online players.\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
        form.appendInput("query", "§fSearch XUID (optional)", "");
        form.sendTo(player,
            [xuid](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
                if (!result) return;
                std::string query = std::get<std::string>(result->at("query"));
                showInviteResults(p, query);
            });
    }

    static void showInviteResults(Player& player, const std::string& query) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g) return;

        // Kumpulkan kandidat: online players, exclude self + member guild
        struct Cand { std::string xuid; std::string name; };
        std::vector<Cand> cands;
        auto level = ll::service::getLevel();
        if (level) {
            level->forEachPlayer([&](Player const& other) {
                std::string ox = other.getXuid();
                if (ox == xuid) return true;
                // skip kalau sudah anggota guild ini
                for (auto const& m : g->members) if (m.xuid == ox) return true;
                // filter query (substring of xuid, case-insensitive)
                if (!query.empty()) {
                    if (ox.find(query) == std::string::npos) return true;
                }
                cands.push_back({ox, std::string(other.getRealName())});
                return true;
            });
        }

        ll::form::SimpleForm form;
        form.setTitle("§l§6✦ §0Invite Player §6✦");

        if (cands.empty()) {
            form.setContent("§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                            "§7  No matching players found.\n"
                            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
            form.appendButton("§l§1  ↻ New Search");
            form.appendButton("§l§0  ← Back");
            form.sendTo(player,
                [xuid](Player& p, int sel, ll::form::FormCancelReason) {
                    if (sel == 0) showInvitePlayer(p);
                });
            return;
        }

        form.setContent(std::format(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7  Found §f{}§7 player{} — tap to invite\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
            (int)cands.size(), cands.size() == 1 ? "" : "s"));

        std::vector<std::string> targetXuids;
        std::vector<std::string> targetNames;
        for (const auto& c : cands) {
            form.appendButton(std::format("§l§2✉ §0{}\n§8  XUID: §8{}", c.name, c.xuid));
            targetXuids.push_back(c.xuid);
            targetNames.push_back(c.name);
        }
        form.appendButton("§l§1  ↻ New Search");
        form.appendButton("§l§0  ← Back");

        std::string guildId   = g->guildId;
        std::string guildName = g->guildName;
        form.sendTo(player,
            [xuid, guildId, guildName, targetXuids, targetNames]
            (Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;
                int n = (int)targetXuids.size();
                if (sel == n)     { showInvitePlayer(p); return; }   // new search
                if (sel == n + 1) { return; }                         // back
                if (sel >= n) return;

                // Send invite mail
                nlohmann::json payload;
                payload["guildId"]   = guildId;
                payload["guildName"] = guildName;
                MailManager::getInstance().send({
                    /*mailId*/   "",
                    /*toXuid*/   targetXuids[sel],
                    /*fromXuid*/ xuid,
                    /*fromName*/ p.getRealName(),
                    /*subject*/  "§aGuild Invitation: " + guildName,
                    /*body*/     "§f" + p.getRealName() + " §finvites you to join guild §6" + guildName + "§f!\n"
                                 "§7Tap Accept to join, or Decline to ignore.",
                    /*type*/     MailType::GuildInvite,
                    /*payload*/  payload.dump(),
                    /*read*/     false,
                    /*sentAt*/   0,
                });
                p.sendMessage(std::format("§a[Guild] §fInvitation sent to §e{}§f.", targetNames[sel]));
            });
    }


    // =========================================================================
    //  14.  MY GUILD  (leader OR member view)
    // =========================================================================

    static void showMyGuild(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g) {
            // Bukan member guild manapun — tampilkan pilihan: create atau request join.
            ll::form::SimpleForm noGuild;
            noGuild.setTitle("§l§6✦ §0My Guild §6✦");
            noGuild.setContent(
                "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                "§7  You are not a member of any guild.\n"
                "§7  What would you like to do?\n"
                "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
            noGuild.appendButton("§l§2  ✦ Create New Guild\n§8You become the leader");
            noGuild.appendButton("§l§1  ✉ Request to Join\n§8Search existing guilds");
            noGuild.appendButton("§l§0  ✕ Close");
            noGuild.sendTo(player,
                [](Player& p, int sel, ll::form::FormCancelReason) {
                    if (sel == 0) showMakeGuild(p);
                    else if (sel == 1) showRequestJoinGuild(p);
                });
            return;
        }

        bool isLeader = (g->leaderXuid == xuid);

        // Helper: format last seen relatif waktu
        long long _now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto fmtLastSeen = [_now](long long ts) -> std::string {
            if (ts == 0) return "§8(never seen)";
            long long d = _now - ts;
            if (d < 60)    return std::format("§8({}s ago)", d);
            if (d < 3600)  return std::format("§8({}m ago)", d / 60);
            if (d < 86400) return std::format("§8({}h ago)", d / 3600);
            return std::format("§8({}d ago)", d / 86400);
        };

        // Member list dengan online status indicator + last seen kalau offline.
        // Reserve capacity upfront — hindari realloc geometris (~5-6x) saat
        // append member list + log entries di body string builder.
        auto& cache = PlayerCache::getInstance();
        std::string members;
        members.reserve(512);
        members = "§6  ✦ MEMBERS §7(" + std::to_string(g->members.size())
                + "/" + std::to_string(kGuildMaxMembers) + ")\n";
        for (const auto& m : g->members) {
            std::string crownTag  = (m.xuid == g->leaderXuid) ? "§6★" : "§7•";
            bool online = cache.isOnline(m.xuid);
            std::string statusDot = online ? "§a●" : "§7○";
            std::string lastSeenSuffix = online ? "" : (" " + fmtLastSeen(m.lastSeen));
            members += "§f  " + crownTag + " " + statusDot + " §f" + m.name + lastSeenSuffix + "\n";
        }

        // Active vote display (with stopwatch)
        std::string voteInfo;
        long long now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        auto fmtRemaining = [now](long long expiresAt) -> std::string {
            long long r = expiresAt - now;
            if (r <= 0) return "§cExpired";
            long long mm = r / 60;
            long long ss = r % 60;
            return std::format("§e⏱ {}m {}s", mm, ss);
        };

        if (auto kv = GuildManager::getInstance().getKickVote(g->guildId); kv && kv->active) {
            int yes = 0, total = 0;
            for (auto const& [_, v] : kv->votes) { if (v) ++yes; ++total; }
            voteInfo += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                        "§c  ✦ KICK VOTE ACTIVE\n"
                        "§f  Target  §8» §c" + kv->targetName + "\n"
                        "§f  Votes   §8» §a" + std::to_string(yes) + "§7/§f" + std::to_string(total) + "\n"
                        "§f  Time    §8» " + fmtRemaining(kv->expiresAt) + "\n";
        }
        if (auto mv = GuildManager::getInstance().getMutinyVote(g->guildId); mv && mv->active) {
            int yes = 0, total = 0;
            for (auto const& [_, v] : mv->votes) { if (v) ++yes; ++total; }
            voteInfo += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                        "§4  ✦ MUTINY VOTE ACTIVE\n"
                        "§f  Initiator §8» §e" + mv->initiatorName + "\n"
                        "§f  Votes     §8» §a" + std::to_string(yes) + "§7/§f" + std::to_string(total) + "\n"
                        "§f  Time      §8» " + fmtRemaining(mv->expiresAt) + "\n";
        }

        // Cari nama leader aktual
        std::string leaderName = "Unknown";
        for (const auto& m : g->members) {
            if (m.xuid == g->leaderXuid) { leaderName = m.name; break; }
        }

        // Activity log (last 8 entries — relative time format)
        std::string logSection;
        if (!g->log.empty()) {
            auto fmtRel = [now](long long t) -> std::string {
                long long d = now - t;
                if (d < 60)     return std::to_string(d) + "s";
                if (d < 3600)   return std::to_string(d / 60) + "m";
                if (d < 86400)  return std::to_string(d / 3600) + "h";
                return std::to_string(d / 86400) + "d";
            };
            logSection = "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                         "§3  ✦ ACTIVITY LOG\n";
            int shown = 0;
            for (const auto& e : g->log) {
                if (shown >= 8) break;
                logSection += "§8  [" + fmtRel(e.timestamp) + "] §r" + e.text + "\n";
                ++shown;
            }
        }

        std::string ffStatus = g->friendlyFireProtection
            ? "§a✔ ON"
            : "§c✗ OFF";

        std::string nameWithEmblem = g->emblem.empty()
            ? ("§6" + g->guildName)
            : ("§6" + g->emblem + " " + g->guildName);

        std::string body;
        body.reserve(2048); // hindari realloc geometris
        body  = "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";
        body += "§f  Name         §8»  " + nameWithEmblem + "\n";
        body += "§f  Leader       §8»  §e" + leaderName + (isLeader ? " §7(You)" : "") + "\n";
        body += "§f  FF Protection §8»  " + ffStatus + "\n";
        body += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";
        body += members;
        body += voteInfo;
        body += logSection;
        body += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        ll::form::SimpleForm form;
        form.setTitle("§l§6✦ §0My Guild §6✦");
        form.setContent(body);

        if (isLeader) {
            form.appendButton("§l§2  ✉ Invite Player");
            form.appendButton(std::format("§l§4  ⚔ Kick Member\n§8Cost: §0{} ZI + member approval", kKickCostZI));
            form.appendButton("§l§6  ↻ Transfer Leadership");
            // Toggle FF Protection — label shows what will happen on click
            std::string ffBtn = g->friendlyFireProtection
                ? "§l§4  🛡 Disable Friendly Fire Protection\n§8Members will be able to harm each other"
                : "§l§2  🛡 Enable Friendly Fire Protection\n§8Members will be protected from each other";
            form.appendButton(ffBtn);
            // Rename Guild — show cooldown / cost info as subtitle
            long long renameCD = GuildManager::getInstance().renameCooldownRemaining(g->guildId);
            std::string renameBtn;
            if (renameCD > 0) {
                long long d = renameCD / 86400;
                long long h = (renameCD % 86400) / 3600;
                renameBtn = std::format("§l§8  ✏ Rename Guild\n§8Cooldown: §c{}d {}h remaining", d, h);
            } else {
                renameBtn = std::format("§l§1  ✏ Rename Guild\n§8Cost: §0{} ZI §8| Cooldown: §014 days", kGuildRenameCostZI);
            }
            form.appendButton(renameBtn);
            form.appendButton("§l§5  ✦ Set Guild Emblem\n§8Customize the symbol on chat & nametag");
            form.appendButton("§l§4  💥 Disband Guild");
            form.appendButton("§l§0  ✕ Close");
        } else {
            form.appendButton(std::format("§l§4  ⚡ Initiate Mutiny\n§8Cost: §0{} ZI + member approval", kMutinyCostZI));
            form.appendButton("§l§4  🚪 Leave Guild");
            form.appendButton("§l§0  ✕ Close");
        }

        bool leaderCap = isLeader;
        form.sendTo(player,
            [leaderCap](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;
                if (leaderCap) {
                    switch (sel) {
                        case 0: showInvitePlayer(p);       break;
                        case 1: showKickSelect(p);         break;
                        case 2: showTransferSelect(p);     break;
                        case 3: showFriendlyFireToggle(p); break;
                        case 4: showRenameGuild(p);        break;
                        case 5: showSetEmblem(p);          break;
                        case 6: showDisbandConfirm(p);     break;
                        default: break;
                    }
                } else {
                    switch (sel) {
                        case 0: showMutinyConfirm(p); break;
                        case 1: showLeaveConfirm(p);  break;
                        default: break;
                    }
                }
            });
    }


    // =========================================================================
    //  15.  KICK SELECT  (leader picks who to kick)
    // =========================================================================

    static void showKickSelect(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid != xuid) return;

        // Cek existing vote
        if (auto kv = GuildManager::getInstance().getKickVote(g->guildId); kv && kv->active) {
            ll::form::SimpleForm err;
            err.setTitle("§4✗ Vote Already Active");
            err.setContent("§fThere is already an active kick vote in this guild.\n"
                           "§7Wait for it to finish or be rejected first.");
            err.appendButton("§l§0  ← Back");
            err.sendTo(player, [](Player& p, int, ll::form::FormCancelReason) {
                showMyGuild(p);
            });
            return;
        }

        ll::form::SimpleForm form;
        form.setTitle("§l§4✦ §0Kick Member §4✦");
        form.setContent(std::format(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§f  Cost: §c{} ZI §7(refunded if rejected/expired)\n"
            "§f  Timeout: §e1 hour\n"
            "§f  Requires: §aunanimous approval§f from other members\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
            kKickCostZI));

        std::vector<std::string> targetXuids;
        std::vector<std::string> targetNames;
        for (const auto& m : g->members) {
            if (m.xuid == xuid) continue;
            form.appendButton("§l§4⚔ §0" + m.name);
            targetXuids.push_back(m.xuid);
            targetNames.push_back(m.name);
        }
        form.appendButton("§l§0  ← Back");

        std::string guildId = g->guildId;
        form.sendTo(player,
            [xuid, guildId, targetXuids, targetNames]
            (Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0 || sel >= (int)targetXuids.size()) {
                    if (sel == (int)targetXuids.size()) showMyGuild(p);
                    return;
                }
                // Confirm
                ll::form::ModalForm confirm;
                confirm.setTitle("§4✦ Confirm Kick");
                confirm.setContent(std::format(
                    "§fStart vote to kick §c{}§f?\n\n"
                    "§7• §c{} ZI §7will be deducted now\n"
                    "§7• All other members must approve within §e1 hour\n"
                    "§7• ZI refunded if vote fails",
                    targetNames[sel], kKickCostZI));
                confirm.setUpperButton("§l§2✔ Start Vote");
                confirm.setLowerButton("§0✗ Cancel");

                std::string target = targetXuids[sel];
                std::string tname  = targetNames[sel];
                confirm.sendTo(p,
                    [xuid, guildId, target, tname]
                    (Player& p2, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                        if (r != ll::form::ModalFormSelectedButton::Upper) return;
                        std::string vid = GuildManager::getInstance()
                            .startKickVote(guildId, xuid, target);
                        if (vid.empty()) {
                            p2.sendMessage("§c[Guild] §fFailed to start vote — check ZI balance.");
                        } else {
                            p2.sendMessage(std::format(
                                "§a[Guild] §fKick vote against §c{}§f started. §710 ZI deducted.", tname));
                        }
                    });
            });
    }


    // =========================================================================
    //  16.  TRANSFER LEADERSHIP
    // =========================================================================

    static void showTransferSelect(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid != xuid) return;

        ll::form::SimpleForm form;
        form.setTitle("§l§6✦ §0Transfer Leadership §6✦");
        form.setContent("§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                        "§7  Select the member to become §6new leader§7.\n"
                        "§7  You will become a regular member.\n"
                        "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");

        std::vector<std::string> targetXuids;
        std::vector<std::string> targetNames;
        for (const auto& m : g->members) {
            if (m.xuid == xuid) continue;
            form.appendButton("§l§6★ §0" + m.name);
            targetXuids.push_back(m.xuid);
            targetNames.push_back(m.name);
        }
        form.appendButton("§l§0  ← Back");

        std::string guildId   = g->guildId;
        std::string guildName = g->guildName;
        form.sendTo(player,
            [xuid, guildId, guildName, targetXuids, targetNames]
            (Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;
                if (sel >= (int)targetXuids.size()) { showMyGuild(p); return; }

                ll::form::ModalForm confirm;
                confirm.setTitle("§6✦ Confirm Transfer");
                confirm.setContent(std::format(
                    "§fTransfer leadership to §6{}§f?\n\n"
                    "§7You will become a regular member.\n"
                    "§7This action cannot be undone.",
                    targetNames[sel]));
                confirm.setUpperButton("§l§2✔ Transfer");
                confirm.setLowerButton("§0✗ Cancel");

                std::string target = targetXuids[sel];
                std::string tname  = targetNames[sel];
                confirm.sendTo(p,
                    [xuid, guildId, guildName, target, tname]
                    (Player& p2, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                        if (r != ll::form::ModalFormSelectedButton::Upper) return;
                        if (GuildManager::getInstance().transferLeader(guildId, target)) {
                            p2.sendMessage(std::format(
                                "§a[Guild] §fLeadership transferred to §6{}§f.", tname));
                            // Notif ke leader baru
                            nlohmann::json pl; pl["guildName"] = guildName;
                            MailManager::getInstance().sendSystem(
                                target,
                                "§6You are now Guild Leader",
                                "§fThe previous leader transferred leadership of §6" + guildName + "§f to you.",
                                MailType::GuildNotice, pl.dump());
                        } else {
                            p2.sendMessage("§c[Guild] §fTransfer failed.");
                        }
                    });
            });
    }


    // =========================================================================
    //  16.5  TOGGLE FRIENDLY FIRE PROTECTION  (leader only)
    // =========================================================================

    static void showFriendlyFireToggle(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid != xuid) {
            player.sendMessage("§c[Guild] §fOnly the guild leader can change this setting.");
            return;
        }

        const bool currentlyEnabled = g->friendlyFireProtection;
        const bool willEnable       = !currentlyEnabled;

        ll::form::ModalForm confirm;
        confirm.setTitle(willEnable
            ? "§2✦ Enable Friendly Fire Protection"
            : "§4✦ Disable Friendly Fire Protection");

        if (willEnable) {
            confirm.setContent(
                "§fRe-enable Friendly Fire Protection?\n\n"
                "§7Members will be §aprotected§7 from each other again.\n"
                "§7All physical, projectile, and splash damage between\n"
                "§7guild members will be blocked.");
            confirm.setUpperButton("§l§2✔ Enable Protection");
        } else {
            confirm.setContent(
                "§fDisable Friendly Fire Protection?\n\n"
                "§c⚠ Members will be able to harm each other.\n"
                "§7Useful for in-house duels, sparring, or PvP practice.\n"
                "§7You can re-enable this at any time.");
            confirm.setUpperButton("§l§4🛡 Disable Protection");
        }
        confirm.setLowerButton("§0✗ Cancel");

        std::string guildId = g->guildId;
        confirm.sendTo(player,
            [xuid, guildId, willEnable]
            (Player& p, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                if (r != ll::form::ModalFormSelectedButton::Upper) return;
                if (!GuildManager::getInstance().setFriendlyFireProtection(guildId, xuid, willEnable)) {
                    p.sendMessage("§c[Guild] §fFailed to update setting.");
                    return;
                }
                p.sendMessage(willEnable
                    ? "§a[Guild] §fFriendly Fire Protection §aENABLED§f. Members are now protected from each other."
                    : "§a[Guild] §fFriendly Fire Protection §cDISABLED§f. Members can now harm each other.");

                // ── Notif ke semua member non-leader ──────────────────────
                // Online → direct chat broadcast (immediate awareness).
                // Offline → mail (akan ke-display saat next login via inbox).
                auto guild = GuildManager::getInstance().getGuild(guildId);
                if (!guild) return;

                const std::string chatMsg = willEnable
                    ? "§6[§eGuild§6] §8» §aFriendly Fire ENABLED! §7Members are protected again."
                    : "§6[§eGuild§6] §8» §cFriendly Fire DISABLED! §7Members can now hurt each other — be careful!";

                for (const auto& m : guild->members) {
                    if (m.xuid == xuid) continue;
                    bool delivered = false;
                    // O(1) Player* lookup — dulu O(N) forEachPlayer per anggota guild.
                    // Untuk guild 50 member di server 100 player, jadi 50×100=5000 → 50.
                    if (auto* op = PlayerCache::getInstance().getOnlinePlayer(m.xuid)) {
                        op->sendMessage(chatMsg);
                        delivered = true;
                    }
                    // Fallback ke mail kalau offline (atau player online tidak ke-fetch)
                    if (!delivered) {
                        MailManager::getInstance().sendSystem(
                            m.xuid,
                            willEnable ? "§aFF Protection Re-enabled" : "§cFF Protection Disabled",
                            willEnable
                                ? "§fThe guild leader has §are-enabled§f Friendly Fire Protection. You are now protected from harm by other guild members."
                                : "§fThe guild leader has §cdisabled§f Friendly Fire Protection. Other guild members can now hurt you — be careful!",
                            MailType::GuildNotice, "");
                    }
                }
            });
    }


    // =========================================================================
    //  16.6  RENAME GUILD  (leader only, 20 ZI + 14-day cooldown)
    // =========================================================================

    static void showRenameGuild(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid != xuid) {
            player.sendMessage("§c[Guild] §fOnly the leader can rename the guild.");
            return;
        }

        long long cd = GuildManager::getInstance().renameCooldownRemaining(g->guildId);
        if (cd > 0) {
            long long d = cd / 86400;
            long long h = (cd % 86400) / 3600;
            ll::form::SimpleForm err;
            err.setTitle("§4✗ Rename Cooldown");
            err.setContent(std::format(
                "§fThe guild was renamed recently.\n\n"
                "§7Cooldown remaining: §c{}d {}h\n"
                "§7Each rename costs §e{} ZI §7and has a §e14-day§7 cooldown.",
                d, h, kGuildRenameCostZI));
            err.appendButton("§l§0  ← Back");
            err.sendTo(player, [xuid](Player& p, int, ll::form::FormCancelReason) {
                showMyGuild(p);
            });
            return;
        }

        ll::form::CustomForm form;
        form.setTitle("§l§1✦ §0Rename Guild §1✦");
        form.appendLabel(std::format(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§f  Current name §8» §6{}\n"
            "§f  Cost         §8» §e{} ZI\n"
            "§f  Cooldown     §8» §714 days after rename\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
            g->guildName, kGuildRenameCostZI));
        form.appendInput("new_name", "§fNew guild name (3-20 chars)", "Enter new name...");

        std::string guildId = g->guildId;
        form.sendTo(player,
            [xuid, guildId](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
                if (!result) return;
                std::string newName = std::get<std::string>(result->at("new_name"));
                while (!newName.empty() && std::isspace((unsigned char)newName.front())) newName.erase(newName.begin());
                while (!newName.empty() && std::isspace((unsigned char)newName.back()))  newName.pop_back();
                if (newName.size() < 3 || newName.size() > 20) {
                    p.sendMessage("§c[Guild] §fName must be 3–20 characters.");
                    return;
                }

                long long remaining = 0;
                int rc = GuildManager::getInstance().renameWithCost(guildId, xuid, newName, remaining);
                switch (rc) {
                    case 0:
                        p.sendMessage(std::format(
                            "§a[Guild] §fRenamed to §6{}§f. §7{} ZI deducted. Next rename allowed in §c14 days§7.",
                            newName, kGuildRenameCostZI));
                        break;
                    case 1: p.sendMessage("§c[Guild] §fOnly the leader can rename."); break;
                    case 2: {
                        long long d = remaining / 86400;
                        long long h = (remaining % 86400) / 3600;
                        p.sendMessage(std::format("§c[Guild] §fCooldown active: §c{}d {}h §fremaining.", d, h));
                        break;
                    }
                    case 3: p.sendMessage(std::format("§c[Guild] §fInsufficient ZI. Need §e{} ZI§f.", kGuildRenameCostZI)); break;
                    case 4: p.sendMessage("§c[Guild] §fInvalid name."); break;
                    default: p.sendMessage("§c[Guild] §fRename failed.");
                }
            });
    }


    // =========================================================================
    //  16.7  SET GUILD EMBLEM  (leader only, free)
    // =========================================================================

    static void showSetEmblem(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid != xuid) {
            player.sendMessage("§c[Guild] §fOnly the leader can set the guild emblem.");
            return;
        }

        const auto& presets = guildEmblemPresets();
        std::string current = g->emblem.empty() ? "§7(none)" : ("§6" + g->emblem);

        ll::form::SimpleForm form;
        form.setTitle("§l§5✦ §0Guild Emblem §5✦");
        form.setContent(std::format(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§f  Current emblem §8» {}\n"
            "§7  Emblem appears in chat prefix and nametag.\n"
            "§7  Example: §6[{} {}]\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
            current,
            g->emblem.empty() ? "★" : g->emblem,
            g->guildName));

        // Per-emblem button
        for (const auto& em : presets) {
            form.appendButton(std::format("§l§6{}  §0Pick this emblem", em));
        }
        form.appendButton("§l§8  🗑 Clear emblem (no symbol)");
        form.appendButton("§l§0  ← Back");

        std::string guildId = g->guildId;
        std::vector<std::string> presetCopy = presets;
        form.sendTo(player,
            [xuid, guildId, presetCopy](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;
                int n = (int)presetCopy.size();
                if (sel < n) {
                    if (GuildManager::getInstance().setEmblem(guildId, xuid, presetCopy[sel])) {
                        p.sendMessage(std::format("§a[Guild] §fEmblem set to §6{}§f.", presetCopy[sel]));
                        // Refresh nametag for this player (others will refresh on their next login)
                        GuildManager::refreshNametag(xuid);
                    }
                } else if (sel == n) {
                    // Clear
                    if (GuildManager::getInstance().setEmblem(guildId, xuid, "")) {
                        p.sendMessage("§a[Guild] §fEmblem cleared.");
                        GuildManager::refreshNametag(xuid);
                    }
                }
                // sel == n+1 → Back, do nothing
            });
    }


    // =========================================================================
    //  17.  DISBAND GUILD
    // =========================================================================

    static void showDisbandConfirm(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid != xuid) return;

        ll::form::ModalForm confirm;
        confirm.setTitle("§4✦ Disband Guild");
        confirm.setContent(std::format(
            "§fAre you sure you want to disband §6{}§f?\n\n"
            "§c⚠ This action cannot be undone.\n"
            "§7All members will be removed and notified.\n"
            "§7Any active votes will be cancelled (ZI refunded).",
            g->guildName));
        confirm.setUpperButton("§l§4💥 Disband");
        confirm.setLowerButton("§0✗ Cancel");

        std::string guildId   = g->guildId;
        std::string guildName = g->guildName;
        std::vector<std::string> memberXuids;
        for (const auto& m : g->members) memberXuids.push_back(m.xuid);

        confirm.sendTo(player,
            [xuid, guildId, guildName, memberXuids]
            (Player& p, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                if (r != ll::form::ModalFormSelectedButton::Upper) return;

                // Refund any active vote ZI before disband
                if (auto kv = GuildManager::getInstance().getKickVote(guildId); kv && kv->active) {
                    GuildManager::getInstance().castKickReject(guildId, kv->initiatorXuid);
                }
                if (auto mv = GuildManager::getInstance().getMutinyVote(guildId); mv && mv->active) {
                    GuildManager::getInstance().castMutinyReject(guildId, mv->initiatorXuid);
                }

                // Notify members before disband
                for (const auto& mx : memberXuids) {
                    if (mx == xuid) continue;
                    MailManager::getInstance().sendSystem(
                        mx,
                        "§4Guild Disbanded",
                        "§fGuild §6" + guildName + "§f has been disbanded by the leader.",
                        MailType::GuildNotice, "");
                }

                GuildManager::getInstance().disband(guildId);
                p.sendMessage(std::format("§a[Guild] §6{} §fhas been disbanded.", guildName));
            });
    }


    // =========================================================================
    //  18.  LEAVE GUILD  (member)
    // =========================================================================

    static void showLeaveConfirm(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g) return;

        ll::form::ModalForm confirm;
        confirm.setTitle("§4✦ Leave Guild");
        confirm.setContent(std::format(
            "§fAre you sure you want to leave §6{}§f?\n\n"
            "§7You can be re-invited by the leader later.",
            g->guildName));
        confirm.setUpperButton("§l§4🚪 Leave");
        confirm.setLowerButton("§0✗ Cancel");

        std::string guildId   = g->guildId;
        std::string guildName = g->guildName;
        std::string leaderX   = g->leaderXuid;
        std::string playerName = player.getRealName();
        confirm.sendTo(player,
            [xuid, guildId, guildName, leaderX, playerName]
            (Player& p, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                if (r != ll::form::ModalFormSelectedButton::Upper) return;
                if (GuildManager::getInstance().removeMember(guildId, xuid)) {
                    p.sendMessage(std::format("§a[Guild] §fYou left §6{}§f.", guildName));
                    // Notif ke leader
                    MailManager::getInstance().sendSystem(
                        leaderX,
                        "§eMember Left",
                        "§f" + playerName + " §7has left the guild.",
                        MailType::GuildNotice, "");
                }
            });
    }


    // =========================================================================
    //  19.5  REQUEST JOIN GUILD  (non-member searches by guild name)
    // =========================================================================

    static void showRequestJoinGuild(Player& player) {
        const std::string xuid = player.getXuid();

        // Sudah punya guild?
        if (GuildManager::getInstance().getGuildIdOfPlayer(xuid)) {
            player.sendMessage("§c[Guild] §fYou are already in a guild.");
            return;
        }

        ll::form::CustomForm form;
        form.setTitle("§l§6✦ §0Request to Join §6✦");
        form.appendLabel(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7  Search guild by name (substring).\n"
            "§7  Leave empty to see all guilds.\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
        form.appendInput("query", "§fGuild name (optional)", "");
        form.sendTo(player,
            [xuid](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
                if (!result) return;
                std::string query = std::get<std::string>(result->at("query"));
                showRequestJoinResults(p, query);
            });
    }

    static void showRequestJoinResults(Player& player, const std::string& query) {
        const std::string xuid = player.getXuid();
        auto results = GuildManager::getInstance().searchByName(query, 16);

        ll::form::SimpleForm form;
        form.setTitle("§l§6✦ §0Request to Join §6✦");

        if (results.empty()) {
            form.setContent("§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                            "§7  No matching guilds found.\n"
                            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬");
            form.appendButton("§l§1  ↻ New Search");
            form.appendButton("§l§0  ← Back");
            form.sendTo(player, [](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel == 0) showRequestJoinGuild(p);
                else if (sel == 1) showMyGuild(p);
            });
            return;
        }

        form.setContent(std::format(
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7  Found §f{}§7 guild{} — tap to request join\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
            (int)results.size(), results.size() == 1 ? "" : "s"));

        std::vector<std::string> guildIds;
        std::vector<std::string> guildNames;
        for (const auto& g : results) {
            int count = (int)g.members.size();
            bool full = count >= kGuildMaxMembers;
            std::string label = std::format(
                "§l§2✦ §0{}\n§8  {}/{} members{}",
                g.guildName, count, kGuildMaxMembers,
                full ? " §4(full)" : "");
            form.appendButton(label);
            guildIds.push_back(g.guildId);
            guildNames.push_back(g.guildName);
        }
        form.appendButton("§l§1  ↻ New Search");
        form.appendButton("§l§0  ← Back");

        form.sendTo(player,
            [xuid, guildIds, guildNames](Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;
                int n = (int)guildIds.size();
                if (sel == n)     { showRequestJoinGuild(p); return; }
                if (sel == n + 1) { showMyGuild(p); return; }
                if (sel >= n) return;

                // Send join request via mail ke leader
                if (GuildManager::getInstance().sendJoinRequest(
                        guildIds[sel], xuid, p.getRealName())) {
                    p.sendMessage(std::format(
                        "§a[Guild] §fJoin request sent to §6{}§f. "
                        "Wait for the leader to respond.", guildNames[sel]));
                } else {
                    p.sendMessage("§c[Guild] §fFailed to send request — guild may be full or no longer exists.");
                }
            });
    }


    // =========================================================================
    //  19.  MUTINY CONFIRM  (member challenges leader)
    // =========================================================================

    static void showMutinyConfirm(Player& player) {
        const std::string xuid = player.getXuid();
        auto g = GuildManager::getInstance().getGuildOfPlayer(xuid);
        if (!g || g->leaderXuid == xuid) return;

        if (auto mv = GuildManager::getInstance().getMutinyVote(g->guildId); mv && mv->active) {
            ll::form::SimpleForm err;
            err.setTitle("§4✗ Vote Already Active");
            err.setContent("§fA mutiny vote is already in progress.\n"
                           "§7Wait for it to finish first.");
            err.appendButton("§l§0  ← Back");
            err.sendTo(player, [](Player& p, int, ll::form::FormCancelReason) {
                showMyGuild(p);
            });
            return;
        }

        ll::form::ModalForm confirm;
        confirm.setTitle("§4✦ Initiate Mutiny");
        confirm.setContent(std::format(
            "§fChallenge the current leader of §6{}§f?\n\n"
            "§7• Cost: §c{} ZI §7(refunded if rejected/expired)\n"
            "§7• Timeout: §e1 hour\n"
            "§7• Requires §aunanimous approval§7 of other non-leader members\n"
            "§7• If approved, §6you become the new leader§7",
            g->guildName, kMutinyCostZI));
        confirm.setUpperButton("§l§4⚡ Start Mutiny");
        confirm.setLowerButton("§0✗ Cancel");

        std::string guildId = g->guildId;
        confirm.sendTo(player,
            [xuid, guildId](Player& p, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                if (r != ll::form::ModalFormSelectedButton::Upper) return;
                std::string vid = GuildManager::getInstance()
                    .startMutinyVote(guildId, xuid, p.getRealName());
                if (vid.empty()) {
                    p.sendMessage("§c[Guild] §fFailed to start mutiny — check ZI balance.");
                } else {
                    p.sendMessage(std::format(
                        "§a[Guild] §fMutiny vote started. §710 ZI deducted."));
                }
            });
    }


    // =========================================================================
    //  20.  QUEST MENU  (dipanggil dari klik kiri ke aetheria:guild_receptionist)
    // =========================================================================

    static void showQuestMenu(Player& player, PlayerProfile& profile) {
        auto& qm = QuestManager::getInstance();
        // Pastikan daily/monthly sudah ter-reset hari ini
        try { qm.checkAndReset(profile); } catch (...) {}

        auto daily   = qm.getActiveDailyQuests(profile);
        auto monthly = qm.getActiveMonthlyQuest(profile);

        // Helper: build progress bar (10 chars wide)
        auto bar = [](int cur, int max) -> std::string {
            constexpr int W = 10;
            int filled = (max > 0) ? std::min(W, cur * W / max) : 0;
            return "§a" + std::string(filled, '|') + "§8" + std::string(W - filled, '|');
        };

        // Build body — list all quests
        std::string body =
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
            "§7Welcome, adventurer! Here are today's missions.\n"
            "§7Daily reset 00:00 WIB · Monthly reset on the 1st.\n"
            "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";

        // Group quests by kind
        auto addQuestLine = [&](const ActiveQuest& q, bool isMonthlyView = false) {
            std::string status;
            if (q.completed) status = " §a✔ §2Completed";
            else             status = std::format(" §7[{}/{}]", q.progress, q.target);

            std::string rewardStr;
            if (q.rewardZI > 0)      rewardStr = std::format(" §8» §e+{} ZI", q.rewardZI);
            else if (q.rewardSD > 0) rewardStr = std::format(" §8» §3+{} SD", q.rewardSD);

            std::string catIcon = (q.category == QuestCategory::Kill) ? "⚔" : "📦";
            body += "§f  " + catIcon + " §f" + q.display + status + rewardStr + "\n";
            if (!q.completed) {
                body += "§8    [" + bar(q.progress, q.target) + "§8]\n";
            }
            (void)isMonthlyView;
        };

        // ─ Normal / Daily ─
        bool anyNormal = false, anyAdvance = false, anySpecial = false;
        for (const auto& q : daily) {
            if (q.kind == QuestType::Normal) anyNormal = true;
            if (q.kind == QuestType::Advance) anyAdvance = true;
            if (q.kind == QuestType::Special) anySpecial = true;
        }

        if (anyNormal) {
            body += "§a  ✦ DAILY QUEST\n";
            for (const auto& q : daily) if (q.kind == QuestType::Normal) addQuestLine(q);
        }
        if (anyAdvance) {
            body += "§b  ✦ ADVANCE QUEST\n";
            for (const auto& q : daily) if (q.kind == QuestType::Advance) addQuestLine(q);
        }
        if (anySpecial) {
            body += "§d  ✦ SPECIAL QUEST\n";
            for (const auto& q : daily) if (q.kind == QuestType::Special) addQuestLine(q);
        }
        if (!anyNormal && !anyAdvance && !anySpecial) {
            body += "§7  (No daily quest. Try /gachaadmin quest reset_daily as admin.)\n";
        }

        // ─ Monthly ─
        body += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n";
        if (monthly) {
            body += "§6  ✦ MONTHLY QUEST\n";
            addQuestLine(*monthly, true);
        } else {
            body += "§7  No monthly quest active.\n";
        }
        body += "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬";

        ll::form::SimpleForm form;
        form.setTitle("§l§6✦ §0Adventure Guild §6✦");
        form.setContent(body);

        // Buttons: untuk setiap Collect quest yang BELUM completed → tombol Submit
        std::vector<std::string> submitQuestIds;
        std::vector<std::string> submitDisplays;
        for (const auto& q : daily) {
            if (q.completed) continue;
            if (q.category != QuestCategory::Collect) continue;
            form.appendButton(std::format("§l§2📦 §0Submit §6{}", q.display));
            submitQuestIds.push_back(q.id);
            submitDisplays.push_back(q.display);
        }
        form.appendButton("§l§0  ✕ Close");

        std::string xuid = player.getXuid();
        form.sendTo(player,
            [xuid, submitQuestIds, submitDisplays]
            (Player& p, int sel, ll::form::FormCancelReason) {
                if (sel < 0) return;
                if (sel < (int)submitQuestIds.size()) {
                    if (!PlayerCache::getInstance().isOnline(xuid)) return;
                    auto& pr = PlayerCache::getInstance().get(xuid);
                    showQuestSubmit(p, pr, submitQuestIds[sel], submitDisplays[sel]);
                }
                // last button = Close → no-op
            });
    }


    // =========================================================================
    //  21.  QUEST SUBMIT  (collect quest — submit items dari inventory)
    // =========================================================================

    static void showQuestSubmit(Player& player, PlayerProfile& profile,
                                const std::string& questId, const std::string& display) {
        auto daily = QuestManager::getInstance().getActiveDailyQuests(profile);

        // Cari quest matching id
        const ActiveQuest* target = nullptr;
        for (const auto& q : daily) {
            if (q.id == questId) { target = &q; break; }
        }
        if (!target || target->completed) {
            player.sendMessage("§c[Quest] §fQuest is no longer available.");
            return;
        }

        // Hitung inventory count — skip NBT items supaya angka di UI cocok dengan
        // yang sebenarnya bisa di-submit oleh QuestManager::submitItems (yang juga
        // skip NBT untuk melindungi isi shulker/bundle/enchanted items).
        auto& inv = player.getInventory();
        int invCount = 0;
        int slots = inv.getContainerSize();
        for (int i = 0; i < slots; ++i) {
            const auto& it = inv.getItem(i);
            if (it.getTypeName() != target->targetId) continue;
            if (it.mUserData != nullptr) continue;
            invCount += static_cast<int>(it.mCount);
        }

        const int needed    = target->target - target->progress;
        const int maxSubmit = std::min(invCount, needed);

        if (maxSubmit <= 0) {
            ll::form::SimpleForm err;
            err.setTitle("§4✗ Not Enough Items");
            err.setContent(std::format(
                "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬\n"
                "§fQuest: §6{}\n"
                "§fItem ID: §7{}\n"
                "§fIn inventory: §c{}\n"
                "§fStill needed: §e{}\n"
                "§8▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬",
                display, target->targetId, invCount, needed));
            err.appendButton("§l§0  ← Back");
            std::string xuid = player.getXuid();
            err.sendTo(player, [xuid](Player& p, int, ll::form::FormCancelReason) {
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);
                showQuestMenu(p, pr);
            });
            return;
        }

        // ── Confirm modal ─────────────────────────────────────
        ll::form::ModalForm confirm;
        confirm.setTitle("§6📦 Confirm Submit");
        const bool willComplete = (target->progress + maxSubmit) >= target->target;
        confirm.setContent(std::format(
            "§fSubmit §e{}§f items to quest §6{}§f?\n\n"
            "§7  Inventory  §8» §c{}\n"
            "§7  Quest needs §8» §e{}\n"
            "§7  Will submit §8» §a{}\n\n"
            "§fProgress after: §a[{}/{}]\n{}",
            maxSubmit, display, invCount, needed, maxSubmit,
            target->progress + maxSubmit, target->target,
            willComplete ? "§a✔ Quest will be COMPLETED!" : "§7§oContinue collecting later."));
        confirm.setUpperButton("§l§2✔ Submit Now");
        confirm.setLowerButton("§0✗ Cancel");

        std::string xuid = player.getXuid();
        confirm.sendTo(player,
            [xuid, questId, maxSubmit]
            (Player& p, ll::form::ModalFormResult r, ll::form::FormCancelReason) {
                if (r != ll::form::ModalFormSelectedButton::Upper) return;
                if (!PlayerCache::getInstance().isOnline(xuid)) return;
                auto& pr = PlayerCache::getInstance().get(xuid);

                int submitted = QuestManager::getInstance().submitItems(pr, p, questId, maxSubmit);
                if (submitted > 0) {
                    p.sendMessage(std::format(
                        "§a[Quest] §fSubmitted §e{}§f items successfully.", submitted));
                } else {
                    p.sendMessage("§c[Quest] §fSubmission failed.");
                }
                // Re-open menu
                showQuestMenu(p, pr);
            });
    }
};

} // namespace gacha_mod
