// Type schema open time, and the fast OOA decrypt against the original path.
//
//   types_open_bench <exe> [runs]
//
// Opens the executable with the default (parallel, AES-NI where available)
// decrypt and with BF6_OOA_LIFT_SOFT=1, and requires the lifted images to be
// byte-identical.
#include "types.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using clk = std::chrono::steady_clock;

static bool open_once(const std::string& exe, bool soft, double& seconds, uint64_t& digest, int& lifted)
{
#ifdef _WIN32
    _putenv_s("BF6_OOA_LIFT_SOFT", soft ? "1" : "0");
#else
    setenv("BF6_OOA_LIFT_SOFT", soft ? "1" : "0", 1);
#endif
    bf6::TypeDb db;
    std::string err;
    const auto t0 = clk::now();
    if (!db.open(exe, err)) { std::printf("open failed: %s\n", err.c_str()); return false; }
    seconds = std::chrono::duration<double>(clk::now() - t0).count();
    digest = db.image_digest();
    lifted = db.lifted();
    std::printf("%s: %.3f s, lifted %d section(s), encrypted after: %s\n", soft ? "software, one thread" : "fast path",
                seconds, lifted, db.looks_encrypted() ? "yes" : "no");
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: types_open_bench <exe> [runs]\n"); return 2; }
    const std::string exe = argv[1];
    const int runs = argc > 2 ? std::atoi(argv[2]) : 3;
    double s = 0; uint64_t fast = 0, soft = 0; int lf = 0, ls = 0;
    for (int i = 0; i < runs; ++i) if (!open_once(exe, false, s, fast, lf)) return 1;
    if (!open_once(exe, true, s, soft, ls)) return 1;
    const bool ok = fast == soft && lf == ls;
    std::printf("image digest fast %016llx soft %016llx: %s\n", (unsigned long long)fast,
                (unsigned long long)soft, ok ? "IDENTICAL" : "DIFFERENT");
    return ok ? 0 : 1;
}
