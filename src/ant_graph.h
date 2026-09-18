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
    const Value* get(const std::string& p) const { auto it = m_.find(p); return it == m_.end() ? nullptr : &it->second; }
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
    std::map<std::string, Value> m_;
    std::vector<std::string> unknown_;
};

/* POSE: per skeleton bone, local rotation (x,y,z,w) and translation. */
struct Pose {
    std::vector<std::array<float, 4>> q;
    std::vector<std::array<float, 3>> t;
    void resize(size_t n) { q.assign(n, {0, 0, 0, 1}); t.assign(n, {0, 0, 0}); }
};

void quat_from_rows(const float* m9, float* q);   /* inverse of loadout's quat_rows */
void rows_from_quat(const float* q, float* m9);
void pose_blend(Pose& dst, const Pose& src, float w, const std::vector<float>* mask);  /* nlerp/lerp toward src */
void pose_add(Pose& dst, const Pose& delta, float w, const std::vector<float>* mask);  /* dst * delta^w */

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
