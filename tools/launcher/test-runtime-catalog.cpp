// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe/ui/runtime_catalog.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace pe::ui;
using Json = nlohmann::json;

static Json original_snapshot(const std::vector<Game>& games, int generation, bool scanning) {
    auto rows = Json::array();
    for (const auto& game : games) {
        Json row = game;
        char title[17]{};
        std::snprintf(title, sizeof(title), "%016llx", static_cast<unsigned long long>(game.title_id));
        row["title_id"] = title;
        row["handle"] = game.file;
        row.erase("cover");
        rows.push_back(row);
    }
    return {{"games", rows}, {"generation", generation}, {"scanning", scanning}};
}

int main() {
    RuntimeCatalog catalog;
    assert(catalog.empty() && catalog.find("missing") == nullptr);
    assert(catalog.snapshot(true) == original_snapshot({}, 0, true));
    assert(!catalog.snapshot(false).at("scanning"));
    std::vector<Game> games(8193);
    for (std::size_t i = 0; i < games.size(); ++i) {
        games[i].name = "Example \xc3\xa9 " + std::to_string(i);
        games[i].file = "game-" + std::to_string(i) + ".xci";
        games[i].title_id = 0x0100000000000000ULL + i;
        games[i].cover = "cover-" + std::to_string(i);
    }
    catalog.replace(games);
    games.resize(8192);
    assert(catalog.snapshot(false) == original_snapshot(games, 1, false));
    assert(catalog.find("game-8192.xci") == nullptr);
    for (const auto& game : games) {
        const auto* found = catalog.find(game.file);
        assert(found && found->name == game.name && found->cover == game.cover);
    }
    const auto* snapshot = &catalog.snapshot(false);
    assert(&catalog.snapshot(true) == snapshot && snapshot->at("scanning"));
    auto copy = catalog.snapshot(false);
    copy["games"][0]["name"] = "changed";
    assert(catalog.snapshot(false).at("games")[0].at("name") == games[0].name);
    catalog.release_cached_views();
    assert(catalog.snapshot(false) == original_snapshot(games, 1, false));
    assert(catalog.find(games.back().file)->name == games.back().name);
    auto duplicate = games[0];
    duplicate.name = "Duplicate";
    catalog.replace({games[0], duplicate});
    assert(catalog.find(games[0].file)->name == games[0].name);
    assert(catalog.find(games[1].file) == nullptr);
    assert(catalog.snapshot(false).at("generation") == 2);
    catalog.replace({});
    assert(catalog.empty() && catalog.find(games[0].file) == nullptr);
    assert(catalog.snapshot(false) == original_snapshot({}, 3, false));

    // Measure repeated host-side queries, excluding the unchanged Lua table conversion.
    // This is a microbenchmark, not a prediction of console frame rate.
    for (int count : {64, 512, 8192}) {
        const std::vector<Game> sample(games.begin(), games.begin() + count);
        catalog.replace(sample);
        catalog.snapshot(false);
        const int iterations = 20;
        std::size_t observed = 0;
        const auto before = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i)
            observed += original_snapshot(sample, 1, false).at("games").size();
        const auto middle = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i)
            observed += catalog.snapshot(false).at("games").size();
        const auto after = std::chrono::steady_clock::now();
        assert(observed == static_cast<std::size_t>(count * iterations * 2));
        const auto micros = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
        std::cout << "catalog games=" << count << " original_us=" << micros(middle-before)/iterations
                  << " cached_us=" << micros(after-middle)/iterations << '\n';
    }
    std::cout << "Catalog serialization, refresh, duplicate and confinement checks passed\n";
}
