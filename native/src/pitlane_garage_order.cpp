// SPDX-License-Identifier: MIT
//
// Dynamic Pitlane Garage Order (pitlane_garage_order.dll)
//
// Reorders the pitlane garages in Career Mode (Driver Career and My Team) according to the
// previous season's Constructors' Championship results.

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

SWOFTY_SETTING(bool, enabled, true);
SWOFTY_SETTING(bool, apply_in_season_1, true);
SWOFTY_SETTING(bool, reverse_order, false);
SWOFTY_SETTING(bool, verbose, true);

namespace {

using namespace swofty::game;
using namespace garage_order;

template <class T> struct GameVec {
    void *alloc = nullptr;
    const T *begin = nullptr;
    const T *end = nullptr;
    size_t cap = 0;
};

template <class T> struct MemSpan {
    uintptr_t begin = 0;
    size_t count = 0;
    explicit operator bool() const { return count > 0; }
    uintptr_t at(size_t i) const   { return begin + i * sizeof(T); }
};

constexpr uintptr_t kSeasonConstructorsInitialBegin = 0x490;
constexpr uintptr_t kSeasonConstructorsInitialEnd   = 0x498;
constexpr size_t kConstructorsInitialStride = 8;

constexpr size_t kMaxSeasons  = 64;
constexpr size_t kMaxRounds   = 64;
constexpr size_t kMaxSessions = 16;
constexpr size_t kMaxRecs     = 128;
constexpr size_t kMaxEntrants = 128;
constexpr size_t kMaxGarages  = 64;
constexpr int32_t kMaxPos     = 28;

std::mutex g_mu;
std::map<uintptr_t, std::vector<uint32_t>> g_cached_order_b;

template <class T> bool rd(uintptr_t addr, T &out)
{
    return swofty::mem::read(addr, out);
}

template <class T> MemSpan<T> rd_vec(uintptr_t vec_base, size_t max_elems)
{
    uintptr_t b = 0, e = 0;
    if (!rd(vec_base, b) || !rd(vec_base + sizeof(void *), e) || !b || e < b || (e - b) % sizeof(T) != 0)
        return {};
    size_t n = (e - b) / sizeof(T);
    if (n > max_elems || (n > 0 && !swofty::mem::readable(b, e - b)))
        return {};
    return {b, n};
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
    return (rd(e + offsetof(CareerEntity, component_registry), reg_vt) && swofty::mem::in_image(reg_vt)) ? e : 0;
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
    ctx.championship = find_championship(ctx.entity);
    if (!ctx.entity || !ctx.championship)
        return false;

    auto seasons = rd_vec<ChampionshipSeasonEntry>(ctx.championship + offsetof(Championship, seasons_begin), kMaxSeasons);
    uint32_t idx = 0;
    if (!seasons || !rd(ctx.championship + offsetof(Championship, season_index), idx) || idx >= seasons.count)
        return false;

    const void *season_vt = vt_Career_Season.get();
    uintptr_t cur = 0;
    if (!rd(seasons.at(idx) + offsetof(ChampionshipSeasonEntry, season), cur) || !has_vtable(cur, season_vt))
        return false;

    ctx.season_index = idx;
    ctx.current_season = cur;

    if (idx > 0) {
        uintptr_t prev = 0;
        if (rd(seasons.at(idx - 1) + offsetof(ChampionshipSeasonEntry, season), prev) && has_vtable(prev, season_vt))
            ctx.prev_season = prev;
    }
    return true;
}

// Source 1: Read Season+0x490 ({u32 team_key, u32 0})
std::vector<uint32_t> read_season_constructors_initial(uintptr_t season)
{
    std::vector<uint32_t> out;
    uintptr_t b = 0, e = 0;
    if (!season || !rd(season + kSeasonConstructorsInitialBegin, b) || !rd(season + kSeasonConstructorsInitialEnd, e) ||
        !b || e <= b || (e - b) % kConstructorsInitialStride != 0)
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

// Source 2: Read cached order_b from Career_Rebuild_SeasonStandings
std::vector<uint32_t> read_cached_order_b(uintptr_t season)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_cached_order_b.find(season);
    return it != g_cached_order_b.end() ? it->second : std::vector<uint32_t>{};
}

// Source 3: Compute constructor standings directly from completed Season's F1SeasonStandings
std::vector<uint32_t> compute_season_constructor_standings(uintptr_t season, uintptr_t entity)
{
    if (!season)
        return {};

    std::map<uint16_t, uint32_t> driver_team;
    auto map_entrants = [&](MemSpan<EntrantRecord> span) {
        for (size_t i = 0; i < span.count; ++i) {
            uintptr_t rec = span.at(i);
            uint32_t dkey = 0, tkey = 0;
            if (rd(rec + offsetof(EntrantRecord, id_lo), dkey) &&
                rd(rec + offsetof(EntrantRecord, team_stat_id), tkey) && valid_team_key(tkey)) {
                driver_team[team_stat_of(dkey)] = tkey;
            }
        }
    };

    if (entity)
        map_entrants(rd_vec<EntrantRecord>(entity + offsetof(CareerEntity, entrants_begin), kMaxEntrants));

    if (driver_team.empty()) {
        auto weekends = rd_vec<Weekend *>(season + offsetof(Season, weekends_begin), kMaxRounds);
        for (size_t r = 0; r < weekends.count && driver_team.empty(); ++r) {
            uintptr_t wk = 0;
            if (rd(weekends.at(r), wk) && wk) {
                auto sessions = rd_vec<Session>(wk + offsetof(Weekend, sessions_begin), kMaxSessions);
                for (size_t s = 0; s < sessions.count; ++s)
                    map_entrants(rd_vec<EntrantRecord>(sessions.at(s) + offsetof(Session, participants_begin), kMaxEntrants));
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

    auto for_each_session = [&](uintptr_t tree_base, auto &&fn) {
        auto rounds = rd_vec<StandingsRound>(tree_base, kMaxRounds);
        for (size_t r = 0; r < rounds.count; ++r) {
            auto sessions = rd_vec<StandingsSession>(rounds.at(r) + offsetof(StandingsRound, sessions_begin), kMaxSessions);
            if (sessions) fn(sessions);
        }
    };

    // Results tree: check main race of each round
    for_each_session(st + offsetof(F1SeasonStandings, results_begin), [&](MemSpan<StandingsSession> sessions) {
        uintptr_t main_sess = sessions.at(sessions.count - 1);
        auto results = rd_vec<StandingsResult>(main_sess + offsetof(StandingsSession, records_begin), kMaxRecs);
        for (size_t c = 0; c < results.count; ++c) {
            StandingsResult res{};
            if (rd(results.at(c), res)) {
                auto &d = drivers[team_stat_of(res.key)];
                if (res.position >= 0 && res.position < kMaxPos && res.status < 2) {
                    d.best_pos = std::min(d.best_pos, res.position);
                    if (res.position == 0) d.wins++;
                }
            }
        }
    });

    // Points tree: sum points across all sessions
    size_t total_points = 0;
    for_each_session(st + offsetof(F1SeasonStandings, points_begin), [&](MemSpan<StandingsSession> sessions) {
        for (size_t s = 0; s < sessions.count; ++s) {
            auto points = rd_vec<StandingsPoints>(sessions.at(s) + offsetof(StandingsSession, records_begin), kMaxRecs);
            for (size_t c = 0; c < points.count; ++c) {
                StandingsPoints pt{};
                if (rd(points.at(c), pt)) {
                    drivers[team_stat_of(pt.key)].points += pt.points;
                    total_points++;
                }
            }
        }
    });

    if (total_points == 0 && drivers.empty())
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
    auto order = read_season_constructors_initial(ctx.current_season);
    if (!order.empty()) {
        source_out = "season+0x490 (constructors start order)";
        return order;
    }
    order = read_cached_order_b(ctx.current_season);
    if (!order.empty()) {
        source_out = "Career_Rebuild_SeasonStandings order_b";
        return order;
    }
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
        if (i > 0) s += ", ";
        uint16_t st = team_stat_of(keys[i]);
        const char *name = fallback_team_name(st, team_kind_of(keys[i]));
        s += "G" + std::to_string(i + 1) + "=" + name;
        if (std::string_view(name) == "Constructor")
            s += "#" + std::to_string(st);
    }
    return s;
}

} // namespace

SWOFTY_WRAP_PRIORITY(-20, Career_Rebuild_SeasonStandings, void, void *season, void *order_a, void *order_b,
                     void *extra_points, void *extra_e)
{
    uintptr_t s = reinterpret_cast<uintptr_t>(season);
    if (s && order_b && has_vtable(s, vt_Career_Season.get())) {
        NeOptionalVec vec{};
        if (rd(reinterpret_cast<uintptr_t>(order_b), vec) && vec.engaged && vec.begin && vec.end > vec.begin) {
            auto b = reinterpret_cast<uintptr_t>(vec.begin);
            auto e = reinterpret_cast<uintptr_t>(vec.end);
            size_t bytes = e - b;
            if (bytes % sizeof(uint32_t) == 0 && (bytes / sizeof(uint32_t)) <= kMaxEntrants && swofty::mem::readable(b, bytes)) {
                size_t n = bytes / sizeof(uint32_t);
                std::vector<uint32_t> keys;
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

SWOFTY_WRAP(Sess_Assign_GarageSlotsByTeamOrder, void, void *out_assignments, void *entrants, void *garages,
            void *team_order, void *fixed_teams)
{
    if (!enabled.get() || !out_assignments || !entrants || !garages || !team_order)
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    auto ent_span = rd_vec<EntrantRecord *>(reinterpret_cast<uintptr_t>(entrants) + offsetof(GameVec<EntrantRecord *>, begin), kMaxEntrants);
    auto gar_span = rd_vec<void *>(reinterpret_cast<uintptr_t>(garages) + offsetof(GameVec<void *>, begin), kMaxGarages);
    if (!ent_span || !gar_span)
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    CareerSeasonContext ctx;
    if (!locate_career_season(ctx))
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    bool is_reverse = reverse_order.get();

    std::vector<uint32_t> session_teams;
    session_teams.reserve(ent_span.count);
    for (size_t i = 0; i < ent_span.count; ++i) {
        uintptr_t rec_ptr = 0;
        uint32_t tkey = 0;
        if (rd(ent_span.at(i), rec_ptr) && rec_ptr && rd(rec_ptr + offsetof(EntrantRecord, team_stat_id), tkey) && valid_team_key(tkey))
            session_teams.push_back(tkey);
    }
    if (session_teams.empty())
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    std::vector<uint32_t> stock_order;
    if (auto ord_span = rd_vec<uint32_t>(reinterpret_cast<uintptr_t>(team_order) + offsetof(GameVec<uint32_t>, begin), kMaxEntrants)) {
        stock_order.reserve(ord_span.count);
        for (size_t i = 0; i < ord_span.count; ++i) {
            uint32_t k = 0;
            if (rd(ord_span.at(i), k) && valid_team_key(k)) stock_order.push_back(k);
        }
    }

    const char *source = "none";
    std::vector<uint32_t> prev_standings = resolve_previous_season_team_order(ctx, source);
    if (prev_standings.empty() && ctx.season_index == 0 && apply_in_season_1) {
        prev_standings = realistic_constructor_baseline_order();
        source = "realistic 2024 constructors baseline (Season 1)";
    }

    if (prev_standings.empty() && !is_reverse)
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);

    if (prev_standings.empty() && is_reverse)
        source = "stock team_order (reversed)";

    auto new_order = build_team_order(session_teams, prev_standings, stock_order, gar_span.count, is_reverse);

    std::vector<uint32_t> unique_teams;
    for (uint32_t k : session_teams) {
        if (std::ranges::none_of(unique_teams, [k](uint32_t u) { return same_team(u, k); }))
            unique_teams.push_back(k);
    }

    size_t expected_slots = std::min(unique_teams.size(), gar_span.count);
    if (new_order.size() < expected_slots) {
        swofty::log("[pitlane-garage-order] fallback to stock garage order: %zu active session team(s) vs %zu ordered slots (track garages: %zu)",
                    unique_teams.size(), new_order.size(), gar_span.count);
        return orig(out_assignments, entrants, garages, team_order, fixed_teams);
    }

    void *alloc = nullptr;
    rd(reinterpret_cast<uintptr_t>(team_order), alloc);
    GameVec<uint32_t> custom_team_order{alloc, new_order.data(), new_order.data() + new_order.size(), new_order.size()};
    GameVec<uint32_t> empty_fixed_teams{alloc, nullptr, nullptr, 0};

    if (verbose) {
        std::string summary = format_team_keys(new_order);
        swofty::log("[pitlane-garage-order] Career Season %u (%s%s, %zu garage(s) on track): %s",
                    ctx.season_index + 1, source, is_reverse ? ", reversed" : "", gar_span.count, summary.c_str());
    }

    orig(out_assignments, entrants, garages, &custom_team_order, &empty_fixed_teams);
}

SWOFTY_ON_LOAD
{
    swofty::log("[pitlane-garage-order] loaded (enabled=%s, season_1=%s, reverse=%s, verbose=%s)",
                enabled ? "true" : "false",
                apply_in_season_1 ? "true" : "false",
                reverse_order ? "true" : "false",
                verbose ? "true" : "false");
}

SWOFTY_ADDON()
