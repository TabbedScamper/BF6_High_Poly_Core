#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rime_text {

struct Line
{
    std::string text;
    float width = 0.f;
};

struct Layout
{
    std::vector<Line> lines;
    float max_width = 0.f;
};

inline bool horizontal_space(char value)
{
    return value == ' ' || value == '\t';
}

inline void trim_right_horizontal(std::string& value)
{
    while (!value.empty() && horizontal_space(value.back()))
        value.pop_back();
}

// Split explicit newlines and, when a finite width is supplied, wrap only at
// ASCII word boundaries. UTF-8 bytes remain opaque and are never split inside
// a code point. A word wider than the box remains whole so overflow/clipping
// policy stays with the Rime label rather than this layout helper.
template <typename Measure>
Layout layout_lines(std::string_view text, float wrap_width, Measure measure)
{
    Layout result;
    const bool wrap = wrap_width > 0.f;

    auto publish = [&](std::string line) {
        trim_right_horizontal(line);
        const float width = line.empty() ? 0.f : measure(line);
        result.max_width = (std::max)(result.max_width, width);
        result.lines.push_back(Line{std::move(line), width});
    };

    size_t paragraph_start = 0;
    for (;;)
    {
        const size_t newline = text.find('\n', paragraph_start);
        const size_t paragraph_end = newline == std::string_view::npos
            ? text.size() : newline;
        const std::string_view paragraph = text.substr(
            paragraph_start, paragraph_end - paragraph_start);

        if (!wrap)
        {
            publish(std::string(paragraph));
        }
        else if (paragraph.empty())
        {
            publish({});
        }
        else
        {
            std::string line;
            size_t begin = 0;
            while (begin < paragraph.size())
            {
                const bool whitespace = horizontal_space(paragraph[begin]);
                size_t end = begin + 1;
                while (end < paragraph.size() &&
                       horizontal_space(paragraph[end]) == whitespace)
                    ++end;
                const std::string_view token = paragraph.substr(
                    begin, end - begin);
                if (whitespace && line.empty())
                {
                    begin = end;
                    continue;
                }

                std::string candidate = line;
                candidate.append(token.data(), token.size());
                const float candidate_width = measure(candidate);
                if (!line.empty() && candidate_width > wrap_width)
                {
                    publish(std::move(line));
                    line.clear();
                    if (!whitespace)
                        line.assign(token.data(), token.size());
                }
                else
                {
                    line = std::move(candidate);
                }
                begin = end;
            }
            publish(std::move(line));
        }

        if (newline == std::string_view::npos) break;
        paragraph_start = newline + 1;
    }
    return result;
}

inline float block_height(size_t line_count, float glyph_height,
                          float line_advance)
{
    if (!line_count) return 0.f;
    const float first_line = (std::max)(glyph_height, line_advance);
    return first_line + static_cast<float>(line_count - 1) * line_advance;
}

// Rime's animated overflow modes still own the label's clip rectangle.  The
// animation changes the text origin; it never permits Marquee or PingPong
// glyphs to paint outside the authored box.
inline bool clips_to_label_box(int overflow_mode)
{
    return overflow_mode >= 1 && overflow_mode <= 4;
}

// ImDrawList wants a glyph-top y while Rime aligns a typographic line box.
// The installed screens' Baseline enum already supplies the glyph-top origin:
// subtracting the face ascent moves the Options navigation labels one full
// ascent above the live-client reference. It therefore bypasses line-box
// alignment and half-leading just like GlyphTop.
inline float first_glyph_y(float box_origin_y, float box_height,
                           float text_height, float glyph_height,
                           float line_advance, int vertical_alignment)
{
    if (vertical_alignment == 6) // Baseline
        return box_origin_y;

    float line_box_y = box_origin_y;
    if (vertical_alignment == 2 || vertical_alignment == 8)
        line_box_y += (box_height - text_height) * .5f;
    else if (vertical_alignment == 4)
        line_box_y += box_height - text_height;

    // GlyphTop explicitly aligns the glyph bounds rather than the line box.
    if (vertical_alignment != 7)
        line_box_y += (line_advance - glyph_height) * .5f;
    return line_box_y;
}

} // namespace rime_text
