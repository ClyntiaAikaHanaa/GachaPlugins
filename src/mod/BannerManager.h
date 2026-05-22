#pragma once
#include <string>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
//  BannerManager — Menyimpan banner week aktif secara global (bukan per-player)
//
//  Data disimpan di: <mod_data_dir>/banner_state.json
//  Format: { "current_week": 1 }
//
//  Week dikendalikan manual oleh admin via /gachaadmin setweek <1-4>.
//  Semua player melihat banner yang sama.
// ─────────────────────────────────────────────────────────────────────────────

class BannerManager {
public:
    static BannerManager& getInstance() {
        static BannerManager instance;
        return instance;
    }

    // Getter & setter week aktif (1–4)
    int  getCurrentWeek() const      { return mCurrentWeek; }
    void setCurrentWeek(int week)    { mCurrentWeek = (week >= 1 && week <= 4) ? week : 1; }

    // Load dari file JSON (dipanggil saat MyMod::load)
    bool load(const std::string& filePath);

    // Simpan ke file JSON (dipanggil setelah setweek)
    bool save() const;

    // Nama banner untuk setiap week (untuk tampilan UI & log)
    static const char* bannerName(int week) {
        switch (week) {
            case 1: return "Blade of Nether";
            case 2: return "Warden's Bastion";
            case 3: return "Hunter's Arsenal";
            case 4: return "Tool Grandmaster";
            default: return "Unknown";
        }
    }

private:
    BannerManager() = default;

    int         mCurrentWeek = 1;
    std::string mFilePath;
};

} // namespace gacha_mod
