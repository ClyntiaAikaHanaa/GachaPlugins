#pragma once
#include "mod/GuildData.h"
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

namespace gacha_mod {

// Max entries per player. Saat send melebihi cap, mailId terlama
// dihapus (FIFO eviction). Default 100 — kalau pemain aktif punya
// banyak mail, prune ke 100 entries terbaru.
inline constexpr int kMailMaxPerPlayer = 100;

// Mail lebih lama dari N hari di-purge oleh tickPurgeOldMail (dipanggil dari
// MyMod tick cleanup periodik). 30 hari = ~1 bulan retention.
inline constexpr int kMailRetentionDays = 30;


// ─────────────────────────────────────────────────────────────────────────────
//  MailManager — Sistem inbox in-game.
//
//  Storage:
//      mail:{xuid}:{mailId}   -> JSON MailMessage
//      mail_index:{xuid}      -> JSON array of mailId (urut terbaru di depan)
//
//  Notifikasi chat dikirim oleh sender saat target online.
// ─────────────────────────────────────────────────────────────────────────────

class MailManager {
public:
    static MailManager& getInstance() {
        static MailManager instance;
        return instance;
    }

    // Generate ID unik (timestamp + counter)
    std::string generateMailId();

    // Kirim mail ke target. Mengembalikan mailId yang dibuat,
    // atau string kosong jika gagal. Otomatis kirim chat notif jika target online.
    std::string send(const MailMessage& msg);

    // Quick-send helper
    std::string sendSystem(const std::string& toXuid,
                           const std::string& subject,
                           const std::string& body,
                           MailType type    = MailType::System,
                           const std::string& payload = "");

    // Ambil satu mail
    std::optional<MailMessage> get(const std::string& xuid, const std::string& mailId) const;

    // List semua mailId milik player (terbaru di depan)
    std::vector<std::string> listIds(const std::string& xuid) const;

    // List MailMessage lengkap milik player
    std::vector<MailMessage> listAll(const std::string& xuid) const;

    // Hitung mail belum dibaca
    int unreadCount(const std::string& xuid) const;

    // Tandai dibaca
    bool markRead(const std::string& xuid, const std::string& mailId);

    // Hapus mail
    bool remove(const std::string& xuid, const std::string& mailId);

    // Hapus semua mail player (dipakai saat akun di-purge — opsional)
    void clearAll(const std::string& xuid);

    // Purge mail yang lebih lama dari kMailRetentionDays untuk semua player
    // yang sedang online. Dipanggil periodik dari tick cleanup. Mail offline
    // ke-purge saat player login next time.
    void tickPurgeOldMail();

    // Invalidate unread count cache untuk player ini.
    // Dipakai saat data berubah dari operasi lain (mis. direct DB write).
    void invalidateUnreadCache(const std::string& xuid);

private:
    MailManager() = default;

    static std::string mailKey(const std::string& xuid, const std::string& mailId) {
        return "mail:" + xuid + ":" + mailId;
    }
    static std::string indexKey(const std::string& xuid) {
        return "mail_index:" + xuid;
    }

    // Serialisasi
    static std::string serialize(const MailMessage& m);
    static std::optional<MailMessage> deserialize(const std::string& json);

    // Helper index (read-modify-write)
    std::vector<std::string> readIndex(const std::string& xuid) const;
    bool writeIndex(const std::string& xuid, const std::vector<std::string>& ids);

    // V2 index format includes persisted unread count — hindari N+1 DB scan
    // saat unreadCount() first call after server restart (cache miss).
    struct IndexData {
        std::vector<std::string> ids;
        int  unread = 0;
        bool fromV1 = false;  // true kalau di-read dari format lama (perlu migrate write)
    };
    IndexData readIndexFull(const std::string& xuid) const;
    bool writeIndexFull(const std::string& xuid, const IndexData& data);

    // Trim index ke kMailMaxPerPlayer terbaru, delete mail entries yang ke-evict.
    void pruneToCapacity(const std::string& xuid, std::vector<std::string>& ids);

    // Cache: xuid → unread count. Hindari N+1 DB query setiap call unreadCount.
    // Cache populate lazy saat first call, invalidate pada send/markRead/remove.
    // -1 = belum dihitung. Thread-safe via mUnreadMu.
    mutable std::mutex                          mUnreadMu;
    mutable std::unordered_map<std::string,int> mUnreadCache;
};

} // namespace gacha_mod
