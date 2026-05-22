#pragma once
#include "mod/GachaTypes.h"
#include "mod/PlayerProfile.h"

namespace gacha_mod {

class GachaEngine {
public:
    // Mengeksekusi 1x pull, memotong ZI (jika diizinkan), dan mengembalikan item
    static GachaResult performPull(PlayerProfile& profile);

private:
    // Thread-safe RNG helpers
    static float rollRNG();
    static float rollCoin();
};

} // namespace gacha_mod