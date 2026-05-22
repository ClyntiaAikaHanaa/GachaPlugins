#pragma once
#include <string>
#include <functional>
#include <optional>
#include <memory>
#include <utility>
#include "ll/api/data/KeyValueDB.h"

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────
//  PLAYER DATA STRUCT
//  Mapping 1:1 ke entry database.
//  Disimpan sebagai JSON string di KeyValueDB.
// ─────────────────────────────────────────────────────────────

struct PlayerData {
    std::string xuid;
    // Default pop up muncul untuk player baru
    bool showWelcomePopup = true;

    // Pity B5
    int  pityB5             = 0;
    int  guaranteeWindowB5  = 70;   // Dynamic — berubah saat kalah 50/50
    bool guaranteedFlagB5   = false;

    // Pity B4
    int  pityB4             = 0;
    bool guaranteedFlagB4   = false;

    // Currency
    int  sheldDust          = 0;
    int  zenIngot           = 0;

    // Quest (disimpan JSON, di-parse oleh QuestManager di Tahap 3)
    std::string dailyQuestJson   = "[]";
    std::string monthlyQuestJson = "{}";
    std::string lastQuestReset   = "";       // YYYY-MM-DD (daily reset stamp)
    std::string lastMonthlyReset = "";       // YYYY-MM    (monthly reset stamp — hindari JSON parse di hot path checkAndReset)

    // Banner minggu aktif (1-4)
    int  activeBannerWeek   = 1;
};

// ─────────────────────────────────────────────────────────────
//  DATABASE CLASS
//  Abstraksi KeyValueDB untuk akses data player.
//  Key format: "player:{xuid}"
// ─────────────────────────────────────────────────────────────

class Database {
public:
    static Database& getInstance();

    // Inisialisasi — dipanggil saat mod load()
    bool init(const std::string& dbPath);

    // Tutup koneksi — dipanggil saat mod disable()
    void close();

    // ── CRUD ──────────────────────────────────────────────────

    // Ambil data player; buat default baru jika belum ada
    PlayerData getOrCreate(const std::string& xuid);

    // Simpan seluruh data player
    void save(const PlayerData& data);

    // Cek apakah player sudah terdaftar
    bool exists(const std::string& xuid);

    // Iterasi semua player (untuk weekly rotation)
    void forEachPlayer(std::function<void(PlayerData&)> callback);

    // ── Update Parsial ────────────────────────────────────────
    // Lebih efisien — hanya update field tertentu
    void savePity(const std::string& xuid,
                  int pityB5, int guaranteeWindowB5, bool guaranteedFlagB5,
                  int pityB4, bool guaranteedFlagB4);

    void saveCurrency(const std::string& xuid, int sheldDust, int zenIngot);

    void saveQuests(const std::string& xuid,
                    const std::string& dailyJson,
                    const std::string& monthlyJson,
                    const std::string& lastReset);

    // ── Raw KV access (dipakai GuildManager & MailManager) ────
    // GuildManager: key "guild:{id}", "guild_by_member:{xuid}", "guild_kick:{guildId}", "guild_mutiny:{guildId}"
    // MailManager : key "mail:{xuid}:{mailId}", "mail_index:{xuid}"
    bool                       kvSet(const std::string& key, const std::string& value);
    std::optional<std::string> kvGet(const std::string& key) const;
    bool                       kvDel(const std::string& key);
    bool                       kvHas(const std::string& key) const;
    // Iterasi prefix — callback dipanggil per entry; return false untuk berhenti
    void kvScanPrefix(const std::string& prefix,
                      std::function<bool(const std::string& key, const std::string& value)> cb) const;

private:
    Database() = default;
    ~Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    std::unique_ptr<ll::data::KeyValueDB> mDb;
    bool mInitialized = false;

    // Serialisasi PlayerData ↔ JSON string
    static std::string  toJson(const PlayerData& data);
    static PlayerData   fromJson(const std::string& xuid, const std::string& json);
    static PlayerData   makeDefault(const std::string& xuid);

    // Helper key
    static std::string makeKey(const std::string& xuid) {
        return "player:" + xuid;
    }
};

} // namespace gacha_mod
