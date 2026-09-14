/* Generate Portal TypeScript weapon/attachment mappings from the installed
 * game and the SDK currently on disk.
 *
 * Runtime inputs are deliberately only the retail install and SDK declaration
 * file. Research TSVs are validation oracles, never inputs to this program.
 */
#include "armory.h"
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using bf6::Ebx;
using bf6::EbxValue;
using bf6::Source;
using bf6::TypeDb;

namespace {

constexpr uint32_t F_NAME = 0x0C59FA06u;
constexpr uint32_t F_VALUE = 0x8CF424E7u;
constexpr uint32_t F_HASH = 0xC124C7D8u;
constexpr uint32_t F_EQUIPMENT = 0xBC5ECAC0u;
constexpr uint32_t F_ASSETS = 0xF69C1CABu;
constexpr uint32_t F_COST = 0x6EE865A5u;
constexpr uint32_t F_COST_MIRROR = 0xF6A09C90u;
constexpr uint32_t F_COST_CONTROL = 0xF793DB67u;
constexpr uint32_t F_BUDGET = 0x3CE3B411u;
constexpr uint32_t F_TELEMETRY = 1437461901u;
constexpr uint32_t F_NAME_SID = 4251553617u;
constexpr uint32_t F_STRING_HASH = 1026853258u;
constexpr uint32_t F_HIAO = 1029689975u;
constexpr uint32_t F_UI_ITEM = 0x8AD04A4Eu;
constexpr uint32_t F_BINARY_CHUNK = 206566248u;
constexpr uint32_t F_HISTOGRAM_CHUNK = 3183768524u;

const char* ATTACHMENT_ENUM =
    "game/glacierportal/modbuilder/library/enums/modbuilder_enum_weaponattachments";
const char* WEAPON_ENUM =
    "game/glacierportal/modbuilder/library/enums/modbuilder_enum_weapons";

const std::array<const char*, 10> SLOT_NAMES = {
    "Ammo", "Barrel", "Bottom", "Right", "Left",
    "Top", "Ergonomic", "Magazine", "Muzzle", "Scope"
};
const std::map<std::string, std::string> SLOT_CODES = {
    {"amo", "Ammo"}, {"brl", "Barrel"}, {"btm", "Bottom"},
    {"rgt", "Right"}, {"lft", "Left"}, {"top", "Top"},
    {"erg", "Ergonomic"}, {"mag", "Magazine"},
    {"mzl", "Muzzle"}, {"scp", "Scope"}, {"sca", "Scope"}
};

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(std::string s)
{
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) ++first;
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) --last;
    return s.substr(first, last - first);
}

std::string slash(std::string s)
{
    for (char& c : s) if (c == '\\') c = '/';
    return lower(std::move(s));
}

std::string strip_ebx(std::string s)
{
    s = slash(std::move(s));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".ebx") == 0) s.resize(s.size() - 4);
    return s;
}

std::string leaf(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string norm(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) if (std::isalnum(c)) out.push_back((char)std::tolower(c));
    return out;
}

std::string ts_comment(std::string s)
{
    for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
    size_t p = 0;
    while ((p = s.find("*/", p)) != std::string::npos) { s.replace(p, 2, "* /"); p += 3; }
    return s;
}

bool number(const EbxValue* v, int64_t& out)
{
    if (!v) return false;
    if (v->kind == EbxValue::Kind::Int) { out = v->i; return true; }
    if (v->kind == EbxValue::Kind::Uint) { out = (int64_t)v->u; return true; }
    return false;
}

const EbxValue* find_value(const EbxValue& v, uint32_t hash)
{
    if (const EbxValue* f = v.field(hash)) return f;
    for (const auto& kv : v.fields) if (const EbxValue* f = find_value(kv.second, hash)) return f;
    for (const EbxValue& item : v.items) if (const EbxValue* f = find_value(item, hash)) return f;
    return nullptr;
}

void collect_numbers(const EbxValue& v, std::vector<int64_t>& out)
{
    int64_t n = 0;
    if (number(&v, n)) out.push_back(n);
    for (const auto& kv : v.fields) collect_numbers(kv.second, out);
    for (const EbxValue& item : v.items) collect_numbers(item, out);
}

void collect_imports(const EbxValue& v, std::vector<std::string>& out)
{
    if (v.kind == EbxValue::Kind::ImportRef)
    {
        std::string p = v.import_path;
        if (!p.empty() && p != "<not indexed>") out.push_back(strip_ebx(p));
    }
    for (const auto& kv : v.fields) collect_imports(kv.second, out);
    for (const EbxValue& item : v.items) collect_imports(item, out);
}

std::vector<uint8_t> read_ebx(Source& src, const std::string& path, std::string& err)
{
    std::vector<uint8_t> raw = src.get_ebx(strip_ebx(path) + ".ebx", err);
    if (raw.empty()) raw = src.get_ebx(strip_ebx(path), err);
    return raw;
}

bool parse_ebx(Source& src, TypeDb& types, const std::string& path, Ebx& ebx, std::string& err)
{
    std::vector<uint8_t> raw = read_ebx(src, path, err);
    if (raw.empty()) return false;
    ebx.set_guid_index(&src.armory_partition_index());
    return ebx.parse(std::move(raw), err);
}

struct SdkEnums {
    std::vector<std::string> weapons;
    std::vector<std::string> attachments;
    std::string version;
    fs::path declarations;
};

fs::path sdk_declarations(fs::path input)
{
    if (fs::is_regular_file(input)) return input;
    const fs::path a = input / "code" / "types" / "mod" / "index.d.ts";
    const fs::path b = input / "types" / "mod" / "index.d.ts";
    if (fs::exists(a)) return a;
    if (fs::exists(b)) return b;
    return input / "index.d.ts";
}

bool parse_sdk(const fs::path& input, SdkEnums& out, std::string& err)
{
    out.declarations = sdk_declarations(input);
    std::ifstream f(out.declarations);
    if (!f) { err = "cannot open SDK declarations: " + out.declarations.string(); return false; }
    enum class State { None, Weapons, Attachments } state = State::None;
    std::regex member(R"(^\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*(?:=\s*[^,]+)?\s*,?\s*(?://.*)?$)");
    std::regex version(R"(Version:\s*([^\s*]+))");
    std::string line;
    while (std::getline(f, line))
    {
        std::smatch m;
        if (out.version.empty() && std::regex_search(line, m, version)) out.version = m[1].str();
        if (line.find("enum WeaponAttachments") != std::string::npos) { state = State::Attachments; continue; }
        if (line.find("enum Weapons") != std::string::npos) { state = State::Weapons; continue; }
        if (state != State::None && line.find('}') != std::string::npos) { state = State::None; continue; }
        if (state != State::None && std::regex_match(line, m, member))
        {
            if (state == State::Weapons) out.weapons.push_back(m[1].str());
            else out.attachments.push_back(m[1].str());
        }
    }
    if (out.weapons.empty() || out.attachments.empty())
    {
        err = "Weapons or WeaponAttachments enum was not found in " + out.declarations.string();
        return false;
    }
    return true;
}

struct EnumMember {
    std::string name;
    int64_t ordinal = -1;
    uint32_t name_hash = 0;
    std::vector<std::string> assets;
};

bool read_enum(Source& src, TypeDb& types, const std::string& path,
               std::vector<EnumMember>& out, std::string& err)
{
    Ebx e(types);
    if (!parse_ebx(src, types, path, e, err)) return false;
    for (size_t i = 0; i < e.instance_count(); ++i)
    {
        EbxValue v = e.read_instance(i);
        const EbxValue* nv = v.field(F_NAME);
        int64_t ordinal = -1, hash = 0;
        if (!nv || nv->kind != EbxValue::Kind::Str || !number(v.field(F_VALUE), ordinal)) continue;
        EnumMember row;
        row.name = nv->s;
        row.ordinal = ordinal;
        if (number(v.field(F_HASH), hash)) row.name_hash = (uint32_t)hash;
        if (const EbxValue* av = v.field(F_ASSETS)) collect_imports(*av, row.assets);
        std::sort(row.assets.begin(), row.assets.end());
        row.assets.erase(std::unique(row.assets.begin(), row.assets.end()), row.assets.end());
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), [](const EnumMember& a, const EnumMember& b) {
        return a.ordinal < b.ordinal;
    });
    return !out.empty();
}

struct WeaponMember : EnumMember {
    std::string equipment;
};

bool read_weapons(Source& src, TypeDb& types, std::vector<WeaponMember>& out, std::string& err)
{
    Ebx e(types);
    if (!parse_ebx(src, types, WEAPON_ENUM, e, err)) return false;
    for (size_t i = 0; i < e.instance_count(); ++i)
    {
        EbxValue v = e.read_instance(i);
        const EbxValue* nv = v.field(F_NAME);
        int64_t ordinal = -1, hash = 0;
        if (!nv || nv->kind != EbxValue::Kind::Str || !number(v.field(F_VALUE), ordinal)) continue;
        WeaponMember row;
        row.name = nv->s;
        row.ordinal = ordinal;
        if (number(v.field(F_HASH), hash)) row.name_hash = (uint32_t)hash;
        if (const EbxValue* eq = v.field(F_EQUIPMENT))
        {
            std::vector<std::string> imports;
            collect_imports(*eq, imports);
            if (!imports.empty()) row.equipment = imports.front();
        }
        if (const EbxValue* av = v.field(F_ASSETS))
        {
            /* One authoring link contains TWO imports: the concrete
             * attachment_<weapon> record and the WeaponAttachments enum
             * member wrapper. Keep the concrete one per array row. Counting
             * both produced a convincing but exactly doubled denominator. */
            for (const EbxValue& item : av->items)
            {
                std::vector<std::string> imports;
                collect_imports(item, imports);
                for (const std::string& p : imports)
                    if (lower(leaf(p)).rfind("attachment_", 0) == 0)
                    { row.assets.push_back(p); break; }
            }
        }
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), [](const WeaponMember& a, const WeaponMember& b) {
        return a.ordinal < b.ordinal;
    });
    return !out.empty();
}

std::string internal_id_from_equipment(const std::string& equipment)
{
    std::string p = strip_ebx(equipment);
    const std::string l = leaf(p);
    const std::string bare = l.rfind("equipment_", 0) == 0 ? l.substr(10) : l;
    const size_t marker = p.find("/weapons/");
    if (marker != std::string::npos)
    {
        std::string tail = p.substr(marker + 9);
        const size_t a = tail.find('/'), b = tail.find('/', a == std::string::npos ? a : a + 1);
        if (a != std::string::npos) return tail.substr(0, a) + "/" + bare;
        (void)b;
    }
    if (p.find("/battlepickups/") != std::string::npos) return "battlepickup/" + bare;
    return std::string("unknown/") + bare;
}

struct CostInfo {
    bool found = false;
    int cost = -1;
    bool mirror_found = false;
    bool mirror_agrees = false;
    bool negative_found = false;
    bool negative_zero = false;
};

CostInfo read_cost(Source& src, TypeDb& types, const std::string& path)
{
    CostInfo out;
    std::string err;
    Ebx e(types);
    if (!parse_ebx(src, types, path, e, err)) return out;
    for (size_t i = 0; i < e.instance_count(); ++i)
    {
        EbxValue v = e.read_instance(i);
        int64_t n = 0;
        if (!number(find_value(v, F_COST), n)) continue;
        out.found = true;
        out.cost = (int)n;
        if (const EbxValue* mv = find_value(v, F_COST_MIRROR))
        {
            std::vector<int64_t> nums;
            collect_numbers(*mv, nums);
            if (!nums.empty())
            {
                out.mirror_found = true;
                out.mirror_agrees = std::all_of(nums.begin(), nums.end(), [&](int64_t x) { return x == n; });
            }
        }
        int64_t control = 0;
        if (number(find_value(v, F_COST_CONTROL), control))
        {
            out.negative_found = true;
            out.negative_zero = control == 0;
        }
        return out;
    }
    return out;
}

int read_budget(Source& src, TypeDb& types, const std::string& equipment)
{
    if (equipment.empty()) return -1;
    std::string err;
    Ebx e(types);
    if (!parse_ebx(src, types, equipment, e, err)) return -1;
    for (size_t i = 0; i < e.instance_count(); ++i)
    {
        EbxValue v = e.read_instance(i);
        int64_t n = 0;
        if (number(find_value(v, F_BUDGET), n)) return (int)n;
    }
    return -1;
}

uint32_t u32le(const std::vector<uint8_t>& b, size_t p)
{
    if (p + 4 > b.size()) return 0;
    return (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8) |
           ((uint32_t)b[p + 2] << 16) | ((uint32_t)b[p + 3] << 24);
}

uint16_t u16le(const std::vector<uint8_t>& b, size_t p)
{
    if (p + 2 > b.size()) return 0;
    return (uint16_t)((uint16_t)b[p] | ((uint16_t)b[p + 1] << 8));
}

void utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800)
    {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 63)));
    }
    else
    {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 63)));
        out.push_back((char)(0x80 | (cp & 63)));
    }
}

std::string resolve_chunk_guid(Source& src, const bf6::TypeGuid& b)
{
    static const char h[] = "0123456789abcdef";
    std::string fwd, rev;
    for (int i = 0; i < 16; ++i) { fwd += h[b[(size_t)i] >> 4]; fwd += h[b[(size_t)i] & 15]; }
    for (int i = 15; i >= 0; --i) { rev += h[b[(size_t)i] >> 4]; rev += h[b[(size_t)i] & 15]; }
    if (src.has_chunk(fwd)) return fwd;
    if (src.has_chunk(rev)) return rev;
    return {};
}

bool load_localization(Source& src, TypeDb& types, std::map<uint32_t, std::string>& out,
                       std::string& err)
{
    Ebx e(types);
    if (!parse_ebx(src, types, "common/localization/languages/fs_us_loc", e, err)) return false;
    std::string bin_guid, hist_guid;
    for (size_t i = 0; i < e.instance_count(); ++i)
    {
        EbxValue v = e.read_instance(i);
        const EbxValue* bv = v.field(F_BINARY_CHUNK);
        const EbxValue* hv = v.field(F_HISTOGRAM_CHUNK);
        if (bv && bv->kind == EbxValue::Kind::Guid) bin_guid = resolve_chunk_guid(src, bv->guid);
        if (hv && hv->kind == EbxValue::Kind::Guid) hist_guid = resolve_chunk_guid(src, hv->guid);
    }
    if (bin_guid.empty() || hist_guid.empty()) { err = "localization chunk GUID did not resolve"; return false; }
    const std::vector<uint8_t> bin = src.get_chunk(bin_guid, err);
    const std::vector<uint8_t> hist = src.get_chunk(hist_guid, err);
    if (bin.size() < 20 || hist.size() < 524 || u32le(bin, 0) != 0x00039000u ||
        u32le(hist, 0) != 0x00039001u) { err = "localization chunk header is not supported"; return false; }
    const uint32_t count = u32le(bin, 8), table = u32le(bin, 12), data = u32le(bin, 16);
    if ((uint64_t)table + (uint64_t)count * 8u != data ||
        (uint64_t)table + ((uint64_t)count + 1u) * 8u > bin.size())
    { err = "localization table bounds failed"; return false; }
    uint32_t cp[256]{};
    for (int i = 0; i < 256; ++i) cp[i] = u16le(hist, 12 + (size_t)i * 2);
    for (int i = 0; i < 128; ++i) cp[i] = (uint32_t)i;
    for (int i = 128; i < 256; ++i) if (!cp[i]) cp[i] = 0xFFFDu;
    const size_t base = (size_t)data + 8;
    for (uint32_t i = 0; i <= count; ++i)
    {
        const uint32_t sid = u32le(bin, (size_t)table + (size_t)i * 8);
        const uint32_t off = u32le(bin, (size_t)table + (size_t)i * 8 + 4);
        size_t p = base + off;
        if (!sid || p >= bin.size()) continue;
        std::string s;
        while (p < bin.size() && bin[p]) utf8(s, cp[bin[p++]]);
        out[sid] = std::move(s);
    }
    return !out.empty();
}

int localization_markup_census(const std::string& game)
{
    std::string err;
    Source src;
    if (!src.open(game, err)) { std::cerr << "game: " << err << "\n"; return 1; }
    src.set_progress([](const char* what, int done, int total) {
        if (done == 0 || done == total || (done % 20000) == 0)
            std::cerr << "[game] " << what << " " << done << "/" << total << "\n";
        return true;
    });
    if (!src.mount_level("", true, err)) { std::cerr << "mount: " << err << "\n"; return 1; }

    TypeDb types;
    bool type_ok = false;
    for (const std::string& exe : TypeDb::exe_candidates(game))
        if (types.open(exe, err)) { type_ok = true; break; }
    if (!type_ok) { std::cerr << "types: " << err << "\n"; return 1; }
    if (types.looks_encrypted())
    {
        std::cerr << "types: encrypted executable detected; use the Steam install\n";
        return 1;
    }

    std::map<uint32_t, std::string> loc;
    if (!load_localization(src, types, loc, err))
    { std::cerr << "localization: " << err << "\n"; return 1; }

    std::map<std::string, size_t> family_counts;
    std::map<std::string, size_t> inline_tags;
    std::set<std::string> image_operands;
    size_t tagged_strings = 0, inline_strings = 0, malformed_open = 0;
    for (const auto& row : loc)
    {
        const std::string& value = row.second;
        bool tagged = false, inlined = false;
        for (size_t begin = value.find('['); begin != std::string::npos;
             begin = value.find('[', begin + 1))
        {
            const size_t end = value.find(']', begin + 1);
            if (end == std::string::npos) { ++malformed_open; break; }
            std::string body = value.substr(begin + 1, end - begin - 1);
            size_t p = 0;
            while (p < body.size() && std::isspace((unsigned char)body[p])) ++p;
            const bool closing = p < body.size() && body[p] == '/';
            if (closing) ++p;
            const size_t name_begin = p;
            while (p < body.size() &&
                   (std::isalnum((unsigned char)body[p]) || body[p] == '_')) ++p;
            if (p == name_begin) continue;
            const std::string family = lower(body.substr(name_begin, p - name_begin));
            ++family_counts[family];
            tagged = true;
            if (family == "img" || family == "icon")
            {
                ++inline_tags[value.substr(begin, end - begin + 1)];
                inlined = true;
                if (!closing && family == "img" && (p == body.size() ||
                    std::isspace((unsigned char)body[p])))
                {
                    const std::string folded = lower(value);
                    const size_t image_close = folded.find("[/img]", end + 1);
                    if (image_close != std::string::npos)
                        image_operands.insert(trim(value.substr(
                            end + 1, image_close - end - 1)));
                }
            }
        }
        if (tagged) ++tagged_strings;
        if (!inlined) continue;
        ++inline_strings;
        std::string printable = value;
        for (char& c : printable) if (c == '\r' || c == '\n') c = ' ';
        std::cout << "ROW\t0x" << std::hex << std::setw(8) << std::setfill('0')
                  << row.first << std::dec << "\t" << printable << "\n";
    }

    std::cout << "SUMMARY\tlocalization_strings\t" << loc.size() << "\n"
              << "SUMMARY\ttagged_strings\t" << tagged_strings << "\n"
              << "SUMMARY\tinline_strings\t" << inline_strings << "\n"
              << "SUMMARY\tmalformed_open_brackets\t" << malformed_open << "\n";
    for (const auto& family : family_counts)
        std::cout << "FAMILY\t" << family.first << "\t" << family.second << "\n";
    for (const auto& tag : inline_tags)
        std::cout << "INLINE\t" << tag.second << "\t" << tag.first << "\n";
    auto print_asset_matches = [&](const std::string& operand) {
        const std::string wanted = norm(operand);
        size_t exact_leaf = 0, substring = 0;
        for (const auto& asset : src.ebx())
        {
            const std::string path = strip_ebx(asset.first);
            if (norm(leaf(path)) == wanted)
            {
                ++exact_leaf;
                std::cout << "ASSET\t" << operand << "\tebx-exact\t"
                          << asset.first << "\n";
            }
            if (lower(path).find(lower(operand)) != std::string::npos) ++substring;
        }
        for (const auto& asset : src.res())
        {
            const std::string path = asset.first;
            if (norm(leaf(path)) == wanted)
            {
                ++exact_leaf;
                std::cout << "ASSET\t" << operand << "\tres-exact\t"
                          << asset.first << "\n";
            }
            if (lower(path).find(lower(operand)) != std::string::npos) ++substring;
        }
        std::cout << "OPERAND\t" << operand << "\texact-leaf\t" << exact_leaf
                  << "\tpath-substring\t" << substring << "\n";
    };
    for (const std::string& operand : image_operands) print_asset_matches(operand);
    print_asset_matches("__bf6_inline_control_missing__");
    return 0;
}

uint32_t localized_sid(Ebx& e, const EbxValue* v)
{
    if (!v) return 0;
    EbxValue owned;
    if (v->kind == EbxValue::Kind::InstanceRef && v->instance >= 0 &&
        v->instance < (int)e.instance_count())
    {
        owned = e.read_instance((size_t)v->instance);
        v = &owned;
    }
    int64_t n = 0;
    return number(find_value(*v, F_STRING_HASH), n) ? (uint32_t)n : 0;
}

uint32_t top_level_sid(Ebx& e, size_t instance, const EbxValue& v)
{
    uint32_t sid = localized_sid(e, v.field(F_NAME_SID));
    if (!sid)
    {
        const int32_t target = e.int_pointer(instance, F_NAME_SID);
        if (target >= 0 && target < (int32_t)e.instance_count())
        {
            EbxValue loc = e.read_instance((size_t)target);
            sid = localized_sid(e, &loc);
        }
    }
    return sid;
}

struct DisplayRow {
    std::string debug;
    std::string display;
    std::string slot;
    int order = -1;
};

std::map<std::string, std::vector<DisplayRow>> read_aam(
    Source& src, TypeDb& types, const std::string& weapon_bare,
    const std::map<uint32_t, std::string>& loc)
{
    std::map<std::string, std::vector<DisplayRow>> by_key;
    const std::string suffix = "/aam_" + lower(weapon_bare);
    std::string path;
    for (const auto& kv : src.ebx())
    {
        std::string p = strip_ebx(kv.first);
        if (p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0)
        { path = p; break; }
    }
    if (path.empty()) return by_key;
    std::string err;
    Ebx e(types);
    if (!parse_ebx(src, types, path, e, err)) return by_key;
    for (size_t i = 0; i < e.instance_count(); ++i)
    {
        EbxValue v = e.read_instance(i);
        const EbxValue* tv = v.field(F_TELEMETRY);
        if (!tv || tv->kind != EbxValue::Kind::Str || tv->s.empty()) continue;
        DisplayRow row;
        row.debug = tv->s;
        const uint32_t sid = top_level_sid(e, i, v);
        const auto lit = loc.find(sid);
        if (lit != loc.end()) row.display = lit->second;
        std::string debug = tv->s;
        size_t first = debug.find(" - ");
        std::string tail = first == std::string::npos ? debug : debug.substr(first + 3);
        size_t second = tail.find(" - ");
        if (second != std::string::npos)
        {
            const std::string code = lower(tail.substr(0, second));
            if (SLOT_CODES.count(code)) row.slot = code;
            tail = tail.substr(second + 3);
        }
        by_key[norm(tail)].push_back(row);
        if (!row.slot.empty()) by_key[norm(row.slot + tail)].push_back(row);
    }
    return by_key;
}

struct ConcreteParts {
    std::string bare;
    std::string slot;
    std::string token;
};

ConcreteParts concrete_parts(const std::string& path)
{
    ConcreteParts out;
    const std::string l = lower(leaf(strip_ebx(path)));
    if (l.rfind("attachment_", 0) != 0) return out;
    std::string rest = l.substr(11);
    const size_t a = rest.find('_');
    const size_t b = a == std::string::npos ? a : rest.find('_', a + 1);
    if (a == std::string::npos || b == std::string::npos) return out;
    out.bare = rest.substr(0, a);
    out.slot = rest.substr(a + 1, b - a - 1);
    out.token = rest.substr(b + 1);
    return out;
}

struct NameMatch {
    std::string display;
    std::string debug;
    bool exact = false;
    bool ambiguous = false;
    bool identity_fallback = false;
};

NameMatch match_display(const std::map<std::string, std::vector<DisplayRow>>& aam,
                        const ConcreteParts& p)
{
    NameMatch out;
    const std::array<std::string, 2> keys = {norm(p.slot + p.token), norm(p.token)};
    for (size_t k = 0; k < keys.size(); ++k)
    {
        const auto it = aam.find(keys[k]);
        if (it == aam.end() || it->second.empty()) continue;
        std::set<std::pair<std::string, std::string>> unique;
        for (const DisplayRow& row : it->second)
            if (row.slot.empty() || row.slot == p.slot) unique.insert({row.display, row.debug});
        if (unique.empty()) continue;
        out.ambiguous = unique.size() != 1;
        out.display = unique.begin()->first;
        out.debug = unique.begin()->second;
        out.exact = k == 0 && !out.ambiguous;
        return out;
    }
    return out;
}

bool find_hiao(const EbxValue& v, std::string& out)
{
    if (const EbxValue* h = v.field(F_HIAO))
    {
        std::vector<std::string> imports;
        collect_imports(*h, imports);
        if (!imports.empty()) { out = imports.front(); return true; }
    }
    for (const auto& kv : v.fields) if (find_hiao(kv.second, out)) return true;
    for (const EbxValue& item : v.items) if (find_hiao(item, out)) return true;
    return false;
}

std::string internal_id_from_hiao(const std::string& hiao)
{
    std::string p = strip_ebx(hiao);
    std::string bare = leaf(p);
    if (bare.rfind("hiao_", 0) == 0) bare = bare.substr(5);
    const size_t marker = p.find("/weapons/");
    if (marker != std::string::npos)
    {
        std::string tail = p.substr(marker + 9);
        const size_t a = tail.find('/');
        if (a != std::string::npos) return tail.substr(0, a) + "/" + bare;
    }
    const size_t battle = p.find("/battlepickups/");
    if (battle != std::string::npos)
    {
        const std::string tail = p.substr(battle + 15);
        const size_t end = tail.find('/');
        return "battlepickup/" + tail.substr(0, end);
    }
    const size_t tools = p.find("/tools/");
    if (tools != std::string::npos)
    {
        const std::string tail = p.substr(tools + 7);
        const size_t end = tail.find('/');
        return "tools/" + tail.substr(0, end);
    }
    return std::string("unknown/") + bare;
}

std::string internal_id_from_ui_item(const EbxValue& v)
{
    const EbxValue* item = find_value(v, F_UI_ITEM);
    if (!item) return {};
    std::vector<std::string> imports;
    collect_imports(*item, imports);
    for (const std::string& imported : imports)
    {
        const std::string p = strip_ebx(imported);
        const size_t marker = p.find("/hardware/gadgets/");
        if (marker == std::string::npos) continue;
        const std::string tail = p.substr(marker + 18);
        const size_t a = tail.find('/'), b = tail.find('/', a == std::string::npos ? a : a + 1);
        if (a != std::string::npos && b != std::string::npos)
            return tail.substr(0, a) + "/" + tail.substr(a + 1, b - a - 1);
    }
    return {};
}

std::map<std::string, std::string> read_weapon_display_names(
    Source& src, TypeDb& types, const std::map<uint32_t, std::string>& loc)
{
    std::map<std::string, std::string> out;
    std::vector<std::string> paths;
    for (const auto& kv : src.ebx())
        if (lower(kv.first).find("uiweaponabilitymetadata") != std::string::npos)
            paths.push_back(strip_ebx(kv.first));
    std::sort(paths.begin(), paths.end());
    for (const std::string& path : paths)
    {
        std::string err;
        Ebx e(types);
        if (!parse_ebx(src, types, path, e, err)) continue;
        for (size_t i = 0; i < e.instance_count(); ++i)
        {
            EbxValue v = e.read_instance(i);
            std::string hiao;
            if (!find_hiao(v, hiao)) continue;
            const uint32_t sid = top_level_sid(e, i, v);
            const auto it = loc.find(sid);
            if (it == loc.end() || it->second.empty()) continue;
            std::string id = internal_id_from_ui_item(v);
            if (id.empty()) id = internal_id_from_hiao(hiao);
            std::string& display = out[id];
            if (display.empty()) display = it->second;
            else if (display != it->second)
            {
                std::set<std::string> names = {display, it->second};
                display.clear();
                for (const std::string& name : names)
                { if (!display.empty()) display += " / "; display += name; }
            }
        }
    }
    return out;
}

std::string variable_name(const std::string& prefix)
{
    static const std::map<std::string, std::string> names = {
        {"AssaultRifle", "assaultRiflesMapping"}, {"BattlePickup", "battlePickupsMapping"},
        {"Carbine", "carbinesMapping"}, {"DMR", "dmrsMapping"}, {"LMG", "lmgsMapping"},
        {"Shotgun", "shotgunsMapping"}, {"Sidearm", "sidearmsMapping"},
        {"SMG", "smgsMapping"}, {"Sniper", "sniperRiflesMapping"}
    };
    const auto it = names.find(prefix);
    if (it != names.end()) return it->second;
    std::string s = prefix;
    if (!s.empty()) s[0] = (char)std::tolower((unsigned char)s[0]);
    return s + "sMapping";
}

std::string prefix_of(const std::string& symbol)
{
    const size_t p = symbol.find('_');
    return p == std::string::npos ? symbol : symbol.substr(0, p);
}

struct Link {
    std::string attachment;
    std::string concrete;
    std::string slot;
    CostInfo cost;
    NameMatch name;
};

std::string cost_comment(const Link& link)
{
    std::ostringstream s;
    if (link.cost.found) s << link.cost.cost << (link.cost.cost == 1 ? " point" : " points");
    else s << "POINTS UNRESOLVED";
    s << " -- ";
    if (!link.name.display.empty()) s << ts_comment(link.name.display);
    else if (!link.name.debug.empty()) s << ts_comment(link.name.debug) << " (localized name unresolved)";
    else s << "IN-GAME NAME UNRESOLVED";
    if (link.name.ambiguous) s << " [ambiguous UI match]";
    if (link.name.identity_fallback) s << " [name joined through live attachment identity]";
    return s.str();
}

void emit_weapon(std::ostream& o, const WeaponMember& weapon,
                 const std::vector<Link>& links, const std::string& display,
                 int budget, const std::string& indent = "    ", bool commented = false,
                 bool comment_links = false)
{
    const std::string c = commented ? "// " : "";
    o << indent << c << "{\n";
    o << indent << c << "    weapon: mod.Weapons." << weapon.name << ",";
    if (!display.empty()) o << " // " << ts_comment(display);
    if (budget >= 0) o << " -- " << budget << " attachment points total";
    else o << " -- attachment-point budget unavailable";
    o << "\n";
    o << indent << c << "    attachment: {\n";
    for (const char* slot : SLOT_NAMES)
    {
        o << indent << c << "        " << slot << ": [\n";
        for (const Link& link : links) if (link.slot == slot)
            o << indent << c << (comment_links ? "            // " : "            ")
              << "mod.WeaponAttachments." << link.attachment
              << ", // " << cost_comment(link) << "\n";
        o << indent << c << "        ],\n";
    }
    o << indent << c << "    },\n";
    o << indent << c << "},\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--localization-markup-census")
        return localization_markup_census(argv[2]);
    if (argc != 4)
    {
        std::cerr << "usage: portal_armory_codegen <Battlefield 6 game dir> <SDK root|index.d.ts> <output.ts>\n"
                  << "   or: portal_armory_codegen --localization-markup-census <Battlefield 6 game dir>\n";
        return 2;
    }
    const std::string game = argv[1];
    const fs::path sdk_input = argv[2];
    const fs::path output = argv[3];
    std::string err;

    SdkEnums sdk;
    if (!parse_sdk(sdk_input, sdk, err)) { std::cerr << "SDK: " << err << "\n"; return 1; }

    Source src;
    if (!src.open(game, err)) { std::cerr << "game: " << err << "\n"; return 1; }
    src.set_progress([](const char* what, int done, int total) {
        if (done == 0 || done == total || (done % 20000) == 0)
            std::cerr << "[game] " << what << " " << done << "/" << total << "\n";
        return true;
    });
    if (!src.mount_level("", true, err)) { std::cerr << "mount: " << err << "\n"; return 1; }

    TypeDb types;
    bool type_ok = false;
    for (const std::string& exe : TypeDb::exe_candidates(game))
        if (types.open(exe, err)) { type_ok = true; break; }
    if (!type_ok) { std::cerr << "types: " << err << "\n"; return 1; }
    if (types.looks_encrypted())
    {
        std::cerr << "types: encrypted executable detected; use the Steam install\n";
        return 1;
    }

    std::cerr << "[game] building bounded armory GUID index\n";
    const size_t indexed = src.armory_partition_index().size();

    std::vector<EnumMember> attachments;
    std::vector<WeaponMember> weapons;
    if (!read_enum(src, types, ATTACHMENT_ENUM, attachments, err))
    { std::cerr << "attachment enum: " << err << "\n"; return 1; }
    if (!read_weapons(src, types, weapons, err))
    { std::cerr << "weapon enum: " << err << "\n"; return 1; }

    std::map<std::string, std::string> concrete_to_attachment;
    for (const EnumMember& a : attachments)
        for (const std::string& p : a.assets)
            concrete_to_attachment.emplace(strip_ebx(p), a.name);

    std::set<std::string> sdk_weapons(sdk.weapons.begin(), sdk.weapons.end());
    std::set<std::string> sdk_attachments(sdk.attachments.begin(), sdk.attachments.end());
    std::set<std::string> live_weapons, live_attachments;
    for (const WeaponMember& w : weapons) live_weapons.insert(w.name);
    for (const EnumMember& a : attachments) live_attachments.insert(a.name);

    std::map<uint32_t, std::string> loc;
    if (!load_localization(src, types, loc, err))
    { std::cerr << "localization: " << err << "\n"; return 1; }
    const std::map<std::string, std::string> weapon_names = read_weapon_display_names(src, types, loc);

    std::map<std::string, CostInfo> costs;
    std::map<std::string, int> budgets;
    std::map<std::string, std::map<std::string, std::vector<DisplayRow>>> aam_cache;
    std::map<std::string, std::vector<Link>> links_by_weapon;
    size_t raw_links = 0, resolved_links = 0, fake_resolved = 0;
    size_t names_resolved = 0, names_ambiguous = 0;
    size_t cost_found = 0, mirror_found = 0, mirror_agree = 0, neg_found = 0, neg_zero = 0;
    std::set<std::pair<std::string, std::string>> distinct_pairs;

    for (const WeaponMember& w : weapons)
    {
        const std::string wid = internal_id_from_equipment(w.equipment);
        const std::string bare = wid.substr(wid.find('/') + 1);
        budgets[w.name] = read_budget(src, types, w.equipment);
        auto& aam = aam_cache[bare];
        if (aam.empty()) aam = read_aam(src, types, bare, loc);
        std::map<std::string, Link> unique;
        for (const std::string& concrete0 : w.assets)
        {
            ++raw_links;
            const std::string concrete = strip_ebx(concrete0);
            const auto ait = concrete_to_attachment.find(concrete);
            if (ait == concrete_to_attachment.end()) continue;
            ++resolved_links;
            if (concrete_to_attachment.count("control/not-real/" + leaf(concrete))) ++fake_resolved;
            distinct_pairs.insert({w.name, ait->second});
            Link link;
            link.attachment = ait->second;
            link.concrete = concrete;
            const ConcreteParts parts = concrete_parts(concrete);
            const auto sit = SLOT_CODES.find(parts.slot);
            link.slot = sit == SLOT_CODES.end() ? prefix_of(link.attachment) : sit->second;
            auto cit = costs.find(concrete);
            if (cit == costs.end()) cit = costs.emplace(concrete, read_cost(src, types, concrete)).first;
            link.cost = cit->second;
            link.name = match_display(aam, parts);
            if (link.cost.found) ++cost_found;
            if (link.cost.mirror_found) ++mirror_found;
            if (link.cost.mirror_agrees) ++mirror_agree;
            if (link.cost.negative_found) ++neg_found;
            if (link.cost.negative_zero) ++neg_zero;
            if (!link.name.display.empty()) ++names_resolved;
            if (link.name.ambiguous) ++names_ambiguous;
            auto u = unique.find(link.attachment);
            if (u == unique.end()) unique.emplace(link.attachment, std::move(link));
            else if (u->second.cost.cost != link.cost.cost)
                u->second.name.debug += " [duplicate concrete row has different cost " +
                    std::to_string(link.cost.cost) + "]";
        }
        for (auto& kv : unique) links_by_weapon[w.name].push_back(std::move(kv.second));
        std::sort(links_by_weapon[w.name].begin(), links_by_weapon[w.name].end(),
                  [](const Link& a, const Link& b) {
                      if (a.slot != b.slot) return a.slot < b.slot;
                      return a.attachment < b.attachment;
                  });
    }

    /* A concrete filename and an AAM authoring name are different vocabularies.
     * For rows that do not join directly, use the live WeaponAttachments enum
     * identity only when every directly resolved occurrence of that identity
     * agrees on one localized display name. This is still a live typed join,
     * not prettification of the SDK symbol. */
    std::map<std::string, std::set<std::string>> display_consensus;
    for (const auto& wk : links_by_weapon)
        for (const Link& link : wk.second)
            if (!link.name.display.empty() && !link.name.ambiguous)
                display_consensus[link.attachment].insert(link.name.display);
    for (auto& wk : links_by_weapon)
        for (Link& link : wk.second)
            if ((link.name.display.empty() || link.name.ambiguous) &&
                display_consensus[link.attachment].size() == 1)
            {
                link.name.display = *display_consensus[link.attachment].begin();
                link.name.ambiguous = false;
                link.name.identity_fallback = true;
            }
    names_resolved = names_ambiguous = 0;
    for (const auto& wk : links_by_weapon) for (const Link& link : wk.second)
    {
        if (!link.name.display.empty()) ++names_resolved;
        if (link.name.ambiguous) ++names_ambiguous;
    }

    /* The game-visible roster is broader than mod.Weapons. Build commented
     * blocks for those items from the same live attachment_* records. */
    std::vector<std::string> mounted_names;
    mounted_names.reserve(src.ebx().size());
    for (const auto& kv : src.ebx()) mounted_names.push_back(strip_ebx(kv.first));
    const bf6::Armory game_armory = bf6::armory_from_names(mounted_names);
    std::map<std::string, const bf6::ArmoryWeapon*> armory_by_id;
    for (const bf6::ArmoryWeapon& w : game_armory.weapons)
        armory_by_id.emplace(w.cls + "/" + w.name, &w);

    std::map<std::string, std::set<std::string>> symbol_by_slot_display;
    for (const auto& wk : links_by_weapon) for (const Link& link : wk.second)
        if (!link.name.display.empty())
            symbol_by_slot_display[link.slot + "\n" + link.name.display].insert(link.attachment);

    struct MissingWeapon {
        std::string id, display;
        std::vector<Link> links;
        int budget = -1;
    };
    std::vector<MissingWeapon> missing_weapons;
    std::map<std::string, std::vector<Link>> unlisted_links_by_weapon;
    std::set<std::string> live_ids;
    for (const WeaponMember& w : weapons) live_ids.insert(internal_id_from_equipment(w.equipment));
    size_t missing_concrete = 0, missing_symbols = 0, display_symbol_joins = 0;
    size_t shuffled_display_symbol_joins = 0;
    for (const auto& named : weapon_names) if (!live_ids.count(named.first))
    {
        MissingWeapon mw;
        mw.id = named.first;
        mw.display = named.second;
        const auto awit = armory_by_id.find(mw.id);
        if (awit != armory_by_id.end())
        {
            const bf6::ArmoryWeapon& aw = *awit->second;
            const auto& aam = aam_cache[aw.name].empty()
                ? (aam_cache[aw.name] = read_aam(src, types, aw.name, loc))
                : aam_cache[aw.name];
            const std::string equipment = strip_ebx(aw.dir) + "/equipment_" + aw.name;
            mw.budget = read_budget(src, types, equipment);
            for (const bf6::ArmoryAttachment& aa : aw.attachments)
            {
                if (aa.ebx.empty()) continue;
                ++missing_concrete;
                Link link;
                link.concrete = strip_ebx(aa.ebx);
                const auto symbol = concrete_to_attachment.find(link.concrete);
                const auto slot = SLOT_CODES.find(aa.slot);
                link.slot = slot == SLOT_CODES.end() ? aa.slot : slot->second;
                auto cit = costs.find(link.concrete);
                if (cit == costs.end()) cit = costs.emplace(link.concrete, read_cost(src, types, link.concrete)).first;
                link.cost = cit->second;
                link.name = match_display(aam, concrete_parts(link.concrete));
                if (symbol != concrete_to_attachment.end()) link.attachment = symbol->second;
                else
                {
                    const auto same_name = symbol_by_slot_display.find(link.slot + "\n" + link.name.display);
                    if (!link.name.display.empty() && same_name != symbol_by_slot_display.end() &&
                        same_name->second.size() == 1)
                    {
                        link.attachment = *same_name->second.begin();
                        ++display_symbol_joins;
                    }
                    else
                    {
                        link.attachment = "<NO_MEMBER_FOR_" + norm(aa.slot + "_" + aa.name) + ">";
                        ++missing_symbols;
                    }
                    const auto pos = std::find(SLOT_NAMES.begin(), SLOT_NAMES.end(), link.slot);
                    if (pos != SLOT_NAMES.end())
                    {
                        const size_t shuffled = ((size_t)(pos - SLOT_NAMES.begin()) + 1) % SLOT_NAMES.size();
                        const auto control = symbol_by_slot_display.find(
                            std::string(SLOT_NAMES[shuffled]) + "\n" + link.name.display);
                        if (!link.name.display.empty() && control != symbol_by_slot_display.end() &&
                            control->second.size() == 1) ++shuffled_display_symbol_joins;
                    }
                }
                if ((link.name.display.empty() || link.name.ambiguous) &&
                    display_consensus[link.attachment].size() == 1)
                {
                    link.name.display = *display_consensus[link.attachment].begin();
                    link.name.ambiguous = false;
                    link.name.identity_fallback = true;
                }
                mw.links.push_back(std::move(link));
            }
            std::sort(mw.links.begin(), mw.links.end(), [](const Link& a, const Link& b) {
                if (a.slot != b.slot) return a.slot < b.slot;
                return a.attachment < b.attachment;
            });
        }
        missing_weapons.push_back(std::move(mw));
    }

    for (const WeaponMember& w : weapons) if (w.assets.empty())
    {
        const std::string wid = internal_id_from_equipment(w.equipment);
        const auto awit = armory_by_id.find(wid);
        if (awit == armory_by_id.end()) continue;
        const bf6::ArmoryWeapon& aw = *awit->second;
        const auto& aam = aam_cache[aw.name].empty()
            ? (aam_cache[aw.name] = read_aam(src, types, aw.name, loc))
            : aam_cache[aw.name];
        for (const bf6::ArmoryAttachment& aa : aw.attachments)
        {
            if (aa.ebx.empty()) continue;
            Link link;
            link.concrete = strip_ebx(aa.ebx);
            const auto symbol = concrete_to_attachment.find(link.concrete);
            const auto slot = SLOT_CODES.find(aa.slot);
            link.slot = slot == SLOT_CODES.end() ? aa.slot : slot->second;
            auto cit = costs.find(link.concrete);
            if (cit == costs.end()) cit = costs.emplace(link.concrete, read_cost(src, types, link.concrete)).first;
            link.cost = cit->second;
            link.name = match_display(aam, concrete_parts(link.concrete));
            if (symbol != concrete_to_attachment.end()) link.attachment = symbol->second;
            else
            {
                const auto same_name = symbol_by_slot_display.find(link.slot + "\n" + link.name.display);
                if (!link.name.display.empty() && same_name != symbol_by_slot_display.end() &&
                    same_name->second.size() == 1) link.attachment = *same_name->second.begin();
                else link.attachment = "<NO_MEMBER_FOR_" + norm(aa.slot + "_" + aa.name) + ">";
            }
            if ((link.name.display.empty() || link.name.ambiguous) &&
                display_consensus[link.attachment].size() == 1)
            {
                link.name.display = *display_consensus[link.attachment].begin();
                link.name.ambiguous = false;
                link.name.identity_fallback = true;
            }
            unlisted_links_by_weapon[w.name].push_back(std::move(link));
        }
        std::sort(unlisted_links_by_weapon[w.name].begin(), unlisted_links_by_weapon[w.name].end(),
                  [](const Link& a, const Link& b) {
                      if (a.slot != b.slot) return a.slot < b.slot;
                      return a.attachment < b.attachment;
                  });
    }

    fs::create_directories(output.has_parent_path() ? output.parent_path() : fs::path("."));
    std::ofstream o(output);
    if (!o) { std::cerr << "output: cannot write " << output << "\n"; return 1; }

    o << "// Generated by portal_armory_codegen from the installed game and Portal SDK.\n";
    o << "// SDK: " << ts_comment(sdk.version.empty() ? "unknown" : sdk.version)
      << " (" << ts_comment(sdk.declarations.string()) << ")\n";
    o << "// Game: " << ts_comment(fs::path(game).string()) << "\n";
    o << "//\n";
    o << "// POINTS: each number is the game's authored cost for this exact\n";
    o << "// (weapon, attachment) record. The same attachment may cost differently on\n";
    o << "// another weapon. 0 means authored/free, not missing. Each weapon comment also\n";
    o << "// carries its live total budget: 100 for primaries and 60 for sidearms. A missing\n";
    o << "// budget means the game ships no customization registry for that item.\n";
    o << "//\n";
    o << "// COMPATIBILITY: these arrays reproduce the SDK/ModBuilder authoring whitelist.\n";
    o << "// The running game applies a separate delivered compatibility table and may\n";
    o << "// silently reject an entry; this static SDK list is not proof of runtime fit.\n";
    o << "//\n";
    o << "// Controls: live asset joins " << resolved_links << "/" << raw_links
      << "; fabricated-path control " << fake_resolved << "/" << raw_links
      << "; cost mirror " << mirror_agree << "/" << mirror_found
      << "; zero-field control " << neg_zero << "/" << neg_found << ".\n\n";
    o << "// Game-only attachment symbol join: " << display_symbol_joins
      << "/" << (missing_symbols + display_symbol_joins)
      << " unique slot+localized-name candidates; shuffled-slot control "
      << shuffled_display_symbol_joins << ". Unresolved symbols remain commented placeholders.\n\n";
    o << "type WeaponAttachmentMapping = {\n";
    o << "    weapon: mod.Weapons;\n";
    o << "    attachment: {\n";
    for (const char* slot : SLOT_NAMES) o << "        " << slot << ": mod.WeaponAttachments[];\n";
    o << "    };\n};\n\n";

    std::string last_prefix;
    for (const WeaponMember& w : weapons)
    {
        if (!sdk_weapons.count(w.name)) continue;
        const std::string prefix = prefix_of(w.name);
        if (prefix != last_prefix)
        {
            if (!last_prefix.empty()) o << "];\n// #endregion\n\n";
            o << "// #region " << prefix << "\n";
            o << "const " << variable_name(prefix) << ": WeaponAttachmentMapping[] = [\n";
            last_prefix = prefix;
        }
        const std::string wid = internal_id_from_equipment(w.equipment);
        const auto nit = weapon_names.find(wid);
        const std::string display = nit == weapon_names.end() ? std::string() : nit->second;
        if (w.assets.empty())
        {
            const auto game_rows = armory_by_id.find(wid);
            size_t art_rows = 0;
            if (game_rows != armory_by_id.end())
                for (const bf6::ArmoryAttachment& a : game_rows->second->attachments)
                    if (!a.ebx.empty()) ++art_rows;
            if (art_rows)
                o << "    // SDK WHITELIST OMISSION: the game has " << art_rows
                  << " concrete attachment rows, but this SDK weapon enum links none.\n"
                  << "    // Those game rows are listed below but kept commented out.\n";
            else
                o << "    // This weapon has no SDK whitelist links and no concrete game attachment rows.\n";
        }
        const bool whitelist_omission = w.assets.empty() && !unlisted_links_by_weapon[w.name].empty();
        const std::vector<Link>& emitted = whitelist_omission
            ? unlisted_links_by_weapon[w.name] : links_by_weapon[w.name];
        emit_weapon(o, w, emitted, display, budgets[w.name], "    ", false, whitelist_omission);
    }
    if (!last_prefix.empty()) o << "];\n// #endregion\n\n";

    o << "// #region GAME WEAPONS MISSING FROM THE SDK\n";
    o << "// These entries are visible in the installed game's weapon UI but cannot be\n";
    o << "// emitted as mod.Weapons values because the current SDK has no enum member.\n";
    for (const MissingWeapon& mw : missing_weapons)
    {
        o << "//\n// SDK-MISSING WEAPON: " << ts_comment(mw.display) << " (game id "
          << ts_comment(mw.id) << ")\n";
        o << "// The entire block stays commented because mod.Weapons has no member for it.\n";
        WeaponMember placeholder;
        placeholder.name = "<NO_MEMBER_FOR_" + norm(mw.id) + ">";
        emit_weapon(o, placeholder, mw.links, mw.display, mw.budget, "", true);
    }
    o << "// #endregion\n";
    o.close();

    auto difference_count = [](const std::set<std::string>& a, const std::set<std::string>& b) {
        size_t n = 0; for (const std::string& x : a) if (!b.count(x)) ++n; return n;
    };
    std::cout << "wrote " << output << "\n";
    std::cout << "SDK " << (sdk.version.empty() ? "unknown" : sdk.version)
              << ": " << sdk.weapons.size() << " weapons, " << sdk.attachments.size() << " attachments\n";
    std::cout << "live enums: " << weapons.size() << " weapons, " << attachments.size()
              << " attachments, " << raw_links << " links (" << distinct_pairs.size() << " distinct pairs)\n";
    std::cout << "SDK/live enum delta: weapons sdk-only=" << difference_count(sdk_weapons, live_weapons)
              << " live-only=" << difference_count(live_weapons, sdk_weapons)
              << "; attachments sdk-only=" << difference_count(sdk_attachments, live_attachments)
              << " live-only=" << difference_count(live_attachments, sdk_attachments) << "\n";
    std::cout << "asset join: " << resolved_links << "/" << raw_links
              << "; fabricated-path control: " << fake_resolved << "/" << raw_links << "\n";
    std::cout << "cost: " << cost_found << "/" << resolved_links
              << "; mirror agrees " << mirror_agree << "/" << mirror_found
              << "; zero-field control " << neg_zero << "/" << neg_found << "\n";
    std::cout << "localized attachment names: " << names_resolved << "/" << resolved_links
              << "; ambiguous " << names_ambiguous << "; English strings " << loc.size() << "\n";
    size_t budget_found = 0, budget_60 = 0, budget_100 = 0;
    for (const auto& kv : budgets) if (kv.second >= 0)
    { ++budget_found; if (kv.second == 60) ++budget_60; if (kv.second == 100) ++budget_100; }
    std::cout << "budgets: " << budget_found << "/" << weapons.size()
              << " (60=" << budget_60 << ", 100=" << budget_100 << ")\n";
    std::cout << "game-only weapons: " << missing_weapons.size() << "; attachment rows "
              << missing_concrete << "; display-to-SDK-symbol joins " << display_symbol_joins
              << " (shuffled-slot control " << shuffled_display_symbol_joins << ")"
              << "; without SDK attachment symbol " << missing_symbols << "\n";
    std::cout << "bounded GUID index: " << indexed << "; weapon UI identities: " << weapon_names.size() << "\n";

    const bool enum_exact = difference_count(sdk_weapons, live_weapons) == 0 &&
                            difference_count(live_weapons, sdk_weapons) == 0 &&
                            difference_count(sdk_attachments, live_attachments) == 0 &&
                            difference_count(live_attachments, sdk_attachments) == 0;
    const bool semantic_control_ok = display_symbol_joins == 0 ||
        display_symbol_joins > shuffled_display_symbol_joins * 3;
    const bool controls_ok = fake_resolved == 0 && mirror_found == mirror_agree &&
                             neg_found == neg_zero && semantic_control_ok;
    if (!enum_exact) std::cerr << "warning: SDK and live ModBuilder enums differ; output contains their safe intersection\n";
    if (!controls_ok) { std::cerr << "ERROR: one or more controls failed; do not publish this output\n"; return 3; }
    return 0;
}
