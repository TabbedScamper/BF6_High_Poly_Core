#include "install_screen_source.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

using ElementKey = std::tuple<std::string, std::vector<bf6_ui::OccurrenceStep>, int32_t>;

ElementKey key_of(const bf6_ui::ElementAddress& address)
{
    return {address.partition, address.occurrence, address.local_instance};
}

struct ConvergenceEvidence {
    int root_edges = 0;
    int convergent_groups = 0;
    int mutated_convergent_groups = 0;
    int distinct_sources = 0;
    int target_instance = -1;
    uint32_t target_event = 0;
};

bool contains_skip(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.find("skip") != std::string::npos;
}

ConvergenceEvidence trace_logo_completion(
    const bf6_ui::DirectInstallScreenDocument& document)
{
    std::map<ElementKey, const rime::Element*> elements;
    const auto& addresses = document.element_addresses();
    const auto& rows = document.screen().elements;
    for (size_t i = 0; i < rows.size() && i < addresses.size(); ++i)
        elements.emplace(key_of(addresses[i]), &rows[i]);

    using OutputKey = std::pair<int32_t, uint32_t>;
    struct Sources { bool movie = false; std::set<int32_t> skip; };
    std::map<OutputKey, Sources> groups;
    std::map<OutputKey, Sources> mutated_groups;
    ConvergenceEvidence evidence;
    for (const bf6_ui::AuthoredEventEdge& edge : document.event_edges()) {
        if (!edge.occurrence.empty() ||
            edge.partition != document.identity().path) continue;
        ++evidence.root_edges;
        const auto source = elements.find(
            {edge.partition, edge.occurrence, edge.source_instance});
        if (source == elements.end()) continue;
        const bool movie = source->second->kind == rime::Kind::Movie;
        const bool skip = contains_skip(source->second->name);
        if (!movie && !skip) continue;
        const OutputKey key{edge.target_instance, edge.target_event};
        Sources& exact = groups[key];
        exact.movie = exact.movie || movie;
        if (skip) exact.skip.insert(edge.source_instance);

        // Negative control: perturb only the movie's target event. If the
        // apparent join is accidental, this should preserve its score.
        const OutputKey mutated{edge.target_instance,
            movie ? edge.target_event ^ 1u : edge.target_event};
        Sources& control = mutated_groups[mutated];
        control.movie = control.movie || movie;
        if (skip) control.skip.insert(edge.source_instance);
    }
    for (const auto& [key, sources] : groups) {
        if (!sources.movie || sources.skip.size() < 2) continue;
        ++evidence.convergent_groups;
        const int distinct = 1 + static_cast<int>(sources.skip.size());
        if (distinct > evidence.distinct_sources) {
            evidence.distinct_sources = distinct;
            evidence.target_instance = key.first;
            evidence.target_event = key.second;
        }
    }
    for (const auto& [key, sources] : mutated_groups) {
        (void)key;
        evidence.mutated_convergent_groups +=
            sources.movie && sources.skip.size() >= 2 ? 1 : 0;
    }
    return evidence;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: ui_boot_route_install_test <Steam BF6 directory>\n");
        return 2;
    }

    std::string error;
    auto source = bf6_ui::DirectInstallScreenSource::open(
        argv[1], bf6_ui::DirectInstallScreenSource::MountMode::WholeInstall,
        error);
    if (!source) {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 1;
    }

    bf6_ui::ScreenRouter router(*source);
    if (!router.refresh(error)) {
        std::fprintf(stderr, "catalogue: %s\n", error.c_str());
        return 1;
    }

    constexpr const char* kLogo =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowlogoscreen";
    constexpr const char* kStart =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowstartscreen";
    constexpr const char* kFake =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowlogoscreen__control__";

    const auto* logo_identity = router.find(kLogo);
    const auto* start_identity = router.find(kStart);
    const bool fake_absent = router.find(kFake) == nullptr;
    if (!logo_identity || !start_identity || !fake_absent) {
        std::fprintf(stderr, "catalogue identities/negative control failed\n");
        return 1;
    }

    if (!router.push(kLogo, error)) {
        std::fprintf(stderr, "logo: %s\n", error.c_str());
        return 1;
    }
    auto logo = std::dynamic_pointer_cast<bf6_ui::DirectInstallScreenDocument>(
        router.current()->document);
    if (!logo) return 1;
    const ConvergenceEvidence convergence = trace_logo_completion(*logo);

    if (!router.replace(kStart, error)) {
        std::fprintf(stderr, "start: %s\n", error.c_str());
        return 1;
    }
    auto start = std::dynamic_pointer_cast<bf6_ui::DirectInstallScreenDocument>(
        router.current()->document);
    if (!start) return 1;

    const bool identities_exact = logo->identity() == *logo_identity &&
                                  start->identity() == *start_identity;
    const bool no_fabricated_stack_edge = router.stack().size() == 1 &&
        router.current()->document->identity().path == kStart;
    const bool authored_completion = convergence.convergent_groups == 1 &&
        convergence.mutated_convergent_groups == 0 &&
        convergence.distinct_sources >= 4;
    std::printf(
                "logo=%d start=%d fake=%d exact=%d explicit_replace=%d "
                "root_edges=%d convergence=%d mutated=%d sources=%d "
                "output=%d:%08X\n",
                logo ? 1 : 0, start ? 1 : 0, fake_absent ? 0 : 1,
                identities_exact ? 1 : 0,
                no_fabricated_stack_edge ? 1 : 0,
                convergence.root_edges, convergence.convergent_groups,
                convergence.mutated_convergent_groups,
                convergence.distinct_sources, convergence.target_instance,
                convergence.target_event);
    return identities_exact && no_fabricated_stack_edge &&
        authored_completion ? 0 : 1;
}
