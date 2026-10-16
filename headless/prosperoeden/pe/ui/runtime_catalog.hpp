// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pe/ui/runtime_models.hpp"
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace pe::ui {

// Rebuilt only when an asynchronous library scan finishes. Lua receives a fresh
// table on every query; the cached host snapshot cannot be modified by a pack.
class RuntimeCatalog {
public:
    void replace(std::vector<Game> games) {
        if (games.size() > 8192) games.resize(8192);
        games_ = std::move(games);
        ++generation_;
        release_cached_views();
    }

    void release_cached_views() {
        index_.clear();
        index_.rehash(0);
        index_valid_ = false;
        snapshot_ = nullptr;
    }

    bool empty() const { return games_.empty(); }

    Game* find(const std::string& file) {
        if (!index_valid_) {
            std::unordered_map<std::string, std::size_t> index;
            index.reserve(games_.size());
            for (std::size_t i = 0; i < games_.size(); ++i)
                index.try_emplace(games_[i].file, i); // Preserve the first duplicate.
            index_ = std::move(index);
            index_valid_ = true;
        }
        const auto found = index_.find(file);
        return found == index_.end() ? nullptr : &games_[found->second];
    }

    const nlohmann::json& snapshot(bool scanning) {
        if (snapshot_.is_null()) {
            auto rows = nlohmann::json::array();
            auto& array = rows.get_ref<nlohmann::json::array_t&>();
            array.reserve(games_.size());
            for (const auto& game : games_) {
                nlohmann::json row = game;
                char title[17]{};
                std::snprintf(title, sizeof(title), "%016llx",
                              static_cast<unsigned long long>(game.title_id));
                row["title_id"] = title;
                row["handle"] = game.file;
                row.erase("cover");
                rows.push_back(std::move(row));
            }
            snapshot_ = {{"games", std::move(rows)}, {"generation", generation_}, {"scanning", scanning}};
        } else {
            snapshot_["scanning"] = scanning;
        }
        return snapshot_;
    }

private:
    std::vector<Game> games_;
    std::unordered_map<std::string, std::size_t> index_;
    nlohmann::json snapshot_;
    int generation_{};
    bool index_valid_{};
};

} // namespace pe::ui
