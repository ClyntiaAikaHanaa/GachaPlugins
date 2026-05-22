#pragma once
#include <string>
#include <vector>

namespace gacha_mod {

// Satu opsi penukaran mata uang, dibaca dari exchange_config.json
struct ExchangeOption {
    std::string id;
    std::string fromCurrency;    // "sd" | "zi" | "diamond"
    int         fromAmount     = 1;
    std::string toCurrency;      // "sd" | "zi"
    int         toAmount       = 1;
    int         bulkMultiplier = 1;  // kalikan from/to dengan nilai ini untuk bulk exchange
    std::string displayLabel;        // teks tombol di form (mendukung §-color)
    std::string description;         // keterangan singkat
};

class ExchangeManager {
public:
    static ExchangeManager& getInstance();

    // Membaca exchange_config.json; return false jika gagal
    bool loadConfig(const std::string& path);

    const std::vector<ExchangeOption>& getOptions() const { return mOptions; }

private:
    ExchangeManager() = default;
    std::vector<ExchangeOption> mOptions;
};

} // namespace gacha_mod
