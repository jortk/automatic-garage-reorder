// SPDX-License-Identifier: MIT
//
// Dynamic Pitlane Garage Order (pitlane_garage_order.dll)
//
// Reorders the pitlane garages in Career Mode (Driver Career and My Team) according to the
// previous season's Constructors' Championship results. Also supports Grand Prix mode.
//
// Why vanilla Career Mode keeps Season 1's garage order:
//   When a session loads, Sess_Assign_GarageSlotsByClass (0x1424a8cf0) reads the static database
//   EntrantList at mgr+0xe18 (set once when the career was created) and passes its static
//   previous_season_team_standings and fixed_garage_assignments vectors to
//   Sess_Assign_GarageSlotsByTeamOrder (0x1424a8830).
//
// How Career Mode stores the previous season's constructor standings:
//   1. When a season starts or loads, Career_Rebuild_ChampionshipStandings (0x140ae78f0) builds
//      two order lists via 0x140ae2d80(championship, season_index, &order_a, &order_b) and passes
//      them to Career_Rebuild_SeasonStandings (0x140ae8040). For season_index > 0, order_b is built
//      from the previous Season's final sorted constructors' standings (prev_season+0x3f8..+0x400),
//      and stored at current_season+0x490..+0x498 as 8-byte {u32 team_key, u32 0} records once
//      weekend[0] has sessions.
//   2. We also wrap Career_Rebuild_SeasonStandings at priority -20 (after Progressive Career's
//      priority 0 wrap) to cache order_b per Season* even when weekend[0] has no sessions yet.
//   3. As a third fallback for season_index > 0, we compute the previous Season's constructor
//      standings directly from prev_season+0x2a8 (F1SeasonStandings points & results).
//
// When Sess_Assign_GarageSlotsByTeamOrder runs on session load:
//   We collect the active teams in the session from the `entrants` vector, order them by the
//   previous season's constructor standings (or realistic 2024/2026 constructors order in Season 1
//   and Grand Prix mode), clamp the list to the world's `garages` vector length (so Loop 1 of
//   Sess_Assign_GarageSlotsByTeamOrder never indexes past the garage vector), and pass an empty
//   `fixed_teams` vector so `fixed_garage_assignments` (such as f1_custom_team_2026's
//   f1_teams_2024) never locks garages 1..10. GarageModelComponent::onGaragesAssigned (0x142a9e9d0)
//   and Game_GarageCrewModule_SpawnGarageCrew then automatically dress each garage and spawn its
//   crew for the assigned team.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <safetyhook.hpp>
#include <swofty/career.hpp>
#include <swofty/events.hpp>
#include <swofty/game.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>

#include "garage_order_logic.hpp"

SWOFTY_SETTING(std::string_view, garage_order_mode, "standings");
SWOFTY_SETTING(bool, apply_in_season_1, true);
SWOFTY_SETTING(bool, apply_in_grand_prix, true);
SWOFTY_SETTING(bool, reverse_order, false);
SWOFTY_SETTING(bool, verbose, true);

namespace {

using namespace swofty::game;
using namespace garage_order;

// Standard game vector layout {NeAllocator *alloc, T *begin, T *end, size_t cap}.
template <class T> struct GameVec {
    void *alloc = nullptr;
    const T *begin = nullptr;
    const T *end = nullptr;
    size_t cap = 0;
};

// Season+0x490..+0x498 holds the constructors' initial order {u32 team_key, u32 0} populated from order_b.
constexpr uintptr_t kSeasonConstructorsInitialBegin = 0x490;
constexpr uintptr_t kSeasonConstructorsInitialEnd = 0x498;
constexpr size_t kConstructorsInitialStride = 8;

// Season+0x3b8 is the constructors' standings sub-object; +0x3f8..+0x400 is its sorted rounds vector.
constexpr size_t kMaxSeasons = 64;
constexpr size_t kMaxRounds = 64;
constexpr size_t kMaxSessions = 16;
constexpr size_t kMaxRecs = 128;
constexpr size_t kMaxEntrants = 128;
constexpr size_t kMaxGarages = 64;
constexpr int32_t kMaxPos = 28;

std::mutex g_mu;
std::map<uintptr_t, std::vector<uint32_t>> g_cached_order_b;

template <class T> bool rd(uintptr_t addr, T &out)
{
    return swofty::mem::read(addr, out);
}

template <class T> bool rd_vec(uintptr_t vec_Base, size_t max_elems, uintptr_t &begin_out, size_t &count_out)
{
    uintptr_t b = 0, e = 0;
    if (!rd(vec_Base, b) || !rd(vec_Base + sizeof(void *), e))
        return false;
    if (!b && !e) {
        begin_out = 0;
        count_out = 0;
        return true;
    }
    if (!b || e < b || (e - b) % sizeof(T) != 0)
        return false;
    size_t n = (e - b) / sizeof(T);
    if (n > max_elems || (n > 0 && !swofty::mem::readable(b, e - b)))
        return false;
    begin_out = b;
    count_out = n;
    return true;
}

bool has_vtable(uintptr_t obj, const void *expected_vt)
{
    uintptr_t vt = 0;
    return obj && expected_vt && swofty::mem::object_like(obj) && rd(obj, vt) &&
           vt == reinterpret_cast<uintptr_t>(expected_vt);
}

uintptr_t find_career_entity()
{
    auto e = reinterpret_cast<uintptr_t>(swofty::career::entity());
    if (!e || !swofty::mem::object_like(e))
        return 0;
    uintptr_t reg_vt = 0;
    if (!rd(e + offsetof(CareerEntity, component_registry), reg_vt) || !swofty::mem::in_image(reg_vt))
        return 0;
    return e;
}

uintptr_t find_championship(uintptr_t entity)
{
    if (!entity)
        return 0;
    const void *want_vt = vt_Career_Championship.get();
    if (!want_vt)
        return 0;

    uintptr_t current = 0;
    if (rd(entity + offsetof(CareerEntity, current_championship), current) && has_vtable(current, want_vt))
        return current;

    if (const void *desc = k_Career_ChampionshipDesc.get()) {
        auto comp = reinterpret_cast<uintptr_t>(swofty::career::component_by_desc(desc));
        if (has_vtable(comp, want_vt))
            return comp;
    }
    return 0;
}

struct CareerSeasonContext {
    uintptr_t entity = 0;
    uintptr_t championship = 0;
    uint32_t season_index = 0;
    uintptr_t current_season = 0;
    uintptr_t prev_season = 0;
};

bool locate_career_season(CareerSeasonContext &ctx)
{
    ctx = {};

    swofty::career::State st{};
    if (swofty::career::state(&st) != swofty::career::Result::ok || !st.loaded)
        return false;
    auto m = swofty::career::mode(st);
    if (m != swofty::career::Mode::my_team && m != swofty::career::Mode::driver_career)
        return false;

    ctx.entity = find_career_entity();
    if (!ctx.entity)
        return false;
    ctx.championship = find_championship(ctx.entity);
    if (!ctx.championship)
        return false;

    uintptr_t sb = 0;
    size_t sn = 0;
    if (!rd_vec<ChampionshipSeasonEntry>(ctx.championship + offsetof(Championship, seasons_begin), kMaxSeasons, sb, sn) ||
        sn == 0)
        return false;

    uint32_t idx = 0;
    if (!rd(ctx.championship + offsetof(Championship, season_index), idx) || idx >= sn)
        return false;

    const void *season_vt = vt_Career_Season.get();
    uintptr_t cur = 0;
    if (!rd(sb + idx * sizeof(ChampionshipSeasonEntry) + offsetof(ChampionshipSeasonEntry, season), cur) ||
        !has_vtable(cur, season_vt))
        return false;

    ctx.season_index = idx;
    ctx.current_season = cur;

    if (idx > 0) {
        uintptr_t prev = 0;
        if (rd(sb + (idx - 1) * sizeof(ChampionshipSeasonEntry) + offsetof(ChampionshipSeasonEntry, season), prev) &&
            has_vtable(prev, season_vt)) {
            ctx.prev_season = prev;
        }
    }
    return true;
}

// Source 1: Read the Season's constructors initial order list at season + 0x490..+0x498 ({u32 team_key, u32 0}).
std::vector<uint32_t> read_season_constructors_initial(uintptr_t season)
{
    std::vector<uint32_t> out;
    uintptr_t b = 0, e = 0;
    if (!season || !rd(season + kSeasonConstructorsInitialBegin, b) || !rd(season + kSeasonConstructorsInitialEnd, e))
        return out;
    if (!b || e <= b || (e - b) % kConstructorsInitialStride != 0)
        return out;
    size_t n = (e - b) / kConstructorsInitialStride;
    if (n > kMaxEntrants || !swofty::mem::readable(b, e - b))
        return out;
    for (size_t i = 0; i < n; ++i) {
        uint32_t key = 0;
        if (rd(b + i * kConstructorsInitialStride, key) && valid_team_key(key))
            out.push_back(key);
    }
    return out;
}

// Source 2: Read cached order_b captured when Career_Rebuild_SeasonStandings ran for this Season*.
std::vector<uint32_t> read_cached_order_b(uintptr_t season)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_cached_order_b.find(season);
    return it != g_cached_order_b.end() ? it->second : std::vector<uint32_t>{};
}

// Source 3: Compute constructor standings directly from a completed Season's F1SeasonStandings (season + 0x2a8).
std::vector<uint32_t> compute_season_constructor_standings(uintptr_t season, uintptr_t entity)
{
    if (!season)
        return {};

    // Map driver_stat_id -> team_key using the season's last weekend with session participants,
    // plus fallback to CareerEntity.entrants_begin..entrants_end.
    std::map<uint16_t, uint32_t> driver_team;

    if (entity) {
        uintptr_t eb = 0;
        size_t en = 0;
        if (rd_vec<EntrantRecord>(entity + offsetof(CareerEntity, entrants_begin), kMaxEntrants, eb, en)) {
            for (size_t i = 0; i < en; ++i) {
                uintptr_t rec = eb + i * sizeof(EntrantRecord);
                uint32_t dkey = 0, tkey = 0;
                if (rd(rec + offsetof(EntrantRecord, id_lo), dkey) &&
                    rd(rec + offsetof(EntrantRecord, team_stat_id), tkey) && valid_team_key(tkey)) {
                    driver_team[team_stat_of(dkey)] = tkey;
                }
            }
        }
    }

    uintptr_t wb = 0;
    size_t wn = 0;
    if (rd_vec<Weekend *>(season + offsetof(Season, weekends_begin), kMaxRounds, wb, wn)) {
        for (size_t r = 0; r < wn; ++r) {
            uintptr_t wk = 0;
            if (!rd(wb + r * sizeof(Weekend *), wk) || !wk)
                continue;
            uintptr_t sb = 0;
            size_t sn = 0;
            if (!rd_vec<Session>(wk + offsetof(Weekend, sessions_begin), kMaxSessions, sb, sn))
                continue;
            for (size_t s = 0; s < sn; ++s) {
                uintptr_t sess = sb + s * sizeof(Session);
                uintptr_t pb = 0;
                size_t pn = 0;
                if (!rd_vec<EntrantRecord>(sess + offsetof(Session, participants_begin), kMaxEntrants, pb, pn))
                    continue;
                for (size_t p = 0; p < pn; ++p) {
                    uintptr_t rec = pb + p * sizeof(EntrantRecord);
                    uint32_t dkey = 0, tkey = 0;
                    if (rd(rec + offsetof(EntrantRecord, id_lo), dkey) &&
                        rd(rec + offsetof(EntrantRecord, team_stat_id), tkey) && valid_team_key(tkey)) {
                        driver_team[team_stat_of(dkey)] = tkey;
                    }
                }
            }
        }
    }

    uintptr_t st = season + offsetof(Season, standings_vtable);

    struct DriverStats {
        int32_t points = 0;
        int32_t wins = 0;
        int32_t best_pos = kMaxPos;
    };
    std::map<uint16_t, DriverStats> drivers;

    // Walk results tree (st + offsetof(F1SeasonStandings, results_begin))
    uintptr_t r_rb = 0;
    size_t r_rn = 0;
    if (rd_vec<StandingsRound>(st + offsetof(F1SeasonStandings, results_begin), kMaxRounds, r_rb, r_rn)) {
        for (size_t r = 0; r < r_rn; ++r) {
            uintptr_t sb = 0;
            size_t sn = 0;
            if (!rd_vec<StandingsSession>(r_rb + r * sizeof(StandingsRound) + offsetof(StandingsRound, sessions_begin),
                                          kMaxSessions, sb, sn) ||
                sn == 0)
                continue;
            uintptr_t main_sess = sb + (sn - 1) * sizeof(StandingsSession);
            uintptr_t cb = 0;
            size_t cn = 0;
            if (!rd_vec<StandingsResult>(main_sess + offsetof(StandingsSession, records_begin), kMaxRecs, cb, cn))
                continue;
            for (size_t c = 0; c < cn; ++c) {
                StandingsResult res{};
                if (!rd(cb + c * sizeof(StandingsResult), res))
                    continue;
                uint16_t dstat = team_stat_of(res.key);
                auto &d = drivers[dstat];
                if (res.position >= 0 && res.position < kMaxPos && res.status < 2) {
                    d.best_pos = std::min(d.best_pos, res.position);
                    if (res.position == 0)
                        d.wins++;
                }
            }
        }
    }

    // Walk points tree (st + offsetof(F1SeasonStandings, points_begin))
    uintptr_t p_rb = 0;
    size_t p_rn = 0;
    size_t total_point_records = 0;
    if (rd_vec<StandingsRound>(st + offsetof(F1SeasonStandings, points_begin), kMaxRounds, p_rb, p_rn)) {
        for (size_t r = 0; r < p_rn; ++r) {
            uintptr_t sb = 0;
            size_t sn = 0;
            if (!rd_vec<StandingsSession>(p_rb + r * sizeof(StandingsRound) + offsetof(StandingsRound, sessions_begin),
                                          kMaxSessions, sb, sn))
                continue;
            for (size_t s = 0; s < sn; ++s) {
                uintptr_t sess = sb + s * sizeof(Session);
                uintptr_t cb = 0;
                size_t cn = 0;
                if (!rd_vec<StandingsPoints>(sess + offsetof(StandingsSession, records_begin), kMaxRecs, cb, cn))
                    continue;
                for (size_t c = 0; c < cn; ++c) {
                    StandingsPoints pt{};
                    if (!rd(cb + c * sizeof(StandingsPoints), pt))
                        continue;
                    uint16_t dstat = team_stat_of(pt.key);
                    drivers[dstat].points += pt.points;
                    total_point_records++;
                }
            }
        }
    }

    if (total_point_records == 0 && drivers.empty())
        return {};

    std::map<uint16_t, TeamStanding> teams;
    for (const auto &[dstat, ds] : drivers) {
        auto it = driver_team.find(dstat);
        if (it == driver_team.end() || !valid_team_key(it->second))
            continue;
        uint16_t tstat = team_stat_of(it->second);
        auto &tm = teams[tstat];
        tm.team_key = it->second;
        tm.stat_id = tstat;
        tm.points += ds.points;
        tm.wins += ds.wins;
        tm.best_finish = std::min(tm.best_finish, ds.best_pos);
    }

    std::vector<TeamStanding> team_list;
    team_list.reserve(teams.size());
    for (const auto &[_, tm] : teams)
        team_list.push_back(tm);

    return sort_team_standings(std::move(team_list));
}

std::vector<uint32_t> resolve_previous_season_team_order(const CareerSeasonContext &ctx, const char *&source_out)
{
    source_out = "none";

    // 1. Check current_season + 0x490..+0x498 (populated by the game's 0x1413cf8a0 from order_b).
    std::vector<uint32_t> order = read_season_constructors_initial(ctx.current_season);
    if (!order.empty()) {
        source_out = "season+0x490 (constructors start order)";
        return order;
    }

    // 2. Check cached order_b captured during Career_Rebuild_SeasonStandings for current_season.
    order = read_cached_order_b(ctx.current_season);
    if (!order.empty()) {
        source_out = "Career_Rebuild_SeasonStandings order_b";
        return order;
    }

    // 3. For Season 2+ (season_index > 0), compute directly from prev_season's F1SeasonStandings.
    if (ctx.season_index > 0 && ctx.prev_season) {
        order = compute_season_constructor_standings(ctx.prev_season, ctx.entity);
        if (!order.empty()) {
            source_out = "previous season F1SeasonStandings";
            return order;
        }
    }

    return {};
}

std::string format_team_keys(std::span<const uint32_t> keys)
{
    std::string s;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0)
            s += ", ";
        uint16_t st = team_stat_of(keys[i]);
        uint8_t kind = team_kind_of(keys[i]);
        const char *name = fallback_team_name(st, kind);
        s += "G" + std::to_string(i + 1) + "=" + name;
        if (std::string_view(name) == "Constructor")
            s += "#" + std::to_string(st);
    }
    return s;
}

} // namespace

// Capture order_b (the constructors' starting order built by 0x140ae2d80 from the previous season's
// final constructor standings, or replaced by Progressive Career at priority 0) whenever a season's
// standings are rebuilt.
SWOFTY_WRAP_PRIORITY(-20, Career_Rebuild_SeasonStandings, void, void *season, void *order_a, void *order_b,
                     void *extra_points, void *extra_e)
{
    uintptr_t s = reinterpret_cast<uintptr_t>(season);
    if (s && order_b && has_vtable(s, vt_Career_Season.get())) {
        NeOptionalVec vec{};
        if (rd(reinterpret_cast<uintptr_t>(order_b), vec) && vec.engaged && vec.begin && vec.end > vec.begin) {
            auto b = reinterpret_cast<uintptr_t>(vec.begin);
            auto e = reinterpret_cast<uintptr_t>(vec.end);
            if ((e - b) % sizeof(uint32_t) == 0 && (e - b) / sizeof(uint32_t) <= kMaxEntrants &&
                swofty::mem::readable(b, e - b)) {
                std::vector<uint32_t> keys;
                size_t n = (e - b) / sizeof(uint32_t);
                keys.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    uint32_t k = 0;
                    if (rd(b + i * sizeof(uint32_t), k) && valid_team_key(k))
                        keys.push_back(k);
                }
                if (!keys.empty()) {
                    std::lock_guard<std::mutex> lock(g_mu);
                    g_cached_order_b[s] = std::move(keys);
                }
            }
        }
    }
    orig(season, order_a, order_b, extra_points, extra_e);
}

// Reorder team_order in Sess_Assign_GarageSlotsByTeamOrder during Career Mode and Grand Prix sessions.
SWOFTY_WRAP(Sess_Assign_GarageSlotsByTeamOrder, void, void *out_assignments, void *entrants, void *garages,
            void *team_order, void *fixed_teams)
{
    if (!out_assignments || !entrants || !garages || !team_order)
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    // 1. Read entrant pointers in this vehicle class.
    uintptr_t ent_b = 0;
    size_t ent_n = 0;
    if (!rd_vec<EntrantRecord *>(reinterpret_cast<uintptr_t>(entrants) + offsetof(GameVec<EntrantRecord *>, begin),
                                 kMaxEntrants, ent_b, ent_n) ||
        ent_n == 0) {
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);
    }

    // 2. Read the world's garage count.
    uintptr_t gar_b = 0;
    size_t garage_count = 0;
    if (!rd_vec<void *>(reinterpret_cast<uintptr_t>(garages) + offsetof(GameVec<void *>, begin), kMaxGarages, gar_b,
                        garage_count) ||
        garage_count == 0) {
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);
    }

    // 3. Check session mode (Career Mode or Grand Prix mode).
    CareerSeasonContext ctx;
    bool in_career = locate_career_season(ctx);
    if (!in_career && !apply_in_grand_prix)
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    OrderMode mode = parse_order_mode(garage_order_mode.get());
    bool is_reverse = (mode == OrderMode::Reverse) || reverse_order.get();
    uint32_t random_seed = (mode == OrderMode::Random) ? (0x1337c0de ^ static_cast<uint32_t>(ent_n * 0x9e3779b9u)) : 0;

    // 4. Collect the packed team keys of all entrants in the session.
    std::vector<uint32_t> session_teams;
    session_teams.reserve(ent_n);
    for (size_t i = 0; i < ent_n; ++i) {
        uintptr_t rec_ptr = 0;
        uint32_t tkey = 0;
        if (rd(ent_b + i * sizeof(EntrantRecord *), rec_ptr) && rec_ptr &&
            rd(rec_ptr + offsetof(EntrantRecord, team_stat_id), tkey) && valid_team_key(tkey)) {
            session_teams.push_back(tkey);
        }
    }
    if (session_teams.empty())
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    // Collect distinct active session teams for validation.
    std::vector<uint32_t> unique_session_teams;
    for (uint32_t k : session_teams) {
        bool exists = false;
        for (uint32_t u : unique_session_teams) {
            if (same_team(u, k)) {
                exists = true;
                break;
            }
        }
        if (!exists)
            unique_session_teams.push_back(k);
    }

    // 5. Read the stock team_order vector passed by Sess_Assign_GarageSlotsByClass.
    std::vector<uint32_t> stock_order;
    uintptr_t ord_b = 0;
    size_t ord_n = 0;
    if (rd_vec<uint32_t>(reinterpret_cast<uintptr_t>(team_order) + offsetof(GameVec<uint32_t>, begin), kMaxEntrants,
                         ord_b, ord_n)) {
        stock_order.reserve(ord_n);
        for (size_t i = 0; i < ord_n; ++i) {
            uint32_t k = 0;
            if (rd(ord_b + i * sizeof(uint32_t), k) && valid_team_key(k))
                stock_order.push_back(k);
        }
    }

    // 6. Resolve the constructor standings order.
    const char *source = "none";
    std::vector<uint32_t> prev_standings;

    if (in_career) {
        prev_standings = resolve_previous_season_team_order(ctx, source);
        if (prev_standings.empty() && ctx.season_index == 0 && apply_in_season_1) {
            prev_standings = realistic_constructor_baseline_order();
            source = "realistic 2024 constructors baseline (Season 1)";
        }
    } else {
        prev_standings = realistic_constructor_baseline_order();
        source = "Grand Prix (realistic constructor baseline)";
    }

    if (prev_standings.empty() && !is_reverse && random_seed == 0)
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    if (prev_standings.empty() && is_reverse)
        source = "stock team_order (reversed)";
    else if (random_seed != 0)
        source = "randomized";

    // 7. Build the new pitlane garage team order, capped to garage_count.
    std::vector<uint32_t> new_order =
        build_team_order(session_teams, prev_standings, stock_order, garage_count, is_reverse, random_seed);

    // Safety fallback: Every active session team up to the track's capacity MUST have a slot assigned.
    size_t expected_slots = std::min(unique_session_teams.size(), garage_count);
    if (new_order.size() < expected_slots) {
        swofty::log("[pitlane-garage-order] fallback to stock garage order: %zu active session team(s) vs %zu ordered slots (track garages: %zu)",
                    unique_session_teams.size(), new_order.size(), garage_count);
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);
    }

    void *alloc = nullptr;
    rd(reinterpret_cast<uintptr_t>(team_order), alloc);

    GameVec<uint32_t> custom_team_order{
        .alloc = alloc,
        .begin = new_order.data(),
        .end = new_order.data() + new_order.size(),
        .cap = new_order.size(),
    };
    GameVec<uint32_t> empty_fixed_teams{
        .alloc = alloc,
        .begin = nullptr,
        .end = nullptr,
        .cap = 0,
    };

    if (verbose) {
        std::string summary = format_team_keys(new_order);
        if (in_career) {
            swofty::log("[pitlane-garage-order] Career Season %u (%s%s, %zu garage(s) on track): %s",
                        ctx.season_index + 1, source, is_reverse ? ", reversed" : "", garage_count, summary.c_str());
        } else {
            swofty::log("[pitlane-garage-order] Grand Prix (%s%s, %zu garage(s) on track): %s",
                        source, is_reverse ? ", reversed" : "", garage_count, summary.c_str());
        }
    }

    orig(out_assignments, entrants, garages, &custom_team_order, &empty_fixed_teams);
}

SWOFTY_ON_LOAD
{
    swofty::log("[pitlane-garage-order] loaded (mode=%s, season_1=%s, grand_prix=%s, verbose=%s)",
                order_mode_name(parse_order_mode(garage_order_mode.get())),
                apply_in_season_1 ? "true" : "false",
                apply_in_grand_prix ? "true" : "false",
                verbose ? "true" : "false");
}

SWOFTY_ADDON()
