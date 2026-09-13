#include "rime_state_bridge.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

rime_state::StateSlot text_slot(int reference_instance,
                                const char* value)
{
    rime_state::StateSlot slot;
    slot.partition = "common/ui/test/cell";
    slot.occurrence.push_back({"common/ui/test/root", reference_instance});
    slot.local_instance = 7;
    slot.field = rime::property_hash("Text");
    slot.value = rime_state::Value::from_string(value);
    return slot;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--wire")
    {
        std::vector<rime_state::StateSlot> streamed;
        rime_state::WireReport report;
        std::string error;
        if (!rime_state::read_wire(std::cin, streamed, report, error))
        {
            std::fprintf(stderr, "FAIL wire: %s\n", error.c_str());
            return 1;
        }
        std::printf("PASS wire route=%s slots=%d omitted=%d controls=%zu "
                    "provider_resolved=%d provider_remaining=%d\n",
                    report.route.c_str(), report.declared_slots,
                    report.omitted_non_scalar_slots, report.controls.size(),
                    report.resolved_consumed_external_provider_slots,
                    report.remaining_consumed_external_provider_slots);
        return streamed.empty() || !report.provider_metadata_present ? 1 : 0;
    }
    const char* wire_text =
        "BF6_RIME_STATE_V3\tcommon/ui/test/root\n"
        "M\t493\t13\t11\n"
        "P\t1\t10\n"
        "D\t1\t37\n"
        "S\tcommon/ui/test/cell\tcommon/ui/test/root@10\t7\t0x12345678\ts\tA%09B\tRimeLabelElementData\n"
        "C\tpassed\t1\n"
        "END\t1\t0\n";
    std::istringstream wire_input(wire_text);
    std::vector<rime_state::StateSlot> wire_slots;
    rime_state::WireReport wire_report;
    std::string wire_error;
    if (!rime_state::read_wire(
            wire_input, wire_slots, wire_report, wire_error) ||
        wire_slots.size() != 1 || wire_slots[0].value.string != "A\tB" ||
        wire_slots[0].source_type != "RimeLabelElementData" ||
        wire_slots[0].occurrence.size() != 1 ||
        !wire_report.requirement_metadata_present ||
        wire_report.host_requirement_slots != 493 ||
        wire_report.external_host_requirement_slots != 13 ||
        wire_report.consumed_external_host_requirement_slots != 11 ||
        !wire_report.provider_metadata_present ||
        !wire_report.dependency_metadata_present ||
        wire_report.dependency_closed_slots != 1 ||
        wire_report.excluded_provider_dependent_slots != 37 ||
        wire_report.resolved_consumed_external_provider_slots != 1 ||
        wire_report.remaining_consumed_external_provider_slots != 10 ||
        wire_report.route != "common/ui/test/root")
        return fail("valid live wire stream was not decoded");

    std::istringstream failed_control(
        "BF6_RIME_STATE_V3\tcommon/ui/test/root\n"
        "M\t0\t0\t0\nP\t0\t0\nD\t0\t0\n"
        "C\tpassed\t0\nEND\t0\t0\n");
    if (rime_state::read_wire(
            failed_control, wire_slots, wire_report, wire_error))
        return fail("failed capture controls were accepted");

    std::istringstream malformed_row(
        "BF6_RIME_STATE_V3\tcommon/ui/test/root\n"
        "S\ttoo\tfew\tfields\nEND\t0\t0\n");
    if (rime_state::read_wire(
            malformed_row, wire_slots, wire_report, wire_error) ||
        wire_error.find("line 2") == std::string::npos ||
        wire_error.find("fields=4") == std::string::npos)
        return fail("malformed wire diagnostic omitted safe row location");

    std::istringstream invalid_metadata(
        "BF6_RIME_STATE_V3\tcommon/ui/test/root\n"
        "M\t10\t11\t3\nC\tpassed\t1\nEND\t0\t0\n");
    if (rime_state::read_wire(
            invalid_metadata, wire_slots, wire_report, wire_error) ||
        wire_error.find("requirement metadata") == std::string::npos)
        return fail("impossible requirement count control was accepted");

    std::istringstream invalid_provider_metadata(
        "BF6_RIME_STATE_V3\tcommon/ui/test/root\n"
        "M\t10\t4\t3\nP\t1\t-1\nC\tpassed\t1\nEND\t0\t0\n");
    if (rime_state::read_wire(
            invalid_provider_metadata, wire_slots, wire_report, wire_error) ||
        wire_error.find("provider metadata") == std::string::npos)
        return fail("invalid provider count control was accepted");

    rime::Screen screen;

    rime::Element first_reference;
    first_reference.kind = rime::Kind::WidgetReference;
    first_reference.partition = "COMMON\\UI\\TEST\\ROOT.EBX";
    first_reference.references_widget = "common/ui/test/cell";
    first_reference.instance = 10;
    first_reference.scope = -1;
    screen.elements.push_back(first_reference);

    rime::Element second_reference = first_reference;
    second_reference.instance = 11;
    screen.elements.push_back(second_reference);

    rime::Element first_label;
    first_label.kind = rime::Kind::Label;
    first_label.partition = "common/ui/test/cell";
    first_label.instance = 7;
    first_label.scope = 0;
    first_label.text = "authored";
    screen.elements.push_back(first_label);

    rime::Element second_label = first_label;
    second_label.scope = 1;
    screen.elements.push_back(second_label);

    const std::vector<rime_state::StateSlot> exact = {
        text_slot(10, "ALPHA"), text_slot(11, "BRAVO")};
    const rime_state::ApplyReport exact_report =
        rime_state::apply_state(screen, exact);
    if (exact_report.applied_slots != 2 ||
        screen.elements[2].text != "ALPHA" ||
        screen.elements[3].text != "BRAVO")
        return fail("exact occurrence paths did not isolate repeated cells");

    /* Shuffled occurrence control: both values still apply, but to the other
     * authored occurrence.  This proves the path participates in selection. */
    rime::Screen shuffled = screen;
    const std::vector<rime_state::StateSlot> swapped = {
        text_slot(11, "ALPHA"), text_slot(10, "BRAVO")};
    const rime_state::ApplyReport shuffled_report =
        rime_state::apply_state(shuffled, swapped);
    if (shuffled_report.applied_slots != 2 ||
        shuffled.elements[2].text != "BRAVO" ||
        shuffled.elements[3].text != "ALPHA")
        return fail("shuffled occurrence control did not discriminate");

    rime::Screen fake = screen;
    const std::vector<rime_state::StateSlot> fake_slots = {
        text_slot(999, "FABRICATED")};
    const rime_state::ApplyReport fake_report =
        rime_state::apply_state(fake, fake_slots);
    if (fake_report.applied_slots != 0 ||
        fake_report.missing_occurrences != 1 ||
        fake.elements[2].text != "ALPHA" || fake.elements[3].text != "BRAVO")
        return fail("fabricated occurrence was not rejected without mutation");

    rime_state::StateSlot wrong_type = text_slot(10, "ignored");
    wrong_type.field = rime::property_hash("Visible");
    wrong_type.value = rime_state::Value::from_string("true");
    const rime_state::ApplyReport type_report =
        rime_state::apply_state(fake, {wrong_type});
    if (type_report.applied_slots != 0 || type_report.type_mismatches != 1)
        return fail("wrong-type control was not rejected");

    rime_state::StateSlot spacing = text_slot(10, "ignored");
    spacing.field = rime::property_hash("ItemSpacing");
    spacing.value = rime_state::Value::from_real(19.5);
    rime_state::StateSlot line_progress = text_slot(10, "ignored");
    line_progress.field = rime::property_hash("StartProgress");
    line_progress.value = rime_state::Value::from_real(0.375);
    rime_state::StateSlot segment_count = text_slot(10, "ignored");
    segment_count.field = rime::property_hash("SegmentCount");
    segment_count.value = rime_state::Value::from_int(7);
    rime::Screen scalar_screen = screen;
    scalar_screen.elements[2].kind = rime::Kind::Line;
    const rime_state::ApplyReport scalar_report = rime_state::apply_state(
        scalar_screen, {spacing, line_progress, segment_count});
    if (scalar_report.applied_slots != 3 ||
        scalar_screen.elements[2].item_spacing != 19.5f ||
        scalar_screen.elements[2].line_start_progress != 0.375f ||
        scalar_screen.elements[2].progress_segment_count != 7)
        return fail("decoded renderer scalar state was not applied");

    rime_state::StateSlot unknown = text_slot(10, "ignored");
    unknown.field ^= 0x80000000u;
    const rime_state::ApplyReport field_report =
        rime_state::apply_state(fake, {unknown});
    if (field_report.applied_slots != 0 || field_report.unsupported_fields != 1)
        return fail("perturbed-field control was not rejected");

    rime::Screen logical = screen;
    logical.elements[2].kind = rime::Kind::Unknown;
    const rime_state::ApplyReport logical_report =
        rime_state::apply_state(logical, {text_slot(10, "NOT_RENDERED")});
    if (logical_report.applied_slots != 0 ||
        logical_report.non_renderer_targets != 1 ||
        logical.elements[2].text != "ALPHA")
        return fail("logical/non-renderer target was mutated");

    std::printf("PASS exact=2 shuffled=2 scalar=3 fake=0 wrong_type=0 "
                "perturbed_field=0\n");
    return 0;
}
