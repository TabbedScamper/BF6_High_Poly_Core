#include "rime_state_bridge.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace rime_state {
namespace {

std::string normalized_partition(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (value.size() > 4 && value.compare(value.size() - 4, 4, ".ebx") == 0)
        value.resize(value.size() - 4);
    return value;
}

bool same_path(const OccurrencePath& left, const OccurrencePath& right)
{
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (normalized_partition(left[index].owner_partition) !=
                normalized_partition(right[index].owner_partition) ||
            left[index].reference_instance != right[index].reference_instance)
            return false;
    return true;
}

bool as_bool(const Value& value, bool& out)
{
    switch (value.kind)
    {
    case ValueKind::Bool: out = value.boolean; return true;
    case ValueKind::Int: out = value.integer != 0; return true;
    case ValueKind::UInt: out = value.unsigned_integer != 0; return true;
    case ValueKind::Real: out = value.real != 0.0; return true;
    default: return false;
    }
}

bool as_real(const Value& value, float& out)
{
    switch (value.kind)
    {
    case ValueKind::Int: out = static_cast<float>(value.integer); return true;
    case ValueKind::UInt:
        out = static_cast<float>(value.unsigned_integer); return true;
    case ValueKind::Real: out = static_cast<float>(value.real); return true;
    default: return false;
    }
}

std::vector<std::string> split(const std::string& value, char delimiter)
{
    std::vector<std::string> result;
    size_t start = 0;
    for (;;)
    {
        const size_t end = value.find(delimiter, start);
        result.push_back(value.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

bool percent_decode(const std::string& encoded, std::string& decoded)
{
    decoded.clear();
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index < encoded.size(); ++index)
    {
        if (encoded[index] != '%')
        {
            decoded.push_back(encoded[index]);
            continue;
        }
        if (index + 2 >= encoded.size()) return false;
        const int high = nibble(encoded[index + 1]);
        const int low = nibble(encoded[index + 2]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return true;
}

template <typename T>
bool parse_integer(const std::string& text, T& out, int base = 10)
{
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    if constexpr (std::is_signed_v<T>)
    {
        const long long value = std::strtoll(text.c_str(), &end, base);
        if (errno || end != text.c_str() + text.size()) return false;
        out = static_cast<T>(value);
    }
    else
    {
        const unsigned long long value = std::strtoull(text.c_str(), &end, base);
        if (errno || end != text.c_str() + text.size()) return false;
        out = static_cast<T>(value);
    }
    return true;
}

} // namespace

Value Value::from_bool(bool value)
{
    Value result;
    result.kind = ValueKind::Bool;
    result.boolean = value;
    return result;
}

Value Value::from_int(int64_t value)
{
    Value result;
    result.kind = ValueKind::Int;
    result.integer = value;
    return result;
}

Value Value::from_uint(uint64_t value)
{
    Value result;
    result.kind = ValueKind::UInt;
    result.unsigned_integer = value;
    return result;
}

Value Value::from_real(double value)
{
    Value result;
    result.kind = ValueKind::Real;
    result.real = value;
    return result;
}

Value Value::from_string(std::string value)
{
    Value result;
    result.kind = ValueKind::String;
    result.string = std::move(value);
    return result;
}

bool read_wire(std::istream& input, std::vector<StateSlot>& slots,
               WireReport& report, std::string& error)
{
    slots.clear();
    report = WireReport{};
    error.clear();
    std::string line;
    if (!std::getline(input, line)) { error = "empty Rime state stream"; return false; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::vector<std::string> header = split(line, '\t');
    if (header.size() != 2 || header[0] != "BF6_RIME_STATE_V3" ||
        !percent_decode(header[1], report.route))
    {
        error = "invalid Rime state stream header";
        return false;
    }

    bool ended = false;
    size_t line_number = 1;
    while (std::getline(input, line))
    {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> fields = split(line, '\t');
        if (fields.empty()) continue;
        if (fields[0] == "S")
        {
            if (fields.size() != 7 && fields.size() != 8)
            {
                std::ostringstream detail;
                detail << "malformed state row at line " << line_number
                       << " (fields=" << fields.size()
                       << ", bytes=" << line.size() << ')';
                error = detail.str();
                return false;
            }
            StateSlot slot;
            slot.partition = fields[1];
            if (fields.size() == 8 &&
                !percent_decode(fields[7], slot.source_type))
            { error = "invalid source type"; return false; }
            if (!fields[2].empty())
                for (const std::string& encoded_step : split(fields[2], ';'))
                {
                    const size_t separator = encoded_step.rfind('@');
                    OccurrenceStep step;
                    if (separator == std::string::npos ||
                        !parse_integer(encoded_step.substr(separator + 1),
                                       step.reference_instance))
                    {
                        error = "malformed occurrence path";
                        return false;
                    }
                    step.owner_partition = encoded_step.substr(0, separator);
                    if (step.owner_partition.empty())
                    {
                        error = "empty occurrence owner";
                        return false;
                    }
                    slot.occurrence.push_back(std::move(step));
                }
            if (!parse_integer(fields[3], slot.local_instance) ||
                !parse_integer(fields[4], slot.field, 0))
            {
                error = "invalid instance or field";
                return false;
            }
            if (fields[5] == "b")
            {
                if (fields[6] != "0" && fields[6] != "1")
                { error = "invalid bool payload"; return false; }
                slot.value = Value::from_bool(fields[6] == "1");
            }
            else if (fields[5] == "i")
            {
                int64_t value = 0;
                if (!parse_integer(fields[6], value))
                { error = "invalid int payload"; return false; }
                slot.value = Value::from_int(value);
            }
            else if (fields[5] == "u")
            {
                uint64_t value = 0;
                if (!parse_integer(fields[6], value))
                { error = "invalid uint payload"; return false; }
                slot.value = Value::from_uint(value);
            }
            else if (fields[5] == "r")
            {
                char* end = nullptr;
                errno = 0;
                const double value = std::strtod(fields[6].c_str(), &end);
                if (errno || end != fields[6].c_str() + fields[6].size())
                { error = "invalid real payload"; return false; }
                slot.value = Value::from_real(value);
            }
            else if (fields[5] == "s")
            {
                std::string value;
                if (!percent_decode(fields[6], value))
                { error = "invalid string payload"; return false; }
                slot.value = Value::from_string(std::move(value));
            }
            else { error = "unknown state value kind"; return false; }
            slots.push_back(std::move(slot));
        }
        else if (fields[0] == "C")
        {
            if (fields.size() != 3 || fields[1].empty() ||
                (fields[2] != "0" && fields[2] != "1" && fields[2] != "n"))
            { error = "malformed control row"; return false; }
            report.controls[fields[1]] = fields[2] == "n" ? -1 :
                                          fields[2] == "1" ? 1 : 0;
        }
        else if (fields[0] == "M")
        {
            const bool shape_ok = fields.size() == 4 &&
                !report.requirement_metadata_present &&
                parse_integer(fields[1], report.host_requirement_slots) &&
                parse_integer(fields[2], report.external_host_requirement_slots) &&
                parse_integer(fields[3],
                    report.consumed_external_host_requirement_slots) &&
                report.host_requirement_slots >= 0 &&
                report.external_host_requirement_slots >= 0 &&
                report.consumed_external_host_requirement_slots >= 0 &&
                report.consumed_external_host_requirement_slots <=
                    report.external_host_requirement_slots &&
                report.external_host_requirement_slots <=
                    report.host_requirement_slots;
            if (!shape_ok)
            {
                std::ostringstream detail;
                detail << "invalid requirement metadata row at line "
                       << line_number << " (fields=" << fields.size() << ')';
                error = detail.str();
                return false;
            }
            report.requirement_metadata_present = true;
        }
        else if (fields[0] == "P")
        {
            const bool shape_ok = fields.size() == 3 &&
                !report.provider_metadata_present &&
                report.requirement_metadata_present &&
                parse_integer(fields[1],
                    report.resolved_consumed_external_provider_slots) &&
                parse_integer(fields[2],
                    report.remaining_consumed_external_provider_slots) &&
                report.resolved_consumed_external_provider_slots >= 0 &&
                report.remaining_consumed_external_provider_slots >= 0 &&
                report.resolved_consumed_external_provider_slots +
                    report.remaining_consumed_external_provider_slots ==
                    report.consumed_external_host_requirement_slots;
            if (!shape_ok)
            {
                std::ostringstream detail;
                detail << "invalid provider metadata row at line "
                       << line_number << " (fields=" << fields.size() << ')';
                error = detail.str();
                return false;
            }
            report.provider_metadata_present = true;
        }
        else if (fields[0] == "D")
        {
            const bool shape_ok = fields.size() == 3 &&
                !report.dependency_metadata_present &&
                report.provider_metadata_present &&
                parse_integer(fields[1], report.dependency_closed_slots) &&
                parse_integer(fields[2],
                    report.excluded_provider_dependent_slots) &&
                report.dependency_closed_slots >= 0 &&
                report.excluded_provider_dependent_slots >= 0;
            if (!shape_ok)
            {
                std::ostringstream detail;
                detail << "invalid dependency metadata row at line "
                       << line_number << " (fields=" << fields.size() << ')';
                error = detail.str();
                return false;
            }
            report.dependency_metadata_present = true;
        }
        else if (fields[0] == "END")
        {
            if (fields.size() != 3 ||
                !parse_integer(fields[1], report.declared_slots) ||
                !parse_integer(fields[2], report.omitted_non_scalar_slots) ||
                report.declared_slots != static_cast<int>(slots.size()))
            { error = "invalid state stream footer"; return false; }
            ended = true;
            break;
        }
        else { error = "unknown Rime state stream row"; return false; }
    }
    if (!ended) { error = "missing Rime state stream footer"; return false; }
    if (!report.dependency_metadata_present ||
        report.dependency_closed_slots != report.declared_slots)
    {
        error = "missing or inconsistent dependency metadata";
        return false;
    }
    const auto passed = report.controls.find("passed");
    if (passed == report.controls.end() || passed->second != 1)
    {
        error = "state capture controls did not pass";
        return false;
    }
    return true;
}

bool occurrence_path(const rime::Screen& screen, size_t element_index,
                     OccurrencePath& out)
{
    out.clear();
    if (element_index >= screen.elements.size()) return false;
    int scope = screen.elements[element_index].scope;
    std::set<int> visited;
    while (scope >= 0)
    {
        if (scope >= static_cast<int>(screen.elements.size()) ||
            !visited.insert(scope).second)
        {
            out.clear();
            return false;
        }
        const rime::Element& reference = screen.elements[static_cast<size_t>(scope)];
        if (reference.kind != rime::Kind::WidgetReference ||
            reference.instance < 0 || reference.partition.empty())
        {
            out.clear();
            return false;
        }
        out.push_back(OccurrenceStep{
            normalized_partition(reference.partition), reference.instance});
        scope = reference.scope;
    }
    std::reverse(out.begin(), out.end());
    return true;
}

ApplyReport apply_state(rime::Screen& screen,
                        const std::vector<StateSlot>& slots)
{
    ApplyReport report;
    report.input_slots = static_cast<int>(slots.size());

    std::vector<OccurrencePath> paths(screen.elements.size());
    std::vector<unsigned char> valid(screen.elements.size(), 0);
    for (size_t index = 0; index < screen.elements.size(); ++index)
    {
        valid[index] = occurrence_path(screen, index, paths[index]) ? 1 : 0;
        if (!valid[index]) ++report.invalid_scope_chains;
    }

    const uint32_t visible_field = rime::property_hash("Visible");
    const uint32_t alpha_field = rime::property_hash("Alpha");
    const uint32_t width_field = rime::property_hash("Width");
    const uint32_t height_field = rime::property_hash("Height");
    const uint32_t item_spacing_field = rime::property_hash("ItemSpacing");
    const uint32_t stack_wrap_spacing_field =
        rime::property_hash("WrapSpacing");
    const uint32_t progress_field = rime::property_hash("Progress");
    const uint32_t segment_gap_field = rime::property_hash("SegmentGap");
    const uint32_t segment_count_field = rime::property_hash("SegmentCount");
    const uint32_t border_thickness_field =
        rime::property_hash("BorderThickness");
    const uint32_t start_alpha_field = rime::property_hash("StartAlpha");
    const uint32_t end_alpha_field = rime::property_hash("EndAlpha");
    const uint32_t start_progress_field =
        rime::property_hash("StartProgress");
    const uint32_t end_progress_field =
        rime::property_hash("EndProgress");
    const uint32_t glow_size_field = rime::property_hash("GlowSize");
    const uint32_t localized_text_field = rime::property_hash("LocalizedText");
    const uint32_t raw_text_field = rime::property_hash("RawText");
    const uint32_t string_id_field = rime::property_hash("StringId");
    const uint32_t text_field = rime::property_hash("Text");

    for (const StateSlot& slot : slots)
    {
        const bool is_text_field = slot.field == localized_text_field ||
            slot.field == raw_text_field || slot.field == string_id_field ||
            slot.field == text_field;
        const bool renderer_field = slot.field == visible_field ||
            slot.field == alpha_field || slot.field == width_field ||
            slot.field == height_field || slot.field == item_spacing_field ||
            slot.field == stack_wrap_spacing_field ||
            slot.field == progress_field || slot.field == segment_gap_field ||
            slot.field == segment_count_field ||
            slot.field == border_thickness_field ||
            slot.field == start_alpha_field || slot.field == end_alpha_field ||
            slot.field == start_progress_field ||
            slot.field == end_progress_field || slot.field == glow_size_field ||
            is_text_field;
        // The evaluator deliberately carries every graph/interface/operator
        // slot. Those are useful evidence, but they are not failures of the
        // renderer occurrence join. Reject unimplemented fields before the
        // element lookup so logical records do not inflate missing counts.
        if (!renderer_field)
        {
            ++report.unsupported_fields;
            ++report.unsupported_field_counts[slot.field];
            continue;
        }
        std::vector<size_t> occurrence_matches;
        std::vector<size_t> instance_matches;
        const std::string partition = normalized_partition(slot.partition);
        for (size_t index = 0; index < screen.elements.size(); ++index)
        {
            if (!valid[index]) continue;
            const rime::Element& element = screen.elements[index];
            if (normalized_partition(element.partition) != partition ||
                !same_path(paths[index], slot.occurrence))
                continue;
            occurrence_matches.push_back(index);
            if (element.instance == slot.local_instance)
                instance_matches.push_back(index);
        }
        if (occurrence_matches.empty())
        {
            // WidgetReferenceObjectData is a logical prefab reference in the
            // schematic graph. It can expose fields such as Visible but does
            // not instantiate a Rime element unless bf6_rime_tree reports it
            // as BF6_RIME_WIDGET_REFERENCE. Absence here is therefore a
            // domain exclusion, not a failed visual occurrence expansion.
            if (slot.source_type == "WidgetReferenceObjectData")
                ++report.non_renderer_targets;
            else
            {
                ++report.missing_occurrences;
                ++report.missing_occurrence_field_counts[slot.field];
                if (report.missing_occurrence_examples.size() < 512)
                    report.missing_occurrence_examples.push_back(slot);
            }
            continue;
        }
        if (instance_matches.empty())
        {
            ++report.missing_instances;
            ++report.missing_instance_field_counts[slot.field];
            continue;
        }
        if (instance_matches.size() != 1)
        {
            ++report.ambiguous_targets;
            continue;
        }

        rime::Element& element = screen.elements[instance_matches[0]];
        if (element.kind == rime::Kind::Unknown ||
            element.kind == rime::Kind::InputBehavior ||
            element.kind == rime::Kind::LayeredIconBinding ||
            element.kind == rime::Kind::HardwareIconBinding ||
            element.kind == rime::Kind::RemoteWidgetPresenter)
        {
            ++report.non_renderer_targets;
            continue;
        }
        if (slot.field == visible_field)
        {
            bool value = false;
            if (!as_bool(slot.value, value)) { ++report.type_mismatches; continue; }
            element.visible = value;
            element.runtime_visibility_unresolved = false;
        }
        else if (slot.field == alpha_field)
        {
            float value = 0.f;
            if (!as_real(slot.value, value)) { ++report.type_mismatches; continue; }
            element.alpha = value;
            element.runtime_alpha_unresolved = false;
        }
        else if (slot.field == width_field || slot.field == height_field)
        {
            float value = 0.f;
            if (!as_real(slot.value, value)) { ++report.type_mismatches; continue; }
            if (slot.field == width_field)
            {
                element.width = value;
                if (element.kind == rime::Kind::VectorShape)
                    element.shape_size[0] = value;
            }
            else
            {
                element.height = value;
                if (element.kind == rime::Kind::VectorShape)
                    element.shape_size[1] = value;
            }
        }
        else if (slot.field == item_spacing_field ||
                 slot.field == stack_wrap_spacing_field ||
                 slot.field == progress_field ||
                 slot.field == segment_gap_field ||
                 slot.field == border_thickness_field ||
                 slot.field == start_alpha_field ||
                 slot.field == end_alpha_field ||
                 slot.field == start_progress_field ||
                 slot.field == end_progress_field ||
                 slot.field == glow_size_field)
        {
            float value = 0.f;
            if (!as_real(slot.value, value)) { ++report.type_mismatches; continue; }
            if (slot.field == item_spacing_field)
                element.item_spacing = value;
            else if (slot.field == stack_wrap_spacing_field)
                element.stack_wrap_spacing = value;
            else if (slot.field == progress_field)
                element.progress = value;
            else if (slot.field == segment_gap_field)
                element.progress_segment_gap = value;
            else if (slot.field == border_thickness_field)
                element.border_thickness = value;
            else if (slot.field == start_alpha_field)
                element.border_start_alpha = value;
            else if (slot.field == end_alpha_field)
                element.border_end_alpha = value;
            else if (slot.field == start_progress_field)
                element.line_start_progress = value;
            else if (slot.field == end_progress_field)
                element.line_end_progress = value;
            else
                element.line_glow_size = value;
        }
        else if (slot.field == segment_count_field)
        {
            if (slot.value.kind == ValueKind::Int)
                element.progress_segment_count =
                    static_cast<int>(slot.value.integer);
            else if (slot.value.kind == ValueKind::UInt)
                element.progress_segment_count =
                    static_cast<int>(slot.value.unsigned_integer);
            else { ++report.type_mismatches; continue; }
        }
        else if (is_text_field)
        {
            if (element.kind != rime::Kind::Label ||
                slot.value.kind != ValueKind::String)
            {
                ++report.type_mismatches;
                continue;
            }
            element.text = slot.value.string;
        }
        ++report.applied_slots;
    }
    return report;
}

} // namespace rime_state
