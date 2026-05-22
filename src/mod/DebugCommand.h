#pragma once

namespace gacha_mod {

class DebugCommand {
public:
    // Daftarkan semua /gacha command overload ke LL CommandRegistrar
    // Dipanggil dari MyMod::enable()
    static void registerAll();
};

} // namespace gacha_mod
