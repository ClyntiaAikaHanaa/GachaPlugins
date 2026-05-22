#include "ll/api/memory/Hook.h"
#include "mc/deps/core/math/IRandom.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/Explosion.h"
#include "mod/MyMod.h"
#include <cmath>

namespace gacha_mod {

// Hook Explosion::explode() untuk melindungi area shrine/guild base dari ledakan.
// Jika bounding box ledakan tumpang-tindih dengan zona shrine/guild, mBreaking
// diset false sehingga tidak ada blok yang hancur — damage entity tetap berlaku.
LL_AUTO_TYPE_INSTANCE_HOOK(
    ShrineExplosionHook,
    ll::memory::HookPriority::Normal,
    Explosion,
    &Explosion::explode,
    bool,
    IRandom& random
) {
    Vec3& epos    = mPos;
    float rad     = mRadius;
    bool  protect = false;

    // Cek Shrine — pakai konstanta bbox dari MyMod.h + expand radius ledakan
    const auto& altars = MyMod::getInstance().getRegisteredAltars();
    for (const auto& altar : altars) {
        float dx = std::abs(epos.x - (static_cast<float>(altar.x) + 0.5f));
        float dy = std::abs(epos.y - (static_cast<float>(altar.y) + 0.5f));
        float dz = std::abs(epos.z - (static_cast<float>(altar.z) + 0.5f));
        if (dx <= kShrineHalfX + rad &&
            dy <= kShrineHalfY + rad &&
            dz <= kShrineHalfZ + rad) {
            protect = true;
            break;
        }
    }

    // Cek Adventure Guild base
    if (!protect) {
        const auto& bases = MyMod::getInstance().getRegisteredGuildBases();
        for (const auto& base : bases) {
            float dx = std::abs(epos.x - (static_cast<float>(base.x) + 0.5f));
            float dy = std::abs(epos.y - (static_cast<float>(base.y) + 0.5f));
            float dz = std::abs(epos.z - (static_cast<float>(base.z) + 0.5f));
            if (dx <= kGuildBaseHalfX + rad &&
                dy <= kGuildBaseHalfY + rad &&
                dz <= kGuildBaseHalfZ + rad) {
                protect = true;
                break;
            }
        }
    }

    if (protect) mBreaking = false;
    return origin(random);
}

} // namespace gacha_mod
