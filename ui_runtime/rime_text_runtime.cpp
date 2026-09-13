#include "rime_text_runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace bf6_ui {
namespace {

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim_copy(const std::string& value)
{
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

bool integer_value(const TextArgument& value, int64_t& out)
{
    if (value.kind == TextArgumentKind::Integer) {
        out = value.integer;
        return true;
    }
    if (value.kind == TextArgumentKind::Real && std::isfinite(value.real)) {
        out = static_cast<int64_t>(std::llround(value.real));
        return true;
    }
    return false;
}

bool real_value(const TextArgument& value, double& out)
{
    if (value.kind == TextArgumentKind::Real) {
        out = value.real;
        return std::isfinite(out);
    }
    if (value.kind == TextArgumentKind::Integer) {
        out = static_cast<double>(value.integer);
        return true;
    }
    return false;
}

std::string string_value(const TextArgument& value)
{
    if (value.kind == TextArgumentKind::String) return value.string;
    if (value.kind == TextArgumentKind::Integer)
        return std::to_string(value.integer);
    std::ostringstream stream;
    stream << value.real;
    return stream.str();
}

bool format_value(const TextArgument& value, const std::string& raw_spec,
                  std::string& out)
{
    const std::string spec = lower_copy(raw_spec);
    if (spec.empty() || spec == "s" || spec == "map" || spec == "icon" ||
        spec == "rarity" || spec == "xp" || spec == "input" ||
        spec.rfind("input:", 0) == 0) {
        out = string_value(value);
        return true;
    }
    if (spec == "d") {
        int64_t integer = 0;
        if (!integer_value(value, integer)) return false;
        out = std::to_string(integer);
        return true;
    }
    if (spec == "ms") {
        int64_t integer = 0;
        if (!integer_value(value, integer)) return false;
        out = std::to_string(integer) + " ms";
        return true;
    }
    if (spec == "time:mm:ss") {
        int64_t seconds = 0;
        if (!integer_value(value, seconds)) return false;
        const bool negative = seconds < 0;
        const uint64_t absolute = negative
            ? static_cast<uint64_t>(-(seconds + 1)) + 1u
            : static_cast<uint64_t>(seconds);
        const uint64_t hours = absolute / 3600u;
        const uint64_t minutes = (absolute / 60u) % 60u;
        const uint64_t remainder = absolute % 60u;
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%s%02llu:%02llu:%02llu",
                      negative ? "-" : "",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(remainder));
        out = buffer;
        return true;
    }
    if (spec.rfind("f.", 0) == 0 && spec.size() > 2) {
        char* end = nullptr;
        const long precision = std::strtol(spec.c_str() + 2, &end, 10);
        double real = 0.0;
        if (!end || *end || precision < 0 || precision > 9 ||
            !real_value(value, real))
            return false;
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(static_cast<int>(precision))
               << real;
        out = stream.str();
        return true;
    }
    return false;
}

struct MarkupState {
    std::string family;
    std::string color_name;
    uint32_t color_rgb = 0;
    bool has_color = false;
    bool uppercase = false;
    bool visible = true;
};

struct StateFrame {
    std::string tag;
    MarkupState state;
};

bool parse_hex_color(std::string value, uint32_t& out)
{
    value = trim_copy(value);
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    if (value.size() == 8) value = value.substr(0, 6); // ignore authored alpha
    if (value.size() != 6) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 16);
    if (!end || *end) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

std::map<std::string, std::string> attributes(const std::string& body)
{
    std::map<std::string, std::string> result;
    size_t cursor = 0;
    while (cursor < body.size()) {
        while (cursor < body.size() &&
               std::isspace(static_cast<unsigned char>(body[cursor])))
            ++cursor;
        const size_t key_begin = cursor;
        while (cursor < body.size() && body[cursor] != '=' &&
               !std::isspace(static_cast<unsigned char>(body[cursor])))
            ++cursor;
        if (cursor == key_begin || cursor >= body.size() ||
            body[cursor] != '=') {
            while (cursor < body.size() &&
                   !std::isspace(static_cast<unsigned char>(body[cursor])))
                ++cursor;
            continue;
        }
        const std::string key = lower_copy(body.substr(key_begin,
                                                       cursor - key_begin));
        ++cursor;
        std::string value;
        if (cursor < body.size() && (body[cursor] == '\'' || body[cursor] == '"')) {
            const char quote = body[cursor++];
            const size_t begin = cursor;
            while (cursor < body.size() && body[cursor] != quote) ++cursor;
            value = body.substr(begin, cursor - begin);
            if (cursor < body.size()) ++cursor;
        } else {
            const size_t begin = cursor;
            while (cursor < body.size() &&
                   !std::isspace(static_cast<unsigned char>(body[cursor])))
                ++cursor;
            value = body.substr(begin, cursor - begin);
        }
        result[key] = value;
    }
    return result;
}

bool bool_attribute(const std::map<std::string, std::string>& values,
                    const char* name)
{
    const auto found = values.find(name);
    if (found == values.end()) return false;
    const std::string value = lower_copy(trim_copy(found->second));
    return value == "true" || value == "1" || value == "yes";
}

bool scalar_attribute(const std::map<std::string, std::string>& values,
                      const char* name, float& out, bool& percent)
{
    const auto found = values.find(name);
    if (found == values.end()) return false;
    std::string value = trim_copy(found->second);
    percent = !value.empty() && value.back() == '%';
    if (percent) value.pop_back();
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    if (!end || end == value.c_str() || *end || !std::isfinite(parsed))
        return false;
    out = parsed;
    return true;
}

void append_text(MarkupResult& result, const MarkupState& state,
                 const std::string& text)
{
    if (!state.visible || text.empty()) return;
    if (!result.runs.empty()) {
        MarkupRun& previous = result.runs.back();
        if (previous.kind == MarkupRunKind::Text &&
            previous.family == state.family &&
            previous.color_name == state.color_name &&
            previous.color_rgb == state.color_rgb &&
            previous.has_color == state.has_color &&
            previous.uppercase == state.uppercase) {
            previous.text += text;
            return;
        }
    }
    MarkupRun run;
    run.kind = MarkupRunKind::Text;
    run.text = text;
    run.family = state.family;
    run.color_name = state.color_name;
    run.color_rgb = state.color_rgb;
    run.has_color = state.has_color;
    run.uppercase = state.uppercase;
    result.runs.push_back(std::move(run));
}

} // namespace

TextArgument text_string(std::string value)
{
    TextArgument argument;
    argument.kind = TextArgumentKind::String;
    argument.string = std::move(value);
    return argument;
}

TextArgument text_integer(int64_t value)
{
    TextArgument argument;
    argument.kind = TextArgumentKind::Integer;
    argument.integer = value;
    return argument;
}

TextArgument text_real(double value)
{
    TextArgument argument;
    argument.kind = TextArgumentKind::Real;
    argument.real = value;
    return argument;
}

TextFormatResult format_rime_text(const std::string& pattern,
                                  const TextFormatArguments& arguments)
{
    TextFormatResult result;
    for (size_t cursor = 0; cursor < pattern.size();) {
        if (pattern[cursor] == '{' && cursor + 1 < pattern.size() &&
            pattern[cursor + 1] == '{') {
            result.text.push_back('{');
            cursor += 2;
            continue;
        }
        if (pattern[cursor] == '}' && cursor + 1 < pattern.size() &&
            pattern[cursor + 1] == '}') {
            result.text.push_back('}');
            cursor += 2;
            continue;
        }
        if (pattern[cursor] != '{') {
            result.text.push_back(pattern[cursor++]);
            continue;
        }
        const size_t close = pattern.find('}', cursor + 1);
        if (close == std::string::npos) {
            ++result.malformed;
            break;
        }
        const std::string token = pattern.substr(cursor + 1,
                                                 close - cursor - 1);
        const size_t colon = token.find(':');
        const std::string key = trim_copy(token.substr(0, colon));
        const std::string spec = colon == std::string::npos
            ? std::string() : trim_copy(token.substr(colon + 1));
        const TextArgument* argument = nullptr;
        if (!key.empty() &&
            std::all_of(key.begin(), key.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            const unsigned long index = std::strtoul(key.c_str(), nullptr, 10);
            if (index < arguments.positional.size())
                argument = &arguments.positional[index];
        } else {
            const auto found = arguments.named.find(key);
            if (found != arguments.named.end()) argument = &found->second;
        }
        std::string formatted;
        if (argument && format_value(*argument, spec, formatted)) {
            result.text += formatted;
            ++result.substitutions;
        } else {
            ++result.unresolved;
        }
        cursor = close + 1;
    }
    return result;
}

MarkupResult parse_rime_markup(const std::string& text, MarkupDevice device)
{
    MarkupResult result;
    MarkupState state;
    std::vector<StateFrame> stack;
    const std::string folded_text = lower_copy(text);
    size_t plain_begin = 0;
    for (size_t cursor = 0; cursor < text.size();) {
        if (text[cursor] != '[') {
            ++cursor;
            continue;
        }
        const size_t close = text.find(']', cursor + 1);
        if (close == std::string::npos) {
            ++result.malformed;
            break;
        }
        const std::string raw = trim_copy(text.substr(cursor + 1,
                                                      close - cursor - 1));
        const std::string lowered = lower_copy(raw);
        bool recognized = false;
        append_text(result, state, text.substr(plain_begin, cursor - plain_begin));

        if (!lowered.empty() && lowered.front() == '/') {
            const std::string closing = trim_copy(lowered.substr(1));
            for (size_t i = stack.size(); i > 0; --i) {
                if (stack[i - 1].tag != closing) continue;
                state = stack[i - 1].state;
                stack.resize(i - 1);
                recognized = true;
                break;
            }
        } else if (lowered.rfind("color=", 0) == 0) {
            uint32_t color = 0;
            const std::string value = trim_copy(raw.substr(raw.find('=') + 1));
            stack.push_back({"color", state});
            if (parse_hex_color(value, color)) {
                state.color_rgb = color;
                state.color_name.clear();
            } else {
                state.color_name = value;
            }
            state.has_color = !value.empty();
            recognized = true;
        } else if (lowered.rfind("family=", 0) == 0) {
            stack.push_back({"family", state});
            state.family = trim_copy(raw.substr(raw.find('=') + 1));
            recognized = true;
        } else if (lowered.rfind("transform=", 0) == 0) {
            stack.push_back({"transform", state});
            state.uppercase = lower_copy(trim_copy(
                raw.substr(raw.find('=') + 1))) == "uppercase";
            recognized = true;
        } else if (lowered.rfind("devicefilter", 0) == 0) {
            stack.push_back({"devicefilter", state});
            const auto values = attributes(raw.substr(std::string("deviceFilter").size()));
            const auto found = values.find("device");
            if (found != values.end()) {
                const std::string wanted = lower_copy(found->second);
                state.visible = state.visible &&
                    (device == MarkupDevice::Any ||
                     (wanted == "keyboard" && device == MarkupDevice::Keyboard) ||
                     (wanted == "pad" && device == MarkupDevice::Pad));
            }
            recognized = true;
        } else if (lowered.rfind("inputaction ", 0) == 0) {
            // Current-install localization uses inputAction as a paired tag:
            // attributes select the binding presentation, while the BODY is
            // the concept key.  The older operand-only form remains accepted
            // because it is also present in retained UI strings.
            const size_t action_close = folded_text.find(
                "[/inputaction]", close + 1);
            const bool paired = action_close != std::string::npos;
            const auto values = paired
                ? attributes(raw.substr(std::string("inputAction").size()))
                : std::map<std::string, std::string>{};
            if (state.visible) {
                MarkupRun run;
                run.kind = MarkupRunKind::InputAction;
                run.text = paired
                    ? trim_copy(text.substr(close + 1,
                                            action_close - close - 1))
                    : trim_copy(raw.substr(std::string("inputAction").size()));
                run.family = state.family;
                run.color_name = state.color_name;
                run.color_rgb = state.color_rgb;
                run.has_color = state.has_color;
                run.uppercase = state.uppercase;
                const auto config = values.find("config");
                if (config != values.end()) run.config = config->second;
                const auto action_map = values.find("actionmap");
                if (action_map != values.end())
                    run.action_map = action_map->second;
                run.tint = bool_attribute(values, "tint");
                run.has_height = scalar_attribute(values, "height", run.height,
                                                  run.height_percent);
                result.runs.push_back(std::move(run));
            }
            if (paired) {
                result.tags += 2;
                cursor = action_close + std::string("[/inputAction]").size();
                plain_begin = cursor;
                continue;
            }
            recognized = true;
        } else if (lowered == "icon" || lowered.rfind("icon ", 0) == 0) {
            if (state.visible) {
                MarkupRun run;
                run.kind = MarkupRunKind::Icon;
                run.asset = trim_copy(raw.substr(4));
                run.color_name = state.color_name;
                run.color_rgb = state.color_rgb;
                run.has_color = state.has_color;
                result.runs.push_back(std::move(run));
            }
            recognized = true;
        } else if (lowered == "img" || lowered.rfind("img ", 0) == 0) {
            const size_t image_close = folded_text.find("[/img]", close + 1);
            if (image_close == std::string::npos) {
                ++result.malformed;
                recognized = true;
            } else {
                const auto values = attributes(raw.substr(3));
                if (state.visible) {
                MarkupRun run;
                run.kind = MarkupRunKind::Image;
                run.asset = trim_copy(text.substr(close + 1,
                                                  image_close - close - 1));
                const auto display = values.find("display");
                if (display != values.end()) run.display = display->second;
                const auto align = values.find("align");
                if (align != values.end()) run.align = align->second;
                run.tint = bool_attribute(values, "tint");
                run.has_width = scalar_attribute(values, "width", run.width,
                                                 run.width_percent);
                run.has_height = scalar_attribute(values, "height", run.height,
                                                  run.height_percent);
                bool ignored_percent = false;
                run.has_alpha = scalar_attribute(values, "alpha", run.alpha,
                                                 ignored_percent);
                run.color_name = state.color_name;
                run.color_rgb = state.color_rgb;
                run.has_color = state.has_color;
                result.runs.push_back(std::move(run));
                }
                result.tags += 2;
                cursor = image_close + 6;
                plain_begin = cursor;
                continue;
            }
        }

        if (recognized) {
            ++result.tags;
        } else {
            append_text(result, state, text.substr(cursor, close - cursor + 1));
            ++result.malformed;
        }
        cursor = close + 1;
        plain_begin = cursor;
    }
    append_text(result, state, text.substr(plain_begin));
    return result;
}

std::string plain_rime_markup(
    const MarkupResult& markup,
    const std::map<std::string, std::string>& input_actions)
{
    std::string result;
    for (const MarkupRun& run : markup.runs) {
        std::string value;
        if (run.kind == MarkupRunKind::Text) value = run.text;
        else if (run.kind == MarkupRunKind::InputAction) {
            const auto found = input_actions.find(run.text);
            if (found != input_actions.end()) value = found->second;
        }
        if (run.uppercase)
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::toupper(c));
                           });
        result += value;
    }
    return result;
}

} // namespace bf6_ui
