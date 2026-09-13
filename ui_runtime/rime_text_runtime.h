#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6_ui {

enum class TextArgumentKind {
    String,
    Integer,
    Real,
};

struct TextArgument {
    TextArgumentKind kind = TextArgumentKind::String;
    std::string string;
    int64_t integer = 0;
    double real = 0.0;
};

struct TextFormatArguments {
    std::vector<TextArgument> positional;
    std::map<std::string, TextArgument> named;
};

struct TextFormatResult {
    std::string text;
    int substitutions = 0;
    int unresolved = 0;
    int malformed = 0;
};

TextArgument text_string(std::string value);
TextArgument text_integer(int64_t value);
TextArgument text_real(double value);

/* BF6's English table uses one brace dialect: {index:specifier}, plus the
 * named {props:map} form. Unknown or unresolved substitutions are omitted so
 * a client-only value can never leak a raw format token into the UI. */
TextFormatResult format_rime_text(const std::string& pattern,
                                  const TextFormatArguments& arguments);

enum class MarkupDevice {
    Any,
    Keyboard,
    Pad,
};

enum class MarkupRunKind {
    Text,
    InputAction,
    Image,
    Icon,
};

struct MarkupRun {
    MarkupRunKind kind = MarkupRunKind::Text;
    std::string text;
    std::string family;
    std::string asset;
    std::string display;
    std::string align;
    std::string config;
    std::string action_map;
    std::string color_name;
    uint32_t color_rgb = 0;
    float width = 0.0f;
    float height = 0.0f;
    float alpha = 1.0f;
    bool has_width = false;
    bool has_height = false;
    bool width_percent = false;
    bool height_percent = false;
    bool has_alpha = false;
    bool tint = false;
    bool has_color = false;
    bool uppercase = false;
};

struct MarkupResult {
    std::vector<MarkupRun> runs;
    int tags = 0;
    int malformed = 0;
};

/* Parses the seven tag families present in the mounted localization table:
 * color, inputAction, deviceFilter, img, family, transform and icon. */
MarkupResult parse_rime_markup(const std::string& text,
                               MarkupDevice device = MarkupDevice::Keyboard);

std::string plain_rime_markup(const MarkupResult& markup,
                              const std::map<std::string, std::string>&
                                  input_actions = {});

} // namespace bf6_ui
