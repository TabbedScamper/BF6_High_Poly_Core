#include "universal_primary_provider.h"

#include <algorithm>
#include <map>
#include <set>

#include "rime.h"

namespace universal_primary {
namespace {

// These are the three exact property edges measured in the current installed
// playgroup graph. Their endpoint topology is validated for every result, so
// an unrelated occurrence of one hash cannot become a navigation entry.
constexpr uint32_t kLocalizedToButtonSource = 0xD7D6CE6Fu;
constexpr uint32_t kLocalizedToButtonTarget = 0x8C93C90Bu;
constexpr uint32_t kButtonToThemeSource = 0x3E4BC2A3u;
constexpr uint32_t kButtonToThemeTarget = 0x66F09750u;
constexpr uint32_t kThemeToCollectionSource = 0xC51B3BB2u;

struct Candidate {
    int collection = -1;
    Entry entry;
};

int input_index(uint32_t field, bool control)
{
    for (int index = 0; index < 32; ++index)
    {
        const std::string name = "Input" + std::to_string(index);
        uint32_t expected = rime::property_hash(name.c_str());
        // InputN hashes form adjacent low-bit pairs, so a low-bit flip is a
        // different real slot rather than a negative control. Perturb the
        // high bit, which cannot alias any authored Input0..Input31 token.
        if (control) expected ^= 0x80000000u;
        if (field == expected) return index;
    }
    return -1;
}

bool has_label(const std::vector<Entry>& entries, const char* label)
{
    return std::any_of(entries.begin(), entries.end(),
        [label](const Entry& entry) { return entry.label == label; });
}

} // namespace

bool read(bf6_ctx* context, Navigation& out)
{
    out = Navigation{};
    if (!context) return false;
    const int count = bf6_rime_connections(
        context, kPanelDataPartition, nullptr, 0);
    out.report.connections = count > 0 ? count : 0;
    out.report.fake_partition_connections = bf6_rime_connections(
        context, "common/ui/__control__/playgroup_primarypaneldata", nullptr,
        0);
    if (count <= 0) return false;
    std::vector<bf6_rime_connection> edges(static_cast<size_t>(count));
    if (bf6_rime_connections(context, kPanelDataPartition, edges.data(),
                             count) != count)
        return false;

    std::set<int> localizedInstances;
    for (const bf6_rime_connection& edge : edges)
        if (edge.source_field == kLocalizedToButtonSource &&
            edge.target_field == kLocalizedToButtonTarget &&
            bf6_rime_string_entity(context, kPanelDataPartition,
                                   edge.source) != 0)
            localizedInstances.insert(edge.source);
    out.report.localized_sources =
        static_cast<int>(localizedInstances.size());

    std::vector<Candidate> candidates;
    for (int localized : localizedInstances)
    {
        std::vector<int> buttons;
        for (const bf6_rime_connection& edge : edges)
            if (edge.source == localized &&
                edge.source_field == kLocalizedToButtonSource &&
                edge.target_field == kLocalizedToButtonTarget)
                buttons.push_back(edge.target);
        std::sort(buttons.begin(), buttons.end());
        buttons.erase(std::unique(buttons.begin(), buttons.end()),
                      buttons.end());

        std::vector<std::pair<int, int>> collectionInputs;
        for (int button : buttons)
            for (const bf6_rime_connection& themeEdge : edges)
            {
                if (themeEdge.source != button ||
                    themeEdge.source_field != kButtonToThemeSource ||
                    themeEdge.target_field != kButtonToThemeTarget)
                    continue;
                for (const bf6_rime_connection& collectionEdge : edges)
                {
                    if (collectionEdge.source != themeEdge.target ||
                        collectionEdge.source_field !=
                            kThemeToCollectionSource)
                        continue;
                    const int index = input_index(
                        collectionEdge.target_field, false);
                    if (index >= 0)
                        collectionInputs.emplace_back(
                            collectionEdge.target, index);
                    if (input_index(collectionEdge.target_field, true) >= 0)
                        ++out.report.input_hash_control_matches;
                }
            }
        std::sort(collectionInputs.begin(), collectionInputs.end());
        collectionInputs.erase(
            std::unique(collectionInputs.begin(), collectionInputs.end()),
            collectionInputs.end());
        if (collectionInputs.empty()) continue;

        const uint32_t stringId = bf6_rime_string_entity(
            context, kPanelDataPartition, localized);
        const char* text = bf6_localized_string(context, stringId);
        if (!text || !text[0]) continue;
        for (const auto& location : collectionInputs)
        {
            Candidate candidate;
            candidate.collection = location.first;
            candidate.entry.input = location.second;
            candidate.entry.localized_instance = localized;
            candidate.entry.label = text;
            candidates.push_back(std::move(candidate));
            ++out.report.complete_paths;
        }
    }

    std::map<int, std::vector<Entry>> collections;
    for (const Candidate& candidate : candidates)
        collections[candidate.collection].push_back(candidate.entry);
    out.report.collections = static_cast<int>(collections.size());
    for (auto& pair : collections)
    {
        std::vector<Entry>& entries = pair.second;
        std::stable_sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.input < b.input; });
        bool uniqueInputs = true;
        for (size_t index = 1; index < entries.size(); ++index)
            if (entries[index - 1].input == entries[index].input)
                uniqueInputs = false;
        if (!uniqueInputs)
        {
            ++out.report.ambiguous_paths;
            continue;
        }
        const bool event = has_label(entries, "Event");
        std::vector<Entry>& destination = event
            ? out.event_entries : out.plain_entries;
        if (entries.size() > destination.size()) destination = entries;
    }
    out.report.event_collection_found = !out.event_entries.empty();
    out.report.plain_collection_found = !out.plain_entries.empty();
    return out.report.event_collection_found &&
           out.report.plain_collection_found &&
           out.report.input_hash_control_matches == 0 &&
           out.report.fake_partition_connections <= 0;
}

std::vector<Entry> visible_offline(const Navigation& navigation,
                                   bool event_available)
{
    const std::vector<Entry>& source = event_available
        ? navigation.event_entries : navigation.plain_entries;
    std::vector<Entry> result;
    for (const Entry& entry : source)
    {
        // These two entries are exact members of the installed collection,
        // but their visibility is selected by unavailable service/kill-switch
        // state. The standalone host has no progression route and the named
        // placeholder is the dynamic options slot, not a textual destination.
        if (entry.label == "Progression" || entry.label == "Placeholder")
            continue;
        result.push_back(entry);
    }
    return result;
}

} // namespace universal_primary
