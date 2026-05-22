#pragma once

#include "ll/api/mod/NativeMod.h"
#include "ll/api/event/ListenerBase.h"
#include "mod/GachaTypes.h"
#include <memory>
#include <vector>
#include <algorithm>        // std::remove_if
#include <cstdlib>          // std::abs

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
//  Bounding Box Constants (X half, Y half, Z half) — single source of truth
//  semua anti-grief layer (PlayerDestroy/Place/FireSpread + Hook native)
//  pakai konstanta yang SAMA di sini untuk konsistensi.
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr int kShrineHalfX = 8;
inline constexpr int kShrineHalfY = 15;
inline constexpr int kShrineHalfZ = 8;

inline constexpr int kGuildBaseHalfX = 17;
inline constexpr int kGuildBaseHalfY = 21;
inline constexpr int kGuildBaseHalfZ = 17;

// Generic bbox check — pos vs center with half-extents
template <typename PosT, typename CenterT>
inline bool inBBox(PosT const& pos, CenterT const& center,
                   int hx, int hy, int hz) {
    return std::abs(pos.x - center.x) <= hx
        && std::abs(pos.y - center.y) <= hy
        && std::abs(pos.z - center.z) <= hz;
}

// Convenience: cek pos masuk ke salah satu shrine di list
template <typename PosT>
inline bool isInsideAnyShrine(PosT const& pos,
                              std::vector<AltarPos> const& altars) {
    for (const auto& a : altars) {
        // Y-axis early exit untuk hot path (kebanyakan blok jauh di Y-axis)
        if (std::abs(pos.y - a.y) > kShrineHalfY) continue;
        if (inBBox(pos, a, kShrineHalfX, kShrineHalfY, kShrineHalfZ)) return true;
    }
    return false;
}

template <typename PosT>
inline bool isInsideAnyGuildBase(PosT const& pos,
                                  std::vector<AltarPos> const& bases) {
    for (const auto& b : bases) {
        if (std::abs(pos.y - b.y) > kGuildBaseHalfY) continue;
        if (inBBox(pos, b, kGuildBaseHalfX, kGuildBaseHalfY, kGuildBaseHalfZ)) return true;
    }
    return false;
}

class MyMod {
public:
    // Pindahkan logika Singleton langsung ke Header
    static MyMod& getInstance() {
        static MyMod instance;
        return instance;
    }

    MyMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    public:
    // Fungsi untuk diakses oleh Command Block
    const std::vector<AltarPos>& getRegisteredAltars() const { return mRegisteredAltars; }

    void registerNewAltar(int x, int y, int z) {
        // 1. Cek duplikasi! Kalau koordinat ini sudah ada, langsung abaikan.
        for (const auto& altar : mRegisteredAltars) {
            if (altar.x == x && altar.y == y && altar.z == z) {
                return; // Diam-diam keluar, tidak nge-print log, tidak nambah RAM
            }
        }

        // 2. Kalau belum ada, baru daftarkan dan print ke Terminal
        mRegisteredAltars.push_back({x, y, z});
        getSelf().getLogger().info("Kuil baru terdaftar di X:{} Y:{} Z:{}", x, y, z);
    }

    // Adventure Guild base — pakai pola yang sama dengan shrine.
    const std::vector<AltarPos>& getRegisteredGuildBases() const { return mRegisteredGuildBases; }

    void registerNewGuildBase(int x, int y, int z) {
        for (const auto& base : mRegisteredGuildBases) {
            if (base.x == x && base.y == y && base.z == z) {
                return;
            }
        }
        mRegisteredGuildBases.push_back({x, y, z});
        getSelf().getLogger().info("Adventure Guild baru terdaftar di X:{} Y:{} Z:{}", x, y, z);
    }

    // ── Unregister ──────────────────────────────────────────────
    // Hapus altar/guildbase dari registry. Return jumlah yang ke-remove
    // (0 kalau gak ada match). Berguna saat admin mau pindah/bersihkan altar
    // tanpa restart server (sebelumnya gak ada cara — vector growing only).
    int unregisterAltar(int x, int y, int z) {
        int removed = 0;
        mRegisteredAltars.erase(
            std::remove_if(mRegisteredAltars.begin(), mRegisteredAltars.end(),
                [&](const AltarPos& a) {
                    if (a.x == x && a.y == y && a.z == z) { ++removed; return true; }
                    return false;
                }),
            mRegisteredAltars.end());
        return removed;
    }

    int unregisterGuildBase(int x, int y, int z) {
        int removed = 0;
        mRegisteredGuildBases.erase(
            std::remove_if(mRegisteredGuildBases.begin(), mRegisteredGuildBases.end(),
                [&](const AltarPos& a) {
                    if (a.x == x && a.y == y && a.z == z) { ++removed; return true; }
                    return false;
                }),
            mRegisteredGuildBases.end());
        return removed;
    }

private:
    ll::mod::NativeMod& mSelf;

    // Event listener — disimpan agar bisa di-unregister saat disable()
    std::shared_ptr<ll::event::ListenerBase> mJoinListener;
    std::shared_ptr<ll::event::ListenerBase> mLeaveListener;
    // Event listener tambahan
    std::shared_ptr<ll::event::ListenerBase> mTickListener;
    std::shared_ptr<ll::event::ListenerBase> mBlockBreakListener;
    std::shared_ptr<ll::event::ListenerBase> mBlockPlaceListener;
    // Interaksi klik kanan pada blok aetheria:gacha_altar
    std::shared_ptr<ll::event::ListenerBase> mInteractBlockListener;
    // Interaksi klik kiri pada entity aetheria:shrine_maiden (via PlayerAttackEvent)
    std::shared_ptr<ll::event::ListenerBase> mInteractActorListener;
    // Guild chat prefix
    std::shared_ptr<ll::event::ListenerBase> mChatListener;
    // Friendly fire dari sumber projectile / non-melee
    std::shared_ptr<ll::event::ListenerBase> mActorHurtListener;
    // Anti-fire spread di area shrine
    std::shared_ptr<ll::event::ListenerBase> mFireSpreadListener;
    // Quest kill tracking
    std::shared_ptr<ll::event::ListenerBase> mMobDieListener;
    // Penyimpanan koordinat altar sementara di memori
    std::vector<AltarPos> mRegisteredAltars;
    // Penyimpanan koordinat Adventure Guild base
    std::vector<AltarPos> mRegisteredGuildBases;
};

} // namespace gacha_mod