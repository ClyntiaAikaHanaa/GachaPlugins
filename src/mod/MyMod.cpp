#include "mod/MyMod.h"
#include "mod/Database.h"
#include "mod/PlayerProfile.h"
#include "mod/DebugCommand.h"
#include "mod/LootPoolManager.h"
#include "mod/ExchangeManager.h"
#include "mod/BannerManager.h"
#include "mod/FormManager.h"
#include "mod/GuildManager.h"
#include "mod/MailManager.h"
#include "mod/QuestManager.h"

#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/player/PlayerAttackEvent.h"
#include "ll/api/event/player/PlayerChatEvent.h"
#include "ll/api/event/entity/ActorHurtEvent.h"
#include "ll/api/event/entity/MobDieEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/event/world/FireSpreadEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/legacy/ActorUniqueID.h"
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────
//  Cooldown Maps — namespace-scope agar bisa di-clear pada disconnect
//  (mencegah growth tak terbatas seiring waktu)
// ─────────────────────────────────────────────────────────────
namespace cooldowns {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> sInteractOpen; // altar + NPC
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> sFfWarn;       // melee + projectile FF

    inline void clearPlayer(const std::string& xuid) {
        sInteractOpen.erase(xuid);
        sFfWarn.erase(xuid);
    }
}

// ─────────────────────────────────────────────────────────────
//  Deferred Join Work — split CPU load across multiple ticks
//
//  Saat player join, BDS native sudah berat (chunk compress + send).
//  Plugin work (mail, nametag, quest reset) di-DEFER ke tick berikutnya
//  supaya tidak nambah spike di tick yang sama.
//
//  Setiap step di-process di tick berbeda → CPU load spread across
//  ~2 detik, bukan numpuk di 1 tick.
// ─────────────────────────────────────────────────────────────
namespace deferred_join {
    // Step yang DEFERRED — sisanya (WelcomeMenu, NametagRefresh) di-sync di handler
    // karena murah (form construct & setNameTag dengan O(1) cache).
    enum Step : int {
        QuestReset = 0,  // ~1 tick   (50ms)   — priority untuk state correctness
        MailNotif  = 1,  // ~10 ticks (500ms)  — heaviest op (mail scan first time)
        Done
    };

    struct PendingWork {
        std::string xuid;
        int         step;
        int         scheduledTickCounter; // tick saat work harus dieksekusi
    };

    // Schedule untuk tiap step (offset dari join). Tick counter increment per tick.
    static constexpr int kStepDelayTicks[] = {1, 10};

    static std::vector<PendingWork> sQueue;
    static int sTickCounter = 0;

    inline void enqueue(const std::string& xuid) {
        sQueue.push_back({xuid, Step::QuestReset,
                          sTickCounter + kStepDelayTicks[Step::QuestReset]});
    }

    // Cancel pending work kalau player disconnect sebelum semua step jalan.
    inline void cancel(const std::string& xuid) {
        sQueue.erase(
            std::remove_if(sQueue.begin(), sQueue.end(),
                [&](const PendingWork& w) { return w.xuid == xuid; }),
            sQueue.end());
    }
}

// ─────────────────────────────────────────────────────────────
//  LOAD — inisialisasi database & config JSON
// ─────────────────────────────────────────────────────────────

bool MyMod::load() {
    getSelf().getLogger().info("GachaMod loading...");

    // 1. Inisialisasi Database LevelDB
    auto dbPath = getSelf().getDataDir() / "gacha_data";
    if (!Database::getInstance().init(dbPath.string())) {
        getSelf().getLogger().error("Gagal membuka database!");
        return false;
    }
    getSelf().getLogger().info("Database OK: {}", dbPath.string());

    // 2. Load konfigurasi gacha_pool.json
    auto configPath = getSelf().getDataDir() / "gacha_pool.json";
    if (!LootPoolManager::getInstance().loadConfig(configPath.string())) {
        getSelf().getLogger().warn("Gagal memuat gacha_pool.json! Pastikan file ada di folder konfigurasi mod.");
    }

    // 2b. Load banner_state.json — week banner aktif (global, dikontrol admin)
    auto bannerStatePath = getSelf().getDataDir() / "banner_state.json";
    BannerManager::getInstance().load(bannerStatePath.string());

    // 2c. Load quest_config.json — definisi pool quest (Normal/Advance/Special/Monthly)
    auto questPath = getSelf().getDataDir() / "quest_config.json";
    if (!QuestManager::getInstance().loadConfig(questPath.string())) {
        getSelf().getLogger().warn("Gagal memuat quest_config.json! Pastikan file ada di folder konfigurasi mod.");
    } else {
        getSelf().getLogger().info("Quest config OK.");
    }

    // 3. Load konfigurasi exchange_config.json
    auto exchangePath = getSelf().getDataDir() / "exchange_config.json";

    // Write default exchange_config.json if missing
    if (!std::filesystem::exists(exchangePath)) {
        std::filesystem::create_directories(exchangePath.parent_path());
        std::ofstream def(exchangePath.string());
        def << R"JSON({
  "exchanges": [
    { "id": "diamond_to_sd",  "from_currency": "diamond", "from_amount": 10, "to_currency": "sd", "to_amount": 1,  "bulk_multiplier": 1,   "description": "Convert Diamonds from your inventory" },
    { "id": "sd_to_zi_x1",   "from_currency": "sd",      "from_amount": 10, "to_currency": "zi", "to_amount": 1,  "bulk_multiplier": 1,   "description": "1 pull worth of Zen Ingot" },
    { "id": "sd_to_zi_x10",  "from_currency": "sd",      "from_amount": 10, "to_currency": "zi", "to_amount": 1,  "bulk_multiplier": 10,  "description": "Bulk x10 — 10 pulls at once" },
    { "id": "sd_to_zi_x100", "from_currency": "sd",      "from_amount": 10, "to_currency": "zi", "to_amount": 1,  "bulk_multiplier": 100, "description": "Mega bulk x100 — 100 pulls" }
  ]
})JSON";
        getSelf().getLogger().info("Created default exchange_config.json");
    }
    if (!ExchangeManager::getInstance().loadConfig(exchangePath.string())) {
        getSelf().getLogger().warn("Gagal memuat exchange_config.json! Menu Currency Exchange tidak akan menampilkan opsi.");
    } else {
        getSelf().getLogger().info("Exchange config OK: {} opsi dimuat.",
            ExchangeManager::getInstance().getOptions().size());
    }

    return true;
}

// ─────────────────────────────────────────────────────────────
//  ENABLE — daftarkan event listener
// ─────────────────────────────────────────────────────────────

bool MyMod::enable() {
    getSelf().getLogger().info("GachaMod enabling...");

    auto& bus = ll::event::EventBus::getInstance();

    // 1. Player Join — HYBRID SYNC + DEFERRED
    //
    // Bedrock native sudah berat saat join (chunk compress + send ke client,
    // skin upload, auth handshake). Plugin work dibagi 2 kategori:
    //
    // SYNC (cheap ops di tick join):
    //   - Profile load (essential untuk state)
    //   - Welcome menu/message (form ~5-10ms, instant UX)
    //   - Nametag refresh (~2-5ms dengan O(1) cache + early exit kalau bukan guild)
    //   - lastSeen marker (in-memory write, microseconds)
    //
    // DEFERRED (expensive ops, spread ke tick berikutnya):
    //   - Quest reset (step 0 @ 50ms — priority state correctness)
    //   - Mail unread notif (step 1 @ 500ms — heaviest scan)
    mJoinListener = bus.emplaceListener<ll::event::PlayerJoinEvent>(
        [](ll::event::PlayerJoinEvent& ev) {
            try {
                auto& player = ev.self();
                const std::string xuid = player.getXuid();

                // ESSENTIAL: load profile + register Player* untuk O(1) lookup.
                auto& profile = PlayerCache::getInstance().onPlayerJoin(xuid, &player);

                // SYNC: Welcome (cheap, fix UX delay trade-off)
                if (profile.shouldShowWelcome()) {
                    FormManager::showWelcomeMenu(player, profile);
                } else {
                    player.sendMessage("§6[§eAetheria§6] §8» §7Welcome back, adventurer!");
                }

                // SYNC: Nametag refresh dengan optimized variant.
                // refreshNametagAtJoin early-exit kalau player gak di guild
                // (Bedrock default nametag sudah benar). Skip IPC call yang gak perlu.
                GuildManager::refreshNametagAtJoin(xuid);

                // SYNC: Mark lastSeen di in-memory buffer (microseconds).
                // Periodic flush via tick listener batchin DB write per guild.
                GuildManager::getInstance().markLastSeen(xuid);

                // DEFERRED: Quest reset (priority) + Mail notif (heaviest).
                deferred_join::enqueue(xuid);

            } catch (const std::exception& e) {
                MyMod::getInstance().getSelf().getLogger().error("Error saat player join: {}", e.what());
            }
        }
    );

    // 2. Player Disconnect
    mLeaveListener = bus.emplaceListener<ll::event::PlayerDisconnectEvent>(
        [](ll::event::PlayerDisconnectEvent& ev) {
            auto& logger = MyMod::getInstance().getSelf().getLogger();
            try {
                std::string xuid = ev.self().getXuid();

                // Mark lastSeen di buffer lalu flush — disconnect adalah event
                // yang penting di-persist segera (player mungkin offline lama).
                // Buffer flush bakal batch process semua pending termasuk yang
                // ini, jadi disconnect handler tetap cepat.
                GuildManager::getInstance().markLastSeen(xuid);
                try { GuildManager::getInstance().flushLastSeenBuffer(); }
                catch (...) {}

                // Bersihkan cooldown maps untuk player ini (mencegah unbounded growth)
                cooldowns::clearPlayer(xuid);

                // Cancel pending deferred join work kalau player disconnect
                // sebelum semua step selesai. Hindari try-process xuid offline.
                deferred_join::cancel(xuid);

                // Drop mail unread cache untuk player ini supaya gak occupy memory
                // selama mereka offline. Saat login lagi → lazy populate.
                MailManager::getInstance().invalidateUnreadCache(xuid);

                PlayerCache::getInstance().onPlayerLeave(xuid);
            } catch (const std::exception& e) {
                logger.error("Error player leave: {}", e.what());
            }
        }
    );

    // 3. Server Level Tick — multi-layer altar particle effect
    mTickListener = bus.emplaceListener<ll::event::ServerLevelTickEvent>(
        [this](ll::event::ServerLevelTickEvent&) {
            static int tickCounter = 0;
            static int orbitStep   = 0;
            tickCounter++;
            deferred_join::sTickCounter = tickCounter;

            // ── Deferred Join Work ──────────────────────────────────────
            // Process pending post-join steps untuk player yang baru masuk.
            // Each step di-execute di tick berbeda (50-500ms setelah join)
            // supaya CPU load gak numpuk di tick yang sama dengan chunk send.
            //
            // STRATEGI: Cari FIRST READY entry (bukan just front) supaya saat
            // multiple player join bareng, queue gak stuck di front saja.
            // Max 1 work per tick — spread CPU load lintas tick.
            if (!deferred_join::sQueue.empty()) {
                auto readyIt = std::find_if(
                    deferred_join::sQueue.begin(),
                    deferred_join::sQueue.end(),
                    [tc = tickCounter](const deferred_join::PendingWork& w) {
                        return tc >= w.scheduledTickCounter;
                    });
                if (readyIt != deferred_join::sQueue.end()) {
                    auto& w = *readyIt;
                    if (tickCounter >= w.scheduledTickCounter) {
                    const std::string xuid = w.xuid;
                    const int step = w.step;

                    try {
                        // Pastikan player masih online sebelum process
                        auto* p = PlayerCache::getInstance().getOnlinePlayer(xuid);
                        if (p && PlayerCache::getInstance().isOnline(xuid)) {
                            auto& profile = PlayerCache::getInstance().get(xuid);

                            switch (step) {
                                case deferred_join::Step::QuestReset: {
                                    // Priority — state correctness. Dilakukan 50ms post-join
                                    // supaya window di mana quest kemarin masih visible tetap
                                    // di bawah 100ms (player belum sempat buka /account).
                                    QuestManager::getInstance().checkAndReset(profile);
                                    break;
                                }
                                case deferred_join::Step::MailNotif: {
                                    // Heaviest — first call scan all mail. Defer ke 500ms
                                    // supaya gak numpuk dengan chunk send native BDS.
                                    int unread = MailManager::getInstance().unreadCount(xuid);
                                    if (unread > 0) {
                                        p->sendMessage(std::format(
                                            "§6[§eAetheria§6] §8» §fYou have §e{} §funread message{}! "
                                            "Use §a/account §f→ Inbox.",
                                            unread, unread == 1 ? "" : "s"));
                                    }
                                    break;
                                }
                                default: break;
                            }
                        }
                    } catch (...) { /* swallow — gak boleh crash tick */ }

                    // Advance ke step berikutnya atau hapus dari queue
                    int nextStep = step + 1;
                    if (nextStep >= deferred_join::Step::Done) {
                        deferred_join::sQueue.erase(readyIt);
                    } else {
                        w.step = nextStep;
                        w.scheduledTickCounter =
                            tickCounter + deferred_join::kStepDelayTicks[nextStep];
                    }
                    }
                }
            }

            // Guild vote cleanup — jalankan setiap 600 tick (~30 detik)
            if (tickCounter % 600 == 0) {
                try { GuildManager::getInstance().tickCleanup(); }
                catch (...) { /* swallow */ }
                // Quest hit-tracker pruning (delayed kill credit memory cleanup)
                try { QuestManager::getInstance().pruneExpiredHits(); }
                catch (...) { /* swallow */ }
            }

            // Mail purge — jarang dipanggil (tiap ~30 menit) karena scan
            // semua online player + iterate mail per player. Ada cap per-player
            // (kMailMaxPerPlayer) tapi retention-based purge bersihin mail lama.
            if (tickCounter % 36000 == 0) { // 36000 ticks = 30 menit
                try { MailManager::getInstance().tickPurgeOldMail(); }
                catch (...) { /* swallow */ }
            }

            // lastSeen buffer flush — tiap 600 tick (~30 detik).
            // Persist in-memory buffer ke DB, batch by guild supaya hemat I/O.
            // Trade-off: kalau server crash, lose <30 sec accuracy (acceptable).
            if (tickCounter % 600 == 300) { // offset dari guild cleanup biar gak numpuk
                try { GuildManager::getInstance().flushLastSeenBuffer(); }
                catch (...) { /* swallow */ }
            }

            if (tickCounter % 10 != 0) return;
            orbitStep++;

            auto level = ll::service::getLevel();
            if (!level) return;
            auto dim = level->getDimension(0).lock();
            if (!dim) return;

            constexpr float kPi        = 3.14159265f;
            constexpr int   kOrbitCycle = 24; // full rotation every 24 steps = 12 seconds
            constexpr float kOrbitR    = 0.85f;

            float angle = (orbitStep % kOrbitCycle) * (kPi * 2.0f / kOrbitCycle);

            for (const auto& altar : mRegisteredAltars) {
                float cx = static_cast<float>(altar.x) + 0.5f;
                float cy = static_cast<float>(altar.y);
                float cz = static_cast<float>(altar.z) + 0.5f;

                // Layer 1: Rising column — sparkles at low, mid, and high
                level->spawnParticleEffect("minecraft:enchanting_table_particle",
                    { cx, cy + 0.5f, cz }, dim.get());
                level->spawnParticleEffect("minecraft:enchanting_table_particle",
                    { cx, cy + 1.2f, cz }, dim.get());
                if (orbitStep % 2 == 0) {
                    level->spawnParticleEffect("minecraft:enchanting_table_particle",
                        { cx, cy + 2.0f, cz }, dim.get());
                }

                // Layer 2: Orbiting pair — two orbs rotate opposite each other
                level->spawnParticleEffect("minecraft:enchanting_table_particle",
                    { cx + std::cos(angle) * kOrbitR,
                      cy + 0.8f,
                      cz + std::sin(angle) * kOrbitR }, dim.get());
                level->spawnParticleEffect("minecraft:enchanting_table_particle",
                    { cx + std::cos(angle + kPi) * kOrbitR,
                      cy + 0.8f,
                      cz + std::sin(angle + kPi) * kOrbitR }, dim.get());

                // Layer 3: Ground wisps — smoke drifting at the base
                level->spawnParticleEffect("minecraft:basic_smoke_particle",
                    { cx + std::cos(angle + kPi * 0.5f) * 0.5f,
                      cy + 0.1f,
                      cz + std::sin(angle + kPi * 0.5f) * 0.5f }, dim.get());

                // Layer 4: Periodic burst — totem flash every 3 seconds
                if (orbitStep % 6 == 0) {
                    level->spawnParticleEffect("minecraft:totem_particle",
                        { cx, cy + 0.5f, cz }, dim.get());
                }
            }
        }
    );

    // 4. Block Break (Anti-Grief Kuil + Guild Base) — pakai bbox helpers
    mBlockBreakListener = bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>(
        [this](ll::event::PlayerDestroyBlockEvent& ev) {
            auto pos = ev.pos();
            if (isInsideAnyShrine(pos, mRegisteredAltars)) {
                ev.cancel();
                ev.self().sendMessage("§6[§eAncient Shrine§6] §cThis sacred altar is shielded by ancient magic — you cannot break it!");
                return;
            }
            if (isInsideAnyGuildBase(pos, mRegisteredGuildBases)) {
                ev.cancel();
                ev.self().sendMessage("§6[§eAdventure Guild§6] §cThis guild hall is protected — you cannot break it!");
                return;
            }
        }
    );

    // 4a. Block Place (Anti-Grief)
    mBlockPlaceListener = bus.emplaceListener<ll::event::PlayerPlacingBlockEvent>(
        [this](ll::event::PlayerPlacingBlockEvent& ev) {
            auto pos = ev.pos();
            if (isInsideAnyShrine(pos, mRegisteredAltars)) {
                ev.cancel();
                ev.self().sendMessage("§6[§eAncient Shrine§6] §cThis sacred ground is protected by ancient magic — you cannot place blocks here!");
                return;
            }
            if (isInsideAnyGuildBase(pos, mRegisteredGuildBases)) {
                ev.cancel();
                ev.self().sendMessage("§6[§eAdventure Guild§6] §cThis guild ground is protected — you cannot place blocks here!");
                return;
            }
        }
    );

    // 4b. Fire Spread (Anti-Grief)
    mFireSpreadListener = bus.emplaceListener<ll::event::FireSpreadEvent>(
        [this](ll::event::FireSpreadEvent& ev) {
            auto pos = ev.pos();
            if (isInsideAnyShrine(pos, mRegisteredAltars) ||
                isInsideAnyGuildBase(pos, mRegisteredGuildBases)) {
                ev.cancel();
                return;
            }
        }
    );
    // 5. Klik kanan pada blok aetheria:gacha_altar → buka Gacha Form
    mInteractBlockListener = bus.emplaceListener<ll::event::PlayerInteractBlockEvent>(
        [](ll::event::PlayerInteractBlockEvent& ev) {
            auto& blockSrc = ev.self().getDimensionBlockSource();
            auto& block    = blockSrc.getBlock(ev.blockPos());
            if (block.getTypeName() != "aetheria:gacha_altar") return;

            // Cooldown per-player (gacha altar): blok fires setiap tick selama held
            std::string xuid = ev.self().getXuid();
            auto now = std::chrono::steady_clock::now();
            auto it  = cooldowns::sInteractOpen.find(xuid);
            if (it != cooldowns::sInteractOpen.end() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < 1500) {
                return;
            }
            cooldowns::sInteractOpen[xuid] = now;

            ev.cancel();
            if (!PlayerCache::getInstance().isOnline(xuid)) return;
            auto& profile = PlayerCache::getInstance().get(xuid);
            FormManager::showGachaMenu(ev.self(), profile);
        }
    );

    // 6. Klik kiri pada entity aetheria:shrine_maiden → buka Exchange Form
    //    Sekaligus: friendly-fire prevention antar anggota guild.
    //    (Tidak ada event klik kanan entity di LL; gunakan PlayerAttackEvent.
    //     Entity tidak menerima damage berkat damage_sensor: deals_damage=false.)
    mInteractActorListener = bus.emplaceListener<ll::event::PlayerAttackEvent>(
        [](ll::event::PlayerAttackEvent& ev) {
            // ── Friendly fire prevention ──────────────────────────────
            auto& target = ev.target();
            if (target.getTypeName() == "minecraft:player") {
                auto& attacker     = ev.self();
                std::string atkXuid = attacker.getXuid();
                std::string tgtXuid = static_cast<Player&>(target).getXuid();
                if (GuildManager::getInstance().isFriendlyFireBlocked(atkXuid, tgtXuid)) {
                    ev.cancel();
                    auto now = std::chrono::steady_clock::now();
                    auto it  = cooldowns::sFfWarn.find(atkXuid);
                    if (it == cooldowns::sFfWarn.end() ||
                        std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= 3) {
                        attacker.sendMessage(
                            "§6[§eGuild§6] §8» §cYou cannot harm a fellow guild member.");
                        cooldowns::sFfWarn[atkXuid] = now;
                    }
                    return;
                }
            }

            // Dispatch by entity type — shrine_maiden = Exchange, guild_receptionist = Quest
            const std::string targetType = std::string(ev.target().getTypeName());
            const bool isShrineMaiden       = (targetType == "aetheria:shrine_maiden");
            const bool isGuildReceptionist  = (targetType == "aetheria:guild_receptionist");
            if (!isShrineMaiden && !isGuildReceptionist) return;
            ev.cancel();

            // Cooldown 1.5s per player to prevent re-open spam (shared dengan altar)
            std::string xuid = ev.self().getXuid();
            auto now = std::chrono::steady_clock::now();
            auto it  = cooldowns::sInteractOpen.find(xuid);
            if (it != cooldowns::sInteractOpen.end() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < 1500) {
                return;
            }
            cooldowns::sInteractOpen[xuid] = now;

            if (!PlayerCache::getInstance().isOnline(xuid)) return;
            auto& profile = PlayerCache::getInstance().get(xuid);

            if (isShrineMaiden) {
                FormManager::showExchangeMenu(ev.self(), profile);
            } else {
                // Guild receptionist — pastikan quest sudah ter-reset hari ini
                try { QuestManager::getInstance().checkAndReset(profile); } catch (...) {}
                FormManager::showQuestMenu(ev.self(), profile);
            }
        }
    );

    // 7. Friendly fire prevention — versi lengkap (projectile, splash potion, trident, dst).
    //    PlayerAttackEvent di atas hanya cover melee. ActorHurtEvent ini fires untuk
    //    SEMUA sumber damage. Strategi: cancel() + set damage=0 via reference biar
    //    pasti effective tidak peduli posisi hook di damage pipeline.
    mActorHurtListener = bus.emplaceListener<ll::event::ActorHurtEvent>(
        [this](ll::event::ActorHurtEvent& ev) {
            auto& target = ev.self();
            auto& source = ev.source();

            if (!source.isEntitySource()) return;

            auto level = ll::service::getLevel();
            if (!level) return;

            // Resolve attacker (handle projectile owner)
            ActorUniqueID attackerUid = source.getDamagingEntityUniqueID();
            Actor* attackerActor = level->fetchEntity(attackerUid, false);
            if (!attackerActor) {
                ActorUniqueID directUid = source.getEntityUniqueID();
                attackerActor = level->fetchEntity(directUid, false);
                if (!attackerActor) return;
            }
            if (attackerActor->getTypeName() != "minecraft:player") {
                ::ActorUniqueID ownerId = attackerActor->getOwnerId();
                Actor* ownerActor = level->fetchEntity(ownerId, false);
                if (ownerActor && std::string(ownerActor->getTypeName()) == "minecraft:player") {
                    attackerActor = ownerActor;
                } else {
                    return; // bukan player attacker — abaikan
                }
            }

            Player* attackerPlayer = static_cast<Player*>(attackerActor);
            std::string atkXuid = attackerPlayer->getXuid();

            // ── Quest hit tracker — untuk delayed kills (fire aspect, dst) ──
            // Catat attacker untuk SEMUA jenis target non-player (mob).
            // Pakai ActorUniqueID (stable int64), bukan pointer — pointer slot
            // bisa reused setelah mob mati → salah credit kill ke player random.
            if (target.getTypeName() != "minecraft:player") {
                QuestManager::getInstance().trackHit(target.getOrCreateUniqueID().rawID, atkXuid);
                return; // mob hit case — no friendly fire concern
            }

            // ── Friendly fire prevention (target = player) ─────────────────
            std::string tgtXuid = static_cast<Player&>(target).getXuid();
            if (atkXuid == tgtXuid) return; // self-damage diperbolehkan

            if (GuildManager::getInstance().isFriendlyFireBlocked(atkXuid, tgtXuid)) {
                // Double-defense: set damage ke 0 via reference + cancel event
                ev.damage() = 0.0f;
                ev.cancel();

                auto now = std::chrono::steady_clock::now();
                auto it  = cooldowns::sFfWarn.find(atkXuid);
                if (it == cooldowns::sFfWarn.end() ||
                    std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= 3) {
                    attackerPlayer->sendMessage(
                        "§6[§eGuild§6] §8» §cYou cannot harm a fellow guild member.");
                    cooldowns::sFfWarn[atkXuid] = now;
                }
            }
        }
    );

    // 9. Mob die → track kill quest progress
    //    Direct entity source → resolve killer dari damage source langsung.
    //    Indirect source (Fire, Drowning, Fall, etc.) → fallback ke hit tracker
    //    yang merekam attacker terakhir lewat ActorHurtEvent.
    mMobDieListener = bus.emplaceListener<ll::event::MobDieEvent>(
        [](ll::event::MobDieEvent& ev) {
            auto& mob = ev.self();
            const std::string mobType = std::string(mob.getTypeName());

            auto& source = ev.source();
            auto level = ll::service::getLevel();
            const int64_t mobUid = mob.getOrCreateUniqueID().rawID;
            if (!level) {
                QuestManager::getInstance().clearHit(mobUid);
                return;
            }

            std::string killerXuid;

            // ── Path A: Direct entity source (melee / projectile / explosion) ──
            // Extract xuid saja (jangan simpan Player* keluar dari scope ini).
            if (source.isEntitySource()) {
                ::ActorUniqueID killerUid = source.getDamagingEntityUniqueID();
                Actor* killerActor = level->fetchEntity(killerUid, false);
                if (killerActor && killerActor->getTypeName() != "minecraft:player") {
                    ::ActorUniqueID ownerId = killerActor->getOwnerId();
                    Actor* ownerActor = level->fetchEntity(ownerId, false);
                    if (ownerActor && std::string(ownerActor->getTypeName()) == "minecraft:player") {
                        killerActor = ownerActor;
                    } else {
                        killerActor = nullptr;
                    }
                }
                if (killerActor) {
                    killerXuid = static_cast<Player*>(killerActor)->getXuid();
                }
            }

            // ── Path B: Fallback — look up hit tracker (fire aspect, drowning) ──
            if (killerXuid.empty()) {
                killerXuid = QuestManager::getInstance().lookupRecentHitter(mobUid);
            }

            // ── Credit kill ──────────────────────────────────────────────────
            // O(1) Player* lookup — dulu O(N) forEachPlayer setiap mob mati.
            // Pointer hanya dipakai dalam scope tick callback ini, lifetime aman.
            if (!killerXuid.empty()) {
                if (auto* killerP = PlayerCache::getInstance().getOnlinePlayer(killerXuid)) {
                    try {
                        auto& profile = PlayerCache::getInstance().get(killerXuid);
                        QuestManager::getInstance().recordKill(profile, *killerP, mobType);
                    } catch (...) { /* swallow */ }
                }
            }

            // Cleanup hit tracker entry untuk mob yang sudah mati
            QuestManager::getInstance().clearHit(mobUid);
        }
    );

    // 10. Chat hook — tambahkan prefix [GuildName] sebelum nama player kalau dia di guild
    mChatListener = bus.emplaceListener<ll::event::PlayerChatEvent>(
        [](ll::event::PlayerChatEvent& ev) {
            auto& player = ev.self();
            auto guildOpt = GuildManager::getInstance().getGuildOfPlayer(player.getXuid());
            if (!guildOpt) return; // bukan di guild — biarkan format default

            // Cancel default chat broadcast, kirim ulang dengan prefix
            ev.cancel();
            std::string emblemPart = guildOpt->emblem.empty() ? "" : (guildOpt->emblem + " ");
            std::string formatted = std::format(
                "§6[{}{}]§r §f<{}>§r {}",
                emblemPart,
                guildOpt->guildName,
                std::string(player.getRealName()),
                std::string(ev.message())
            );
            auto level = ll::service::getLevel();
            if (!level) return;
            level->forEachPlayer([&](Player const& p) {
                const_cast<Player&>(p).sendMessage(formatted);
                return true;
            });
        }
    );

    // Daftarkan semua /gacha command di sini (paling akhir)
    DebugCommand::registerAll();

    getSelf().getLogger().info("GachaMod aktif.");
    return true;
}

// ─────────────────────────────────────────────────────────────
//  DISABLE — flush data dan bersihkan resources
// ─────────────────────────────────────────────────────────────

bool MyMod::disable() {
    getSelf().getLogger().info("GachaMod disabling...");

    // Unregister event
    auto& bus = ll::event::EventBus::getInstance();
    if (mJoinListener)          bus.removeListener(mJoinListener);
    if (mLeaveListener)         bus.removeListener(mLeaveListener);
    if (mTickListener)          bus.removeListener(mTickListener);
    if (mBlockBreakListener)    bus.removeListener(mBlockBreakListener);
    if (mBlockPlaceListener)    bus.removeListener(mBlockPlaceListener);
    if (mInteractBlockListener) bus.removeListener(mInteractBlockListener);
    if (mInteractActorListener) bus.removeListener(mInteractActorListener);
    if (mChatListener)          bus.removeListener(mChatListener);
    if (mActorHurtListener)     bus.removeListener(mActorHurtListener);
    if (mMobDieListener)        bus.removeListener(mMobDieListener);
    if (mFireSpreadListener)    bus.removeListener(mFireSpreadListener);

    // Flush semua player online
    PlayerCache::getInstance().saveAll();

    // Tutup DB
    Database::getInstance().close();

    getSelf().getLogger().info("GachaMod shutdown selesai.");
    return true;
}

} // namespace gacha_mod

LL_REGISTER_MOD(gacha_mod::MyMod, gacha_mod::MyMod::getInstance());