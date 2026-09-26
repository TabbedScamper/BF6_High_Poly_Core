#include "expression_registry.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "stdio_compat.h"

namespace bf6 { namespace expression {
namespace {

struct Section {
    std::string name;
    uint32_t va = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_offset = 0;
    uint32_t raw_size = 0;
    uint32_t characteristics = 0;
};

static bool fits(const std::vector<uint8_t>& d, size_t off, size_t bytes)
{
    return off <= d.size() && bytes <= d.size() - off;
}

static uint16_t rd16(const std::vector<uint8_t>& d, size_t off)
{
    uint16_t v = 0; std::memcpy(&v, d.data() + off, sizeof(v)); return v;
}

static uint32_t rd32(const std::vector<uint8_t>& d, size_t off)
{
    uint32_t v = 0; std::memcpy(&v, d.data() + off, sizeof(v)); return v;
}

static uint64_t rd64(const std::vector<uint8_t>& d, size_t off)
{
    uint64_t v = 0; std::memcpy(&v, d.data() + off, sizeof(v)); return v;
}

/* A NUL-terminated printable C string at a file offset, or empty. The gate
 * matters: a pointer field that happens to land on arbitrary bytes must come
 * back empty rather than as a "name" made of whatever was there. */
static std::string reflected_c_string(const std::vector<uint8_t>& d, int64_t at)
{
    if (at < 0 || (size_t)at >= d.size()) return std::string();
    size_t i = (size_t)at;
    std::string s;
    while (i < d.size() && d[i] >= 0x20 && d[i] <= 0x7e && s.size() <= 191) {
        s.push_back((char)d[i]);
        ++i;
    }
    if (i >= d.size() || d[i] != 0 || s.size() < 2) return std::string();
    return s;
}

static bool executable_va(uint64_t va, uint64_t image_base,
                          const std::vector<Section>& sections)
{
    if (va < image_base) return false;
    const uint64_t rva = va - image_base;
    for (const Section& s : sections) {
        if (!(s.characteristics & 0x20000000u)) continue;
        const uint64_t span = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.va && rva < (uint64_t)s.va + span) return true;
    }
    return false;
}

static int64_t va_to_file(uint64_t va, uint64_t image_base,
                          const std::vector<Section>& sections,
                          size_t file_size)
{
    if (va < image_base) return -1;
    const uint64_t rva = va - image_base;
    for (const Section& s : sections) {
        const uint64_t span = std::max(s.virtual_size, s.raw_size);
        if (rva < s.va || rva >= (uint64_t)s.va + span) continue;
        const uint64_t file = (uint64_t)s.raw_offset + (rva - s.va);
        return file < file_size ? (int64_t)file : -1;
    }
    return -1;
}

} // namespace

bool read_descriptor_operators(const std::string& exe_path,
                               std::vector<DescriptorOperator>& out,
                               std::string& error)
{
    out.clear(); error.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) {
        error = "short executable read"; return false;
    }
    if (rd16(data, 0) != 0x5a4d) { error = "missing MZ header"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }

    for (const Section& s : sections) {
        // Registry records are writable data. Scanning executable sections
        // would create millions of instruction-immediate false candidates.
        if (!(s.characteristics & 0x80000000u) ||
            (s.characteristics & 0x20000000u)) continue;
        if (!fits(data, s.raw_offset, s.raw_size)) continue;
        const size_t begin = s.raw_offset;
        const size_t end = begin + s.raw_size;
        for (size_t at = begin; at + 32 <= end; at += 8) {
            const uint64_t implementation = rd64(data, at);
            const uint32_t key = rd32(data, at + 8);
            const uint32_t flags = rd32(data, at + 12);
            const uint64_t self = rd64(data, at + 16);
            const uint64_t zero = rd64(data, at + 24);
            const uint64_t record_va = image_base + s.va + (at - begin);
            if (self != record_va || zero != 0 || flags > 1 ||
                !executable_va(implementation, image_base, sections)) continue;
            out.push_back({key, flags, implementation, record_va});
        }
    }
    std::sort(out.begin(), out.end(), [](const DescriptorOperator& a,
                                         const DescriptorOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.record_va < b.record_va;
    });
    if (out.empty()) {
        error = "no expression descriptor records passed the structural scan";
        return false;
    }
    return true;
}

bool read_method_operators(const std::string& exe_path,
                           const std::vector<uint32_t>& query_keys,
                           std::vector<MethodOperator>& out,
                           std::string& error)
{
    out.clear(); error.clear();
    const std::set<uint32_t> wanted(query_keys.begin(), query_keys.end());
    if (wanted.empty()) return true;
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }
    if (rd16(data, 0) != 0x5a4d) { error = "missing MZ header"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }

    for (const Section& s : sections) {
        if (!(s.characteristics & 0x80000000u) ||
            (s.characteristics & 0x20000000u) ||
            !fits(data, s.raw_offset, s.raw_size)) continue;
        const size_t begin = s.raw_offset;
        const size_t end = begin + s.raw_size;
        for (size_t at = begin; at + 16 <= end; at += 8) {
            const uint64_t implementation = rd64(data, at);
            const uint32_t key = rd32(data, at + 8);
            const uint32_t flags = rd32(data, at + 12);
            if (wanted.find(key) == wanted.end() || flags > 1u ||
                !executable_va(implementation, image_base, sections)) continue;
            const uint64_t record_va = image_base + s.va + (at - begin);
            // The 32-byte descriptor registry begins with this same 16-byte
            // prefix. Exclude its independently proven self+zero suffix.
            if (at + 32 <= end && rd64(data, at + 16) == record_va &&
                rd64(data, at + 24) == 0) continue;
            MethodOperator row;
            row.implementation_va = implementation;
            row.key = key;
            row.flags = flags;
            row.record_va = record_va;
            out.push_back(row);
        }
    }
    std::sort(out.begin(), out.end(), [](const MethodOperator& a,
                                         const MethodOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.record_va < b.record_va;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const MethodOperator& a,
                                                     const MethodOperator& b) {
        return a.key == b.key && a.record_va == b.record_va;
    }), out.end());
    return true;
}

bool read_reflected_operators(const std::string& exe_path,
                              std::vector<ReflectedOperator>& out,
                              std::string& error)
{
    out.clear(); error.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }
    const Section* typeinfo = nullptr;
    for (const Section& s : sections)
        if (s.name == "typeinfo") { typeinfo = &s; break; }
    if (!typeinfo || !fits(data, typeinfo->raw_offset, typeinfo->raw_size)) {
        error = "typeinfo section is missing or truncated"; return false;
    }
    const size_t begin = typeinfo->raw_offset;
    const size_t end = begin + typeinfo->raw_size;
    for (size_t guid_at = begin + 8; guid_at + 96 <= end; guid_at += 8) {
        const uint16_t flags = rd16(data, guid_at - 4);
        const uint16_t object_size = rd16(data, guid_at - 2);
        if (flags != 0x030du || object_size != 8) continue;
        bool guid_nonzero = false;
        for (size_t i = 0; i < 16; ++i) guid_nonzero |= data[guid_at + i] != 0;
        if (!guid_nonzero) continue;
        const uint64_t namespace_va = rd64(data, guid_at + 16);
        const uint64_t array_type_va = rd64(data, guid_at + 24);
        const uint16_t alignment = rd16(data, guid_at + 32);
        const uint16_t parameter_count = rd16(data, guid_at + 34);
        const uint32_t signature = rd32(data, guid_at + 36);
        const uint64_t unused_1 = rd64(data, guid_at + 48);
        const uint64_t unused_4 = rd64(data, guid_at + 72);
        const uint64_t parameters_va = rd64(data, guid_at + 80);
        if (array_type_va != 0 || alignment != 8 || parameter_count > 256 ||
            unused_1 != 0 || unused_4 != 0 ||
            va_to_file(namespace_va, image_base, sections, data.size()) < 0 ||
            (parameter_count == 0
                ? parameters_va != 0
                : va_to_file(parameters_va, image_base, sections, data.size()) < 0))
            continue;
        ReflectedOperator row;
        row.key = rd32(data, guid_at - 8);
        row.parameter_count = parameter_count;
        row.signature = signature;
        row.descriptor_va = image_base + typeinfo->va + (guid_at - begin);
        row.parameters_va = parameters_va;

        /* The namespace record begins with a pointer to its own name. */
        {
            const int64_t ns = va_to_file(namespace_va, image_base, sections, data.size());
            if (ns >= 0 && fits(data, (size_t)ns, 8))
                row.name_space = reflected_c_string(
                    data, va_to_file(rd64(data, (size_t)ns), image_base, sections,
                                     data.size()));
        }
        /* Parameters are a packed array of 32-byte records, each beginning with
         * a pointer to its name. The stride is MEASURED, not assumed: at 32,
         * 2983 of 3029 descriptors yield exactly parameter_count readable names,
         * against 676 at every other stride tried - and 676 is just the count of
         * single-parameter descriptors, where the stride cannot matter. A wrong
         * stride drifts out of step and the agreement collapses, so that spread
         * is the proof.
         *
         * Names are taken only when the WHOLE array reads cleanly. A partial
         * array means the layout did not hold for this descriptor, and half a
         * signature invites more confident misreading than no signature. */
        if (parameter_count > 0 && parameters_va != 0) {
            const int64_t pa = va_to_file(parameters_va, image_base, sections, data.size());
            std::vector<std::string> names;
            for (uint16_t p = 0; pa >= 0 && p < parameter_count; ++p) {
                const size_t at = (size_t)pa + (size_t)p * 32u;
                if (!fits(data, at, 8)) break;
                std::string nm = reflected_c_string(
                    data, va_to_file(rd64(data, at), image_base, sections, data.size()));
                if (nm.empty()) break;
                names.push_back(nm);
            }
            if (names.size() == (size_t)parameter_count)
                row.parameter_names.swap(names);
        }
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(), [](const ReflectedOperator& a,
                                         const ReflectedOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.descriptor_va < b.descriptor_va;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const ReflectedOperator& a,
                                                     const ReflectedOperator& b) {
        return a.key == b.key;
    }), out.end());
    if (out.empty()) { error = "no reflected Function descriptors found"; return false; }
    return true;
}

bool read_key_first_operators(const std::string& exe_path,
                              std::vector<KeyFirstOperator>& out,
                              std::string& error)
{
    out.clear(); error.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }
    if (rd16(data, 0) != 0x5a4d) { error = "missing MZ header"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }

    for (const Section& s : sections) {
        // Registry records are writable data, like the other registries here.
        // Scanning executable sections would turn every instruction immediate
        // that happens to equal a key into a candidate.
        if (!(s.characteristics & 0x80000000u) ||
            (s.characteristics & 0x20000000u)) continue;
        if (!fits(data, s.raw_offset, s.raw_size)) continue;
        const size_t begin = s.raw_offset;
        const size_t end = begin + s.raw_size;
        // 8-byte stepping: the qword fields must be aligned for the record to
        // be what it claims, and it drops the candidate count fourfold.
        for (size_t at = begin; at + 32 <= end; at += 8) {
            if (rd32(data, at + 4) != 1u) continue;
            if (rd64(data, at + 16) != 0u) continue;
            const uint64_t slot = rd64(data, at + 8);
            const uint64_t impl = rd64(data, at + 24);
            // THE CHECK THAT MAKES THIS A REGISTRY AND NOT A COINCIDENCE: the
            // implementation must land in executable memory. Without it the
            // shape is loose enough to match ordinary data.
            if (!executable_va(impl, image_base, sections)) continue;
            if (va_to_file(slot, image_base, sections, data.size()) < 0) continue;
            KeyFirstOperator row;
            row.key = rd32(data, at);
            row.flags = 1;
            row.slot_va = slot;
            row.implementation_va = impl;
            row.record_va = image_base + s.va + (at - begin);
            out.push_back(row);
        }
    }
    std::sort(out.begin(), out.end(), [](const KeyFirstOperator& a,
                                         const KeyFirstOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.record_va < b.record_va;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const KeyFirstOperator& a,
                                                     const KeyFirstOperator& b) {
        return a.key == b.key;
    }), out.end());
    if (out.empty()) { error = "no key-first registry records found"; return false; }
    return true;
}

uint32_t operator_name_crc32(const uint8_t* bytes, size_t size)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i << 24;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc & 0x80000000u) ?
                    (crc << 1) ^ 0x04c11db7u : crc << 1;
            result[i] = crc;
        }
        return result;
    }();
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i)
        crc = (crc << 8) ^ table[((crc >> 24) ^ bytes[i]) & 0xffu];
    return ~crc;
}

bool read_named_builtins(const std::string& exe_path,
                         std::vector<NamedBuiltin>& out,
                         std::string& error)
{
    out.clear(); error.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) { std::fclose(f); error = "not a PE"; return false; }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }
    if (rd16(data, 0) != 0x5a4d) { error = "missing MZ header"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }

    /* 1. THE DESCRIPTORS: {implementation, key 0, flags 1} in writable data with the
     *    implementation landing in executable memory. The ZERO key is the whole
     *    point - it is what hides these from a key-shaped scan. */
    std::map<uint64_t, uint64_t> impl_of;      /* descriptor va -> implementation */
    for (const Section& s : sections) {
        if (!(s.characteristics & 0x80000000u) ||
            (s.characteristics & 0x20000000u)) continue;
        if (!fits(data, s.raw_offset, s.raw_size)) continue;
        for (size_t at = s.raw_offset; at + 16 <= (size_t)s.raw_offset + s.raw_size; at += 8) {
            if (rd32(data, at + 8) != 0u || rd32(data, at + 12) != 1u) continue;
            const uint64_t impl = rd64(data, at);
            if (!executable_va(impl, image_base, sections)) continue;
            impl_of[image_base + s.va + (at - s.raw_offset)] = impl;
        }
    }
    if (impl_of.empty()) { error = "no named-builtin descriptors found"; return false; }

    /* Where the read-only literals are, so a reference can be told from a number. */
    uint64_t rd_lo = ~0ull, rd_hi = 0;
    for (const Section& s : sections)
        if (s.name.compare(0, 6, ".rdata") == 0) {
            rd_lo = std::min(rd_lo, image_base + s.va);
            rd_hi = std::max(rd_hi, (uint64_t)image_base + s.va + s.virtual_size);
        }

    /* 2. every RIP-relative reference from code to a descriptor, and 3. the literal
     *    that same initializer references. Each byte offset is a candidate disp32 -
     *    a coarse net, but the target has to BE one of the descriptors, which no
     *    accidental displacement survives. */
    for (const Section& s : sections) {
        if (!(s.characteristics & 0x20000000u)) continue;
        if (!fits(data, s.raw_offset, s.raw_size) || s.raw_size < 8) continue;
        for (size_t i = 0; i + 4 <= s.raw_size; ++i) {
            const int32_t disp = (int32_t)rd32(data, s.raw_offset + i);
            const uint64_t target = image_base + s.va + i + 4 + (int64_t)disp;
            const auto d = impl_of.find(target);
            if (d == impl_of.end()) continue;
            /* The initializer walks the literal before returning the descriptor, so
             * the function body behind the reference is where the name is. */
            const size_t lo = i > 400 ? i - 400 : 0;
            const size_t hi = std::min((size_t)s.raw_size - 4, i + 80);
            std::string name;
            for (size_t j = lo; j <= hi && name.empty(); ++j) {
                const int32_t d2 = (int32_t)rd32(data, s.raw_offset + j);
                const uint64_t t2 = image_base + s.va + j + 4 + (int64_t)d2;
                if (t2 < rd_lo || t2 >= rd_hi) continue;
                const int64_t fo = va_to_file(t2, image_base, sections, data.size());
                if (fo < 0) continue;
                const std::string cand = reflected_c_string(data, fo);
                /* An operator name is an identifier; anything else is a different
                 * literal the same function happened to touch. */
                if (cand.size() < 3 || cand.size() > 96) continue;
                if (!(std::isalpha((unsigned char)cand[0]) || cand[0] == '_')) continue;
                bool ident = true;
                for (char c : cand)
                    if (!(std::isalnum((unsigned char)c) || c == '_')) { ident = false; break; }
                if (ident) name = cand;
            }
            if (name.empty()) continue;
            NamedBuiltin row;
            row.name = name;
            row.key = operator_name_crc32((const uint8_t*)name.data(), name.size());
            row.implementation_va = d->second;
            row.descriptor_va = target;
            out.push_back(std::move(row));
        }
    }
    std::sort(out.begin(), out.end(), [](const NamedBuiltin& a, const NamedBuiltin& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.name < b.name;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const NamedBuiltin& a,
                                                     const NamedBuiltin& b) {
        return a.key == b.key && a.name == b.name;
    }), out.end());
    /* A key reached from two DIFFERENT names would put the ambiguity back, so both
     * go rather than one being picked. Measured over the whole set: none. */
    std::vector<NamedBuiltin> kept;
    for (size_t i = 0; i < out.size();) {
        size_t j = i;
        while (j < out.size() && out[j].key == out[i].key) ++j;
        if (j - i == 1) kept.push_back(out[i]);
        i = j;
    }
    out.swap(kept);
    if (out.empty()) { error = "no named builtins resolved"; return false; }
    return true;
}

/* THE LITERAL INDEX, built once per executable. Hashing every string literal in
 * the 176 MB executable took ~400 ms, and it ran once PER GRAPH: a vehicle hosting
 * its feature graphs (thirty on the quad) spent twelve seconds opening. The index
 * keeps (key, offset, length) for every NUL-terminated literal, sorted by key; names
 * are read back from the file only for the keys a graph actually asks about. */
namespace {
struct LiteralEntry { uint32_t key; uint32_t offset; uint8_t length; };
struct LiteralIndex { std::vector<LiteralEntry> entries; bool ok = false; std::string error; };

const LiteralIndex& literal_index(const std::string& exe_path) {
    static std::mutex mutex;
    static std::map<std::string, LiteralIndex> cache;
    std::lock_guard<std::mutex> guard(mutex);
    auto it = cache.find(exe_path);
    if (it != cache.end()) return it->second;
    LiteralIndex& ix = cache[exe_path];
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { ix.error = "cannot open " + exe_path; return ix; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { std::fclose(f); ix.error = "empty executable"; return ix; }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { ix.error = "short executable read"; return ix; }
    size_t at = 0;
    while (at < data.size()) {
        if (data[at] < 0x20 || data[at] > 0x7e) { ++at; continue; }
        const size_t begin = at;
        while (at < data.size() && data[at] >= 0x20 && data[at] <= 0x7e &&
               at - begin <= 127) ++at;
        const size_t length = at - begin;
        // Registration names are ordinary C literals. Requiring the NUL is a
        // rejection gate against hashing instruction/data fragments.
        if (length >= 2 && length <= 127 && at < data.size() && data[at] == 0)
            ix.entries.push_back({operator_name_crc32(data.data() + begin, length),
                                  (uint32_t)begin, (uint8_t)length});
        // A run longer than 127 was deliberately not accepted. Advance past
        // the rest of it so suffixes cannot masquerade as separate literals.
        while (at < data.size() && data[at] >= 0x20 && data[at] <= 0x7e) ++at;
        if (at < data.size()) ++at;
    }
    std::sort(ix.entries.begin(), ix.entries.end(),
              [](const LiteralEntry& a, const LiteralEntry& b) { return a.key < b.key; });
    ix.ok = true;
    return ix;
}
} // namespace

bool resolve_named_operators(const std::string& exe_path,
                             const std::vector<uint32_t>& query_keys,
                             std::vector<NamedOperator>& out,
                             std::string& error)
{
    out.clear(); error.clear();
    std::set<uint32_t> wanted(query_keys.begin(), query_keys.end());
    if (wanted.empty()) return true;
    const LiteralIndex& ix = literal_index(exe_path);
    if (!ix.ok) { error = ix.error; return false; }
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    for (uint32_t key : wanted) {
        auto lo = std::lower_bound(ix.entries.begin(), ix.entries.end(), key,
                                   [](const LiteralEntry& e, uint32_t k) { return e.key < k; });
        std::set<std::string> names;
        for (auto e = lo; e != ix.entries.end() && e->key == key; ++e) {
            char buf[128] = {};
            std::fseek(f, (long)e->offset, SEEK_SET);
            if (std::fread(buf, 1, e->length, f) == e->length)
                names.insert(std::string(buf, e->length));
        }
        if (names.empty()) continue;
        NamedOperator row;
        row.key = key;
        row.match_count = (uint32_t)names.size();
        if (row.match_count == 1) row.name = *names.begin();
        out.push_back(std::move(row));
    }
    std::fclose(f);
    return true;
}

}} // namespace bf6::expression
