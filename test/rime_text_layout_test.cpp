#include "rime_text_layout.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

bool near(float left, float right)
{
    return std::fabs(left - right) < 0.001f;
}

} // namespace

int main()
{
    const auto measure = [](const std::string& text) {
        return static_cast<float>(text.size());
    };

    const auto explicit_lines = rime_text::layout_lines(
        "ALPHA\n\nBRAVO", 0.f, measure);
    check(explicit_lines.lines.size() == 3 &&
          explicit_lines.lines[0].text == "ALPHA" &&
          explicit_lines.lines[1].text.empty() &&
          explicit_lines.lines[2].text == "BRAVO",
          "explicit and empty lines are retained");

    const auto wrapped = rime_text::layout_lines(
        "ONE TWO THREE", 7.f, measure);
    check(wrapped.lines.size() == 2 &&
          wrapped.lines[0].text == "ONE TWO" &&
          wrapped.lines[1].text == "THREE",
          "wrapping occurs at a measured word boundary");

    const auto leading = rime_text::layout_lines(
        "ONE      TWO", 5.f, measure);
    check(leading.lines.size() == 2 &&
          leading.lines[0].text == "ONE" &&
          leading.lines[1].text == "TWO",
          "wrapped lines discard boundary whitespace");

    const auto long_word = rime_text::layout_lines(
        "OVERSIZED", 4.f, measure);
    check(long_word.lines.size() == 1 &&
          long_word.lines[0].text == "OVERSIZED" &&
          near(long_word.max_width, 9.f),
          "an oversized UTF-8-safe token remains intact for overflow policy");

    const auto terminal = rime_text::layout_lines(
        "ONE\n", 0.f, measure);
    check(terminal.lines.size() == 2 && terminal.lines[1].text.empty(),
          "a terminal newline retains its empty line");

    check(near(rime_text::block_height(3, 18.f, 24.f), 72.f) &&
          near(rime_text::block_height(3, 30.f, 24.f), 78.f),
          "pixel line advance handles loose and tight authored leading");

    check(!rime_text::clips_to_label_box(0) &&
          rime_text::clips_to_label_box(1) &&
          rime_text::clips_to_label_box(2) &&
          rime_text::clips_to_label_box(3) &&
          rime_text::clips_to_label_box(4) &&
          !rime_text::clips_to_label_box(5),
          "animated and truncating overflow modes retain the label clip");

    check(near(rime_text::first_glyph_y(
                   100.f, 40.f, 24.f, 18.f, 24.f, 3), 103.f) &&
          near(rime_text::first_glyph_y(
                   100.f, 40.f, 24.f, 18.f, 24.f, 2), 111.f) &&
          near(rime_text::first_glyph_y(
                   100.f, 40.f, 24.f, 18.f, 24.f, 4), 119.f) &&
          near(rime_text::first_glyph_y(
                   100.f, 40.f, 24.f, 18.f, 24.f, 7), 100.f) &&
          near(rime_text::first_glyph_y(
                   100.f, 40.f, 24.f, 18.f, 24.f, 6), 100.f),
          "line-box half-leading preserves authored glyph-top alignments");

    return failures ? 1 : 0;
}
