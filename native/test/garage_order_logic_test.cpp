// SPDX-License-Identifier: MIT
#include "../src/garage_order_logic.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace garage_order;

void test_season2_reorders_by_previous_constructor_standings()
{
    // 10 F1 teams + 1 My Team (stat 488, kind 1) in session entrants
    std::vector<uint32_t> session_entrants = {
        make_team_key(1),       make_team_key(1),       // McLaren
        make_team_key(2),       make_team_key(2),       // Ferrari
        make_team_key(3),       make_team_key(3),       // Red Bull
        make_team_key(4),       make_team_key(4),       // Mercedes
        make_team_key(488, 1),  make_team_key(488, 1),  // My Team
    };

    // Stock order from Season 1 entrant list (1, 2, 3, 4)
    std::vector<uint32_t> stock_order = {
        make_team_key(1),
        make_team_key(2),
        make_team_key(3),
        make_team_key(4),
    };

    // Previous season constructor standings: Ferrari (2) P1, My Team (488) P2, Mercedes (4) P3, McLaren (1) P4, Red Bull (3) P5
    std::vector<uint32_t> prev_standings = {
        make_team_key(2),
        make_team_key(488, 0), // even if standings key has kind 0, session key kind 1 must be preserved!
        make_team_key(4),
        make_team_key(1),
        make_team_key(3),
    };

    auto result = build_team_order(session_entrants, prev_standings, stock_order, 11, false);
    assert(result.size() == 5);
    assert(result[0] == make_team_key(2));
    assert(result[1] == make_team_key(488, 1)); // preserved session key kind 1
    assert(result[2] == make_team_key(4));
    assert(result[3] == make_team_key(1));
    assert(result[4] == make_team_key(3));
}

void test_garage_count_clamping_prevents_oob()
{
    std::vector<uint32_t> session_entrants = {
        make_team_key(1), make_team_key(2), make_team_key(3), make_team_key(4),
    };
    std::vector<uint32_t> prev_standings = {
        make_team_key(4), make_team_key(3), make_team_key(2), make_team_key(1),
    };
    auto result = build_team_order(session_entrants, prev_standings, {}, 3, false);
    assert(result.size() == 3);
    assert(result[0] == make_team_key(4));
    assert(result[1] == make_team_key(3));
    assert(result[2] == make_team_key(2));
}

void test_reverse_order()
{
    std::vector<uint32_t> session_entrants = {
        make_team_key(10), make_team_key(20), make_team_key(30),
    };
    std::vector<uint32_t> prev_standings = {
        make_team_key(10), make_team_key(20), make_team_key(30),
    };
    auto result = build_team_order(session_entrants, prev_standings, {}, 10, true);
    assert(result.size() == 3);
    assert(result[0] == make_team_key(30));
    assert(result[1] == make_team_key(20));
    assert(result[2] == make_team_key(10));
}

void test_random_order()
{
    std::vector<uint32_t> session_entrants = {
        make_team_key(1), make_team_key(2), make_team_key(3),
        make_team_key(4), make_team_key(5), make_team_key(6),
    };
    std::vector<uint32_t> prev_standings = {
        make_team_key(1), make_team_key(2), make_team_key(3),
        make_team_key(4), make_team_key(5), make_team_key(6),
    };
    auto result = build_team_order(session_entrants, prev_standings, {}, 10, false, 0x1337c0de);
    assert(result.size() == 6);
    // Should be shuffled and not strictly equal to ordered
    bool different = false;
    for (size_t i = 0; i < 6; ++i) {
        if (result[i] != prev_standings[i]) {
            different = true;
            break;
        }
    }
    assert(different);
}

void test_realistic_constructor_baseline()
{
    auto base = realistic_constructor_baseline_order();
    assert(base.size() == 12);
    assert(base[0] == make_team_key(1));  // McLaren in G1
    assert(base[1] == make_team_key(2));  // Ferrari in G2
    assert(base[2] == make_team_key(3));  // Red Bull in G3
    assert(base[3] == make_team_key(4));  // Mercedes in G4
    assert(base[4] == make_team_key(5));  // Aston Martin in G5
    assert(base[5] == make_team_key(6));  // Alpine in G6
    assert(base[6] == make_team_key(10)); // Haas in G7
    assert(base[7] == make_team_key(8));  // RB in G8
    assert(base[8] == make_team_key(7));  // Williams in G9
    assert(base[9] == make_team_key(9));  // Audi/Sauber in G10
    assert(base[10] == make_team_key(11)); // Cadillac in G11
    assert(base[11] == make_team_key(488, 1)); // My Team in G12
}

void test_sort_team_standings_tiebreakers()
{
    std::vector<TeamStanding> teams = {
        {.team_key = make_team_key(1), .stat_id = 1, .points = 400, .wins = 3, .best_finish = 0},
        {.team_key = make_team_key(2), .stat_id = 2, .points = 400, .wins = 5, .best_finish = 0},
        {.team_key = make_team_key(3), .stat_id = 3, .points = 100, .wins = 0, .best_finish = 2},
        {.team_key = make_team_key(4), .stat_id = 4, .points = 100, .wins = 0, .best_finish = 1},
    };
    auto sorted = sort_team_standings(teams);
    assert(sorted.size() == 4);
    assert(sorted[0] == make_team_key(2)); // 400 pts, 5 wins
    assert(sorted[1] == make_team_key(1)); // 400 pts, 3 wins
    assert(sorted[2] == make_team_key(4)); // 100 pts, 0 wins, best finish P2 (1)
    assert(sorted[3] == make_team_key(3)); // 100 pts, 0 wins, best finish P3 (2)
}

void test_team_id_0_is_placed_on_2025_data()
{
    // Team 0 is a valid team in 2025 data (stat_id 0)
    assert(valid_team_key(make_team_key(0)));
    assert(valid_team_key(0));
    assert(!valid_team_key(0x3fff0000u)); // kNoTeamStat
    assert(!valid_team_key(0xffff0000u));

    std::vector<uint32_t> session_entrants = {
        make_team_key(0), make_team_key(0), // Team 0
        make_team_key(1), make_team_key(1), // Team 1
        make_team_key(2), make_team_key(2), // Team 2
    };

    std::vector<uint32_t> prev_standings = {
        make_team_key(1),
        make_team_key(0),
        make_team_key(2),
    };

    auto result = build_team_order(session_entrants, prev_standings, {}, 10, false);
    assert(result.size() == 3);
    assert(result[0] == make_team_key(1));
    assert(result[1] == make_team_key(0)); // Team 0 is placed in G2
    assert(result[2] == make_team_key(2));
}

void test_my_team_and_team_0_coexist()
{
    // Team 0 (stat 0, kind 0) and My Team (stat 0, kind 1)
    uint32_t team0 = make_team_key(0, 0);
    uint32_t my_team = make_team_key(0, 1);
    assert(!same_team(team0, my_team));

    std::vector<uint32_t> session_entrants = {
        team0, team0,
        make_team_key(1), make_team_key(1),
        my_team, my_team,
    };

    std::vector<uint32_t> prev_standings = {
        make_team_key(1),
        team0,
    };

    auto result = build_team_order(session_entrants, prev_standings, {}, 12, false);
    assert(result.size() == 3);
    assert(result[0] == make_team_key(1));
    assert(result[1] == team0);
    assert(result[2] == my_team); // My Team appended as 3rd team!
}

void test_my_team_season_1_appended_to_11_ai_constructors()
{
    // 11 AI teams (2026 grid) + 1 My Team on track (12 garages)
    std::vector<uint32_t> session_entrants;
    for (uint16_t t = 1; t <= 11; ++t) {
        session_entrants.push_back(make_team_key(t));
        session_entrants.push_back(make_team_key(t));
    }
    uint32_t my_team = make_team_key(488, 1);
    session_entrants.push_back(my_team);
    session_entrants.push_back(my_team);

    // Season 1 starting order only lists the 11 AI teams
    std::vector<uint32_t> s1_ai_order;
    for (uint16_t t = 1; t <= 11; ++t) {
        s1_ai_order.push_back(make_team_key(t));
    }

    auto result = build_team_order(session_entrants, s1_ai_order, {}, 12, false);
    assert(result.size() == 12);
    for (size_t i = 0; i < 11; ++i) {
        assert(result[i] == make_team_key(static_cast<uint16_t>(i + 1)));
    }
    assert(result[11] == my_team); // My Team assigned Garage 12!
}

int main()
{
    test_season2_reorders_by_previous_constructor_standings();
    test_garage_count_clamping_prevents_oob();
    test_reverse_order();
    test_random_order();
    test_realistic_constructor_baseline();
    test_sort_team_standings_tiebreakers();
    test_team_id_0_is_placed_on_2025_data();
    test_my_team_and_team_0_coexist();
    test_my_team_season_1_appended_to_11_ai_constructors();
    std::printf("All pitlane garage order logic tests passed.\n");
    return 0;
}
