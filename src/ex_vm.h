#ifndef BF6_EX_VM_H
#define BF6_EX_VM_H

/* THE EX EXPRESSION-PROGRAM INTERPRETER - the game's own per-frame logic for the
 * first-person soldier (1p.preupdate.expsop, 1p.postupdate.expsop, ...), run as the
 * executable runs it.
 *
 * Specification: native/EX_VM_SPEC.md (the interpreter, FUN_142485d00) and
 * native/EX_KERNELS_SPEC.md (the kernels). This is a transcription, not a model:
 * the instance memory has the engine's layout byte for byte, so every operand
 * (region, offset) a shipped program names lands where the engine would put it.
 *
 *   Program   the image (field 0x5db2cfc4) plus the asset fields that set it up
 *   Instance  one running copy: persistent regions 1 and 2 survive between runs
 *   Host      what the engine supplies - game states, the pose arena, delta time
 *
 * WHY IT EXISTS: first-person ADS. The on-foot sight-to-eye is written by the
 * post-update program's DofWriter calls, not by any animation controller, so
 * without running it the weapon never reaches the eye.
 */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6ex {

/* The engine side. Every call receives the 8-byte handle the program was given for
 * that state at setup (Host::bind_state), never a name. */
struct Host {
    virtual ~Host() = default;
    /* An input's handle: called once per program input at setup. `key` is the
     * per-input interface key (field 0x653bfba0); return any 8-byte value. */
    virtual uint64_t bind_state(const std::string& path, uint32_t key) = 0;
    /* Read / write a state through its handle. `bytes` is 1 (bool), 4 (float, int)
     * or 16 (vector3 padded to 16, quaternion). */
    virtual void read_state(uint64_t handle, void* out, int bytes) = 0;
    virtual void write_state(uint64_t handle, const void* in, int bytes) = 0;
};

/* THE POSE ARENA the Dof kernels address: a DOF table (index -> byte offset, width)
 * over a byte arena, with one validity byte per DOF (the engine's base+0x10+index). */
struct PoseArena {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> valid;          /* nonzero: live pose data at that DOF */
    std::vector<uint32_t> offset, width; /* per DOF index */
    std::map<uint32_t, int32_t> by_hash; /* seed name hash -> DOF index */
};

struct DofHandle { int32_t index; uint32_t offset; };
constexpr int32_t kUnbound = INT32_MIN;

uint32_t kernel_hash(const char* name);   /* ~CRC32 MSB-first, poly 0x04C11DB7 */

struct Program {
    std::string path;
    std::vector<uint8_t> image;
    uint32_t cbase = 0, code = 0, clen = 0;
    /* header words the instance layout is built from */
    uint32_t h20 = 0, h28 = 0, h30 = 0, h34 = 0, h44 = 0;
    /* table C: call site pc -> kernel name hash */
    std::map<uint32_t, uint32_t> call_sites;
    struct Input { std::string path; uint32_t a = 0, b = 0, key = 0; };
    std::vector<Input> inputs;
    struct Seed { uint32_t hash = 0, kind = 0, a = 0, b = 0, da = 0, db = 0; };
    std::vector<Seed> seeds;
    uint32_t slot_ctx0[2] = {~0u, ~0u};  /* +0x64 first host 8-byte value (the pose context) */
    uint32_t slot_ctx1[2] = {~0u, ~0u};  /* +0x6c second host value */
    uint32_t slot_dt[2]   = {~0u, ~0u};  /* +0x74 delta time */
    uint32_t slot_ctx2[2] = {~0u, ~0u};  /* +0x7c the state-interface context */
    uint32_t slot_ctx3[2] = {~0u, ~0u};  /* +0x84 kernel evaluation context */

    bool open_image(std::string& err);   /* after `image` is filled */
    /* kernel names this program calls that have no implementation here */
    std::vector<std::string> missing_kernels() const;
};

struct Instance {
    const Program* p = nullptr;
    std::vector<uint8_t> mem;             /* the engine's instance block */
    std::vector<uint8_t> konst;           /* this instance's copy of the image (region 0 is patchable) */
    std::vector<uint64_t> ext;            /* regions 3.. */
    uint32_t r1 = 0, table = 0, r2 = 0;   /* offsets into mem */
    uint32_t stack_at = 0;
    Host* host = nullptr;
    PoseArena* pose = nullptr;
    struct PoseCtx { PoseArena* pose; } pose_ctx{};
    struct StateCtx { Host* host; } state_ctx{};
    uint64_t steps = 0;
    std::vector<std::string> notes;

    bool init(const Program& prog, Host* h, PoseArena* arena, std::string& err);
    /* Bind the seed rows against the arena (the engine redoes this when the pose owner changes). */
    void bind_seeds();
    /* One evaluation from `entry` (0 for a scene op) with dt in 60 Hz TICKS. */
    bool run(float dt_ticks, std::string& err, uint32_t entry = 0, uint64_t max_steps = 5000000);

    uint8_t* addr(uint32_t a, uint32_t b);
};

/* Load a program from its asset (the op or the program asset itself). */
struct LoadedProgram { Program prog; std::string error; };
}  // namespace bf6ex

struct bf6_ctx;
namespace bf6ex {
bool load_program(bf6_ctx* c, const std::string& asset, Program& out, std::string& err);
}

#endif
