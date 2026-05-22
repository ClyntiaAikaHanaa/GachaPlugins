#include "mod/DebugCommand.h"
#include "mod/MyMod.h"
#include "mod/PlayerProfile.h"
#include "mod/Database.h"
#include "mod/FormManager.h"
#include "mod/BannerManager.h"
#include "mod/LootPoolManager.h"
#include "mod/QuestManager.h"

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

// #include "mc/world/item/ItemStack.h"  // disable dulu sementara
// #include "mc/world/item/registry/ItemStackNetIdVariant.h" // Ini yang bikin error
#include "mc/world/actor/player/Player.h"
#include "mod/GachaEngine.h"

#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────
//  PARAMETER STRUCTS
// ─────────────────────────────────────────────────────────────

struct ParamAccountInfo {};

struct ParamAdminAdd { std::string xuid; std::string type; int amount; };
struct ParamAdminPitySet { std::string tier; std::string xuid; int value; };
struct ParamAdminPityResetGlobal {};
struct ParamAdminDbDump {};
struct ParamAdminDbReload {};
struct ParamAdminDbPlayerInfo { std::string xuid; };

struct ParamAdminGachaStart { std::string xuid; int count; };
struct ParamGachaSpend { std::string xuid; int count; };
struct ParamAdminRegisterAltar {};
struct ParamAdminRegisterGuildBase{};
struct ParamAdminUnregisterAltar     {};   // /gachaadmin unregister_altar (pakai posisi command block)
struct ParamAdminUnregisterGuildBase {};   // /gachaadmin unregister_guildbase

struct ParamAdminSetWeek   { int week; };

struct ParamMyGuild   {};   // /guild — pintu masuk semua fitur guild (create / request / manage)

// /gachaadmin get all loot pool ...
struct ParamAdminGetB5Week { int week; };
struct ParamAdminGetB4Week { int week; };
struct ParamAdminGetB5Std  {};
struct ParamAdminGetB4Std  {};
struct ParamAdminGetB3     {};

// /gachaadmin quest ...
struct ParamAdminQuestResetDaily   {};
struct ParamAdminQuestResetMonthly {};
struct ParamAdminQuestList         {};
struct ParamAdminQuestKill { std::string mobId; int count; };

// ─────────────────────────────────────────────────────────────
//  HELPER FUNCTIONS
// ─────────────────────────────────────────────────────────────

static void showAccountInfo(Player* player) {
    if (!player) return;
    try {
        auto& profile = PlayerCache::getInstance().get(player->getXuid());
        FormManager::showAccountInfo(*player, profile);
    } catch (...) {
        player->sendMessage("§c[GachaMod] Failed to load account data.");
    }
}

// Berikan semua item dari sebuah pool ke player. Dipakai oleh
// /gachaadmin get all loot pool ... untuk keperluan debug loot pool.
static void giveAllPoolItems(Player* player,
                             const std::vector<PoolItem>& pool,
                             const std::string& poolLabel) {
    if (!player) return;
    try {
        auto& profile = PlayerCache::getInstance().get(player->getXuid());
        int claimed = 0, dropped = 0, currency = 0;

        for (const auto& item : pool) {
            GachaResult res;
            res.tier              = GachaTier::B5; // tier tidak relevan untuk delivery
            res.rateType          = RateType::None;
            res.itemId            = item.itemId;
            res.displayName       = item.name;
            res.ingameDisplayName = item.ingameDisplayName;
            res.count             = item.count;
            res.enchantments      = item.enchantments;
            res.extraNbt          = item.extraNbt;

            int r = form_util::giveGachaItem(*player, profile, res);
            if      (r == 1) ++claimed;
            else if (r == 2) ++dropped;
            else if (r == 3) ++currency;
        }

        player->sendMessage(std::format(
            "§a[Debug] §6{}§f — total §e{}§f entries. "
            "§a{}§f claimed, §e{}§f dropped (inv full), §b{}§f currency.",
            poolLabel, (int)pool.size(), claimed, dropped, currency));
    } catch (...) {
        player->sendMessage("§c[Debug] Failed to load player profile.");
    }
}

// ─────────────────────────────────────────────────────────────
//  COMMAND REGISTRATION
// ─────────────────────────────────────────────────────────────

void DebugCommand::registerAll() {
    auto& registrar = ll::command::CommandRegistrar::getInstance(false);

    // ==============================================================
    // 1. COMMAND PLAYER
    // ==============================================================
    // /account — single command (no subcommand) opens the Account Info window
    auto& accCmd = registrar.getOrCreateCommand("account", "Open your Account window", CommandPermissionLevel::Any);
    accCmd.overload<ParamAccountInfo>()
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAccountInfo const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Only players in-game can use this command!"); return; }
            showAccountInfo(static_cast<Player*>(entity));
            output.success("Account window opened.");
        });

    // /guild — single entry-point untuk semua fitur guild:
    //   • Tidak punya guild  → tampilkan Create / Request to Join
    //   • Leader             → kontrol penuh (invite, kick, transfer, disband)
    //   • Member             → leave / mutiny
    auto& guildCmd = registrar.getOrCreateCommand("guild", "Manage your guild", CommandPermissionLevel::Any);
    guildCmd.overload<ParamMyGuild>()
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamMyGuild const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Only players in-game can use this command!"); return; }
            FormManager::showMyGuild(*static_cast<Player*>(entity));
            output.success("Opening guild management.");
        });

    // ==============================================================
    // 2. COMMAND ADMIN
    // ==============================================================
    auto& adminCmd = registrar.getOrCreateCommand("gachaadmin", "Admin Control Gacha", CommandPermissionLevel::GameDirectors);

    // /gachaadmin add
    adminCmd.overload<ParamAdminAdd>()
        .text("add")
        .required("xuid")
        .required("type")
        .required("amount")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminAdd const& param) {
            if (param.amount <= 0) { output.error("Invalid amount."); return; }
            try {
                if (PlayerCache::getInstance().isOnline(param.xuid)) {
                    auto& profile = PlayerCache::getInstance().get(param.xuid);
                    if (param.type == "sd") profile.addSheldDust(param.amount);
                    else if (param.type == "zi") profile.addZenIngot(param.amount);
                    else { output.error("Use 'sd' or 'zi'"); return; }
                    
                    // FIX: Bypass const (Read-Only) restriction untuk mengirim chat
                    Level* level = origin.getLevel();
                    if (level) {
                        level->forEachPlayer([&](auto const& p) {
                            if (p.getXuid() == param.xuid) {
                                Player* targetPlayer = const_cast<Player*>(&p);
                                std::string currencyName = (param.type == "sd") ? "SD" : "ZI";
                                targetPlayer->sendMessage("§a[Aetheria] §fYour top-up of §e" + std::to_string(param.amount) + " " + currencyName + " §fwas successful! Thank you (❁´◡`❁)");
                            }
                            return true;
                        });
                    }
                    output.success("Top-up successful.");
                } else {
                    if (Database::getInstance().exists(param.xuid)) {
                        PlayerData data = Database::getInstance().getOrCreate(param.xuid);
                        if (param.type == "sd") data.sheldDust += param.amount;
                        else if (param.type == "zi") data.zenIngot += param.amount;
                        else { output.error("Use 'sd' or 'zi'"); return; }
                        Database::getInstance().save(data);
                        output.success("Top-up applied to offline database.");
                    } else {
                        output.error("XUID not found in database.");
                    }
                }
            } catch (...) { output.error("Internal Error."); }
        });

    // /gachaadmin pity_set
    adminCmd.overload<ParamAdminPitySet>()
        .text("pity_set")
        .required("tier")
        .required("xuid")
        .required("value")
        .execute([](CommandOrigin const& /*origin*/, CommandOutput& output, ParamAdminPitySet const& param) {
            try {
                if (PlayerCache::getInstance().isOnline(param.xuid)) {
                    auto& profile = PlayerCache::getInstance().get(param.xuid);
                    if (param.tier == "b5") profile.setPityB5(param.value);
                    else if (param.tier == "b4") profile.setPityB4(param.value);
                    else { output.error("Use 'b5' or 'b4'"); return; }
                    output.success("Pity (Online) " + param.tier + " for " + param.xuid + " set to " + std::to_string(param.value));
                } else {
                    if (Database::getInstance().exists(param.xuid)) {
                        PlayerData data = Database::getInstance().getOrCreate(param.xuid);
                        if (param.tier == "b5") data.pityB5 = param.value;
                        else if (param.tier == "b4") data.pityB4 = param.value;
                        else { output.error("Use 'b5' or 'b4'"); return; }
                        Database::getInstance().save(data);
                        output.success("Pity (Offline) " + param.tier + " for " + param.xuid + " set to " + std::to_string(param.value));
                    } else {
                        output.error("XUID not found in database.");
                    }
                }
            } catch (...) { output.error("Failed to set pity."); }
        });

    // /gachaadmin pity_reset_global
    adminCmd.overload<ParamAdminPityResetGlobal>()
        .text("pity_reset_global")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminPityResetGlobal const&) {
            
            std::vector<std::string> allXuids;
            Database::getInstance().forEachPlayer([&allXuids](PlayerData& data) {
                allXuids.push_back(data.xuid);
            });

            for (const auto& xuid : allXuids) {
                if (PlayerCache::getInstance().isOnline(xuid)) {
                    auto& profile = PlayerCache::getInstance().get(xuid);
                    profile.resetPityB5();
                    profile.resetPityB4();
                } else {
                    PlayerData data = Database::getInstance().getOrCreate(xuid);
                    data.pityB5 = 0;
                    data.guaranteeWindowB5 = 70;
                    data.guaranteedFlagB5 = false;
                    data.pityB4 = 0;
                    data.guaranteedFlagB4 = false;
                    Database::getInstance().save(data);
                }
            }

            // FIX: Bypass const restriction dan Auto-tampilkan Account Info
            Level* level = origin.getLevel();
            if (level) {
                level->forEachPlayer([&](auto const& p) {
                    Player* targetPlayer = const_cast<Player*>(&p);
                    targetPlayer->sendMessage("§e[Aetheria] §fAttention! A season reset has been executed by an administrator.");
                    targetPlayer->sendMessage("§a[Aetheria] §fSeason reset complete! Your pity counter has been reset to 0.");
                    showAccountInfo(targetPlayer);
                    return true;
                });
            }

            output.success("Season reset complete! All players notified.");
        });

    // /gachaadmin db_dump
    adminCmd.overload<ParamAdminDbDump>()
        .text("db_dump")
        .execute([](CommandOrigin const& /*origin*/, CommandOutput& output, ParamAdminDbDump const&) {
            int count = 0;
            Database::getInstance().forEachPlayer([&](PlayerData& data) {
                output.success("Player: " + data.xuid + " | ZI: " + std::to_string(data.zenIngot) + " | SD: " + std::to_string(data.sheldDust));
                count++;
            });
            output.success("Total Data: " + std::to_string(count));
        });

    // /gachaadmin db_reload
    adminCmd.overload<ParamAdminDbReload>()
        .text("db_reload")
        .execute([](CommandOrigin const& /*origin*/, CommandOutput& output, ParamAdminDbReload const&) {
            PlayerCache::getInstance().saveAll();
            output.success("Memory synchronized to Database (Save All).");
        });

    // /gachaadmin db_player_info
    adminCmd.overload<ParamAdminDbPlayerInfo>()
        .text("db_player_info")
        .required("xuid")
        .execute([](CommandOrigin const& /*origin*/, CommandOutput& output, ParamAdminDbPlayerInfo const& param) {
            std::string info = "§l§e--- PLAYER INFO (" + param.xuid + ") ---§r\n";
            if (PlayerCache::getInstance().isOnline(param.xuid)) {
                auto& profile = PlayerCache::getInstance().get(param.xuid);
                info += "§aStatus: ONLINE (Live Cache)§r\n";
                info += "§fZI: §e" + std::to_string(profile.getZenIngot()) + " §f| SD: §3" + std::to_string(profile.getSheldDust()) + "\n";
                info += "§fPity B5: §e" + std::to_string(profile.getPityB5()) + "§7/" + std::to_string(profile.getGuaranteeWindowB5()) + " §f(Rate: " + (profile.isGuaranteedB5() ? "§cGuaranteed§f" : "§750/50§f") + ")\n";
                info += "§fPity B4: §e" + std::to_string(profile.getPityB4()) + "§7/20 §f(Rate: " + (profile.isGuaranteedB4() ? "§cGuaranteed§f" : "§750/50§f") + ")";
            } else {
                if (Database::getInstance().exists(param.xuid)) {
                    PlayerData data = Database::getInstance().getOrCreate(param.xuid);
                    info += "§cStatus: OFFLINE (Database)§r\n";
                    info += "§fZI: §e" + std::to_string(data.zenIngot) + " §f| SD: §3" + std::to_string(data.sheldDust) + "\n";
                    info += "§fPity B5: §e" + std::to_string(data.pityB5) + "§7/" + std::to_string(data.guaranteeWindowB5) + " §f(Rate: " + (data.guaranteedFlagB5 ? "§cGuaranteed§f" : "§750/50§f") + ")\n";
                    info += "§fPity B4: §e" + std::to_string(data.pityB4) + "§7/20 §f(Rate: " + (data.guaranteedFlagB4 ? "§cGuaranteed§f" : "§750/50§f") + ")";
                } else {
                    output.error("XUID " + param.xuid + " not found in database!");
                    return;
                }
            }
            output.success(info);
        });

    // /gachaadmin gacha start <xuid> <count>
    adminCmd.overload<ParamAdminGachaStart>()
        .text("gacha")
        .text("start")
        .required("xuid")
        .required("count")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminGachaStart const& param) {
            if (param.count <= 0) {
                output.error("Jumlah pull harus lebih dari 0.");
                return;
            }

            try {
                if (!PlayerCache::getInstance().isOnline(param.xuid)) {
                    output.error("Player dengan XUID tersebut sedang offline. Command ini hanya untuk testing online.");
                    return;
                }

                auto& profile = PlayerCache::getInstance().get(param.xuid);
                Level* level = origin.getLevel();

                level->forEachPlayer([&](auto const& p) {
                    if (p.getXuid() == param.xuid) {
                        Player* player = const_cast<Player*>(&p);
                        
                        player->sendMessage(std::format("§e[Debug] §fMemulai §a{}x §fGacha Pull...", param.count));

                        for (int i = 0; i < param.count; ++i) {
                            // Eksekusi Gacha Engine (RNG & Pity Management)
                            GachaResult res = GachaEngine::performPull(profile);

                            // Log ke console admin
                            output.success(std::format("Pull #{}: {} ({})", i + 1, res.displayName, res.itemId));

                            // Logic Memberikan Item Fisik ke Player
                            // Catatan: Ini adalah placeholder logic. Kamu perlu menyesuaikan dengan 
                            // cara pembuatan ItemStack di versi LeviLamina/BDS-mu.
                            player->sendMessage(std::format("§7- Mendapatkan: §f{}", res.displayName));
                            
                            // Contoh simulasi penambahan item (pseudo-code):
                            // ItemStack item(res.itemId, 1);
                            // player->add(item);
                            // player->refreshInventory();
                        }
                        
                        player->sendMessage("§a[Debug] §fGacha testing selesai.");
                        return false; // Berhenti mencari player
                    }
                    return true;
                });

                output.success(std::format("Berhasil menjalankan {}x pull untuk {}", param.count, param.xuid));
            } catch (const std::exception& e) {
                output.error(std::format("Error: {}", e.what()));
            }
        });

    // /gachaadmin register_altar (TANPA PARAMETER KOORDINAT)
    adminCmd.overload<ParamAdminRegisterAltar>()
        .text("register_altar")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminRegisterAltar const&) {
            
            // Langsung sadap koordinat blok yang mengeksekusi command ini (si Command Block)
            // getBlockPosition() otomatis mengembalikan nilai integer bulat (x, y, z)
            auto pos = origin.getBlockPosition();

            // Y ditambah 1 karena posisi Altar Emas ada persis di atas Command Block!
            MyMod::getInstance().registerNewAltar(pos.x, pos.y + 1, pos.z);
            
            output.success("Altar berhasil diregistrasi oleh sistem.");
        });
    
    // /gachaadmin register_guildbase (TANPA PARAMETER KOORDINAT)
    adminCmd.overload<ParamAdminRegisterGuildBase>()
        .text("register_guildbase")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminRegisterGuildBase const&) {

            // Langsung sadap koordinat blok yang mengeksekusi command ini (si Command Block)
            // getBlockPosition() otomatis mengembalikan nilai integer bulat (x, y, z)
            auto pos = origin.getBlockPosition();

            // Y ditambah 1 karena posisi Altar Emas ada persis di atas Command Block!
            MyMod::getInstance().registerNewGuildBase(pos.x, pos.y + 1, pos.z);

            output.success("Guild Base berhasil diregistrasi oleh sistem.");
        });

    // /gachaadmin unregister_altar — remove altar yang ke-register di posisi
    // command block. Pasangan untuk register_altar — sebelumnya gak ada cara
    // bersihkan altar tanpa restart, list growing only.
    adminCmd.overload<ParamAdminUnregisterAltar>()
        .text("unregister_altar")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminUnregisterAltar const&) {
            auto pos = origin.getBlockPosition();
            int removed = MyMod::getInstance().unregisterAltar(pos.x, pos.y + 1, pos.z);
            if (removed > 0) output.success("Altar removed dari registry (" + std::to_string(removed) + " entry).");
            else             output.error("Tidak ada altar terregistrasi di posisi ini.");
        });

    // /gachaadmin unregister_guildbase — pasangan untuk register_guildbase.
    adminCmd.overload<ParamAdminUnregisterGuildBase>()
        .text("unregister_guildbase")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminUnregisterGuildBase const&) {
            auto pos = origin.getBlockPosition();
            int removed = MyMod::getInstance().unregisterGuildBase(pos.x, pos.y + 1, pos.z);
            if (removed > 0) output.success("Guild Base removed dari registry (" + std::to_string(removed) + " entry).");
            else             output.error("Tidak ada guild base terregistrasi di posisi ini.");
        });

    // /gachaadmin setweek <1-4>
    // Ganti banner week aktif, broadcast countdown 30 detik, lalu shutdown server.
    // Server harus dinyalakan ulang manual — banner_state.json sudah tersimpan.
    adminCmd.overload<ParamAdminSetWeek>()
        .text("setweek")
        .required("week")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminSetWeek const& param) {
            if (param.week < 1 || param.week > 4) {
                output.error("Week must be between 1 and 4. Example: /gachaadmin setweek 2");
                return;
            }

            // Simpan banner week baru ke memori + file (persisten)
            BannerManager::getInstance().setCurrentWeek(param.week);
            BannerManager::getInstance().save();

            // Flush semua data player sekarang
            PlayerCache::getInstance().saveAll();

            const int        targetWeek = param.week;
            const std::string bName     = BannerManager::bannerName(targetWeek);

            output.success(std::format(
                "Banner week set to Week {} — {}. Server shutdown countdown started.", targetWeek, bName));

            // Countdown di background thread.
            // FIX: Jangan capture Level* by value — pointer bisa dangling kalau
            // server shutdown manual selama countdown. Re-fetch via service tiap
            // broadcast supaya selalu pakai pointer terkini (atau bail kalau null).
            std::thread([targetWeek, bName]() {
                auto broadcast = [](const std::string& msg) {
                    auto lv = ll::service::getLevel();
                    if (!lv) return; // server sudah shutdown — abort broadcast
                    lv->forEachPlayer([&](Player const& p) {
                        const_cast<Player&>(p).sendMessage(msg);
                        return true;
                    });
                };

                const std::string hdr = "§8[§6⚔ Aetheria§8] §r";

                // Pengumuman awal
                broadcast(hdr + "§6✦ §eThe banner has changed to §6Week "
                    + std::to_string(targetWeek) + " — " + bName + "§e! ✦");
                broadcast(hdr + "§c⚠ Server is shutting down in §f30 seconds§c. Please prepare!");

                std::this_thread::sleep_for(std::chrono::seconds(20));
                if (!ll::service::getLevel()) return; // server udah down — abort
                broadcast(hdr + "§c⚠ Server shutting down in §f10 seconds§c!");

                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!ll::service::getLevel()) return;
                broadcast(hdr + "§c⚠ Server shutting down in §f5 seconds§c!");

                std::this_thread::sleep_for(std::chrono::seconds(1));
                broadcast(hdr + "§c● §f4§c...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                broadcast(hdr + "§c● §f3§c...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                broadcast(hdr + "§c● §f2§c...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                broadcast(hdr + "§c● §f1§c...");
                std::this_thread::sleep_for(std::chrono::seconds(1));

                broadcast(hdr + "§4Server is shutting down. See you soon!");

                // Final save sebelum exit — re-check level supaya gak crash kalau
                // server sudah shutdown mendahului countdown.
                if (ll::service::getLevel()) {
                    PlayerCache::getInstance().saveAll();
                }

                // Shutdown — nyalakan server manual setelahnya
                std::exit(0);
            }).detach();
        });

    // ==============================================================
    // /gachaadmin get all loot pool ...  (DEBUG)
    //
    // Memberikan semua item dari sebuah pool ke inventory player.
    // Pakai untuk verifikasi loot pool setelah edit gacha_pool.json.
    // Variants:
    //   get all loot pool b5 week <1-4>
    //   get all loot pool b4 week <1-4>
    //   get all loot pool b5 std
    //   get all loot pool b4 std
    //   get all loot pool b3
    // ==============================================================

    // /gachaadmin get all loot pool b5 week <1-4>
    adminCmd.overload<ParamAdminGetB5Week>()
        .text("get").text("all").text("loot").text("pool").text("b5").text("week").required("week")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminGetB5Week const& param) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            if (param.week < 1 || param.week > 4) {
                output.error("Week must be between 1 and 4.");
                return;
            }
            const auto* pool = LootPoolManager::getInstance().getB5RateOn(param.week);
            if (!pool || pool->empty()) {
                output.error(std::format("B5 Rate-On Week {} pool is empty or not found.", param.week));
                return;
            }
            giveAllPoolItems(static_cast<Player*>(entity), *pool,
                std::format("B5 Rate-On Week {}", param.week));
            output.success(std::format("Dumped B5 Rate-On Week {} ({} items).",
                param.week, (int)pool->size()));
        });

    // /gachaadmin get all loot pool b4 week <1-4>
    adminCmd.overload<ParamAdminGetB4Week>()
        .text("get").text("all").text("loot").text("pool").text("b4").text("week").required("week")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminGetB4Week const& param) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            if (param.week < 1 || param.week > 4) {
                output.error("Week must be between 1 and 4.");
                return;
            }
            const auto* pool = LootPoolManager::getInstance().getB4RateOn(param.week);
            if (!pool || pool->empty()) {
                output.error(std::format("B4 Rate-On Week {} pool is empty or not found.", param.week));
                return;
            }
            giveAllPoolItems(static_cast<Player*>(entity), *pool,
                std::format("B4 Rate-On Week {}", param.week));
            output.success(std::format("Dumped B4 Rate-On Week {} ({} items).",
                param.week, (int)pool->size()));
        });

    // /gachaadmin get all loot pool b5 std
    adminCmd.overload<ParamAdminGetB5Std>()
        .text("get").text("all").text("loot").text("pool").text("b5").text("std")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminGetB5Std const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            const auto& pool = LootPoolManager::getInstance().getB5Standard();
            if (pool.empty()) { output.error("B5 Standard pool is empty."); return; }
            giveAllPoolItems(static_cast<Player*>(entity), pool, "B5 Standard");
            output.success(std::format("Dumped B5 Standard ({} items).", (int)pool.size()));
        });

    // /gachaadmin get all loot pool b4 std
    adminCmd.overload<ParamAdminGetB4Std>()
        .text("get").text("all").text("loot").text("pool").text("b4").text("std")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminGetB4Std const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            const auto& pool = LootPoolManager::getInstance().getB4Standard();
            if (pool.empty()) { output.error("B4 Standard pool is empty."); return; }
            giveAllPoolItems(static_cast<Player*>(entity), pool, "B4 Standard");
            output.success(std::format("Dumped B4 Standard ({} items).", (int)pool.size()));
        });

    // /gachaadmin get all loot pool b3
    adminCmd.overload<ParamAdminGetB3>()
        .text("get").text("all").text("loot").text("pool").text("b3")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminGetB3 const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            const auto& pool = LootPoolManager::getInstance().getB3Pool();
            if (pool.empty()) { output.error("B3 pool is empty."); return; }
            giveAllPoolItems(static_cast<Player*>(entity), pool, "B3 Pool");
            output.success(std::format("Dumped B3 ({} items).", (int)pool.size()));
        });

    // ==============================================================
    // /gachaadmin quest ...  (DEBUG quest system)
    // ==============================================================

    // /gachaadmin quest reset_daily — force regenerate daily quests for caller
    adminCmd.overload<ParamAdminQuestResetDaily>()
        .text("quest").text("reset_daily")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminQuestResetDaily const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            auto* player = static_cast<Player*>(entity);
            try {
                auto& profile = PlayerCache::getInstance().get(player->getXuid());
                QuestManager::getInstance().generateDailyQuests(profile);
                profile.save();
                output.success("Daily quests regenerated.");
                player->sendMessage("§a[Quest] §fYour daily quests have been refreshed.");
            } catch (...) { output.error("Failed to regenerate quests."); }
        });

    // /gachaadmin quest reset_monthly — force regenerate monthly quest for caller
    adminCmd.overload<ParamAdminQuestResetMonthly>()
        .text("quest").text("reset_monthly")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminQuestResetMonthly const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            auto* player = static_cast<Player*>(entity);
            try {
                auto& profile = PlayerCache::getInstance().get(player->getXuid());
                QuestManager::getInstance().generateMonthlyQuest(profile);
                profile.save();
                output.success("Monthly quest regenerated.");
                player->sendMessage("§a[Quest] §fYour monthly quest has been refreshed.");
            } catch (...) { output.error("Failed to regenerate quest."); }
        });

    // /gachaadmin quest list — show active quests for caller in chat
    adminCmd.overload<ParamAdminQuestList>()
        .text("quest").text("list")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminQuestList const&) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            auto* player = static_cast<Player*>(entity);
            try {
                auto& profile = PlayerCache::getInstance().get(player->getXuid());
                auto daily   = QuestManager::getInstance().getActiveDailyQuests(profile);
                auto monthly = QuestManager::getInstance().getActiveMonthlyQuest(profile);
                std::string msg = "§a[Quest] §fActive quests:\n";
                for (const auto& q : daily) {
                    msg += std::format("  §7[{}] §f{} §8» §a{}/{}{}\n",
                        questTypeToString(q.kind), q.display,
                        q.progress, q.target,
                        q.completed ? " §a(done)" : "");
                }
                if (monthly) {
                    msg += std::format("  §7[Monthly] §f{} §8» §a{}/{}{}",
                        monthly->display, monthly->progress, monthly->target,
                        monthly->completed ? " §a(done)" : "");
                }
                player->sendMessage(msg);
                output.success("Listed quests.");
            } catch (...) { output.error("Failed to list."); }
        });

    // /gachaadmin quest kill <mob_id> <count> — simulate killing mob N times (for testing kill quests)
    adminCmd.overload<ParamAdminQuestKill>()
        .text("quest").text("kill")
        .required("mobId")
        .required("count")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ParamAdminQuestKill const& param) {
            auto* entity = origin.getEntity();
            if (!entity) { output.error("Player-only command."); return; }
            auto* player = static_cast<Player*>(entity);
            if (param.count <= 0 || param.count > 1000) {
                output.error("Count must be 1-1000.");
                return;
            }
            try {
                auto& profile = PlayerCache::getInstance().get(player->getXuid());
                for (int i = 0; i < param.count; ++i) {
                    QuestManager::getInstance().recordKill(profile, *player, param.mobId);
                }
                output.success(std::format("Simulated {} kills of {}.", param.count, param.mobId));
            } catch (...) { output.error("Failed to record."); }
        });

    // ==============================================================
    // 3. /shrine — REMOVED: Exchange menu is opened exclusively via
    //    left-click on the shrine_maiden NPC (PlayerAttackEvent in MyMod.cpp).
    //    Keeping the command would let players bypass the NPC entirely.
    // ==============================================================

    // /gachaadmin gacha test_spend "XUID" 10
    adminCmd.overload<ParamGachaSpend>()
    .text("gacha")
    .text("test_spend")
    .required("xuid")
    .required("count")
    .execute([](CommandOrigin const& origin, CommandOutput& output, ParamGachaSpend const& param) {
        try {
            if (!PlayerCache::getInstance().isOnline(param.xuid)) {
                output.error("Player offline.");
                return;
            }

            auto& profile = PlayerCache::getInstance().get(param.xuid);
            Level* level = origin.getLevel();
            Player* targetPlayer = nullptr;

            // Cari object Player untuk dikirimkan notifikasi chat
            if (level) {
                level->forEachPlayer([&](auto const& p) {
                    if (p.getXuid() == param.xuid) {
                        targetPlayer = const_cast<Player*>(&p);
                        return false; // Berhenti mencari karena sudah ketemu
                    }
                    return true;
                });
            }
            
            // Logika Biaya 
            int cost = (param.count >= 10) ? 10 : param.count; 
            int totalPulls = (param.count >= 10) ? 11 : param.count;

            // Eksekusi Pemotongan Saldo
            if (profile.spendZenIngot(cost)) {
                output.success(std::format("§a[Success] §fBerhasil memotong §e{} ZI §funtuk §a{}x Pull.", cost, totalPulls));
                
                // Kasih intro chat ke player
                if (targetPlayer) {
                    targetPlayer->sendMessage(std::format("§e✦ Memulai §a{}x §eGacha Pull... ✦", totalPulls));
                }
                
                for (int i = 0; i < totalPulls; ++i) {
                    GachaResult res = GachaEngine::performPull(profile);
                    
                    // Format warna berdasarkan Tier
                    if (targetPlayer) {
                        std::string tierColor = (res.tier == GachaTier::B5) ? "§6" : (res.tier == GachaTier::B4 ? "§5" : "§b");
                        
                        // Cek apakah item ini Rate On atau Rate Off (Khusus B5 & B4)
                        std::string rateStr = "";
                        if (res.tier != GachaTier::B3) {
                            rateStr = (res.rateType == RateType::RateOn) ? " §a[Rate On]" : " §c[Rate Off]";
                        }
                        
                        // Kirim hasil per-pull ke chat player
                        targetPlayer->sendMessage(std::format("§7{} | {}{}{}", i + 1, tierColor, res.displayName, rateStr));
                    }
                }
            } else {
                output.error(std::format("Gagal! Saldo ZI tidak cukup. Butuh: {}, Punya: {}", cost, profile.getZenIngot()));
            }
        } catch (const std::exception& e) {
            output.error(e.what());
        }
    });
}

} // namespace gacha_mod