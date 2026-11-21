// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace garage_order {

constexpr uint16_t kNoTeamStat = 0x3fff;

constexpr uint16_t team_stat_of(uint32_t key)    { return static_cast<uint16_t>(key >> 16); }
constexpr uint8_t  team_kind_of(uint32_t key)    { return static_cast<uint8_t>(key & 0xff); }
constexpr bool     same_team(uint32_t a, uint32_t b) { return ((a ^ b) & 0xffff00ffu) == 0; }

constexpr bool valid_team_key(uint32_t key)
{
    uint16_t st = team_stat_of(key);
    return st != kNoTeamStat && st != 0xffff;
}

constexpr uint32_t make_team_key(uint16_t stat_id, uint8_t kind = 0)
{
    return (static_cast<uint32_t>(stat_id) << 16) | kind;
}

// Realistic 2024 / 2025 Constructor Championship baseline order:
// P1 McLaren (1), P2 Ferrari (2), P3 Red Bull (3), P4 Mercedes (4),
// P5 Aston Martin (5), P6 Alpine (6), P7 Haas (10), P8 RB (8),
// P9 Williams (7), P10 Audi / Sauber (9), P11 Cadillac (11), P12 My Team (488, kind 1).
inline std::vector<uint32_t> realistic_constructor_baseline_order()
{
    return {
        make_team_key(1), make_team_key(2), make_team_key(3), make_team_key(4),
        make_team_key(5), make_team_key(6), make_team_key(10), make_team_key(8),
        make_team_key(7), make_team_key(9), make_team_key(11), make_team_key(488, 1)
    };
}

inline const char *fallback_team_name(uint16_t stat_id, uint8_t kind)
{
    if (kind == 1 || stat_id == 488)
        return "My Team";
    static constexpr const char *kNames[] = {
        "McLaren", "Ferrari", "Red Bull", "Mercedes", "Aston Martin",
        "Alpine", "Williams", "RB", "Audi", "Haas", "Cadillac"
    };
    return (stat_id >= 1 && stat_id <= 11) ? kNames[stat_id - 1] : "Constructor";
}

struct TeamStanding {
    uint32_t team_key = 0;
    uint16_t stat_id = 0;
    int32_t points = 0;
    int32_t wins = 0;
    int32_t best_finish = 999;
};

// Sorts constructor standings: highest points first, then most race wins, then best race finish
// (0-based position, lower is better), then lowest stat_id for deterministic tie-breaking.
inline std::vector<uint32_t> sort_team_standings(std::vector<TeamStanding> teams)
{
    std::sort(teams.begin(), teams.end(), [](const TeamStanding &a, const TeamStanding &b) {
        if (a.points != b.points) return a.points > b.points;
        if (a.wins != b.wins)     return a.wins > b.wins;
        if (a.best_finish != b.best_finish) return a.best_finish < b.best_finish;
        return a.stat_id < b.stat_id;
    });
    std::vector<uint32_t> out;
    out.reserve(teams.size());
    for (const auto &t : teams) {
        if (valid_team_key(t.team_key))
            out.push_back(t.team_key);
    }
    return out;
}

// Builds the final team_order vector of packed u32 team keys to hand to Sess_Assign_GarageSlotsByTeamOrder.
inline std::vector<uint32_t> build_team_order(std::span<const uint32_t> session_team_keys,
                                              std::span<const uint32_t> prev_standings_keys,
                                              std::span<const uint32_t> stock_team_order,
                                              size_t garage_count,
                                              bool reverse_order = false)
{
    if (garage_count == 0 || session_team_keys.empty())
        return {};

    // Deduplicate active teams in the session, preserving each team's exact packed key from EntrantRecords.
    std::vector<uint32_t> active_keys;
    for (uint32_t k : session_team_keys) {
        if (valid_team_key(k) && std::ranges::none_of(active_keys, [k](uint32_t ex) { return same_team(ex, k); })) {
            active_keys.push_back(k);
        }
    }
    if (active_keys.empty())
        return {};

    auto find_active_key = [&](uint32_t candidate, uint32_t &out) -> bool {
        for (uint32_t k : active_keys) {
            if (same_team(k, candidate)) { out = k; return true; }
        }
        uint16_t cand_stat = team_stat_of(candidate);
        auto it = std::ranges::find_if(active_keys, [cand_stat](uint32_t k) { return team_stat_of(k) == cand_stat; });
        if (it != active_keys.end() && std::count_if(active_keys.begin(), active_keys.end(),
            [cand_stat](uint32_t k) { return team_stat_of(k) == cand_stat; }) == 1) {
            out = *it;
            return true;
        }
        return false;
    };

    std::vector<uint32_t> ordered;
    ordered.reserve(active_keys.size());

    auto append_if_active = [&](uint32_t candidate) {
        uint32_t key = 0;
        if (valid_team_key(candidate) && find_active_key(candidate, key)) {
            if (std::ranges::none_of(ordered, [key](uint32_t o) { return same_team(o, key); })) {
                ordered.push_back(key);
            }
        }
    };

    // 1. Prev standings, 2. stock fallback, 3. remaining active session teams
    for (auto list : {prev_standings_keys, stock_team_order, std::span<const uint32_t>(active_keys)}) {
        for (uint32_t k : list)
            append_if_active(k);
    }

    if (reverse_order)
        std::reverse(ordered.begin(), ordered.end());

    if (ordered.size() > garage_count)
        ordered.resize(garage_count);

    return ordered;
}

} // namespace garage_order
