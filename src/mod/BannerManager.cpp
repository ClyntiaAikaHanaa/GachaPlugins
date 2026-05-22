#include "mod/BannerManager.h"
#include "mod/MyMod.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;

namespace gacha_mod {

bool BannerManager::load(const std::string& filePath) {
    mFilePath = filePath;

    std::ifstream file(filePath);
    if (!file.is_open()) {
        // File belum ada — default ke week 1 dan buat file baru
        mCurrentWeek = 1;
        save();
        MyMod::getInstance().getSelf().getLogger()
            .info("[BannerManager] banner_state.json tidak ditemukan, dibuat baru (Week 1).");
        return true;
    }

    try {
        json j;
        file >> j;
        int week = j.value("current_week", 1);
        mCurrentWeek = (week >= 1 && week <= 4) ? week : 1;
        MyMod::getInstance().getSelf().getLogger()
            .info("[BannerManager] Banner aktif: Week {} — {}", mCurrentWeek, bannerName(mCurrentWeek));
        return true;
    } catch (const std::exception& e) {
        MyMod::getInstance().getSelf().getLogger()
            .error("[BannerManager] Gagal membaca banner_state.json: {}", e.what());
        mCurrentWeek = 1;
        return false;
    }
}

bool BannerManager::save() const {
    if (mFilePath.empty()) return false;
    try {
        std::filesystem::create_directories(
            std::filesystem::path(mFilePath).parent_path());
        std::ofstream file(mFilePath);
        if (!file.is_open()) return false;
        json j;
        j["current_week"] = mCurrentWeek;
        file << j.dump(2) << "\n";
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace gacha_mod
