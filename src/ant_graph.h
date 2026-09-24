/* ant_graph.h - internal to the core: ANT's asset graph, game states and pose
 * sampling, the foundation the ANT runtime (ant_runtime_ext.inc) builds on.
 *
 * Nothing here decides animation behaviour. It loads the authored graph
 * exactly as shipped, answers "what is this game state's value", and turns a
 * clip at a time into bone transforms. The controller semantics live in the
 * runtime, each one read from the executable. */
#ifndef LIBBF6_ANT_GRAPH_H
#define LIBBF6_ANT_GRAPH_H

#include "bf6_core.h"
#include "ebx.h"
#include "types.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct bf6_ctx;

namespace bf6ant {

/* One instance of one partition, decoded through the reflection. */
struct Obj {
    std::string       path;       /* owning partition */
    int               index = -1; /* instance within it */
    std::string       type;       /* type guid, dashed lowercase */
    bf6::EbxValue     v;
    const bf6::EbxValue* f(uint32_t h) const { return v.field(h); }
};

struct Part {
    std::string path;
    std::vector<std::unique_ptr<Obj>> objs;
    std::map<std::string, int> by_guid;  /* exported instance guid -> index */
};

/* Loads partitions on demand and resolves references between them. */
class Graph {
public:
    explicit Graph(bf6_ctx* c) : c_(c) {}
    /* The partition's primary instance (index 0), or null. */
    const Obj* root(const std::string& path);
    /* An ImportRef (to another partition) or InstanceRef (within `from`). */
    const Obj* resolve(const bf6::EbxValue* ref, const Obj* from);
    const std::string& error() const { return err_; }
    /* Every partition loaded so far, for diagnostics. */
    size_t loaded() const { return parts_.size(); }
private:
    Part* part(const std::string& path);
    bf6_ctx* c_;
    std::map<std::string, std::unique_ptr<Part>> parts_;
    std::string err_;
};

/* GAME STATES. The caller sets values by game-state asset path; everything the
 * graph reads goes through value(). Only state types whose semantics are
 * established are evaluated; any other is reported through unknown(), never
 * silently treated as false. */
struct Value { int kind = 0; bool b = false; float f = 0.f; int32_t i = 0; };  /* kind: 0 unset, 1 bool, 2 float, 3 int */

class States {
public:
    void set_bool(const std::string& p, bool v)   { m_[p] = Value{1, v, v ? 1.f : 0.f, v ? 1 : 0}; }
    void set_float(const std::string& p, float v) { m_[p] = Value{2, v != 0.f, v, (int32_t)v}; }
    void set_int(const std::string& p, int32_t v) { m_[p] = Value{3, v != 0, (float)v, v}; }
    /* A TAG'S WRITE sits OVER the caller's value until the tag resets it. The caller
     * plays the game's gameplay code and re-sends its states every frame; a clip or
     * node tag (the reload node setting 13p.wep.handikdisable to Both) must hold for as
     * long as it is active rather than be erased by the next frame's re-send - which is
     * what happened: the upper body read the caller's value while the IK read the tag's,
     * and the reload arms were driven by neither. Resetting a tag clears only its layer,
     * so the state goes back to the caller's value (or, unset, the authored default). */
    void tag_set_bool(const std::string& p, bool v)   { t_[p] = Value{1, v, v ? 1.f : 0.f, v ? 1 : 0}; }
    void tag_set_float(const std::string& p, float v) { t_[p] = Value{2, v != 0.f, v, (int32_t)v}; }
    void tag_set_int(const std::string& p, int32_t v) { t_[p] = Value{3, v != 0, (float)v, v}; }
    void tag_clear(const std::string& p) { t_.erase(p); }
    const Value* get(const std::string& p) const {
        auto t = t_.find(p);
        if (t != t_.end()) return &t->second;
        auto it = m_.find(p);
        return it == m_.end() ? nullptr : &it->second;
    }
    void clear(const std::string& p) { m_.erase(p); }
    /* The value faces of a game-state asset. `ok` false = not evaluable. */
    bool  as_bool(Graph& g, const Obj* o, bool& ok);
    float as_float(Graph& g, const Obj* o, bool& ok);
    int32_t as_int(Graph& g, const Obj* o, bool& ok);
    const std::vector<std::string>& unknown() const { return unknown_; }
    /* SignalChooserPolicy entry test; ok=false when an input is not evaluable. */
    bool signal_pass(Graph& g, const Obj* entry, bool& ok);
private:
    void note_unknown(const Obj* o);
    /* The same list, for a reason that is not simply an unknown type: a chooser
     * that could not be evaluated says WHY rather than returning a bare false. */
    void note_unknown_str(const std::string& why);
    std::map<std::string, Value> m_;
    std::map<std::string, Value> t_;   /* active tags' writes, over m_ */
    std::vector<std::string> unknown_;
};

/* POSE: per skeleton bone, local rotation (x,y,z,w) and translation, each with
 * a validity flag. A slot nothing has written is INVALID: the engine's Blend
 * copies into it instead of lerping against it (research spec_superlayers,
 * FUN_140845cb0). Values of invalid slots hold the bind pose so a final pose
 * always has something to show. */
struct Pose {
    std::vector<std::array<float, 4>> q;
    std::vector<std::array<float, 3>> t;
    std::vector<uint8_t> vq, vt;
    void resize(size_t n) { q.assign(n, {0, 0, 0, 1}); t.assign(n, {0, 0, 0}); vq.assign(n, 0); vt.assign(n, 0); }
};

/* Per-bone blend mask: separate weights for the rotation and translation DOFs
 * of each bone. Null = unmasked. */
struct Mask { std::vector<float> rot, tr; };

void quat_from_rows(const float* m9, float* q);   /* inverse of loadout's quat_rows */
void rows_from_quat(const float* q, float* m9);
/* Blend_Mode 0: q = nlerp(dst, src, w), v = lerp; invalid dst slots copy src. */
void pose_blend(Pose& dst, const Pose& src, float w, const Mask* mask);
/* Blend_Mode 1 (Additive), 4 (Subtractive = w negated): local space,
 * q = dst (x) nlerp(I, add', |w|) with add' = conj(add) when w < 0; v += w*add.
 * Masked: w_slot = w*mask, q = dst (x) nlerp(I, add, w_slot), no conjugate.
 * A slot the destination never wrote stays invalid: there is nothing to add
 * to, and the bind value is not a pose the graph chose. */
void pose_add(Pose& dst, const Pose& add, float w, const Mask* mask);

/* A clip bound to one rig/skeleton pair, sampled at a time in FRAMES into a
 * pose. Bones the clip does not drive are left as they are in `out`. */
class ClipSource {
public:
    bool open(bf6_ctx* c, const std::string& clip, const char* rig, const char* skeleton, std::string& err);
    ~ClipSource();
    bool sample(bf6_ctx* c, float frames, bool loop, Pose& out) const;
    int  frame_count() const { return frames_; }
private:
    bf6_anim_clip* clip_ = nullptr;
    bf6_ctx* owner_ = nullptr;
    std::vector<int32_t> ch_, comp_, bone_;
    int channels_ = 0, frames_ = 0;
};

}  // namespace bf6ant

#endif
