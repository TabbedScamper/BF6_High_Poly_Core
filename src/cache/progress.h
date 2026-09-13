// Precache progress: one ordered model per build, weighted by measured cost,
// never moving backwards. Safe to read from a UI thread every frame.
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bf6::cache {

struct LayerSpec {
    std::string id;       // stable identifier, e.g. "terrain"
    double weight = 1.0;  // relative cost within one map (measured)
};

class Progress {
public:
    enum class State { Idle = 0, Running = 1, Cancelling = 2, Done = 3, Failed = 4 };
    enum class MapState { Waiting = 0, Running = 1, Done = 2, Failed = 3 };

    // Maps carry relative costs so a large map advances the overall bar more.
    void reset(const std::vector<std::string>& maps, const std::vector<double>& map_costs,
               const std::vector<LayerSpec>& layers, double shared_cost);

    // Fraction 0..1 of one layer of one map. Lower values than already reported
    // are ignored, so no caller can move a bar backwards.
    void layer(const std::string& map, const std::string& layer, double fraction, const std::string& item = {});
    void map_done(const std::string& map, bool ok);
    void shared(double fraction, const std::string& item = {});
    void state(State s, const std::string& error = {});

    struct Snapshot {
        State state = State::Idle;
        double overall = 0.0;
        int map_count = 0, maps_done = 0;
        std::string current_map, current_layer, current_item, error;
        double seconds_since_update = 0.0;
        double shared = 0.0;
    };
    struct MapSnapshot {
        std::string level;
        MapState state = MapState::Waiting;
        double progress = 0.0;
        std::vector<double> layers;
    };

    Snapshot snapshot() const;
    std::vector<MapSnapshot> maps() const;
    std::vector<LayerSpec> layer_specs() const;

private:
    struct MapRow {
        double cost = 1.0;
        MapState state = MapState::Waiting;
        std::vector<double> layers;
    };
    double overall_locked() const;
    void touch(const std::string& map, const std::string& layer, const std::string& item);

    mutable std::mutex mutex_;
    State state_ = State::Idle;
    std::vector<LayerSpec> layers_;
    std::map<std::string, std::size_t> layer_index_;
    std::vector<std::string> order_;
    std::map<std::string, MapRow> rows_;
    double shared_cost_ = 0.0, shared_ = 0.0;
    mutable double reported_ = 0.0;
    std::string current_map_, current_layer_, current_item_, error_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

}  // namespace bf6::cache
