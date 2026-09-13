#include "bf6_core.h"
#include "expression_graph.h"
#include "expression_registry.h"
#include "expression_vm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

static const uint32_t kExpressionType = 0x7dd4cc89u;

bf6::expression::Value f32(float value)
{
    bf6::expression::Value out;
    out.bytes.resize(4);
    std::memcpy(out.bytes.data(), &value, 4);
    out.known = true;
    return out;
}

float as_f32(const bf6::expression::Value& value)
{
    float out = 0.f;
    if (value.bytes.size() >= 4)
        std::memcpy(&out, value.bytes.data(), 4);
    return out;
}

std::vector<bf6::expression::Value> arguments_for(const std::string& name)
{
    using bf6::expression::Value;
    if (name.find("rateoffire") != std::string::npos)
        return {f32(900.f)};
    if (name.find("controlattributedelegate1") != std::string::npos)
        return {f32(1.f), f32(0.2f), f32(900.f)};
    if (name.find("hipfire") != std::string::npos)
        return {Value::from_u32(3), f32(1.f), f32(0.5f), f32(0.1f)};
    if (name.find("headshot") != std::string::npos ||
        name.find("collateral") != std::string::npos)
        return {Value::from_u32(3)};
    if (name.find("mobility") != std::string::npos)
        return {Value::from_bool(false), Value::from_u32(6),
                Value::from_u32(3), Value::from_u32(3),
                Value::from_u32(3), Value::from_u32(3)};
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    const char* game = argc > 1 ? argv[1] :
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char error[1024] = {};
    bf6_ctx* context = bf6_open(game, error, (int)sizeof(error));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 2; }
    if (!bf6_mount_frontend(context, error, (int)sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 2;
    }

    const int total = bf6_list_res(context, "attributedelegates", nullptr, 0);
    std::vector<bf6_asset> assets((size_t)(std::max)(0, total));
    const int count = total > 0
        ? bf6_list_res(context, "attributedelegates", assets.data(), total) : 0;
    int parsed = 0, sound = 0, nullSound = 0, unresolved = 0;
    std::printf("weapon attribute DiceExpression runtime\n");
    for (int index = 0; index < count; ++index)
    {
        const bf6_asset& asset = assets[(size_t)index];
        if (asset.type != kExpressionType || !asset.name) continue;
        const uint8_t* raw = nullptr;
        const int64_t bytes = bf6_read_raw(context, BF6_RAW_RES,
                                           asset.name, &raw);
        bf6::expression::Graph graph;
        std::string why;
        if (bytes <= 0 || !raw ||
            !bf6::expression::parse(raw, (size_t)bytes, graph, why))
        {
            std::printf("  FAIL %s: %s\n", asset.name, why.c_str());
            continue;
        }
        ++parsed;
        std::set<uint32_t> keySet;
        for (const auto& fixup : graph.fixups) keySet.insert(fixup.key);
        std::vector<uint32_t> keys(keySet.begin(), keySet.end());
        std::vector<bf6::expression::NamedOperator> names;
        std::string scanError;
        if (!bf6::expression::resolve_named_operators(
                std::string(game) + "\\bf6.exe", keys, names, scanError))
        {
            std::printf("  FAIL %s: %s\n", asset.name, scanError.c_str());
            continue;
        }
        bf6::expression::NamedBuiltins host;
        std::map<uint32_t, std::string> operatorNames;
        for (const auto& row : names)
            if (row.match_count == 1) {
                host.add(row.key, row.name);
                operatorNames[row.key] = row.name;
            }
        bf6::expression::Instance instance;
        if (!bf6::expression::make_instance(graph, instance, why)) continue;
        const std::vector<bf6::expression::Value> args =
            arguments_for(asset.name);
        const auto real = bf6::expression::evaluate(graph, &instance,
                                                     args, &host);
        bf6::expression::Instance nullInstance;
        bf6::expression::make_instance(graph, nullInstance, why);
        const auto control = bf6::expression::evaluate(graph, &nullInstance,
                                                        args, nullptr);
        const bool realSound = real.result.known && !real.result.tainted &&
                               real.unresolved_keys.empty() &&
                               std::isfinite(as_f32(real.result));
        const bool controlSound = control.result.known &&
                                  !control.result.tainted &&
                                  control.unresolved_keys.empty();
        sound += realSound ? 1 : 0;
        nullSound += controlSound ? 1 : 0;
        unresolved += (int)real.unresolved_keys.size();
        std::printf("  %-78s args=%zu result=%g known=%d tainted=%d "
                    "unresolved=%zu guessed=%u control_sound=%d\n",
                    asset.name, args.size(), as_f32(real.result),
                    real.result.known ? 1 : 0,
                    real.result.tainted ? 1 : 0,
                    real.unresolved_keys.size(), real.guessed_branches,
                    controlSound ? 1 : 0);
        for (uint32_t key : real.unresolved_keys)
        {
            std::printf("      unresolved 0x%08X %s\n", key,
                        operatorNames.count(key)
                            ? operatorNames[key].c_str() : "<descriptor>");
            for (const auto& record : graph.records)
            {
                if (record.operator_key != key) continue;
                std::printf("        record=0x%X kind=0x%02X operands=%zu:",
                            record.offset, record.kind, record.operands.size());
                for (const auto& operand : record.operands)
                    std::printf(" %u:%u", operand.region, operand.offset);
                std::printf("\n");
            }
        }
        for (const std::string& diagnostic : real.diagnostics)
            std::printf("      diagnostic: %s\n", diagnostic.c_str());
    }
    bf6_close(context);
    std::printf("  parsed/sound/null-control-sound/unresolved: %d/%d/%d/%d\n",
                parsed, sound, nullSound, unresolved);
    return parsed == 7 && sound == 7 && nullSound == 0 && unresolved == 0
        ? 0 : 1;
}
