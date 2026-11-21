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

constexpr uint16_t team_stat_of(uint32_t key)
{
    return static_cast<uint16_t>(key >> 16);
}

constexpr uint8_t team_kind_of(uint32_t key)
{
    return static_cast<uint8_t>(key & 0xff);
}

constexpr bool same_team(uint32_t a, uint32_t b)
{
    return ((a ^ b) & 0xffff00ffu) == 0;
}

constexpr bool valid_team_key(uint32_t key)
{
    uint16_t st = team_stat_of(key);
    return st != kNoTeamStat && st != 0xffff;
}

constexpr uint32_t make_team_key(uint16_t stat_id, uint8_t kind = 0)
{
    return (static_cast<uint32_t>(stat_id) << 16) | kind;
}

enum class OrderMode : uint8_t {
    Standings = 0,
    Reverse = 1,
    Random = 2
};

inline OrderMode parse_order_mode(std::string_view s)
{
    if (s == "reverse" || s == "inverted")
        return OrderMode::Reverse;
    if (s == "random" || s == "randomized")
        return OrderMode::Random;
    return OrderMode::Standings;
}

inline const char *order_mode_name(OrderMode m)
{
    switch (m) {
        case OrderMode::Reverse: return "Inverted standings";
        case OrderMode::Random:  return "Randomized";
        case OrderMode::Standings:
        default:                 return "Championship standings";
    }
}

// Realistic 2024 / 2025 Constructor Championship baseline order:
// P1 McLaren (1), P2 Ferrari (2), P3 Red Bull (3), P4 Mercedes (4),
// P5 Aston Martin (5), P6 Alpine (6), P7 Haas (10), P8 RB (8),
// P9 Williams (7), P10 Audi / Sauber (9), P11 Cadillac (11), P12 My Team (488, kind 1).
inline std::vector<uint32_t> realistic_constructor_baseline_order()
{
    return {
        make_team_key(1),        // McLaren (2024 WCC Champions)
        make_team_key(2),        // Ferrari (P2)
        make_team_key(3),        // Red Bull Racing (P3)
        make_team_key(4),        // Mercedes (P4)
        make_team_key(5),        // Aston Martin (P5)
        make_team_key(6),        // Alpine (P6)
        make_team_key(10),       // Haas (P7)
        make_team_key(8),        // RB (P8)
        make_team_key(7),        // Williams (P9)
        make_team_key(9),        // Sauber / Audi (P10)
        make_team_key(11),       // Cadillac
        make_team_key(488, 1),   // My Team / Custom
    };
}

inline const char *fallback_team_name(uint16_t stat_id, uint8_t kind)
{
    if (kind == 1 || stat_id == 488)
        return "My Team";
    switch (stat_id) {
        case 1:  return "McLaren";
        case 2:  return "Ferrari";
        case 3:  return "Red Bull";
        case 4:  return "Mercedes";
        case 5:  return "Aston Martin";
        case 6:  return "Alpine";
        case 7:  return "Williams";
        case 8:  return "RB";
        case 9:  return "Audi";
        case 10: return "Haas";
        case 11: return "Cadillac";
        default: return "Constructor";
    }
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
        if (a.points != b.points)
            return a.points > b.points;
        if (a.wins != b.wins)
            return a.wins > b.wins;
        if (a.best_finish != b.best_finish)
            return a.best_finish < b.best_finish;
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

inline std::vector<uint32_t> shuffle_team_order(std::vector<uint32_t> order, uint32_t seed)
{
    for (size_t i = order.size(); i > 1; --i) {
        seed = ((seed >> 16) ^ seed) * 0x45d9f3bu;
        seed = ((seed >> 16) ^ seed) * 0x45d9f3bu;
        seed = (seed >> 16) ^ seed;
        size_t j = seed % i;
        std::swap(order[i - 1], order[j]);
    }
    return order;
}

// Builds the final team_order vector of packed u32 team keys to hand to Sess_Assign_GarageSlotsByTeamOrder.
//
// - session_team_keys: teams of the entrants participating in the current session (in first-seen order).
// - prev_standings_keys: constructor order from the previous season (P1 first, P2 second, ...).
// - stock_team_order: the stock team_order vector passed by Sess_Assign_GarageSlotsByClass (fallback for
//   any active team that was not in prev_standings_keys, e.g. an expansion team in its debut season).
// - garage_count: number of garages in the world's garage vector.
// - reverse_order: if true, inverts the final active team order.
// - random_seed: non-zero seed when OrderMode::Random is chosen.
inline std::vector<uint32_t> build_team_order(std::span<const uint32_t> session_team_keys,
                                              std::span<const uint32_t> prev_standings_keys,
                                              std::span<const uint32_t> stock_team_order,
                                              size_t garage_count,
                                              bool reverse_order = false,
                                              uint32_t random_seed = 0)
{
    if (garage_count == 0 || session_team_keys.empty())
        return {};

    // Deduplicate active teams in the session, preserving each team's exact packed u32 key from the session's
    // EntrantRecords so Sess_Group_EntrantsByTeam_Thunk matches every entry in Loop 1.
    std::vector<uint32_t> active_keys;
    auto find_active_key = [&](uint32_t candidate_key, uint32_t &out_key) -> bool {
        // 1. Check exact or same_team match (matches both stat_id in high word and kind in low byte)
        for (uint32_t k : active_keys) {
            if (same_team(k, candidate_key)) {
                out_key = k;
                return true;
            }
        }
        // 2. Disambiguated stat_id match (e.g. if standings key has kind 0 but session entrant has kind 1)
        uint16_t cand_stat = team_stat_of(candidate_key);
        uint32_t match = 0;
        size_t count = 0;
        for (uint32_t k : active_keys) {
            if (team_stat_of(k) == cand_stat) {
                match = k;
                ++count;
            }
        }
        if (count == 1) {
            out_key = match;
            return true;
        }
        return false;
    };

    for (uint32_t k : session_team_keys) {
        if (!valid_team_key(k))
            continue;
        bool exists = false;
        for (uint32_t existing : active_keys) {
            if (same_team(existing, k)) {
                exists = true;
                break;
            }
        }
        if (!exists)
            active_keys.push_back(k);
    }

    if (active_keys.empty())
        return {};

    std::vector<uint32_t> ordered;
    ordered.reserve(active_keys.size());

    auto append_if_active = [&](uint32_t candidate_key) {
        if (!valid_team_key(candidate_key))
            return;
        uint32_t session_key = 0;
        if (!find_active_key(candidate_key, session_key))
            return;
        for (uint32_t already : ordered) {
            if (same_team(already, session_key))
                return;
        }
        ordered.push_back(session_key);
    };

    // 1. Place active teams in constructor championship order.
    for (uint32_t k : prev_standings_keys)
        append_if_active(k);

    // 2. Append any remaining active teams (e.g. a new team in Season 1) in stock team_order.
    for (uint32_t k : stock_team_order)
        append_if_active(k);

    // 3. Append any remaining active teams from session entrants (guarantees every session team is placed).
    for (uint32_t k : active_keys)
        append_if_active(k);

    if (random_seed != 0) {
        ordered = shuffle_team_order(std::move(ordered), random_seed);
    } else if (reverse_order) {
        std::reverse(ordered.begin(), ordered.end());
    }

    if (ordered.size() > garage_count)
        ordered.resize(garage_count);

    return ordered;
}

} // namespace garage_order
