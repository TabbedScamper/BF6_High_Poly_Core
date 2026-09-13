#include "progress.h"

#include <algorithm>

namespace bf6::cache {

void Progress::reset(const std::vector<std::string>& maps, const std::vector<double>& map_costs,
                     const std::vector<LayerSpec>& layers, double shared_cost)
{
    std::lock_guard<std::mutex> lock(mutex_);
    layers_ = layers;
    layer_index_.clear();
    for (std::size_t i = 0; i < layers_.size(); ++i) layer_index_[layers_[i].id] = i;
    order_ = maps;
    rows_.clear();
    for (std::size_t i = 0; i < maps.size(); ++i) {
        MapRow row;
        row.cost = i < map_costs.size() && map_costs[i] > 0.0 ? map_costs[i] : 1.0;
        row.layers.assign(layers_.size(), 0.0);
        rows_[maps[i]] = row;
    }
    shared_cost_ = std::max(0.0, shared_cost);
    shared_ = 0.0;
    reported_ = 0.0;
    state_ = State::Idle;
    current_map_.clear(); current_layer_.clear(); current_item_.clear(); error_.clear();
    last_ = std::chrono::steady_clock::now();
}

void Progress::touch(const std::string& map, const std::string& layer, const std::string& item)
{
    current_map_ = map;
    current_layer_ = layer;
    if (!item.empty()) current_item_ = item;
    last_ = std::chrono::steady_clock::now();
}

void Progress::layer(const std::string& map, const std::string& layer, double fraction, const std::string& item)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto row = rows_.find(map);
    const auto li = layer_index_.find(layer);
    if (row == rows_.end() || li == layer_index_.end()) return;
    fraction = std::clamp(fraction, 0.0, 1.0);
    double& value = row->second.layers[li->second];
    if (fraction > value) value = fraction;
    if (row->second.state == MapState::Waiting) row->second.state = MapState::Running;
    touch(map, layer, item);
}

void Progress::map_done(const std::string& map, bool ok)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto row = rows_.find(map);
    if (row == rows_.end()) return;
    row->second.state = ok ? MapState::Done : MapState::Failed;
    if (ok) std::fill(row->second.layers.begin(), row->second.layers.end(), 1.0);
    touch(map, {}, ok ? "done" : "failed");
}

void Progress::shared(double fraction, const std::string& item)
{
    std::lock_guard<std::mutex> lock(mutex_);
    shared_ = std::max(shared_, std::clamp(fraction, 0.0, 1.0));
    touch({}, "shared", item);
}

void Progress::state(State s, const std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = s;
    if (!error.empty()) error_ = error;
    last_ = std::chrono::steady_clock::now();
}

double Progress::overall_locked() const
{
    double layer_total = 0.0;
    for (const LayerSpec& l : layers_) layer_total += std::max(0.0, l.weight);
    if (layer_total <= 0.0) layer_total = 1.0;
    double done = 0.0, total = shared_cost_;
    done += shared_ * shared_cost_;
    for (const auto& kv : rows_) {
        const MapRow& row = kv.second;
        double fraction = 0.0;
        if (row.state == MapState::Done) fraction = 1.0;
        else
            for (std::size_t i = 0; i < layers_.size(); ++i)
                fraction += std::max(0.0, layers_[i].weight) / layer_total * row.layers[i];
        done += row.cost * fraction;
        total += row.cost;
    }
    const double value = total > 0.0 ? done / total : 0.0;
    // Monotonic even if a caller re-weights or a map is retried.
    reported_ = std::max(reported_, std::clamp(value, 0.0, 1.0));
    return reported_;
}

Progress::Snapshot Progress::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.state = state_;
    s.overall = overall_locked();
    s.map_count = (int)rows_.size();
    for (const auto& kv : rows_) if (kv.second.state == MapState::Done) ++s.maps_done;
    s.current_map = current_map_;
    s.current_layer = current_layer_;
    s.current_item = current_item_;
    s.error = error_;
    s.shared = shared_;
    s.seconds_since_update = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_).count();
    return s;
}

std::vector<Progress::MapSnapshot> Progress::maps() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    double layer_total = 0.0;
    for (const LayerSpec& l : layers_) layer_total += std::max(0.0, l.weight);
    if (layer_total <= 0.0) layer_total = 1.0;
    std::vector<MapSnapshot> out;
    out.reserve(order_.size());
    for (const std::string& level : order_) {
        const MapRow& row = rows_.at(level);
        MapSnapshot m;
        m.level = level;
        m.state = row.state;
        m.layers = row.layers;
        for (std::size_t i = 0; i < layers_.size(); ++i)
            m.progress += std::max(0.0, layers_[i].weight) / layer_total * row.layers[i];
        if (row.state == MapState::Done) m.progress = 1.0;
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<LayerSpec> Progress::layer_specs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return layers_;
}

}  // namespace bf6::cache
