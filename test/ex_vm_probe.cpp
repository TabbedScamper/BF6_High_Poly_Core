/* RUN AN EX PROGRAM STANDALONE through bf6ex (core/src/ex_vm.*): every game state
 * starts at zero (or a value given on the command line), no pose is bound, and the
 * program is stepped N times. Reports whether each run halts, the kernels the
 * program calls that are not implemented, and every state it wrote.
 *
 *   ex_vm_probe <game> <expression asset> [ticks=3] [state=value ...]
 *
 * A value is a float; "state" is a substring of the game-state path.
 */
#include "bf6_core.h"
#include "ex_vm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
struct MapHost : bf6ex::Host {
    std::vector<std::string> path;
    std::vector<std::vector<uint8_t>> value;
    std::vector<int> writes;
    std::map<std::string, float> preset;
    uint64_t bind_state(const std::string& p, uint32_t) override {
        for (size_t i = 0; i < path.size(); ++i) if (path[i] == p) return i;
        path.push_back(p); value.emplace_back(16, 0); writes.push_back(0);
        for (const auto& [k, v] : preset)
            if (p.find(k) != std::string::npos) {
                std::memcpy(value.back().data(), &v, 4);
                if (p.size() > 5 && p.compare(p.size() - 5, 5, ".bool") == 0) value.back()[0] = v != 0.f;
            }
        return path.size() - 1;
    }
    void read_state(uint64_t h, void* out, int n) override { if (h < value.size()) std::memcpy(out, value[h].data(), n); }
    void write_state(uint64_t h, const void* in, int n) override {
        if (h < value.size()) { std::memcpy(value[h].data(), in, n); ++writes[h]; }
    }
};
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::fprintf(stderr, "usage: ex_vm_probe <game> <asset> [ticks] [state=value...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    bf6ex::Program prog;
    std::string e;
    if (!bf6ex::load_program(c, argv[2], prog, e)) { std::printf("load: %s\n", e.c_str()); return 1; }
    const int ticks = argc > 3 ? std::atoi(argv[3]) : 3;
    MapHost host;
    for (int a = 4; a < argc; ++a) {
        const char* eq = std::strchr(argv[a], '=');
        if (eq) host.preset[std::string(argv[a], (size_t)(eq - argv[a]))] = (float)std::atof(eq + 1);
    }
    std::printf("%s: code %u bytes, %zu call sites, %zu inputs, %zu seeds, h20 %u h28 %u h30 %u h34 %u\n",
                prog.path.c_str(), prog.clen, prog.call_sites.size(), prog.inputs.size(), prog.seeds.size(),
                prog.h20, prog.h28, prog.h30, prog.h34);
    std::printf("  special slots: ctx0 %u:%u ctx1 %u:%u dt %u:%u ctx2 %u:%u ctx3 %u:%u\n",
                prog.slot_ctx0[0], prog.slot_ctx0[1], prog.slot_ctx1[0], prog.slot_ctx1[1], prog.slot_dt[0], prog.slot_dt[1],
                prog.slot_ctx2[0], prog.slot_ctx2[1], prog.slot_ctx3[0], prog.slot_ctx3[1]);
    for (const std::string& m : prog.missing_kernels()) std::printf("  missing kernel %s\n", m.c_str());
    bf6ex::PoseArena arena;
    bf6ex::Instance inst;
    if (!inst.init(prog, &host, &arena, e)) { std::printf("init: %s\n", e.c_str()); return 1; }
    for (int t = 0; t < ticks; ++t) {
        const uint64_t before = inst.steps;
        const bool ok = inst.run(1.0f, e);
        std::printf("tick %d: %s, %llu instructions\n", t, ok ? "halted" : e.c_str(),
                    (unsigned long long)(inst.steps - before));
        if (!ok) break;
    }
    std::map<std::string, int> notes;
    for (const auto& n : inst.notes) ++notes[n];
    for (const auto& [n, k] : notes) std::printf("  note x%d: %s\n", k, n.c_str());
    for (size_t i = 0; i < host.path.size(); ++i)
        if (host.writes[i]) {
            float f; std::memcpy(&f, host.value[i].data(), 4);
            const float* v = (const float*)host.value[i].data();
            std::printf("  wrote %-70s b%u f%g v(%g %g %g %g)\n", host.path[i].c_str(), host.value[i][0], f, v[0], v[1], v[2], v[3]);
        }
    return 0;
}
