// Regression test for bf6_ocean_replay.h, the engine-neutral transcription of
// the Unreal add-on's production CPU ocean replay (BF6HighPolyWaterFFT.cpp).
//
// Every reference is evaluated independently in double precision: a direct
// O(n^4) DFT, and a per-pixel wave sum written in physical wave-vector and
// position form. The references never use the bit-reversal, the separable 2D
// pass or the (-1)^(x+y) recentring the replay relies on. The normal and foam
// references restate the replay's central-difference stencil, reciprocal foam
// gain and history decay from the specification, on the double displacement.
//
// Each comparison is paired with a deliberately wrong reference that must be
// rejected: normalised by N^2, negated exponent, wrong-sign X wave vector,
// non-reciprocal foam gain, swapped history lerp. Both scores are printed.
//
// Scope. Synthetic cases pin the replay's own conventions and claim no game
// parity. With <game_dir> <level> [exe], authored cascades are read through
// bf6_level_water_sims, H0 comes from bf6_water_spectrum_h0 at the AUTHORED
// resolution, and one frame is checked against the wave sum at sampled pixels.
// Whether that H0 equals the executable's builder, and whether the replay's
// output equals the game's GPU ocean, is NOT tested here.
//
// usage: ocean_replay_test [<game_dir> <level> [exe]]
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../include/bf6_ocean_replay.h"

namespace {
using cd = std::complex<double>;
using bf6_ocean::Cascade;
using bf6_ocean::Complex;
using bf6_ocean::Pixel;

const double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}

struct Rng {
    uint64_t s;
    double next() // uniform in [-1, 1)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(s >> 11) / double(1ULL << 53) * 2.0 - 1.0;
    }
};

// Max absolute error, relative to the reference's max magnitude.
struct Score {
    double err = 0, ref = 0;
    void add(double got, double want)
    {
        err = std::max(err, std::fabs(got - want));
        ref = std::max(ref, std::fabs(want));
    }
    double rel() const { return ref > 0 ? err / ref : err; }
};

// A real score must pass `tol`; a control must be rejected by the same metric.
void gate(const char* label, double real, double tol, double control, double control_min)
{
    const bool ok = real <= tol && control >= control_min;
    std::printf("  %-26s real=%.3e (tol %.0e)  control=%.3e (min %.0e)  %s\n",
                label, real, tol, control, control_min, ok ? "pass" : "FAIL");
    if (!ok) ++failures;
}

bool finite(float v) { return std::isfinite(v); }

// ------------------------------------------------------------------ 2D FFT
void test_fft2(int n)
{
    Rng r{ 0x9E3779B97F4A7C15ULL ^ uint64_t(n) };
    std::vector<Complex> in(size_t(n) * n);
    for (Complex& c : in) { c.r = float(r.next()); c.i = float(r.next()); }
    in[1].r += 3.f; in[size_t(n) + 3].i -= 2.f; // no accidental symmetry

    std::vector<Complex> got = in;
    bf6_ocean::fft2(got, n);

    Score real, normalised, negated;
    bool fin = true;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            cd pos, neg;
            for (int v = 0; v < n; ++v)
                for (int u = 0; u < n; ++u) {
                    const cd a(in[size_t(v) * n + u].r, in[size_t(v) * n + u].i);
                    const double th = 2.0 * kPi * double(u * x + v * y) / n;
                    pos += a * cd(std::cos(th), std::sin(th));
                    neg += a * cd(std::cos(th), -std::sin(th));
                }
            const Complex& g = got[size_t(y) * n + x];
            fin = fin && finite(g.r) && finite(g.i);
            const double nn = double(n) * n;
            real.add(g.r, pos.real());          real.add(g.i, pos.imag());
            normalised.add(g.r / nn, pos.real()); normalised.add(g.i / nn, pos.imag());
            negated.add(g.r, neg.real());       negated.add(g.i, neg.imag());
        }
    std::printf("fft2 %dx%d vs direct positive unnormalised DFT\n", n, n);
    check(fin, "fft2 finite");
    gate("vs /N^2 control", real.rel(), 1e-4, normalised.rel(), 0.5);
    gate("vs negated-exponent ctl", real.rel(), 1e-4, negated.rel(), 0.1);
}

// ------------------------------------------------------- shared references
bf6_water_sim_v2 synth_sim(int n, float tile)
{
    bf6_water_sim_v2 s{};
    s.enabled = 1; s.resolution = n; s.tile_dimension = tile;
    s.foam_enable = 1; s.foam_threshold = 0.1f; s.foam_max = 0.8f; s.foam_half_life = 2.5f;
    return s;
}

// Sizes the cascade without the native H0 builder, for caller-supplied H0.
void prepare(Cascade& c, const bf6_water_sim_v2& s)
{
    const size_t count = size_t(s.resolution) * s.resolution;
    c.input = s;
    c.h0.assign(count, Complex{}); c.height.assign(count, Complex{});
    c.dx.assign(count, Complex{}); c.dz.assign(count, Complex{});
    c.displacement.assign(count, Pixel{}); c.normal.assign(count, Pixel{});
    c.history.assign(count, 0.f); c.work.assign(count, 0.f);
}

struct Spectrum {
    int n = 0; double L = 0;
    std::vector<cd> h, fx, fz;
    std::vector<double> kx, kz;
};

// Deep-water dispersion w = sqrt(g k); a real surface pairs each bin with the
// conjugate of its -k partner. xsign -1 is the production -X wave vector,
// +1 the wrong-sign control.
Spectrum evolved_reference(const std::vector<Complex>& h0, int n, double L, double t, double xsign)
{
    Spectrum S;
    S.n = n; S.L = L;
    const size_t count = size_t(n) * n;
    S.h.resize(count); S.fx.resize(count); S.fz.resize(count);
    S.kx.resize(count); S.kz.resize(count);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t i = size_t(y) * n + x;
            const size_t m = size_t((n - y) % n) * n + size_t((n - x) % n);
            const double kx = xsign * (2.0 * x - n) * kPi / L;
            const double kz = (2.0 * y - n) * kPi / L;
            const double k = std::sqrt(kx * kx + kz * kz), w = std::sqrt(9.8 * k);
            const cd ht = cd(h0[i].r, h0[i].i) * std::polar(1.0, w * t)
                        + std::conj(cd(h0[m].r, h0[m].i)) * std::polar(1.0, -w * t);
            S.h[i] = ht;
            S.fx[i] = cd(0, -kx / (k + 1e-5)) * ht;
            S.fz[i] = cd(0, -kz / (k + 1e-5)) * ht;
            S.kx[i] = kx; S.kz[i] = kz;
        }
    return S;
}

struct D3 { double x = 0, y = 0, z = 0; };

// Surface at grid sample (px, pz), world position (px, pz) * L / n, phase
// exp(i(-kx X + kz Z)).
D3 sample(const Spectrum& S, int px, int pz)
{
    const int n = S.n;
    px = ((px % n) + n) % n; pz = ((pz % n) + n) % n;
    const double X = px * S.L / n, Z = pz * S.L / n;
    cd sx, sy, sz;
    for (size_t i = 0; i < S.h.size(); ++i) {
        const double th = -S.kx[i] * X + S.kz[i] * Z;
        const cd e(std::cos(th), std::sin(th));
        sx += S.fx[i] * e; sy += S.h[i] * e; sz += S.fz[i] * e;
    }
    return { sx.real(), sy.real(), sz.real() };
}

std::vector<D3> sample_field(const Spectrum& S)
{
    std::vector<D3> d(size_t(S.n) * S.n);
    for (int y = 0; y < S.n; ++y)
        for (int x = 0; x < S.n; ++x) d[size_t(y) * S.n + x] = sample(S, x, y);
    return d;
}

struct FoamModel {
    bool enable; double threshold, max, half_life;
    bool non_reciprocal_gain; // control: gain = foam_max
    bool swapped_lerp;        // control: history and instant weights swapped
};

double foam_gain(const FoamModel& f)
{
    if (!f.enable) return 0;
    if (f.non_reciprocal_gain) return f.max;
    // Per-second decay weight of the half-life; the gain is its reciprocal
    // scaled by foam_max, and a zero foam_max authors no foam.
    const double norm = f.half_life > 0 ? 1.0 - std::pow(0.5, 1.0 / f.half_life) : 1.0;
    return (f.max > 0 && norm > 1e-6) ? 1.0 / (norm * f.max) : 0.0;
}

double foam_lag(const FoamModel& f, double dt)
{
    return (f.enable && f.half_life > 0) ? std::pow(0.5, dt / f.half_life) : 0.0;
}

struct Surface { std::vector<double> enc_x, enc_z, foam, work; };

Surface ref_surface(const std::vector<D3>& d, int n, double L, const FoamModel& f,
                    double dt, const std::vector<double>& history)
{
    auto at = [n](int x, int y) {
        return size_t(((y % n) + n) % n) * n + size_t(((x % n) + n) % n);
    };
    const size_t count = size_t(n) * n;
    const double ds = n / (2.0 * L), gain = foam_gain(f), lag = foam_lag(f, dt);
    Surface s;
    s.enc_x.assign(count, 0); s.enc_z.assign(count, 0);
    s.foam.assign(count, 0); s.work.assign(count, 0);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t i = at(x, y);
            const double fold = ds * ((d[at(x + 1, y)].x - d[at(x - 1, y)].x)
                                    + (d[at(x, y + 1)].z - d[at(x, y - 1)].z));
            const double instant = std::max(0.0, fold - f.threshold) * gain;
            s.work[i] = f.swapped_lerp ? lag * instant + (1 - lag) * history[i]
                                       : (1 - lag) * instant + lag * history[i];
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t i = at(x, y);
            const double nx = -ds * (d[at(x + 1, y)].y - d[at(x - 1, y)].y);
            const double nz = -ds * (d[at(x, y + 1)].y - d[at(x, y - 1)].y);
            const double len = std::sqrt(nx * nx + nz * nz + 1);
            s.enc_x[i] = 0.5 + 0.5 * nx / len;
            s.enc_z[i] = 0.5 + 0.5 * nz / len;
            double foam = 0;
            for (int oy = -1; oy <= 1; ++oy)
                for (int ox = -1; ox <= 1; ++ox)
                    foam += (2 - std::abs(ox)) * (2 - std::abs(oy)) / 16.0 * s.work[at(x + ox, y + oy)];
            s.foam[i] = foam;
        }
    return s;
}

bool structurally_sound(const Cascade& c)
{
    for (size_t i = 0; i < c.displacement.size(); ++i) {
        const Pixel& d = c.displacement[i];
        const Pixel& m = c.normal[i];
        if (!finite(d.x) || !finite(d.y) || !finite(d.z) || d.w != 0) return false;
        if (!finite(m.x) || m.x < 0 || m.w != 1) return false;
        if (!(m.y >= 0 && m.y <= 1 && m.z >= 0 && m.z <= 1)) return false;
        if (!finite(c.history[i])) return false;
    }
    return true;
}

// ------------------------------------------------- invalid input, zero H0
void test_invalid_and_zero()
{
    std::printf("initialize rejects invalid input; zero H0 is a flat neutral surface\n");
    Cascade bad;
    bf6_water_sim_v2 s = synth_sim(12, 32.f);
    check(!bad.initialize(s), "initialize accepted non-power-of-two resolution");
    s = synth_sim(1, 32.f);
    check(!bad.initialize(s), "initialize accepted resolution 1");
    s = synth_sim(16, 0.f);
    check(!bad.initialize(s), "initialize accepted zero tile");
    s = synth_sim(16, std::nan(""));
    check(!bad.initialize(s), "initialize accepted NaN tile");

    Cascade c;
    prepare(c, synth_sim(16, 35.5f));
    int off = 0;
    for (int frame = 0; frame < 2; ++frame) {
        c.evolve(1.25f + 0.4f * frame, 0.4f);
        for (size_t i = 0; i < c.displacement.size(); ++i) {
            const Pixel& d = c.displacement[i];
            const Pixel& m = c.normal[i];
            if (d.x != 0 || d.y != 0 || d.z != 0 || d.w != 0) ++off;
            if (m.x != 0 || m.y != 0.5f || m.z != 0.5f || m.w != 1) ++off;
            if (c.history[i] != 0) ++off;
        }
    }
    std::printf("  zero H0 non-neutral texels over 2 frames: %d\n", off);
    check(off == 0, "zero H0 output not flat/neutral");
}

// ------------------------------------------------------ synthetic H0 frames
void test_synthetic(int n, float tile, uint64_t seed)
{
    std::printf("synthetic H0 %dx%d tile=%.1f (replay conventions only, no game parity)\n",
                n, n, tile);
    Rng r{ seed };
    const size_t count = size_t(n) * n;
    std::vector<Complex> h0(count);
    const double amp = 0.6 / n; // summed height O(1) at either size
    for (Complex& c : h0) { c.r = float(amp * r.next()); c.i = float(amp * r.next()); }

    const float t1 = 1.37f, dt = 0.4f, t2 = t1 + dt;
    const double L = tile;
    const std::vector<D3> d1 = sample_field(evolved_reference(h0, n, L, t1, -1));
    const std::vector<D3> d2 = sample_field(evolved_reference(h0, n, L, t2, -1));
    const std::vector<D3> w2 = sample_field(evolved_reference(h0, n, L, t2, +1));

    // Threshold at the median frame-1 fold so roughly half the texels foam.
    std::vector<double> folds(count);
    {
        const double ds = n / (2.0 * L);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                folds[size_t(y) * n + x] = ds * (
                    (d1[size_t(y) * n + (x + 1) % n].x - d1[size_t(y) * n + (x + n - 1) % n].x) +
                    (d1[size_t((y + 1) % n) * n + x].z - d1[size_t((y + n - 1) % n) * n + x].z));
        std::nth_element(folds.begin(), folds.begin() + count / 2, folds.end());
    }
    const float threshold = float(folds[count / 2]);

    FoamModel prod{ true, threshold, 0.8, 2.5, false, false };
    FoamModel gainCtl = prod; gainCtl.non_reciprocal_gain = true;
    FoamModel lerpCtl = prod; lerpCtl.swapped_lerp = true;
    const std::vector<double> zero(count, 0.0);
    const Surface s1 = ref_surface(d1, n, L, prod, dt, zero);
    const Surface s2 = ref_surface(d2, n, L, prod, dt, s1.work);
    const Surface g2 = ref_surface(d2, n, L, gainCtl, dt, ref_surface(d1, n, L, gainCtl, dt, zero).work);
    const Surface l2 = ref_surface(d2, n, L, lerpCtl, dt, ref_surface(d1, n, L, lerpCtl, dt, zero).work);
    const Surface ws2 = ref_surface(w2, n, L, prod, dt, s1.work);

    bf6_water_sim_v2 sim = synth_sim(n, tile);
    sim.foam_threshold = threshold;
    Cascade c;
    prepare(c, sim);
    c.h0 = h0;

    for (int frame = 1; frame <= 2; ++frame) {
        c.evolve(frame == 1 ? t1 : t2, dt);
        const std::vector<D3>& d = frame == 1 ? d1 : d2;
        const Surface& s = frame == 1 ? s1 : s2;
        check(structurally_sound(c), "synthetic frame finite/encoding");
        check(c.history == c.work, "history not carried from work");

        Score X, Y, Z, NX, NZ, F, Yn, cX, cY, cZ, cNX, cF, lF;
        for (size_t i = 0; i < count; ++i) {
            const Pixel& p = c.displacement[i];
            const Pixel& m = c.normal[i];
            X.add(p.x, d[i].x); Y.add(p.y, d[i].y); Z.add(p.z, d[i].z);
            NX.add(m.y - 0.5, s.enc_x[i] - 0.5); NZ.add(m.z - 0.5, s.enc_z[i] - 0.5);
            F.add(m.x, s.foam[i]);
            Yn.add(p.y / double(count), d[i].y);
            if (frame == 2) {
                cX.add(p.x, w2[i].x); cY.add(p.y, w2[i].y); cZ.add(p.z, w2[i].z);
                cNX.add(m.y - 0.5, ws2.enc_x[i] - 0.5);
                cF.add(m.x, g2.foam[i]); lF.add(m.x, l2.foam[i]);
            }
        }
        std::printf(" frame %d t=%.2f dt=%.2f  foam ref max=%.3g\n", frame,
                    frame == 1 ? t1 : t2, dt, F.ref);
        check(F.ref > 1e-3, "synthetic foam reference is trivial");
        if (frame == 1) {
            gate("height Y vs /N^2 ctl", Y.rel(), 5e-4, Yn.rel(), 0.5);
            std::printf("  X=%.3e Z=%.3e nX=%.3e nZ=%.3e foam=%.3e\n",
                        X.rel(), Z.rel(), NX.rel(), NZ.rel(), F.rel());
            check(X.rel() <= 5e-4 && Z.rel() <= 5e-4, "frame 1 horizontal displacement");
            check(NX.rel() <= 5e-4 && NZ.rel() <= 5e-4, "frame 1 normal encoding");
            check(F.rel() <= 1e-3, "frame 1 foam");
        } else {
            gate("disp X vs wrong-sign-X", X.rel(), 5e-4, cX.rel(), 0.05);
            gate("height Y vs wrong-sign-X", Y.rel(), 5e-4, cY.rel(), 0.05);
            gate("disp Z vs wrong-sign-X", Z.rel(), 5e-4, cZ.rel(), 0.05);
            gate("normal X vs wrong-sign-X", NX.rel(), 5e-4, cNX.rel(), 0.05);
            std::printf("  normal Z real=%.3e\n", NZ.rel());
            check(NZ.rel() <= 5e-4, "frame 2 normal Z");
            gate("foam vs gain=foam_max ctl", F.rel(), 1e-3, cF.rel(), 0.05);
            gate("foam vs swapped-lerp ctl", F.rel(), 1e-3, lF.rel(), 0.05);
        }
    }

    // Reciprocal gain as behaviour: doubling foam_max halves every foam texel.
    // foam_max 0 or foam disabled produce no foam and no history.
    auto run = [&](float foam_max, int enable) {
        bf6_water_sim_v2 v = sim;
        v.foam_max = foam_max; v.foam_enable = enable;
        Cascade k;
        prepare(k, v);
        k.h0 = h0;
        k.evolve(t1, dt); k.evolve(t2, dt);
        return k;
    };
    const Cascade a = run(0.8f, 1), b = run(1.6f, 1), zmax = run(0.f, 1), off = run(0.8f, 0);
    Score half, same;
    int foamed = 0;
    for (size_t i = 0; i < count; ++i) {
        half.add(2.0 * b.normal[i].x, a.normal[i].x);
        same.add(b.normal[i].x, a.normal[i].x);
        if (zmax.normal[i].x != 0 || zmax.history[i] != 0) ++foamed;
        if (off.normal[i].x != 0 || off.history[i] != 0) ++foamed;
    }
    gate("2*foam(2max) vs same-foam", half.rel(), 1e-4, same.rel(), 0.25);
    std::printf("  foam_max=0 / foam disabled nonzero texels: %d\n", foamed);
    check(foamed == 0, "foam_max 0 or disabled produced foam");
}

// ------------------------------------------------------------- game cascades
void test_game_cascade(const bf6_water_sim_v2& s, int row)
{
    const int n = s.resolution;
    std::printf("cascade %d: source=%d res=%d tile=%.3f wind=%.3f@%.1fdeg amp=%.4g chop=%.4g "
                "foam=%d thr=%.4g max=%.4g half=%.4g\n",
                row, s.source_index, n, s.tile_dimension, s.wind_speed, s.wind_angle_degrees,
                s.wave_amplitude, s.choppiness, s.foam_enable, s.foam_threshold, s.foam_max,
                s.foam_half_life);
    const int need = bf6_water_spectrum_h0(&s, nullptr, 0);
    check(need == n * n * 2, "bf6_water_spectrum_h0 count-only size");
    bf6_water_sim_v2 invalid = s;
    invalid.resolution = 0;
    check(bf6_water_spectrum_h0(&invalid, nullptr, 0) == 0, "H0 builder accepted resolution 0");

    Cascade c;
    if (!c.initialize(s)) { check(false, "initialize on authored cascade"); return; }
    bool fin = true;
    size_t nonzero = 0;
    double energy = 0;
    for (const Complex& h : c.h0) {
        fin = fin && finite(h.r) && finite(h.i);
        if (h.r != 0 || h.i != 0) ++nonzero;
        energy += double(h.r) * h.r + double(h.i) * h.i;
    }
    std::printf("  native H0: finite=%d nonzero bins=%zu/%zu rms=%.4g\n", int(fin), nonzero,
                c.h0.size(), std::sqrt(energy / double(c.h0.size())));
    check(fin && nonzero > 0, "native H0 finite and nonzero");

    const float t = 2.5f, dt = 1.f / 30.f;
    c.evolve(t, dt);
    check(structurally_sound(c), "authored frame finite/encoding");
    if (n > 512) {
        std::printf("  sampled wave-sum comparison skipped: res %d above bounded reference size\n", n);
        return;
    }

    const double L = s.tile_dimension;
    const Spectrum S = evolved_reference(c.h0, n, L, t, -1);
    const Spectrum W = evolved_reference(c.h0, n, L, t, +1);
    const FoamModel f{ s.foam_enable != 0, s.foam_threshold, s.foam_max, s.foam_half_life, false, false };
    const double ds = n / (2.0 * L), gain = foam_gain(f), lag = foam_lag(f, dt);

    Score X, Y, Z, NX, NZ, F, cX, cY, cZ;
    Rng r{ 0xC0FFEEULL + uint64_t(row) };
    const int samples = 16;
    for (int k = 0; k < samples; ++k) {
        int px = k == 0 ? 0 : k == 1 ? n - 1 : int((r.next() * 0.5 + 0.5) * n) % n;
        int py = k == 0 ? 0 : k == 1 ? n - 1 : int((r.next() * 0.5 + 0.5) * n) % n;
        D3 patch[5][5];
        for (int oy = -2; oy <= 2; ++oy)
            for (int ox = -2; ox <= 2; ++ox) patch[oy + 2][ox + 2] = sample(S, px + ox, py + oy);
        auto P = [&](int ox, int oy) -> const D3& { return patch[oy + 2][ox + 2]; };

        const size_t i = size_t(py) * n + px;
        const Pixel& p = c.displacement[i];
        const Pixel& m = c.normal[i];
        X.add(p.x, P(0, 0).x); Y.add(p.y, P(0, 0).y); Z.add(p.z, P(0, 0).z);
        const double nx = -ds * (P(1, 0).y - P(-1, 0).y), nz = -ds * (P(0, 1).y - P(0, -1).y);
        const double len = std::sqrt(nx * nx + nz * nz + 1);
        NX.add(m.y - 0.5, 0.5 * nx / len); NZ.add(m.z - 0.5, 0.5 * nz / len);
        double foam = 0; // first frame: history is zero
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox) {
                const double fold = ds * ((P(ox + 1, oy).x - P(ox - 1, oy).x)
                                        + (P(ox, oy + 1).z - P(ox, oy - 1).z));
                const double work = (1 - lag) * std::max(0.0, fold - f.threshold) * gain;
                foam += (2 - std::abs(ox)) * (2 - std::abs(oy)) / 16.0 * work;
            }
        F.add(m.x, foam);
        const D3 w = sample(W, px, py);
        cX.add(p.x, w.x); cY.add(p.y, w.y); cZ.add(p.z, w.z);
    }
    std::printf("  %d sampled pixels at authored res, t=%.2f dt=%.4f, ref max |Y|=%.4g foam=%.4g\n",
                samples, t, dt, Y.ref, F.ref);
    gate("disp X vs wrong-sign-X", X.rel(), 2e-3, cX.rel(), 0.05);
    gate("height Y vs wrong-sign-X", Y.rel(), 2e-3, cY.rel(), 0.05);
    gate("disp Z vs wrong-sign-X", Z.rel(), 2e-3, cZ.rel(), 0.05);
    std::printf("  normal X=%.3e Z=%.3e foam=%.3e%s\n", NX.rel(), NZ.rel(), F.rel(),
                F.ref > 0 ? "" : " (foam reference zero at samples: trivial)");
    check(NX.rel() <= 2e-3 && NZ.rel() <= 2e-3, "authored normals at samples");
    check(F.ref > 0 ? F.rel() <= 2e-3 : F.err == 0, "authored foam at samples");
}

void test_game(const char* game_dir, const char* level, const char* exe)
{
    std::printf("game: %s %s\n", game_dir, level);
    char err[512] = { 0 };
    bf6_ctx* ctx = bf6_open(game_dir, err, int(sizeof(err)));
    if (!ctx) { std::printf("open failed: %s\n", err); check(false, "bf6_open"); return; }
    if (bf6_open_level(ctx, level, exe, 0, err, int(sizeof(err))) != 0) {
        std::printf("open_level failed: %s\n", err);
        check(false, "bf6_open_level"); bf6_close(ctx); return;
    }
    const int total = bf6_level_water_sims(ctx, level, nullptr, 0);
    std::printf("  enabled cascades: %d\n", total);
    if (total <= 0) {
        check(false, "level has no readable water cascades");
    } else {
        std::vector<bf6_water_sim_v2> sims(static_cast<size_t>(total));
        const int got = std::min(total, bf6_level_water_sims(ctx, level, sims.data(), total));
        for (int i = 0; i < got; ++i) test_game_cascade(sims[size_t(i)], i);
    }
    bf6_close(ctx);
}
} // namespace

int main(int argc, char** argv)
{
    test_fft2(8);
    test_fft2(16);
    test_invalid_and_zero();
    test_synthetic(8, 19.5f, 0x51D2A7F3ULL);
    test_synthetic(16, 35.5f, 0x0CEA11ULL);
    if (argc >= 3) test_game(argv[1], argv[2], argc > 3 ? argv[3] : nullptr);
    else std::printf("game: not run (usage: ocean_replay_test [<game_dir> <level> [exe]])\n");
    if (failures) std::printf("ocean_replay_test: FAILED (%d)\n", failures);
    else std::printf("ocean_replay_test: ok\n");
    return failures ? 1 : 0;
}
