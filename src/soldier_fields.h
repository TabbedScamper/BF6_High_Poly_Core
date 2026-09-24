/* The soldier's named state fields, as the game's motion machine defines them.
 *
 * The soldier motion-machine expression graphs (common/gameplay/soldier/traversal/**) read
 * soldier values through four engine operators that take a constant DESCRIPTOR - a u16
 * field id, a flags byte and a lane byte - instead of a named channel:
 *
 *   0x63E71248  bool   (native 144328E80)     0xF4311C3D  float  (native 144328FC0)
 *   0x9FC488C4  int    (native 144328F40)     0x32E399EB  vec4   (native 144329040)
 *
 * No graph asset names those ids. The motion-machine asset itself does:
 * common/gameplay/soldier/soldiermotionmachine carries one instance per field, typed by
 * kind, with its name (0x0c59fa06), its id (0x51480447), its lane (0xdfd68748) and its
 * authored default (0x42c8b257). Measured against every descriptor the 118 soldier graphs
 * use, each (lane, id) lands inside its kind's list.
 *
 * Offline, a field's value is what the walker supplies for it by NAME (set_live), and
 * otherwise its authored default - which is what a soldier that nothing has written to
 * reads. A descriptor that names no field is refused, not defaulted.
 */
#ifndef BF6_SOLDIER_FIELDS_H
#define BF6_SOLDIER_FIELDS_H

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf6 {

class Source;
class TypeDb;

class SoldierFields {
public:
    /* kXform: the transform fields (LadderTransform, LadderInteractWorldTransform, ...)
     * read by 0xD927CB31 as a LinearTransform built from a stored rotation + position. */
    enum Kind { kBool = 0, kFloat = 1, kInt = 2, kVec = 3, kXform = 4 };
    struct Field {
        std::string name;
        int kind = 0, lane = 0, id = 0;
        float def[4] = {0, 0, 0, 0};
    };

    static SoldierFields& get();
    /* Reads the table from the mounted install. Safe to call more than once. */
    bool load(Source& src, TypeDb& types, std::string& err);
    bool loaded() const { return loaded_; }
    size_t size() const { return by_key_.size(); }

    const Field* find(int kind, int lane, int id) const;
    /* The value a reader returns: the live value set for this field's name, else its
     * authored default. `out` gets up to four lanes. */
    void value(const Field& f, float out[4]) const;
    void set_live(const std::string& name, const float* v, int n);
    void clear_live() { live_.clear(); xform_live_.clear(); unknown_.clear(); }
    /* A transform field: 16 floats (Frostbite LinearTransform rows), identity when never
     * set. INFERRED default: these fields carry no scalar default in the asset, and a
     * soldier with no ladder or interaction has no transform to report. */
    void xform(const Field& f, float out[16]) const;
    void set_live_xform(const std::string& name, const float m[16]);
    /* What a graph's STORE writes: the value lands on the field's live value, so later
     * graphs read it and the walker can read it back. */
    void store(const Field& f, const float* v, int n) { set_live(f.name, v, n); unknown_.erase(f.name); }
    /* A store whose value is not known: later reads of the field must refuse, not fall
     * back to the default the store overwrote. */
    void store_unknown(const Field& f) { unknown_.insert(f.name); }
    bool known(const Field& f) const { return unknown_.count(f.name) == 0; }
    /* Every field something has written (a live value, or an unknown store), for a caller
     * that wants to see what a graph changed. */
    std::vector<std::string> written() const {
        std::vector<std::string> out;
        for (const auto& kv : live_) out.push_back(kv.first);
        for (const std::string& n : unknown_) if (!live_.count(n)) out.push_back(n);
        return out;
    }
    /* A field named by its 32-bit link hash (the machine's hash list), or null. */
    const Field* by_hash(uint32_t h) const {
        auto it = by_hash_.find(h);
        return it == by_hash_.end() ? nullptr : it->second;
    }
    size_t hash_count() const { return by_hash_.size(); }
    const Field* by_name(const std::string& name) const {
        auto it = by_name_.find(name);
        return it == by_name_.end() ? nullptr : it->second;
    }
    /* A field's current value by name (live, else default); false when no such field. */
    bool get_by_name(const std::string& name, float out[4]) const;

private:
    static uint64_t key(int kind, int lane, int id) {
        return ((uint64_t)kind << 40) | ((uint64_t)(lane & 0xFF) << 32) | (uint32_t)id;
    }
    bool loaded_ = false;
    std::map<uint64_t, Field> by_key_;
    std::map<std::string, std::array<float, 4>> live_;
    std::map<std::string, std::array<float, 16>> xform_live_;
    std::map<std::string, const Field*> by_name_;
    std::set<std::string> unknown_;
    std::map<uint32_t, const Field*> by_hash_;
};

}  // namespace bf6

#endif
