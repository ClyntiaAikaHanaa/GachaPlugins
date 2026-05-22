#include "ll/api/memory/Hook.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mod/MyMod.h"
#include <cmath>
#include <string>

namespace gacha_mod {

// Daftar block type yang BOLEH di-remove meskipun di dalam area protected.
// Ini block "transient" yang harus bisa hilang natural (fire die, snow melt,
// dll). Tanpa exception ini, fire yang spread ke boundary → stuck forever
// → invisible fire damage player.
static bool isAllowedTransientBlock(const std::string& typeName) {
    return typeName == "minecraft:fire"
        || typeName == "minecraft:soul_fire"
        || typeName == "minecraft:snow_layer"        // top snow that melts
        || typeName == "minecraft:water"             // flowing water boundary
        || typeName == "minecraft:flowing_water"
        || typeName == "minecraft:lava"              // flowing lava (rare in shrine, but safe)
        || typeName == "minecraft:flowing_lava";
}

// Hook BlockSource::removeBlock — catch-all block destruction path.
//
// Mengapa hook ini diperlukan meskipun ExplosionHook & FireBurnHook sudah ada:
// Beberapa entity boss (terutama Wither) merusak blok via direct call ke
// BlockSource::removeBlock, BUKAN via Explosion::explode. ExplosionHook tidak
// catches Wither's body-slam / movement block-break.
//
// Catatan: player block-break sudah di-cancel di PlayerDestroyBlockEvent (event
// listener) SEBELUM removeBlock dipanggil, jadi hook ini tidak konflik dengan
// player gameplay normal.
//
// EXCEPTION: transient blocks (fire, water, snow_layer) harus tetap bisa
// self-remove untuk natural dying. Hook block self-removal-nya = invisible
// fire bug (fire mati di-client tapi server still has block).
LL_AUTO_TYPE_INSTANCE_HOOK(
    ShrineRemoveBlockHook,
    ll::memory::HookPriority::Normal,
    BlockSource,
    &BlockSource::$removeBlock,
    bool,
    ::BlockPos const&          pos,
    ::BlockChangeContext const& ctx
) {
    // Cek lokasi dulu — kalau bukan di protected area, langsung jalan normal.
    bool inShrine = isInsideAnyShrine(pos, MyMod::getInstance().getRegisteredAltars());
    bool inGuild  = isInsideAnyGuildBase(pos, MyMod::getInstance().getRegisteredGuildBases());
    if (!inShrine && !inGuild) return origin(pos, ctx);

    // Di dalam protected area — cek apakah block ini transient (fire, water, dll).
    // Kalau iya, ALLOW removal supaya natural die / flow lifecycle gak stuck.
    auto& block = getBlock(pos);
    std::string typeName = block.getTypeName();
    if (isAllowedTransientBlock(typeName)) {
        return origin(pos, ctx);
    }

    // Block "real" (kayu, batu, dll) di dalam protected area → DENY.
    return false;
}

} // namespace gacha_mod
