#include "mod/ExchangeManager.h"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace gacha_mod {

ExchangeManager& ExchangeManager::getInstance() {
    static ExchangeManager instance;
    return instance;
}

bool ExchangeManager::loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    try {
        json j;
        file >> j;
        mOptions.clear();
        for (const auto& item : j.at("exchanges")) {
            ExchangeOption opt;
            opt.id             = item.at("id").get<std::string>();
            opt.fromCurrency   = item.at("from_currency").get<std::string>();
            opt.fromAmount     = item.at("from_amount").get<int>();
            opt.toCurrency     = item.at("to_currency").get<std::string>();
            opt.toAmount       = item.at("to_amount").get<int>();
            opt.bulkMultiplier = item.value("bulk_multiplier", 1);
            opt.displayLabel   = item.value("display_label", "");
            opt.description    = item.value("description", "");
            mOptions.push_back(std::move(opt));
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace gacha_mod
