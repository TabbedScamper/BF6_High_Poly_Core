#include "rime_text_runtime.h"

#include <cstdio>
#include <map>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    std::printf("%-58s %s\n", name, condition ? "ok" : "FAIL");
    if (!condition) ++failures;
}

} // namespace

int main()
{
    bf6_ui::TextFormatArguments values;
    values.positional = {
        bf6_ui::text_string("Bravo"),
        bf6_ui::text_integer(125),
        bf6_ui::text_real(3.45),
        bf6_ui::text_integer(3723),
        bf6_ui::text_string("SPACE"),
    };
    values.named.emplace("props", bf6_ui::text_string("Wake Island"));

    const auto formatted = bf6_ui::format_rime_text(
        "{0:s} {1:d} {2:f.1} {1:ms} {3:time:mm:ss} "
        "{4:input:SingleKeyIcon} {props:map}", values);
    check(formatted.text ==
              "Bravo 125 3.5 125 ms 01:02:03 SPACE Wake Island",
          "all installed brace specifier families format");
    check(formatted.substitutions == 7 && !formatted.unresolved &&
              !formatted.malformed,
          "complete arguments leave no raw-token failures");

    const auto missing = bf6_ui::format_rime_text(
        "VERSION: {0:S} CODE: {9:d} {{literal}}", values);
    check(missing.text == "VERSION: Bravo CODE:  {literal}",
          "unavailable client values disappear without invented text");
    check(missing.unresolved == 1,
          "missing substitution is reported");

    const std::string marked =
        "A [color=#59BFF8]blue [family=Numbers_Monospaced_Medium]"
        "number[/family][/color] [transform=Uppercase]word[/transform] "
        "[inputAction ConceptMenuBack]"
        "[deviceFilter device=pad]PAD[/deviceFilter]"
        "[deviceFilter device=keyboard]KEY[/deviceFilter]";
    const auto markup = bf6_ui::parse_rime_markup(
        marked, bf6_ui::MarkupDevice::Keyboard);
    check(markup.malformed == 0 && markup.tags == 11,
          "nested installed markup families parse without literal tags");
    check(markup.runs.size() >= 6,
          "style and action boundaries remain separate runs");
    std::map<std::string, std::string> actions = {
        {"ConceptMenuBack", "ESC"},
    };
    check(bf6_ui::plain_rime_markup(markup, actions) ==
              "A blue number WORD ESCKEY",
          "device filter, transform, and input action flatten correctly");

    const auto paired_action = bf6_ui::parse_rime_markup(
        "Press [inputAction config=Hint actionMap=infantry height=150% "
        "tint=True]ConceptPingInput[/inputAction] to order your squad");
    check(paired_action.malformed == 0 && paired_action.tags == 2 &&
              paired_action.runs.size() == 3 &&
              paired_action.runs[1].kind ==
                  bf6_ui::MarkupRunKind::InputAction &&
              paired_action.runs[1].text == "ConceptPingInput" &&
              paired_action.runs[1].config == "Hint" &&
              paired_action.runs[1].action_map == "infantry" &&
              paired_action.runs[1].has_height &&
              paired_action.runs[1].height_percent &&
              paired_action.runs[1].height == 150.0f &&
              paired_action.runs[1].tint,
          "paired inputAction reads concept body and presentation attributes");
    actions["ConceptPingInput"] = "Q";
    check(bf6_ui::plain_rime_markup(paired_action, actions) ==
              "Press Q to order your squad",
          "paired inputAction closing tag is consumed, never painted literally");

    const auto image = bf6_ui::parse_rime_markup(
        "[img display=inline tint=True align=center height=175%]bot[/img] "
        "[img tint=true width=16 alpha=0.5]SpeechToText[/img] [icon]");
    check(image.runs.size() == 5 &&
              image.runs[0].kind == bf6_ui::MarkupRunKind::Image &&
              image.runs[0].asset == "bot" &&
              image.runs[0].display == "inline" &&
              image.runs[0].align == "center" && image.runs[0].tint &&
              image.runs[0].has_height && image.runs[0].height_percent &&
              image.runs[0].height == 175.0f &&
              image.runs[2].kind == bf6_ui::MarkupRunKind::Image &&
              image.runs[2].asset == "SpeechToText" &&
              image.runs[2].has_width && image.runs[2].width == 16.0f &&
              image.runs[2].has_alpha && image.runs[2].alpha == 0.5f,
          "installed img tags retain body asset and layout attributes");
    check(image.tags == 5 && image.malformed == 0,
          "paired img and standalone icon tags are consumed exactly");

    const auto icon = bf6_ui::parse_rime_markup("MCOM [icon]");
    check(icon.runs.size() == 2 &&
              icon.runs[1].kind == bf6_ui::MarkupRunKind::Icon &&
              icon.runs[1].asset.empty(),
          "installed operand-free icon tag remains an explicit run");

    const auto broken_image = bf6_ui::parse_rime_markup(
        "before [img height=150%]bomb after");
    check(broken_image.malformed == 1 && broken_image.runs.size() == 1 &&
              broken_image.runs[0].kind == bf6_ui::MarkupRunKind::Text &&
              broken_image.runs[0].text == "before bomb after",
          "unclosed img falls back to readable text, never an image run");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED",
                failures);
    return failures ? 1 : 0;
}
