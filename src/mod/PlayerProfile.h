#pragma once
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "mod/Database.h"
#include "mod/GachaTypes.h"

// Forward declaration — Player* hanya disimpan sebagai pointer; tidak perlu
// include header berat di sini.
class Player;

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────
//  PLAYER PROFILE
//  State in-memory satu pemain yang sedang online.
//  Semua sistem game (Gacha, Quest, Currency) pakai class ini,
//  tidak pernah langsung ke Database.
// ─────────────────────────────────────────────────────────────

class PlayerProfile {
public:
    explicit PlayerProfile(const std::string& xuid);

    const std::string& getXuid() const { return mXuid; }

    // Wellcome popup
    bool shouldShowWelcome() const { return mShowWelcomePopup; }
    void setWelcomePopup(bool val) { mShowWelcomePopup = val; flushSettings(); }

    // ── Session Flags ─────────────────────────────────────────
    bool hasReceivedWelcome() const { return mWelcomeReceived; }
    void setWelcomeReceived()       { mWelcomeReceived = true; }

    // ── Currency ──────────────────────────────────────────────
    int  getSheldDust() const { return mSheldDust; }
    int  getZenIngot()  const { return mZenIngot;  }

    // Tambah currency; selalu berhasil untuk add
    void addSheldDust(int amount);
    void addZenIngot(int amount);

    // Kurang currency; return false jika saldo tidak cukup
    bool spendSheldDust(int amount);
    bool spendZenIngot(int amount);  // Dipanggil saat pull gacha

    // Konversi — pengambilan item inventory dilakukan di layer hook sebelumnya
    bool convertDiamondToSD(int diamonds);   // 10 Diamond = 1 SD
    bool convertSDtoZI(int sdAmount);        // 10 SD = 1 ZI

    // ── Pity B5 ───────────────────────────────────────────────
    
    int  getPityB5()            const { return mPityB5;            }
    void setPityB5(int val)           { mPityB5 = val; flushPity();}
    int  getGuaranteeWindowB5() const { return mGuaranteeWindowB5; }
    bool isGuaranteedB5()       const { return mGuaranteedFlagB5;  }

    // Dipanggil tiap pull (meskipun tidak dapat B5)
    void incrementPityB5();

    // Menang 50/50 atau dapat guarantee — full reset
    void resetPityB5();

    // Kalah 50/50 — set flag + sesuaikan dynamic window
    // PENTING: counter TIDAK direset (state persistence)
    void handleB5RateOff();

    // ── Pity B4 ───────────────────────────────────────────────

    int  getPityB4()      const { return mPityB4;            }
    void setPityB4(int val)     { mPityB4 = val; flushPity();}
    bool isGuaranteedB4() const { return mGuaranteedFlagB4;  }

    void incrementPityB4();
    void resetPityB4();
    void handleB4RateOff();  // Set guaranteed flag B4

    // ── Rate Calculation ──────────────────────────────────────

    // Return 0.0–1.0 (sudah di-cap)
    double calcRateB5() const;
    double calcRateB4() const;

    // ── Banner ────────────────────────────────────────────────

    int  getBannerWeek() const           { return mActiveBannerWeek; }
    void setBannerWeek(int week)         { mActiveBannerWeek = week; }

    // ── Persistence ───────────────────────────────────────────

    void save() const;
    void reload();

    // Ringkasan untuk ditampilkan ke player via chat
    std::string getPitySummary() const;

    // Akses raw quest JSON (dipakai QuestManager Tahap 3)
    const std::string& getDailyQuestJson()    const { return mDailyQuestJson;    }
    const std::string& getMonthlyQuestJson()  const { return mMonthlyQuestJson;  }
    const std::string& getLastQuestReset()    const { return mLastQuestReset;    }
    const std::string& getLastMonthlyReset()  const { return mLastMonthlyReset;  }
    void setDailyQuestJson(const std::string& j)   { mDailyQuestJson    = j;   }
    void setMonthlyQuestJson(const std::string& j) { mMonthlyQuestJson  = j;   }
    void setLastQuestReset(const std::string& t)   { mLastQuestReset    = t;   }
    void setLastMonthlyReset(const std::string& t) { mLastMonthlyReset  = t;   }

private:
    std::string mXuid;

    bool mShowWelcomePopup = true;
    void flushSettings() const;

    bool mWelcomeReceived = false;
    
    int  mSheldDust         = 0;
    int  mZenIngot          = 0;

    int  mPityB5            = 0;
    int  mGuaranteeWindowB5 = 70;
    bool mGuaranteedFlagB5  = false;

    int  mPityB4            = 0;
    bool mGuaranteedFlagB4  = false;

    int  mActiveBannerWeek  = 1;

    std::string mDailyQuestJson    = "[]";
    std::string mMonthlyQuestJson  = "{}";
    std::string mLastQuestReset    = "";   // YYYY-MM-DD
    std::string mLastMonthlyReset  = "";   // YYYY-MM

    // ── Konstanta Sistem ──────────────────────────────────────
    static constexpr double kRateB5Base   = 0.006;  // 0.6%
    static constexpr int    kSoftPityB5   = 40;     // Eksponensial mulai sini
    static constexpr double kRateB4Base   = 0.051;  // 5.1%
    static constexpr int    kHardPityB4   = 20;     // Guaranteed B4
    static constexpr int    kGuaranteeAdd = 30;     // Tambahan window kalah 50/50
    static constexpr int    kDiaToSD      = 10;     // 10 Diamond = 1 SD
    static constexpr int    kSDtoZI       = 10;     // 10 SD = 1 ZI

    void loadFromData(const PlayerData& data);
    PlayerData toData() const;

    void flushPity() const;
    void flushCurrency() const;
};

// ─────────────────────────────────────────────────────────────
//  PLAYER CACHE
//  Menyimpan semua PlayerProfile yang sedang online.
//  Thread-safe dengan mutex.
// ─────────────────────────────────────────────────────────────

class PlayerCache {
public:
    static PlayerCache& getInstance();

    // Event join — load dari DB ke cache. Player* opsional disimpan untuk
    // lookup O(1) lewat getOnlinePlayer() (menghindari level->forEachPlayer scan
    // pada setiap mail/notif/refresh nametag).
    PlayerProfile& onPlayerJoin(const std::string& xuid, ::Player* p = nullptr);

    // Event leave — flush ke DB, hapus dari cache + player pointer
    void onPlayerLeave(const std::string& xuid);

    // Ambil profile; throw jika tidak online
    PlayerProfile& get(const std::string& xuid);

    bool isOnline(const std::string& xuid) const;

    // O(1) lookup Player* untuk XUID yang online. Return nullptr kalau offline
    // atau pointer belum ter-register (mis. event order race). Aman untuk dipakai
    // dari thread main game saja — pointer tidak boleh disimpan lintas tick.
    ::Player* getOnlinePlayer(const std::string& xuid) const;

    // Flush semua ke DB — dipanggil periodik atau saat disable()
    void saveAll();

    // Jumlah player online
    size_t count() const;

private:
    PlayerCache() = default;
    mutable std::mutex mMutex;
    std::unordered_map<std::string, std::unique_ptr<PlayerProfile>> mCache;
    std::unordered_map<std::string, ::Player*>                       mOnlinePlayers;
};

} // namespace gacha_mod
