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
#include <string>

namespace bf6 {

class Source;
class TypeDb;

class SoldierFields {
public:
    enum Kind { kBool = 0, kFloat = 1, kInt = 2, kVec = 3 };
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
    void clear_live() { live_.clear(); }

private:
    static uint64_t key(int kind, int lane, int id) {
        return ((uint64_t)kind << 40) | ((uint64_t)(lane & 0xFF) << 32) | (uint32_t)id;
    }
    bool loaded_ = false;
    std::map<uint64_t, Field> by_key_;
    std::map<std::string, std::array<float, 4>> live_;
};

}  // namespace bf6

#endif
