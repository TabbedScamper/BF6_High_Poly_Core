/* BCn decoder equivalence: the word bit reader against the per-bit reference.
 *
 * Standalone, no game install. Everything goes through the public
 * bcn_to_rgba8. BF6_BCN_REFERENCE_BITS is read by the decoder on every call,
 * so each case decodes twice in the same process: once with the variable set to
 * "1" (per-bit reference reader) and once with it removed (word reader), and
 * the success flag, error text and every RGBA byte must match.
 *
 * Route equality alone would pass if both routes were broken the same way, so
 * hand-built blocks also carry spec-derived expected pixels, several of them
 * placing a field across the bit-64 word boundary. A deliberate one-bit
 * perturbation of such a block must then FAIL its expectation, proving the
 * checks can see a single-level change.
 *
 * Covered: known answers for BC7 modes 4, 5 (both rotations), 6 and the
 * reserved mode; random blocks forced into each of the eight modes, unforced
 * random blocks, walking-one / walking-zero blocks over all 128 bit positions,
 * odd image sizes, DXGI 98 and 99; short payloads, trailing bytes, zero size
 * and unsupported formats; BC1 (both ramps), BC3, BC4 and BC5 known answers
 * and random controls.
 *
 * Exit code 0 when everything passes; prints each failure otherwise.
 *
 *   bcn_decoder_equivalence_test
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "terraincomposite.h"

using namespace bf6;

namespace {

const char* kEnv = "BF6_BCN_REFERENCE_BITS";
int g_failures = 0;
int g_checks = 0;

void fail(const std::string& label, const std::string& what)
{
    g_failures++;
    std::fprintf(stderr, "FAIL %s: %s\n", label.c_str(), what.c_str());
}

void set_reference(bool on)
{
#ifdef _WIN32
    _putenv_s(kEnv, on ? "1" : "");   // an empty value removes the variable
#else
    if (on) setenv(kEnv, "1", 1); else unsetenv(kEnv);
#endif
    const char* now = std::getenv(kEnv);
    const bool seen = now && std::strcmp(now, "1") == 0;
    if (seen != on) fail("environment", "could not switch BF6_BCN_REFERENCE_BITS");
}

struct Decoded { bool ok = false; std::string err; std::vector<uint8_t> rgba; };

Decoded decode(bool reference, const std::vector<uint8_t>& blocks, size_t nbytes,
               int w, int h, int dxgi)
{
    set_reference(reference);
    Decoded d;
    d.ok = bcn_to_rgba8(blocks.empty() ? nullptr : blocks.data(), nbytes, w, h,
                        dxgi, d.rgba, d.err);
    return d;
}

// Decodes through both routes; reports any difference and returns the word
// reader's result for further checks.
Decoded both_routes(const std::string& label, const std::vector<uint8_t>& blocks,
                    size_t nbytes, int w, int h, int dxgi)
{
    const Decoded ref = decode(true, blocks, nbytes, w, h, dxgi);
    const Decoded opt = decode(false, blocks, nbytes, w, h, dxgi);
    g_checks++;
    if (ref.ok != opt.ok)
        fail(label, std::string("ok differs: reference ") + (ref.ok ? "true" : "false"));
    else if (ref.err != opt.err)
        fail(label, "error differs: '" + ref.err + "' vs '" + opt.err + "'");
    else if (ref.ok && ref.rgba != opt.rgba)
    {
        size_t i = 0;
        while (i < ref.rgba.size() && i < opt.rgba.size() && ref.rgba[i] == opt.rgba[i]) i++;
        char msg[128];
        std::snprintf(msg, sizeof(msg), "rgba differs at byte %zu (pixel %zu channel %zu)",
                      i, i / 4, i % 4);
        fail(label, msg);
    }
    return opt;
}

Decoded both_routes(const std::string& label, const std::vector<uint8_t>& blocks,
                    int w, int h, int dxgi)
{ return both_routes(label, blocks, blocks.size(), w, h, dxgi); }

// LSB-first within each byte, the order BC7 is specified in.
struct BitWriter {
    uint8_t b[16] = {};
    int pos = 0;
    void put(uint32_t v, int n)
    {
        for (int i = 0; i < n; i++, pos++)
            if ((v >> i) & 1u) b[pos >> 3] |= (uint8_t)(1u << (pos & 7));
    }
    std::vector<uint8_t> block(const std::string& label) const
    {
        if (pos != 128) fail(label, "test block layout is not 128 bits");
        return std::vector<uint8_t>(b, b + 16);
    }
};

using Pixels = uint8_t[16][4];

// Compares a single 4x4 decode against expected pixels without reporting;
// used both for real expectations and for the perturbation probe.
bool matches(const Decoded& d, const Pixels& want, std::string* why)
{
    if (!d.ok) { if (why) *why = "decode failed: " + d.err; return false; }
    if (d.rgba.size() != 64) { if (why) *why = "wrong output size"; return false; }
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++)
            if (d.rgba[i * 4 + c] != want[i][c])
            {
                if (why)
                {
                    char msg[96];
                    std::snprintf(msg, sizeof(msg), "pixel %d channel %d = %d, want %d",
                                  i, c, d.rgba[i * 4 + c], want[i][c]);
                    *why = msg;
                }
                return false;
            }
    return true;
}

void expect_block(const std::string& label, const std::vector<uint8_t>& block,
                  int dxgi, const Pixels& want)
{
    const Decoded d = both_routes(label, block, 4, 4, dxgi);
    // The reference route has to meet the expectation as well, not just agree.
    const Decoded ref = decode(true, block, block.size(), 4, 4, dxgi);
    std::string why;
    g_checks++;
    if (!matches(d, want, &why)) fail(label + " (word reader)", why);
    if (!matches(ref, want, &why)) fail(label + " (reference reader)", why);
}

// ---- BC7 known answers -----------------------------------------------------

// Mode 6. Endpoint 0 is all zero with p-bit 0, endpoint 1 is all 127 with p-bit
// 1, giving 0 and 255 on all four channels. The two p-bits sit at bits 63 and
// 64, astride the word boundary; the 3-bit anchor index follows at 65. Index i
// is i, so pixel i is round((255 * W4[i]) / 64).
std::vector<uint8_t> mode6_block(const std::string& label)
{
    BitWriter bw;
    bw.put(1u << 6, 7);
    for (int ch = 0; ch < 4; ch++) { bw.put(0, 7); bw.put(127, 7); }
    bw.put(0, 1);   // p0, bit 63
    bw.put(1, 1);   // p1, bit 64
    bw.put(0, 3);
    for (int i = 1; i < 16; i++) bw.put((uint32_t)i, 4);
    return bw.block(label);
}

// Unperturbed (0 -> 255) this is 0 16 36 52 68 84 104 120 135 151 171 187 203
// 219 239 255; the literal is checked against the formula below in main.
void mode6_expect(Pixels& px, int e0 = 0, int e1 = 255)
{
    static const int w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++)
            px[i][c] = (uint8_t)((e0 * (64 - w4[i]) + e1 * w4[i] + 32) >> 6);
}

void bc7_known_answers()
{
    Pixels want;

    {   // mode 6, cross-word p-bits
        const std::string label = "bc7 mode 6 known answer";
        mode6_expect(want);
        expect_block(label, mode6_block(label), 98, want);
        expect_block(label + " srgb", mode6_block(label), 99, want);
    }

    // Mode 5. R1 = 127 (-> 255), everything else colour zero; A0 = 0 and
    // A1 = 0xA5, whose 8 bits occupy 58..65 across the boundary. Both index
    // sets are 1 for the anchor (weight 21) and 3 elsewhere (weight 64).
    for (int rot = 0; rot < 2; rot++)
    {
        const std::string label = rot ? "bc7 mode 5 rotation 1 known answer"
                                      : "bc7 mode 5 known answer";
        BitWriter bw;
        bw.put(1u << 5, 6);
        bw.put((uint32_t)rot, 2);
        bw.put(0, 7); bw.put(127, 7);                // R
        bw.put(0, 7); bw.put(0, 7);                  // G
        bw.put(0, 7); bw.put(0, 7);                  // B
        bw.put(0, 8); bw.put(0xA5, 8);               // A, bits 50..65
        bw.put(1, 1); for (int i = 1; i < 16; i++) bw.put(3, 2);
        bw.put(1, 1); for (int i = 1; i < 16; i++) bw.put(3, 2);
        for (int i = 0; i < 16; i++)
        {
            const uint8_t r = i ? 255 : 84;          // (255*21+32)>>6
            const uint8_t a = i ? 165 : 54;          // (165*21+32)>>6
            want[i][0] = rot ? a : r;
            want[i][1] = 0;
            want[i][2] = 0;
            want[i][3] = rot ? r : a;
        }
        expect_block(label, bw.block(label), 98, want);
    }

    {   // Mode 4 with index selector 1: colour takes the 3-bit set, alpha the
        // 2-bit set. Colour endpoints 0 and 31 (-> 255), alpha 0 and 63; the
        // 2-bit indices are all zero so alpha is 0, the 3-bit anchor is 3
        // (weight 27) and the rest 7.
        const std::string label = "bc7 mode 4 index-select known answer";
        BitWriter bw;
        bw.put(1u << 4, 5);
        bw.put(0, 2);
        bw.put(1, 1);
        for (int ch = 0; ch < 3; ch++) { bw.put(0, 5); bw.put(31, 5); }
        bw.put(0, 6); bw.put(63, 6);
        bw.put(0, 1); for (int i = 1; i < 16; i++) bw.put(0, 2);
        bw.put(3, 2); for (int i = 1; i < 16; i++) bw.put(7, 3);
        for (int i = 0; i < 16; i++)
        {
            const uint8_t v = i ? 255 : 108;         // (255*27+32)>>6
            want[i][0] = want[i][1] = want[i][2] = v;
            want[i][3] = 0;
        }
        expect_block(label, bw.block(label), 98, want);
    }

    {   // Reserved mode: no bit set in the first byte. Opaque black, whatever
        // follows.
        std::vector<uint8_t> block(16, 0xFF);
        block[0] = 0;
        for (int i = 0; i < 16; i++)
        { want[i][0] = want[i][1] = want[i][2] = 0; want[i][3] = 255; }
        expect_block("bc7 reserved mode known answer", block, 98, want);
    }
}

// A deliberate one-bit change must be caught, both by the known-answer check
// and by the same byte comparison both_routes uses.
void perturbation_is_detected()
{
    const std::string label = "bc7 perturbation";
    const std::vector<uint8_t> good = mode6_block(label);
    Pixels want;
    mode6_expect(want);

    for (const int bit : {63, 64})   // each side of the word boundary
    {
        std::vector<uint8_t> bad = good;
        bad[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
        char name[64];
        std::snprintf(name, sizeof(name), "%s bit %d", label.c_str(), bit);

        g_checks++;
        for (const bool reference : {true, false})
        {
            const Decoded d = decode(reference, bad, bad.size(), 4, 4, 98);
            if (matches(d, want, nullptr))
                fail(name, reference ? "reference reader missed a one-bit change"
                                     : "word reader missed a one-bit change");
            const Decoded base = decode(reference, good, good.size(), 4, 4, 98);
            if (d.rgba == base.rgba)
                fail(name, "byte comparison cannot see a one-bit change");
        }
        // Flipped p-bit 0 makes endpoint 0 read 1; flipped p-bit 1 makes
        // endpoint 1 read 254. The whole ramp moves accordingly.
        Pixels moved;
        if (bit == 64) mode6_expect(moved, 0, 254);
        else           mode6_expect(moved, 1, 255);
        expect_block(std::string(name) + " exact", bad, 98, moved);
    }
}

// ---- BC7 route equivalence over many blocks --------------------------------

struct Rng {
    uint64_t s;
    uint8_t next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint8_t)(s >> 24);
    }
};

std::vector<uint8_t> random_blocks(Rng& rng, size_t nblocks, size_t bb)
{
    std::vector<uint8_t> v(nblocks * bb);
    for (auto& b : v) b = rng.next();
    return v;
}

void bc7_equivalence()
{
    Rng rng{0x9E3779B97F4A7C15ull};

    // Every mode, 1024 random blocks each, forced by rewriting the mode prefix.
    for (int mode = 0; mode < 8; mode++)
    {
        std::vector<uint8_t> v = random_blocks(rng, 32 * 32, 16);
        const uint8_t low = (uint8_t)((2u << mode) - 1u);
        for (size_t i = 0; i < v.size(); i += 16)
            v[i] = (uint8_t)((v[i] & ~low) | (1u << mode));
        char label[64];
        std::snprintf(label, sizeof(label), "bc7 random mode %d", mode);
        both_routes(label, v, 128, 128, 98);
        both_routes(std::string(label) + " srgb", v, 128, 128, 99);
    }

    {   // unforced: the mode mix, including reserved blocks, as the bits fall
        std::vector<uint8_t> v = random_blocks(rng, 64 * 64, 16);
        for (size_t i = 0; i < v.size(); i += 16 * 7) v[i] = 0;
        both_routes("bc7 random mixed", v, 256, 256, 98);
    }

    {   // odd sizes: partial edge blocks are cropped, not wrapped
        const std::vector<uint8_t> v = random_blocks(rng, 16 * 10, 16);
        both_routes("bc7 random 61x37", v, 61, 37, 98);
        const std::vector<uint8_t> one = random_blocks(rng, 1, 16);
        both_routes("bc7 random 1x1", one, 1, 1, 98);
    }

    // A single set bit, and a single clear bit, at every position 0..127, under
    // every mode prefix. Every field of every mode sees an isolated 1 and 0,
    // including each field that straddles bit 64.
    for (int mode = 0; mode < 8; mode++)
    {
        std::vector<uint8_t> ones(128 * 16, 0), zeros(128 * 16, 0xFF);
        const uint8_t low = (uint8_t)((2u << mode) - 1u);
        for (int bit = 0; bit < 128; bit++)
        {
            uint8_t* a = &ones[(size_t)bit * 16];
            uint8_t* z = &zeros[(size_t)bit * 16];
            a[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
            z[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
            a[0] = (uint8_t)((a[0] & ~low) | (1u << mode));
            z[0] = (uint8_t)((z[0] & ~low) | (1u << mode));
        }
        char label[64];
        std::snprintf(label, sizeof(label), "bc7 walking bit mode %d", mode);
        both_routes(std::string(label) + " one", ones, 128 * 4, 4, 98);
        both_routes(std::string(label) + " zero", zeros, 128 * 4, 4, 98);
    }

    {   // fixed patterns
        const uint8_t fills[] = {0x00, 0xFF, 0xAA, 0x55, 0x80, 0x01};
        for (const uint8_t f : fills)
        {
            const std::vector<uint8_t> v(16, f);
            char label[48];
            std::snprintf(label, sizeof(label), "bc7 fill 0x%02X", f);
            both_routes(label, v, 4, 4, 98);
        }
    }
}

// ---- malformed input -------------------------------------------------------

void malformed()
{
    Rng rng{0x0123456789ABCDEFull};
    const std::vector<uint8_t> v = random_blocks(rng, 4 * 4, 16);   // 16x16 of BC7

    Decoded d = both_routes("bc7 short payload", v, v.size() - 1, 16, 16, 98);
    if (d.ok) fail("bc7 short payload", "accepted a payload one byte short");
    d = both_routes("bc7 empty payload", v, 0, 16, 16, 98);
    if (d.ok) fail("bc7 empty payload", "accepted an empty payload");
    d = both_routes("bc7 zero width", v, 0, 16, 98);
    if (d.ok) fail("bc7 zero width", "accepted zero width");
    d = both_routes("bc7 negative height", v, 16, -4, 98);
    if (d.ok) fail("bc7 negative height", "accepted negative height");
    d = both_routes("bc6h unsupported", v, 16, 16, 95);
    if (d.ok) fail("bc6h unsupported", "accepted BC6H");

    // Trailing bytes past the last block are ignored, identically.
    std::vector<uint8_t> longer = v;
    longer.resize(v.size() + 7, 0xEE);
    d = both_routes("bc7 trailing bytes", longer, 16, 16, 98);
    if (!d.ok) fail("bc7 trailing bytes", d.err);

    // The payload ends exactly at the last block; an overrun would be past the
    // allocation (visible under a sanitizer or debug heap).
    std::vector<uint8_t> exact = random_blocks(rng, 3, 16);
    exact[0] = 0x80;   // mode 7
    exact[16] = 0x40;  // mode 6
    exact[32] = 0x10;  // mode 4
    d = both_routes("bc7 exact-size payload", exact, 12, 4, 98);
    if (!d.ok) fail("bc7 exact-size payload", d.err);
}

// ---- BC1/BC3/BC4/BC5 controls ----------------------------------------------

void bcn_controls()
{
    Pixels want;

    {   // BC1 four-colour: red / blue, index i = i % 4
        const std::vector<uint8_t> b = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
        const uint8_t ramp[4][3] = {{255, 0, 0}, {0, 0, 255}, {170, 0, 85}, {85, 0, 170}};
        for (int i = 0; i < 16; i++)
        {
            std::memcpy(want[i], ramp[i % 4], 3);
            want[i][3] = 255;
        }
        expect_block("bc1 four-colour known answer", b, 71, want);
    }
    {   // BC1 three-colour + transparent: blue <= red, same indices
        const std::vector<uint8_t> b = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4};
        const uint8_t ramp[4][4] = {{0, 0, 255, 255}, {255, 0, 0, 255},
                                    {128, 0, 128, 255}, {0, 0, 0, 0}};
        for (int i = 0; i < 16; i++) std::memcpy(want[i], ramp[i % 4], 4);
        expect_block("bc1 punch-through known answer", b, 72, want);
    }

    // BC4 half used below: a0 200, a1 100 (eight-value ramp); pixel 0 index 1
    // (100), pixel 1 index 2 ((6*200+100+3)/7 = 186), the rest index 0 (200).
    const uint8_t bc4a[8] = {200, 100, 17, 0, 0, 0, 0, 0};
    const uint8_t bc4a_px[3] = {100, 186, 200};

    {   // BC3: that alpha, over a BC1 half whose c0 < c1 must still use four
        // colours. Index 3 everywhere: ((0 + 2*255 + 1)/3, 0, (255 + 0 + 1)/3).
        std::vector<uint8_t> b(bc4a, bc4a + 8);
        const uint8_t colour[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};
        b.insert(b.end(), colour, colour + 8);
        for (int i = 0; i < 16; i++)
        {
            want[i][0] = 170; want[i][1] = 0; want[i][2] = 85;
            want[i][3] = bc4a_px[i < 2 ? i : 2];
        }
        expect_block("bc3 known answer", b, 77, want);
    }
    {   // BC4 greyscale
        const std::vector<uint8_t> b(bc4a, bc4a + 8);
        for (int i = 0; i < 16; i++)
        {
            want[i][0] = want[i][1] = want[i][2] = bc4a_px[i < 2 ? i : 2];
            want[i][3] = 255;
        }
        expect_block("bc4 known answer", b, 80, want);
    }
    {   // BC5: R from the half above; G a0 10 <= a1 20 (six-value ramp with
        // explicit 0 and 255), pixel 0 index 6 (0), pixel 1 index 7 (255), rest
        // index 0 (10).
        std::vector<uint8_t> b(bc4a, bc4a + 8);
        const uint8_t g[8] = {10, 20, 62, 0, 0, 0, 0, 0};
        b.insert(b.end(), g, g + 8);
        const uint8_t gpx[3] = {0, 255, 10};
        for (int i = 0; i < 16; i++)
        {
            want[i][0] = bc4a_px[i < 2 ? i : 2];
            want[i][1] = gpx[i < 2 ? i : 2];
            want[i][2] = 0; want[i][3] = 255;
        }
        expect_block("bc5 known answer", b, 83, want);
    }

    Rng rng{0xC0FFEEull};
    const struct { int dxgi; size_t bb; const char* name; } fmts[] = {
        {71, 8, "bc1"}, {72, 8, "bc1 srgb"}, {77, 16, "bc3"}, {78, 16, "bc3 srgb"},
        {80, 8, "bc4"}, {83, 16, "bc5"}};
    for (const auto& f : fmts)
    {
        const std::vector<uint8_t> v = random_blocks(rng, 16 * 16, f.bb);
        both_routes(std::string(f.name) + " random", v, 63, 61, f.dxgi);
    }
}

}  // namespace

int main()
{
    {   // the mode 6 expectation formula against hand-computed values
        static const uint8_t ramp[16] = {0, 16, 36, 52, 68, 84, 104, 120,
                                         135, 151, 171, 187, 203, 219, 239, 255};
        Pixels px;
        mode6_expect(px);
        g_checks++;
        for (int i = 0; i < 16; i++)
            if (px[i][0] != ramp[i]) fail("mode 6 expectation", "formula disagrees with literal");
    }
    bc7_known_answers();
    perturbation_is_detected();
    bc7_equivalence();
    malformed();
    bcn_controls();
    set_reference(false);

    std::printf("bcn_decoder_equivalence: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
