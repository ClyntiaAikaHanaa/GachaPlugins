#pragma once
#include "mod/GuildData.h"
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
//  GuildManager — singleton, semua operasi guild & voting.
//
//  Storage LevelDB:
//      guild:{guildId}            -> JSON GuildData
//      guild_by_member:{xuid}     -> guildId  (reverse lookup)
//      guild_kick:{guildId}       -> JSON KickVote   (max 1 aktif/guild)
//      guild_mutiny:{guildId}     -> JSON MutinyVote (max 1 aktif/guild)
//
//  Konsistensi: semua mutasi dilakukan lewat method ini. UI tidak boleh
//  mengubah file LevelDB langsung.
// ─────────────────────────────────────────────────────────────────────────────

class GuildManager {
public:
    static GuildManager& getInstance() {
        static GuildManager instance;
        return instance;
    }

    // ── Lifecycle ─────────────────────────────────────────────
    // Dipanggil dari tick loop. Bersihkan vote yang sudah expired.
    void tickCleanup();

    // ── ID Generator ──────────────────────────────────────────
    static std::string generateGuildId();
    static std::string generateVoteId();

    // ── Guild CRUD ────────────────────────────────────────────

    // Buat guild baru. founder otomatis menjadi leader. Mengembalikan guildId
    // atau string kosong jika gagal (mis. founder sudah punya guild atau nama kosong).
    std::string createGuild(const std::string& founderXuid,
                            const std::string& founderName,
                            const std::string& guildName);

    // Tambah member (dipanggil saat undangan diterima)
    bool addMember(const std::string& guildId,
                   const std::string& xuid,
                   const std::string& name);

    // Hapus member. Jika yang dihapus leader & masih ada anggota, akan otomatis
    // dialihkan ke member tertua. Jika guild kosong setelah remove, guild dibubarkan.
    // customLogText opsional — kalau diset, ganti pesan default "left" di log
    // (untuk membedakan kick vs leave tanpa double-fetch).
    bool removeMember(const std::string& guildId, const std::string& xuid,
                      const std::string& customLogText = "");

    // Bubarkan guild. Hapus semua reverse-lookup, vote, dst.
    bool disband(const std::string& guildId);

    // Ganti leader (transfer)
    bool transferLeader(const std::string& guildId, const std::string& newLeaderXuid);

    // Rename guild (opsional helper)
    bool rename(const std::string& guildId, const std::string& newName);

    // ── Lookup ────────────────────────────────────────────────

    std::optional<GuildData>  getGuild(const std::string& guildId) const;
    std::optional<std::string> getGuildIdOfPlayer(const std::string& xuid) const;
    std::optional<GuildData>  getGuildOfPlayer(const std::string& xuid) const;

    bool isInSameGuild(const std::string& a, const std::string& b) const;
    bool isLeader(const std::string& guildId, const std::string& xuid) const;
    bool isMember(const std::string& guildId, const std::string& xuid) const;

    // Cek apakah serangan antar 2 player harus di-block (same guild + FF protection ON)
    bool isFriendlyFireBlocked(const std::string& a, const std::string& b) const;

    // Leader only — toggle FF protection. Return true kalau berhasil.
    bool setFriendlyFireProtection(const std::string& guildId,
                                   const std::string& callerXuid,
                                   bool enabled);

    // ── Rename Guild (cost 20 ZI + 14-day cooldown) ──────────
    // Mengembalikan: 0=success, 1=not leader, 2=cooldown active,
    //                3=insufficient ZI, 4=invalid name, 5=guild not found
    int renameWithCost(const std::string& guildId,
                       const std::string& callerXuid,
                       const std::string& newName,
                       long long& outRemainingCooldownSec);

    // Sisa cooldown rename. 0 = bisa rename sekarang.
    long long renameCooldownRemaining(const std::string& guildId) const;

    // ── Guild Emblem ─────────────────────────────────────────
    // Leader only. Empty string = clear emblem.
    bool setEmblem(const std::string& guildId,
                   const std::string& callerXuid,
                   const std::string& emblem);

    // ── Last Seen Tracking ───────────────────────────────────
    // Update timestamp lastSeen untuk player ini di guild-nya (kalau ada).
    // HEAVY — read+write seluruh guild data. Dipanggil saat disconnect dan
    // dari periodic flush (batch by guild di flushLastSeenBuffer).
    void updateMemberLastSeen(const std::string& xuid, long long timestamp);

    // Mark player sebagai "last seen sekarang" — in-memory buffer only,
    // microseconds cost. Dipanggil dari PlayerJoin handler (hot path).
    // Periodic tick fires flushLastSeenBuffer() untuk persist ke DB
    // (batch per-guild supaya gak per-player DB write).
    void markLastSeen(const std::string& xuid);

    // Flush in-memory lastSeen buffer ke DB. Batch by guildId supaya hanya
    // 1 guild read+write per guild (bukan per-player). Dipanggil dari
    // tick listener periodik (~30 detik) + saat disconnect untuk satu xuid.
    void flushLastSeenBuffer();

    // ── Invite (tidak ada storage lokal — di-track lewat MailMessage payload) ──
    // Helper: parse payload {"guildId":"..."} dari mail invite.
    static std::string payloadGuildId(const std::string& payload);
    static std::string payloadString(const std::string& payload, const std::string& key);

    // Parse JSON payload sekali, kembalikan map<key,string> untuk akses berikutnya.
    // Lebih efisien daripada panggil payloadString() berkali-kali (yang re-parse).
    static std::unordered_map<std::string, std::string> parsePayload(const std::string& payload);

    // ── Activity Log ──────────────────────────────────────────
    void appendLog(const std::string& guildId, const std::string& text);

    // ── Search by Name ────────────────────────────────────────
    // Cari guild dengan substring nama (case-insensitive).
    std::vector<GuildData> searchByName(const std::string& query, int maxResults = 8) const;

    // ── Join Request ──────────────────────────────────────────
    // Kirim mail GuildJoinRequest ke leader.
    bool sendJoinRequest(const std::string& guildId,
                         const std::string& requesterXuid,
                         const std::string& requesterName);

    // ── Nametag Refresh ───────────────────────────────────────
    // Refresh nametag player kalau dia online. Aman dipanggil dari mana saja.
    // ALWAYS set — guarantee correct state (reset ke default kalau bukan guild).
    static void refreshNametag(const std::string& xuid);

    // Optimized variant untuk PlayerJoin handler — early exit kalau bukan
    // guild member (Bedrock default nametag sudah benar saat join).
    // JANGAN dipakai di remove/disband/rename (butuh RESET).
    static void refreshNametagAtJoin(const std::string& xuid);

    // ── Kick Vote ─────────────────────────────────────────────
    // Inisiasi kick. Mengirim mail vote ke semua member non-target non-leader.
    // Mengembalikan voteId atau kosong kalau gagal (sudah ada vote aktif, dst).
    // Biaya 10 ZI dikurangi dari leader saat vote dibuat (refund kalau gagal/expired).
    std::string startKickVote(const std::string& guildId,
                              const std::string& leaderXuid,
                              const std::string& targetXuid);

    std::optional<KickVote> getKickVote(const std::string& guildId) const;

    // Catat suara setuju. Return true kalau vote menjadi unanimous & dieksekusi.
    bool castKickApprove(const std::string& guildId, const std::string& voterXuid);

    // Tolak. Vote langsung gugur & ZI dikembalikan.
    bool castKickReject(const std::string& guildId, const std::string& voterXuid);

    // ── Mutiny Vote ───────────────────────────────────────────
    std::string startMutinyVote(const std::string& guildId,
                                const std::string& initiatorXuid,
                                const std::string& initiatorName);

    std::optional<MutinyVote> getMutinyVote(const std::string& guildId) const;

    bool castMutinyApprove(const std::string& guildId, const std::string& voterXuid);
    bool castMutinyReject(const std::string& guildId, const std::string& voterXuid);

private:
    GuildManager() = default;

    // In-memory tracking of guilds with active votes — biar tickCleanup tidak
    // perlu full DB prefix-scan saat tidak ada vote aktif (umumnya 99% case).
    // Mutex protect concurrent access dari tick thread + form callback thread.
    mutable std::mutex                            mActiveVotesMu;
    mutable std::unordered_map<std::string, bool> mActiveKickGuilds;   // guildId → active
    mutable std::unordered_map<std::string, bool> mActiveMutinyGuilds; // guildId → active

    // Hot-path cache: xuid → guildId. Dipakai isFriendlyFireBlocked() yang
    // fires PER HIT di ActorHurtEvent. Tanpa cache, tiap hit = 3x DB lookup
    // (getGuildIdOfPlayer A, B, plus getGuild) → tick lag spike saat combat.
    // Cache di-invalidate di addMember/removeMember/disband.
    // Empty string value = player TIDAK di guild (negative caching valid).
    mutable std::mutex                                       mGuildCacheMu;
    mutable std::unordered_map<std::string, std::string>     mXuidToGuildId;

    // lastSeen buffer — in-memory write buat hot path join.
    // Periodic flush (~30s) atau disconnect flush ke DB, batched by guildId.
    // Tanpa buffer, tiap join = 1 DB read + 1 DB write guild full data.
    std::mutex                                  mLastSeenMu;
    std::unordered_map<std::string, long long>  mLastSeenBuffer; // xuid → timestamp

    // ── Storage Keys ──────────────────────────────────────────
    static std::string keyGuild(const std::string& id)        { return "guild:" + id; }
    static std::string keyMember(const std::string& xuid)     { return "guild_by_member:" + xuid; }
    static std::string keyKick(const std::string& id)         { return "guild_kick:" + id; }
    static std::string keyMutiny(const std::string& id)       { return "guild_mutiny:" + id; }

    // ── Serialisasi ───────────────────────────────────────────
    static std::string serializeGuild(const GuildData& g);
    static std::optional<GuildData> deserializeGuild(const std::string& s);

    static std::string serializeKick(const KickVote& v);
    static std::optional<KickVote> deserializeKick(const std::string& s);

    static std::string serializeMutiny(const MutinyVote& v);
    static std::optional<MutinyVote> deserializeMutiny(const std::string& s);

    bool saveGuild(const GuildData& g);
    bool saveKick(const KickVote& v);
    bool saveMutiny(const MutinyVote& v);

    // ── Execution helpers ─────────────────────────────────────
    // Eksekusi kick. Pastikan leader sudah dikenakan biaya.
    bool executeKick(const GuildData& g, const KickVote& v);

    // Eksekusi mutiny. Initiator menjadi leader baru.
    bool executeMutiny(const GuildData& g, const MutinyVote& v);

    // Refund ZI ke player (online or offline)
    void refundZI(const std::string& xuid, int amount);

    // Charge ZI dari player. Return false kalau saldo kurang.
    bool chargeZI(const std::string& xuid, int amount);

    // Cek apakah semua voter sudah setuju
    static bool isUnanimous(const std::map<std::string, bool>& votes);

    // Broadcast notice mail ke semua anggota selain `exclude`
    void noticeAllMembers(const GuildData& g,
                          const std::string& subject,
                          const std::string& body,
                          const std::string& excludeXuid = "");
};

} // namespace gacha_mod
