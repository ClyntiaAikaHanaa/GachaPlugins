#include "ll/api/memory/Hook.h"
#include "mc/world/level/block/FireBlock.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/ActorBlockSyncMessage.h"
#include "mc/deps/core/math/IRandom.h"
#include "mod/MyMod.h"
#include <string>

namespace gacha_mod {

// ─────────────────────────────────────────────────────────────────────────────
// Hook #1: FireBlock::checkBurn()
//
// Dipanggil oleh fire-tick untuk MELAHAP blok flammable adjacent ke fire.
// Skip kalau target block (pos) inside area protected.
// ─────────────────────────────────────────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ShrineFireBurnHook,
    ll::memory::HookPriority::Normal,
    FireBlock,
    &FireBlock::checkBurn,
    void,
    ::BlockSource&    region,
    ::BlockPos const& pos,
    int               chance,
    ::IRandom&        random,
    int               age,
    ::BlockPos const& firePos
) {
    if (isInsideAnyShrine(pos, MyMod::getInstance().getRegisteredAltars())) return;
    if (isInsideAnyGuildBase(pos, MyMod::getInstance().getRegisteredGuildBases())) return;
    return origin(region, pos, chance, random, age, firePos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook #2: FireBlock::$mayPlace()
//
// Dipanggil SEBELUM fire block di-place ke world (dari spread, lava drip,
// lightning strike, flint+steel). Return false = fire TIDAK akan di-place.
//
// Bug yang di-fix:
//   - FireSpreadEvent.cancel() di event listener TIDAK selalu efektif —
//     fire block kadang ke-place duluan, lalu cancel cuma prevent NEXT
//     spread. Hasilnya: invisible fire block yang masih damage player.
//   - Hook ini ngeblok di tingkat NATIVE Bedrock, lebih reliable daripada
//     event listener cancel.
//
// Effect: FIRE BLOCK SAMA SEKALI TIDAK BISA EXIST di dalam area protected.
// ─────────────────────────────────────────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ShrineFirePlaceHook,
    ll::memory::HookPriority::Normal,
    FireBlock,
    &FireBlock::$mayPlace,
    bool,
    ::BlockSource&    region,
    ::BlockPos const& pos
) {
    if (isInsideAnyShrine(pos, MyMod::getInstance().getRegisteredAltars())) return false;
    if (isInsideAnyGuildBase(pos, MyMod::getInstance().getRegisteredGuildBases())) return false;
    return origin(region, pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook #3: FireBlock::entityInside()
//
// Defense-in-depth — kalau somehow fire BERHASIL di-place inside area
// (edge case, race condition, native Bedrock bug yang belum ketauan),
// fire ini TETAP tidak boleh damage entity.
//
// Hook ini bypass damage application kalau fire pos inside protected area.
// Player bisa lewat aman meskipun ada invisible fire block.
// ─────────────────────────────────────────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ShrineFireDamageHook,
    ll::memory::HookPriority::Normal,
    FireBlock,
    &FireBlock::$entityInside,
    void,
    ::BlockSource&    region,
    ::BlockPos const& pos,
    ::Actor&          entity
) {
    if (isInsideAnyShrine(pos, MyMod::getInstance().getRegisteredAltars())) return;
    if (isInsideAnyGuildBase(pos, MyMod::getInstance().getRegisteredGuildBases())) return;
    return origin(region, pos, entity);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook #4: BlockSource::$setBlock — Filter Fire Placement
//
// Bug yang di-fix:
//   FireBlock::$mayPlace TIDAK dipanggil saat fire SPREAD natural (Bedrock
//   fire spread mechanism langsung pakai setBlock tanpa via mayPlace). Akibat:
//   fire bisa spread ke dalam protected area meskipun mayPlace di-deny.
//
// Hook ini catch ALL setBlock calls, filter berdasar block type:
//   - Block selain fire → langsung lewat (99% case, ~10 cycles overhead)
//   - Block fire di luar area → lewat (vanilla behavior)
//   - Block fire di dalam area → DENY (return false, prevent placement)
//
// Overhead: ~10-30 cycles per setBlock call (string compare). Negligible
// untuk normal server load.
// ─────────────────────────────────────────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ShrineBlockSetHook,
    ll::memory::HookPriority::Normal,
    BlockSource,
    &BlockSource::$setBlock,
    bool,
    ::BlockPos const&              pos,
    ::Block const&                 block,
    int                            updateFlags,
    ::ActorBlockSyncMessage const* syncMsg,
    ::BlockChangeContext const&    changeSourceContext
) {
    // Fast path: filter by block type — 99% case (non-fire) lewat tanpa
    // area check yang lebih expensive.
    const std::string typeName = block.getTypeName();
    if (typeName != "minecraft:fire" && typeName != "minecraft:soul_fire") {
        return origin(pos, block, updateFlags, syncMsg, changeSourceContext);
    }

    // Slow path: fire block placement. Cek area sebelum allow.
    if (isInsideAnyShrine(pos, MyMod::getInstance().getRegisteredAltars())) return false;
    if (isInsideAnyGuildBase(pos, MyMod::getInstance().getRegisteredGuildBases())) return false;
    return origin(pos, block, updateFlags, syncMsg, changeSourceContext);
}

} // namespace gacha_mod
