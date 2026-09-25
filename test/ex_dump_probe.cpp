/* DUMP AN EX EXPRESSION PROGRAM (an .expsop / exprigop / expcontroller asset) as
 * plain rows, so its data flow can be traced outside the engine.
 *
 *   ex_dump_probe <game> <expression asset path>
 *
 * The asset is either the program itself or an op that names it in 0x20280736.
 * Rows:
 *   IN   <index> <slot a> <slot b> <0x653bfba0 value> <game state path>
 *   DT   <slot a> <slot b>                       (the tick's delta-time slot)
 *   OP   <pc> <opcode> <kernel hash> ins <a:b ...> outs <a:b ...>
 *   K    <pool offset> <as float> <as u32>        (every constant operand used)
 * An operand whose first half is 0 is a constant-pool offset (research spec_expression 2,
 * as bf6_inspect_aim_params_read reads it). The ExProg decoder is the same one
 * inspect_input_ext.inc uses, copied because that one is file-local.
 */
#include "bf6_core.h"
#include "ant_graph.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int64_t iv(const bf6::EbxValue* v)
{
    if (!v) return 0;
    return v->kind == bf6::EbxValue::Kind::Int ? v->i : (int64_t)(int32_t)(uint32_t)v->u;
}

struct ExOp { uint32_t pc = 0, op = 0, fn = 0; std::vector<std::pair<uint32_t, uint32_t>> ins, outs; };

struct ExProg {
    std::vector<uint8_t> b;
    uint32_t cbase = 0, code = 0, clen = 0;
    std::map<uint32_t, uint32_t> rel;
    uint32_t u32(size_t o) const { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; }
    uint16_t u16(size_t o) const { uint16_t v = 0; if (o + 2 <= b.size()) std::memcpy(&v, &b[o], 2); return v; }
    bool open() {
        if (b.size() < 0x50) return false;
        cbase = ((uint32_t)(u16(0x3e) + u16(0x40) + u16(0x42)) * 4 + 0x5f) & ~0xfu;
        code = (u32(0x24) + 3 + cbase) & ~3u;
        clen = u32(0x2c) * 4;
        const uint32_t n38 = u16(0x38), n3a = u16(0x3a), n3c = u16(0x3c);
        const size_t relC = (size_t)code + clen + 8u * (n38 + n3a);
        if (relC + 8ull * n3c > b.size()) return false;
        for (uint32_t i = 0; i < n3c; ++i) rel[u32(relC + 8 * i + 4)] = u32(relC + 8 * i);
        return true;
    }
    uint32_t ku(uint32_t off) const { return u32((size_t)cbase + off); }
    std::pair<uint32_t, uint32_t> opnd(size_t o) const { return { u32(o), u32(o + 4) }; }
    bool decode(uint32_t pc, ExOp& d, uint32_t& next, std::vector<uint32_t>& targets) const {
        const size_t o = (size_t)code + pc;
        if (o + 4 > b.size()) return false;
        const uint32_t w = u32(o);
        d = ExOp{}; d.pc = pc; d.op = w & 0xff; next = w >> 8;
        auto it = rel.find(pc);
        d.fn = it == rel.end() ? 0 : it->second;
        const uint32_t op = d.op;
        if (op <= 0x13) { for (uint32_t i = 0; i <= op; ++i) d.ins.push_back(opnd(o + 0xc + 8 * i)); }
        else if (op >= 0x14 && op <= 0x1d) {
            static const int m[10][2] = { {1,1},{1,2},{1,3},{1,4},{2,0},{2,1},{3,0},{3,1},{4,0},{4,1} };
            const int ni = m[op - 0x14][0], no = m[op - 0x14][1];
            for (int i = 0; i < ni; ++i) d.ins.push_back(opnd(o + 0xc + 8 * (size_t)i));
            for (int i = 0; i < no; ++i) d.outs.push_back(opnd(o + 0xc + 8 * (size_t)(ni + i)));
        } else if (op == 0x23) {
            const uint32_t ni = u32(o + 0xc);
            size_t p = o + 0x10;
            for (uint32_t i = 0; i < ni && i < 64; ++i, p += 8) d.ins.push_back(opnd(p));
            const uint32_t no = u32(p); p += 4;
            for (uint32_t i = 0; i < no && i < 64; ++i, p += 8) d.outs.push_back(opnd(p));
        } else if (op >= 0x1e && op <= 0x22) {
            /* MOVES, read off the raw words: source then destination, one per width
             * (0x1e a byte between bool slots, 0x20 and 0x22 wider values). */
            d.ins.push_back(opnd(o + 4)); d.outs.push_back(opnd(o + 0xc));
        } else if (op >= 0x39 && op <= 0x3d) {
            /* SELECTS: condition, value if true, value if false, result - 0x39 with
             * bool constants, 0x3b with float ones, 0x3d on wider values. */
            d.ins.push_back(opnd(o + 4)); d.ins.push_back(opnd(o + 0xc)); d.ins.push_back(opnd(o + 0x14));
            d.outs.push_back(opnd(o + 0x1c));
        } else if (op == 0x26) targets.push_back(u32(o + 0xc));
        else if (op == 0x28) { const uint32_t n = u32(o + 0xc); for (uint32_t i = 0; i < n && i < 4096; ++i) targets.push_back(u32(o + 0x10 + 4 * (size_t)(n + i))); }
        else if (op == 0x29 || op == 0x2a) targets.push_back(u32(o + 4));
        return true;
    }
    std::vector<ExOp> walk() const {
        std::vector<ExOp> out;
        std::vector<uint32_t> todo{0};
        std::set<uint32_t> seen;
        while (!todo.empty()) {
            const uint32_t pc = todo.back(); todo.pop_back();
            if (pc >= clen || !seen.insert(pc).second) continue;
            ExOp d; uint32_t next = 0; std::vector<uint32_t> tg;
            if (!decode(pc, d, next, tg)) continue;
            out.push_back(d);
            if (d.op == 0x2b || d.op == 0x2c) continue;
            for (uint32_t t : tg) todo.push_back(t);
            todo.push_back(next);
        }
        return out;
    }
};
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: ex_dump_probe <game> <expression asset>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    bf6ant::Graph g(c);
    const bf6ant::Obj* ex = g.root(argv[2]);
    if (!ex) { std::printf("no asset %s\n", argv[2]); return 1; }
    if (!ex->f(0x5db2cfc4u)) {
        if (const bf6ant::Obj* p = g.resolve(ex->f(0x20280736u), ex)) ex = p;
    }
    ExProg P;
    if (const bf6::EbxValue* img = ex->f(0x5db2cfc4u))
        for (const auto& r : img->items)
            for (uint32_t h : { 0x3901db14u, 0x42fc0f5eu, 0x32a99b9cu, 0x7c8062f2u }) {
                const uint32_t v = (uint32_t)iv(r.field(h));
                for (int k = 0; k < 4; ++k) P.b.push_back((uint8_t)(v >> (8 * k)));
            }
    if (!P.open()) { std::printf("program image unreadable (%zu bytes)\n", P.b.size()); return 1; }
    std::printf("PROGRAM %s  image %zu bytes, code %u bytes\n", ex->path.c_str(), P.b.size(), P.clen);
    const bf6::EbxValue* in = ex->f(0x202e54f7u);
    const bf6::EbxValue* sl = ex->f(0x246d8578u);
    const bf6::EbxValue* dir = ex->f(0x653bfba0u);
    if (in && sl)
        for (size_t i = 0; i < in->items.size() && 2 * i + 1 < sl->items.size(); ++i) {
            const bf6ant::Obj* o = g.resolve(&in->items[i], ex);
            std::printf("IN %zu %u %u %lld %s\n", i, (uint32_t)iv(&sl->items[2 * i]), (uint32_t)iv(&sl->items[2 * i + 1]),
                        (long long)(dir && i < dir->items.size() ? iv(&dir->items[i]) : -1), o ? o->path.c_str() : "?");
        }
    std::printf("DT %u %u\n", (uint32_t)iv(ex->f(0x5d4fe63fu)), (uint32_t)iv(ex->f(0xd4c91e35u)));
    /* THE TYPED INITIALISER TABLES (FUN_142487220 over region 2 at instance build):
     * dword tables at P+0x50+4*n3e (n40 dwords) and P+0x50+4*(n3e+n40) (n42 dwords);
     * a record is w0 type id, w1-2 constructor (patched), w3-4 aux, w5-8, w9 count,
     * then `count` region offsets. */
    {
        const uint32_t n3e = P.u16(0x3e), n40 = P.u16(0x40), n42 = P.u16(0x42);
        std::printf("HDR n3e %u n40 %u n42 %u h20 %u h28 %u h30 %u h34 %u h44 %u\n", n3e, n40, n42,
                    P.u32(0x20), P.u32(0x28), P.u32(0x30), P.u32(0x34), (unsigned)P.b[0x44]);
        for (int tab = 0; tab < 2; ++tab) {
            size_t o = 0x50 + 4 * (size_t)(n3e + (tab ? n40 : 0));
            const size_t end = o + 4 * (size_t)(tab ? n42 : n40);
            while (o + 40 <= end) {
                const uint32_t cnt = P.u32(o + 36);
                std::printf("INIT%d type %08x aux %u %u w5-8 %u %u %u %u count %u offs", tab, P.u32(o),
                            P.u32(o + 12), P.u32(o + 16), P.u32(o + 20), P.u32(o + 24), P.u32(o + 28), P.u32(o + 32), cnt);
                for (uint32_t k = 0; k < cnt && k < 64; ++k) std::printf(" %u", P.u32(o + 40 + 4 * (size_t)k));
                std::printf("\n");
                o += 40 + 4 * (size_t)cnt;
            }
        }
        /* and the typed-dispatch records (P+0x50, n3e dwords, 5 per record) */
        for (size_t o = 0x50; o + 20 <= 0x50 + 4 * (size_t)n3e; o += 20)
            std::printf("TDISP %u type %08x %u %u %u %u\n", (unsigned)((o - 0x50) / 4), P.u32(o), P.u32(o + 4), P.u32(o + 8), P.u32(o + 12), P.u32(o + 16));
    }
    /* THE SLOTS THE ENGINE SEEDS BEFORE A RUN (0x68b8a237), six numbers a row: a name
     * hash, a kind, then slot a:b and a second pair. The DofReader/DofWriter handles
     * are among these slots. */
    if (const bf6::EbxValue* t = ex->f(0x68b8a237u))
        for (size_t i = 0; i + 5 < t->items.size(); i += 6) {
            std::printf("SEED");
            for (size_t k = 0; k < 6; ++k) std::printf(" %u", (uint32_t)iv(&t->items[i + k]));
            std::printf("\n");
        }
    std::set<uint32_t> ks;
    for (const ExOp& d : P.walk()) {
        std::printf("OP %u %u %08x ins", d.pc, d.op, d.fn);
        for (auto& o : d.ins) { std::printf(" %u:%u", o.first, o.second); if (o.first == 0) ks.insert(o.second); }
        std::printf(" outs");
        for (auto& o : d.outs) { std::printf(" %u:%u", o.first, o.second); if (o.first == 0) ks.insert(o.second); }
        /* THE RAW WORDS of every instruction up to its successor's pc, so forms the
         * decoder does not know can be read by eye. */
        {
            const uint32_t next = P.u32((size_t)P.code + d.pc) >> 8;
            std::printf(" raw");
            for (uint32_t k = 4; d.pc + k < next && k < 0x80; k += 4) std::printf(" %u", P.u32((size_t)P.code + d.pc + k));
        }
        std::printf("\n");
    }
    for (uint32_t k : ks) {
        const uint32_t u = P.ku(k);
        float f; std::memcpy(&f, &u, 4);
        std::printf("K %u %g %u\n", k, f, u);
    }
    return 0;
}
