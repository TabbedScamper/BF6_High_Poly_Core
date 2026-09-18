/* THE POSE-TRAJECTORY MATCHING DATABASE, FIELD BY FIELD.
 *
 *   ptm_probe <game_dir> [--dump <partition>]
 *
 * BF6's soldier locomotion is motion-matched: the gait's speed IS the
 * animation's root motion, and which animation plays is decided every tick by
 * querying a compiled database with the current pose and the desired
 * trajectory. That query is the thing standing between our walk mode and a 1:1
 * first-person controller - everything above it (tweakables, eye height, FOV,
 * the 1P rig) is already read from the install.
 *
 * WHAT IS ALREADY ESTABLISHED, and is not re-derived here:
 *   - header type 0x8CC57AFE owns compiled database 0x7ABAE1FC via 0xFE039875,
 *     41/41, with a back-reference 0xB2233B4D to the header;
 *   - the database declares 72 fields;
 *   - 1,789 source-motion descriptors across the set;
 *   - PTMMatchController returns a cost plus a packed word:
 *         mirror = word & 1
 *         clipIndex = (word >> 1) & 0x7FF
 *         clipFrame = (word >> 12) & 0x3FFF
 *
 * WHAT IS OPEN, and is what this probe attacks: "the exact feature-table field
 * mapping, scoring equation, threshold units, acceleration structure and most
 * flags remain unread."
 *
 * So this enumerates the database's fields as the game serialises them - id,
 * kind, element count and the first values - because a field mapping cannot be
 * reasoned out from a type name. It prints what is there and leaves the naming
 * to whoever can match a shape to a meaning.
 */
#include "bf6_core.h"
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

static const char* kHeaderGuid   = "82177194-1584-c52a-d0cc-0ea73c19b41b";
static const char* kDatabaseGuid = "a01b861d-a4ef-a934-7a3d-c1b43f331227";

static const char* kind_name(EbxValue::Kind k)
{
    switch (k) {
        case EbxValue::Kind::Null: return "null";
        case EbxValue::Kind::Bool: return "bool";
        case EbxValue::Kind::Int: return "int";
        case EbxValue::Kind::Uint: return "uint";
        case EbxValue::Kind::Real: return "real";
        case EbxValue::Kind::Str: return "str";
        case EbxValue::Kind::Guid: return "guid";
        case EbxValue::Kind::ResRef: return "resref";
        case EbxValue::Kind::Struct: return "struct";
        case EbxValue::Kind::Array: return "ARRAY";
        case EbxValue::Kind::InstanceRef: return "instref";
        case EbxValue::Kind::ImportRef: return "importref";
        default: return "unknown";
    }
}

/* A one-line description of a value, with enough of an array's contents to see
 * its shape. Printing three elements is deliberate: it distinguishes a
 * quantised feature row from an index table at a glance, which a count alone
 * does not. */
static std::string brief(const EbxValue& v)
{
    char b[256];
    switch (v.kind) {
        case EbxValue::Kind::Bool:
            std::snprintf(b, sizeof(b), "%s", v.b ? "true" : "false"); break;
        case EbxValue::Kind::Int: {
            // Same dual reading as Uint below: these rows are typed as
            // integers and hold float bits.
            if (v.i > 0x30000000ll && v.i <= 0xffffffffll) {
                float fv = 0.0f;
                const uint32_t bits = (uint32_t)v.i;
                std::memcpy(&fv, &bits, 4);
                if (fv == fv && fv > 1e-6f && fv < 1e9f) {
                    std::snprintf(b, sizeof(b), "%g", (double)fv);
                    break;
                }
            }
            std::snprintf(b, sizeof(b), "%lld", (long long)v.i);
            break;
        }
        case EbxValue::Kind::Uint: {
            /* Weight rows arrive typed as Uint but hold IEEE-754 bit patterns.
             * Both readings are printed rather than one chosen: a value that is
             * a plausible float AND a plausible integer should be judged by
             * whoever knows the field, not silently reinterpreted here. */
            if (v.u <= 0xffffffffull && v.u > 0x30000000ull) {
                float fv = 0.0f;
                const uint32_t bits = (uint32_t)v.u;
                std::memcpy(&fv, &bits, 4);
                if (fv == fv && fv > 1e-6f && fv < 1e9f) {
                    std::snprintf(b, sizeof(b), "%g", (double)fv);
                    break;
                }
            }
            std::snprintf(b, sizeof(b), "%llu", (unsigned long long)v.u);
            break;
        }
        case EbxValue::Kind::Real:
            std::snprintf(b, sizeof(b), "%g", v.f); break;
        case EbxValue::Kind::Str:
            std::snprintf(b, sizeof(b), "\"%s\"", v.s.c_str()); break;
        case EbxValue::Kind::ImportRef:
            std::snprintf(b, sizeof(b), "-> %s", v.import_path.c_str()); break;
        case EbxValue::Kind::ResRef:
            std::snprintf(b, sizeof(b), "res 0x%llx", (unsigned long long)v.u); break;
        case EbxValue::Kind::Array: {
            // A short array is printed WHOLE. These carry the feature list and
            // its offsets, and three of seven elements hides exactly the part
            // that matters - the shape of the vector being matched.
            const size_t cap = v.items.size() <= 24 ? v.items.size() : 8;
            std::string s = "n=" + std::to_string(v.items.size());
            if (!v.items.empty()) {
                s += "  [";
                for (size_t i = 0; i < cap; ++i) {
                    if (i) s += ", ";
                    s += brief(v.items[i]);
                }
                if (v.items.size() > cap) s += ", ...";
                s += "]";
            }
            return s;
        }
        case EbxValue::Kind::Struct: {
            // One level of nesting is printed, not a count. A weight row that
            // reads "{4 field(s)}" tells you nothing; "{0x..=1.5, 0x..=0.2}"
            // is the weighting itself.
            std::string s = "{";
            for (size_t i = 0; i < v.fields.size(); ++i) {
                if (i) s += ", ";
                char id[16];
                std::snprintf(id, sizeof(id), "%08x=", v.fields[i].first);
                s += id;
                const EbxValue& f = v.fields[i].second;
                // Recurse one more level. The per-candidate feature rows sit
                // two structs deep, and stopping at "{...}" is what kept them
                // looking like they were in a chunk somewhere else.
                if (f.kind == EbxValue::Kind::Struct) s += brief(f);
                else if (f.kind == EbxValue::Kind::Array) {
                    // Print a nested array's CONTENTS when it is short enough to
                    // read. A per-component table inside a struct is exactly
                    // where a baked scale would live, and "n=32" hides it.
                    s += "n=" + std::to_string(f.items.size());
                    if (!f.items.empty() && f.items.size() <= 200) {
                        s += "[";
                        for (size_t k = 0; k < f.items.size(); ++k) {
                            if (k) s += ",";
                            s += brief(f.items[k]);
                        }
                        s += "]";
                    }
                }
                else s += brief(f);
            }
            return s + "}";
        }
        default:
            std::snprintf(b, sizeof(b), "-"); break;
    }
    return b;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: ptm_probe <game_dir> <exe> [--dump <partition>]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string want_dump, want_any, want_weights, want_raw, want_cost;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--dump") == 0) want_dump = argv[i + 1];
        // --any dumps EVERY instance of a partition, not just a PTM database.
        // The database's own fields point at separate partitions - the cost
        // weights and the neighbour count among them - and those are where the
        // scoring equation actually lives.
        if (std::strcmp(argv[i], "--any") == 0) want_any = argv[i + 1];
        // --weights follows a database to its weights and pose-features and
        // prints the cost weights grouped by feature.
        if (std::strcmp(argv[i], "--weights") == 0) want_weights = argv[i + 1];
        if (std::strcmp(argv[i], "--raw") == 0) want_raw = argv[i + 1];
        // --cost tests the hypothesised match metric against the database's
        // own temporal structure.
        if (std::strcmp(argv[i], "--cost") == 0) want_cost = argv[i + 1];
    }

    Source src;
    std::string e;
    if (!src.open(argv[1], e)) { std::printf("source: %s\n", e.c_str()); return 1; }
    // The animation catalogue is shared rather than per level, so the whole
    // catalogue has to resolve for the PTM partitions to be reachable.
    if (!src.mount_level("mp_isolated", true, e))
        std::printf("mount warning: %s\n", e.c_str());
    TypeDb types;
    if (!types.open(argv[2], e)) { std::printf("types: %s\n", e.c_str()); return 1; }

    /* ---- --weights: the cost weights, aligned to the feature vector ----
     *
     * The weight array is serialised as 8 structs of 4. That grouping is the
     * container's, not the data's: flattened it is 32 values, one per component
     * of the 32-float feature vector, and a 3-wide joint channel carries the
     * same weight on x, y and z.
     *
     * Printing it grouped by FEATURE rather than by container row is what makes
     * that visible. Read as rows of four, consecutive channels appear to bleed
     * into each other and the data looks corrupt; read against the offsets, six
     * channels come out uniform in a row, which no misalignment produces.
     */
    if (!want_weights.empty()) {
        std::string werr;
        auto load = [&](const std::string& p, Ebx& out) -> bool {
            std::vector<uint8_t> raw = src.get_ebx(p, werr);
            if (raw.empty()) return false;
            out.set_guid_index(&src.partition_index());
            return out.parse(std::move(raw), werr);
        };
        auto strip = [](std::string s) {
            if (s.size() > 4 && s.compare(s.size() - 4, 4, ".ebx") == 0) s.resize(s.size() - 4);
            return s;
        };
        Ebx dbx(types);
        if (!load(want_weights, dbx)) { std::printf("cannot read %s: %s\n",
                                                    want_weights.c_str(), werr.c_str()); return 1; }
        std::string weights_part, feats_part;
        for (size_t i = 0; i < dbx.instance_count(); ++i) {
            if (TypeDb::guid_str(dbx.instance_type(i)) != kDatabaseGuid) continue;
            const EbxValue d = dbx.read_instance(i);
            if (const EbxValue* w = d.field(0x94db53ebu))
                if (w->kind == EbxValue::Kind::ImportRef) weights_part = strip(w->import_path);
            break;
        }
        if (weights_part.empty()) { std::printf("no weights reference on %s\n",
                                               want_weights.c_str()); return 1; }
        Ebx wbx(types);
        if (!load(weights_part, wbx)) { std::printf("cannot read %s: %s\n",
                                                    weights_part.c_str(), werr.c_str()); return 1; }
        std::vector<float> flat;
        for (size_t i = 0; i < wbx.instance_count(); ++i) {
            const EbxValue d = wbx.read_instance(i);
            if (const EbxValue* f = d.field(0xe2c0dd7du))
                if (f->kind == EbxValue::Kind::ImportRef) feats_part = strip(f->import_path);
            if (const EbxValue* a = d.field(0x51def40bu)) {
                for (const EbxValue& row : a->items)
                    for (const auto& cell : row.fields) {
                        uint32_t bits = cell.second.kind == EbxValue::Kind::Int
                            ? (uint32_t)cell.second.i : (uint32_t)cell.second.u;
                        float v = 0.0f;
                        std::memcpy(&v, &bits, 4);
                        flat.push_back(v);
                    }
            }
        }
        std::printf("weights   %s\n", weights_part.c_str());
        std::printf("features  %s\n", feats_part.c_str());
        std::printf("%zu weight value(s)\n\n", flat.size());
        if (feats_part.empty()) { std::printf("no pose-features reference\n"); return 1; }
        Ebx fbx(types);
        if (!load(feats_part, fbx)) { std::printf("cannot read %s: %s\n",
                                                  feats_part.c_str(), werr.c_str()); return 1; }
        std::vector<std::string> names;
        std::vector<long> offs;
        /* THE VECTOR IS SHORTER THAN THE WEIGHT ARRAY. The weights serialise as
         * structs of four, so the array is padded up to a multiple of 4, while
         * 0x686fc9e9 declares the real component count. On the 9-channel set
         * that is 38 against an array of 40, and taking the array length as the
         * vector length silently hands the last channel two weights that are
         * padding. 14 + 8*3 = 38 exactly. */
        long declared = 0;
        for (size_t i = 0; i < fbx.instance_count(); ++i) {
            const EbxValue d = fbx.read_instance(i);
            if (const EbxValue* n = d.field(0x686fc9e9u))
                declared = n->kind == EbxValue::Kind::Int ? (long)n->i : (long)n->u;
            if (const EbxValue* a = d.field(0xcbbdaf30u))
                for (const EbxValue& it : a->items)
                    names.push_back(it.kind == EbxValue::Kind::ImportRef
                                    ? it.import_path : std::string("?"));
            if (const EbxValue* a = d.field(0xd06adaf5u))
                for (const EbxValue& it : a->items)
                    offs.push_back(it.kind == EbxValue::Kind::Int ? (long)it.i : (long)it.u);
        }
        if (names.size() != offs.size() || names.empty()) {
            std::printf("feature list and offsets disagree (%zu vs %zu)\n",
                        names.size(), offs.size());
            return 1;
        }
        std::printf("%-46s %-10s %s\n", "feature", "range", "weights");
        long uniform = 0, multi = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            const long end = declared > 0 ? declared : (long)flat.size();
            const long lo = offs[i];
            const long hi = (i + 1 < offs.size()) ? offs[i + 1] : end;
            std::string leaf = names[i];
            const size_t slash = leaf.find_last_of('/');
            if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
            char range[32];
            std::snprintf(range, sizeof(range), "[%ld..%ld]", lo, hi - 1);
            std::string vals;
            bool same = true;
            for (long k = lo; k < hi && k < (long)flat.size(); ++k) {
                char v[32];
                std::snprintf(v, sizeof(v), "%g", (double)flat[(size_t)k]);
                if (!vals.empty()) vals += ", ";
                vals += v;
                if (flat[(size_t)k] != flat[(size_t)lo]) same = false;
            }
            if (hi - lo > 1) { if (same) ++uniform; else ++multi; }
            std::printf("%-46s %-10s %s%s\n", leaf.c_str(), range, vals.c_str(),
                        (hi - lo > 1 && same) ? "   UNIFORM" : "");
        }
        /* The control for the whole reading: a wrong alignment does not leave
         * six consecutive multi-component channels internally uniform. */
        std::printf("\n%ld of %ld multi-component channel(s) carry one weight throughout\n",
                    uniform, uniform + multi);
        return 0;
    }

    /* --raw hexdumps a partition. The trajectory schema asset decodes to three
     * fields, one of them null - and a null here means the decoder could not
     * type it, not that nothing is there. The layout it should carry is a short
     * run of small integers, which is visible by eye and invisible to a
     * decoder that does not know the field's type. */
    /* ---- --cost: TEST the hypothesised match cost against the data ----
     *
     * The weights are perfect squares, which suggests the cost is a weighted
     * sum of SQUARED differences with the square folded into the constant.
     * That is an inference from the constants, and this is the test that can
     * confirm or kill it without reading the code.
     *
     * THE PROPERTY BEING USED: a motion-matching database is temporally
     * coherent. Frame N of a clip and frame N+1 are a fraction of a second
     * apart, so under the metric the engine actually matches with, a frame's
     * nearest neighbour other than itself should overwhelmingly be its own
     * temporal neighbour. Under a wrong metric that structure degrades.
     *
     * So the same question is asked three ways and the answers compared:
     *   - the authored weights          (the hypothesis)
     *   - no weights at all             (does weighting matter?)
     *   - the authored weights SHUFFLED (does it matter that they are THESE
     *                                    weights, or just that some vary?)
     * The shuffled run is the control that separates "weighting helps" from
     * "any uneven scaling helps".
     */
    if (!want_cost.empty()) {
        std::string cerr2;
        auto load = [&](const std::string& p, Ebx& out) -> bool {
            std::vector<uint8_t> raw = src.get_ebx(p, cerr2);
            if (raw.empty()) return false;
            out.set_guid_index(&src.partition_index());
            return out.parse(std::move(raw), cerr2);
        };
        auto strip = [](std::string s) {
            if (s.size() > 4 && s.compare(s.size() - 4, 4, ".ebx") == 0) s.resize(s.size() - 4);
            return s;
        };
        auto as_float = [](const EbxValue& v) {
            uint32_t bits = v.kind == EbxValue::Kind::Int ? (uint32_t)v.i : (uint32_t)v.u;
            if (v.kind == EbxValue::Kind::Real) return (float)v.f;
            float f = 0.0f; std::memcpy(&f, &bits, 4); return f;
        };
        Ebx dbx(types);
        if (!load(want_cost, dbx)) { std::printf("cannot read %s\n", want_cost.c_str()); return 1; }
        std::string wpart;
        std::vector<std::string> motions;
        for (size_t i = 0; i < dbx.instance_count(); ++i) {
            if (TypeDb::guid_str(dbx.instance_type(i)) != kDatabaseGuid) continue;
            const EbxValue d = dbx.read_instance(i);
            if (const EbxValue* w = d.field(0x94db53ebu))
                if (w->kind == EbxValue::Kind::ImportRef) wpart = strip(w->import_path);
            if (const EbxValue* a = d.field(0xc348952du))
                for (const EbxValue& it : a->items)
                    if (it.kind == EbxValue::Kind::ImportRef) motions.push_back(strip(it.import_path));
            break;
        }
        std::vector<float> weights;
        std::string cfeats;
        if (!wpart.empty()) {
            Ebx wbx(types);
            if (load(wpart, wbx))
                for (size_t i = 0; i < wbx.instance_count(); ++i) {
                    const EbxValue d = wbx.read_instance(i);
                    if (const EbxValue* f = d.field(0xe2c0dd7du))
                        if (f->kind == EbxValue::Kind::ImportRef) cfeats = strip(f->import_path);
                    if (const EbxValue* a = d.field(0x51def40bu))
                        for (const EbxValue& row : a->items)
                            for (const auto& c2 : row.fields) weights.push_back(as_float(c2.second));
                }
        }
        if (weights.empty() || motions.empty()) {
            std::printf("need weights and source motions; got %zu weights, %zu motions\n",
                        weights.size(), motions.size());
            return 1;
        }
        /* THE ROW STRIDE IS PER DATABASE, NOT 32. This was hardcoded to 32 and
         * that is the whole of why 3p.loco.unarmed.jumps scored 0.0% under
         * every metric including no weighting: it uses the 9-channel set, 38
         * components in an array padded to 40, so a 32-float stride straddles
         * a frame boundary on every row after the first and the frames being
         * compared are not frames. A result identical across three different
         * metrics was the tell - no metric can recover order from misaligned
         * rows. Read the declared count the way --weights already does, and
         * refuse to guess if it is not there. */
        size_t DIM = 0;
        if (!cfeats.empty()) {
            Ebx fbx(types);
            if (load(cfeats, fbx))
                for (size_t i = 0; i < fbx.instance_count(); ++i) {
                    const EbxValue d = fbx.read_instance(i);
                    if (const EbxValue* n = d.field(0x686fc9e9u))
                        DIM = (size_t)(n->kind == EbxValue::Kind::Int ? (long)n->i : (long)n->u);
                }
        }
        if (DIM == 0) {
            std::printf("no declared component count (0x686fc9e9) via %s; refusing to\n"
                        "assume a stride - a wrong one silently scrambles every row\n",
                        cfeats.empty() ? "(no pose-features reference)" : cfeats.c_str());
            return 1;
        }
        std::printf("features  %s\n%zu weight value(s), %zu declared component(s)\n",
                    cfeats.c_str(), weights.size(), DIM);
        if (weights.size() < DIM) {
            std::printf("fewer weights (%zu) than components (%zu)\n", weights.size(), DIM);
            return 1;
        }

        /* Gather every candidate frame across the source motions. */
        std::vector<std::vector<float>> feat;   // one 32-float row per frame
        std::vector<int> clip_of, frame_of;
        for (size_t m = 0; m < motions.size(); ++m) {
            Ebx mbx(types);
            if (!load(motions[m], mbx)) continue;
            for (size_t i = 0; i < mbx.instance_count(); ++i) {
                const EbxValue d = mbx.read_instance(i);
                const EbxValue* a = d.field(0x0d91911du);
                if (!a || a->items.empty()) continue;
                std::vector<float> flat;
                for (const EbxValue& row : a->items)
                    for (const auto& c2 : row.fields) flat.push_back(as_float(c2.second));
                const size_t frames = flat.size() / DIM;
                for (size_t f = 0; f < frames; ++f) {
                    feat.emplace_back(flat.begin() + (size_t)(f * DIM),
                                      flat.begin() + (size_t)((f + 1) * DIM));
                    clip_of.push_back((int)m);
                    frame_of.push_back((int)f);
                }
                break;
            }
        }
        std::printf("%zu source motion(s), %zu candidate frame(s), %zu-float rows\n\n",
                    motions.size(), feat.size(), DIM);
        if (feat.size() < 8) { std::printf("too few frames to test\n"); return 1; }

        /* IS THE WEIGHTING BAKED INTO THE STORED ROWS? Every search for a
         * function that applies the weights has come back empty - no routine
         * computes a weighted sum, and none reads the features and the weights
         * together. That leaves the possibility that the rows themselves are
         * stored pre-scaled, so a plain unweighted distance over them already
         * IS the weighted distance.
         *
         * If a component were stored scaled by sqrt(weight), its magnitudes
         * would be inflated in proportion. Per-component RMS against
         * sqrt(weight) tests that on the data alone: a constant ratio down the
         * column means baked, a ratio that swings with the weight means not. */
        {
            std::printf("%-5s %11s %11s %11s   %s\n",
                        "comp", "RMS", "sqrt(w)", "RMS/sqrt(w)", "channel");
            const char* chan[32] = {
                "TrajVel.x","TrajVel.y","TrajVel.z","PastPos.x","PastPos.y","PastPos.z",
                "FuturePos.x","FuturePos.y","FuturePos.z","FutureVel.x","FutureVel.y",
                "FutureVel.z","MidFaceAngle","FutureFaceAngle",
                "Lfoot_pos.x","Lfoot_pos.y","Lfoot_pos.z","Lfoot_vel.x","Lfoot_vel.y",
                "Lfoot_vel.z","Rfoot_pos.x","Rfoot_pos.y","Rfoot_pos.z","Rfoot_vel.x",
                "Rfoot_vel.y","Rfoot_vel.z","hips_pos.x","hips_pos.y","hips_pos.z",
                "hips_vel.x","hips_vel.y","hips_vel.z" };
            double lo_r = 1e30, hi_r = 0.0;
            for (size_t k = 0; k < DIM; ++k) {
                double ss = 0.0;
                for (const std::vector<float>& row : feat)
                    ss += (double)row[k] * (double)row[k];
                const double rms = std::sqrt(ss / (double)feat.size());
                const double sw = std::sqrt((double)weights[k]);
                const double ratio = sw > 0.0 ? rms / sw : 0.0;
                if (ratio > 0.0) { lo_r = std::min(lo_r, ratio); hi_r = std::max(hi_r, ratio); }
                std::printf("%-5zu %11.4g %11.4g %11.4g   %s\n",
                            k, rms, sw, ratio, k < 32 ? chan[k] : "?");
            }
            std::printf("\nratio spread %.4g .. %.4g  (x%.1f)\n", lo_r, hi_r,
                        lo_r > 0 ? hi_r / lo_r : 0.0);
            std::printf("A baked scale would hold the ratio roughly CONSTANT.\n\n");
        }

        std::vector<float> none(DIM, 1.0f), shuf(weights.begin(), weights.begin() + DIM);
        // A fixed shuffle so the control is reproducible.
        for (size_t i = shuf.size(); i > 1; --i) {
            const size_t j = (i * 2654435761u) % i;
            std::swap(shuf[i - 1], shuf[j]);
        }
        struct Run { const char* name; const std::vector<float>* w; };
        const Run runs[] = { {"authored weights", &weights},
                             {"no weights", &none},
                             {"weights SHUFFLED (control)", &shuf} };
        std::printf("%-28s %14s %14s\n", "metric", "neighbour hit", "same clip");
        for (const Run& r : runs) {
            long adjacent = 0, same_clip = 0, total = 0;
            for (size_t q = 0; q < feat.size(); ++q) {
                double best = 1e30; size_t best_i = (size_t)-1;
                for (size_t c = 0; c < feat.size(); ++c) {
                    if (c == q) continue;
                    double s = 0.0;
                    for (size_t k = 0; k < DIM; ++k) {
                        const double d2 = (double)feat[q][k] - (double)feat[c][k];
                        s += (double)(*r.w)[k] * d2 * d2;
                    }
                    if (s < best) { best = s; best_i = c; }
                }
                if (best_i == (size_t)-1) continue;
                ++total;
                if (clip_of[best_i] == clip_of[q]) {
                    ++same_clip;
                    if (std::abs(frame_of[best_i] - frame_of[q]) == 1) ++adjacent;
                }
            }
            std::printf("%-28s %11.1f%% %13.1f%%\n", r.name,
                        total ? 100.0 * (double)adjacent / (double)total : 0.0,
                        total ? 100.0 * (double)same_clip / (double)total : 0.0);
        }
        std::printf("\n\"neighbour hit\" = the nearest OTHER frame is this frame's own\n"
                    "temporal neighbour. A database the engine matches on should score\n"
                    "high under the metric it was authored for.\n");
        return 0;
    }

    if (!want_raw.empty()) {
        std::string rerr;
        std::vector<uint8_t> raw = src.get_ebx(want_raw, rerr);
        if (raw.empty()) { std::printf("cannot read %s: %s\n", want_raw.c_str(), rerr.c_str()); return 1; }
        std::printf("=== %s: %zu bytes ===\n", want_raw.c_str(), raw.size());
        for (size_t o = 0; o < raw.size(); o += 16) {
            std::printf("%06zx  ", o);
            for (size_t i = 0; i < 16; ++i)
                if (o + i < raw.size()) std::printf("%02x ", raw[o + i]);
                else std::printf("   ");
            std::printf(" ");
            for (size_t i = 0; i < 16 && o + i < raw.size(); ++i) {
                const uint8_t ch = raw[o + i];
                std::printf("%c", (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.');
            }
            std::printf("\n");
        }
        return 0;
    }

    if (!want_any.empty()) {
        std::string aerr;
        std::vector<uint8_t> raw = src.get_ebx(want_any, aerr);
        if (raw.empty()) { std::printf("cannot read %s: %s\n", want_any.c_str(), aerr.c_str()); return 1; }
        Ebx ebx(types);
        ebx.set_guid_index(&src.partition_index());
        if (!ebx.parse(std::move(raw), aerr)) { std::printf("parse: %s\n", aerr.c_str()); return 1; }
        std::printf("=== %s: %zu instance(s) ===\n", want_any.c_str(), ebx.instance_count());
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            std::printf("--- instance %zu, type %s ---\n", i,
                        TypeDb::guid_str(ebx.instance_type(i)).c_str());
            const EbxValue d = ebx.read_instance(i);
            for (const auto& f : d.fields)
                std::printf("  0x%08x  %-10s %s\n", f.first, kind_name(f.second.kind),
                            brief(f.second).c_str());
        }
        return 0;
    }

    /* ---- find every partition carrying a PTM database ---- */
    std::vector<std::string> parts;
    for (const auto& kv : src.ebx()) {
        const std::string& n = kv.first;
        // Cheap prefilter: the naming scheme is documented and a full parse of
        // every EBX in the game to find 41 files is not worth the minutes.
        std::string low = n;
        for (char& ch : low) ch = (char)tolower((unsigned char)ch);
        if (low.find("ptm") == std::string::npos &&
            low.find("posetrajectory") == std::string::npos) continue;
        parts.push_back(n);
    }
    std::printf("partitions whose name suggests PTM: %zu\n", parts.size());

    int with_db = 0, headers = 0;
    std::map<uint32_t, std::map<std::string, long>> field_kinds; // field id -> kind -> count
    std::map<uint32_t, long> field_seen;
    std::map<uint32_t, size_t> array_max;
    std::string first_db_partition;
    // Named so the weights reading can be checked on more than one database:
    // a pattern seen once is a sample, not a rule.
    std::vector<std::string> db_partitions;

    for (const std::string& part : parts) {
        std::string perr;
        std::vector<uint8_t> raw = src.get_ebx(part, perr);
        if (raw.empty()) continue;
        Ebx ebx(types);
        ebx.set_guid_index(&src.partition_index());
        if (!ebx.parse(std::move(raw), perr)) continue;
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            const std::string g = TypeDb::guid_str(ebx.instance_type(i));
            if (g == kHeaderGuid) { ++headers; continue; }
            if (g != kDatabaseGuid) continue;
            ++with_db;
            if (first_db_partition.empty()) first_db_partition = part;
            db_partitions.push_back(part);
            const EbxValue d = ebx.read_instance(i);
            for (const auto& f : d.fields) {
                ++field_seen[f.first];
                ++field_kinds[f.first][kind_name(f.second.kind)];
                if (f.second.kind == EbxValue::Kind::Array)
                    array_max[f.first] = std::max(array_max[f.first], f.second.items.size());
            }
        }
    }
    std::printf("PTM headers %d, compiled databases %d\n\n", headers, with_db);
    if (with_db == 0) {
        std::printf("No database instance parsed - the prefilter or the type guid is wrong.\n");
        return 1;
    }

    /* ---- the field census ----
     * Present in ALL databases versus only some is the useful split: a field
     * every database carries is structural, one only a few carry is optional
     * and probably a mode or an acceleration table that not every set needs. */
    std::printf("%-12s %-5s %-10s %12s  %s\n",
                "field", "in", "kind", "max array n", "note");
    std::vector<std::pair<uint32_t, long>> order;
    for (const auto& kv : field_seen) order.emplace_back(kv.first, kv.second);
    std::sort(order.begin(), order.end(),
              [](const std::pair<uint32_t, long>& a, const std::pair<uint32_t, long>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    for (const auto& kv : order) {
        std::string kinds;
        for (const auto& k : field_kinds[kv.first]) {
            if (!kinds.empty()) kinds += "/";
            kinds += k.first;
        }
        const size_t amax = array_max.count(kv.first) ? array_max[kv.first] : 0;
        std::printf("0x%08x   %2ld/%-2d %-10s %12zu  %s\n",
                    kv.first, kv.second, with_db, kinds.c_str(), amax,
                    kv.second == with_db ? "structural" : "only some databases");
    }
    std::printf("\n%zu distinct field(s) across %d database(s)\n", field_seen.size(), with_db);
    std::printf("\nthe databases:\n");
    for (const std::string& p : db_partitions) std::printf("  %s\n", p.c_str());

    /* ---- one database in full, so the shapes are concrete ---- */
    const std::string show = want_dump.empty() ? first_db_partition : want_dump;
    std::printf("\n--- %s, every field ---\n", show.c_str());
    std::string perr;
    std::vector<uint8_t> raw = src.get_ebx(show, perr);
    if (raw.empty()) { std::printf("cannot read %s\n", show.c_str()); return 1; }
    Ebx ebx(types);
    ebx.set_guid_index(&src.partition_index());
    if (!ebx.parse(std::move(raw), perr)) { std::printf("parse: %s\n", perr.c_str()); return 1; }
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        if (TypeDb::guid_str(ebx.instance_type(i)) != kDatabaseGuid) continue;
        const EbxValue d = ebx.read_instance(i);
        for (const auto& f : d.fields)
            std::printf("  0x%08x  %-10s %s\n", f.first, kind_name(f.second.kind),
                        brief(f.second).c_str());
        break;
    }
    return 0;
}
