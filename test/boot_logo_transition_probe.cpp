#include "install_screen_source.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

using ElementKey = std::tuple<std::string, std::vector<bf6_ui::OccurrenceStep>, int32_t>;

ElementKey key_of(const bf6_ui::ElementAddress& address)
{
    return {address.partition, address.occurrence, address.local_instance};
}

bool contains_skip(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.find("skip") != std::string::npos;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: boot_logo_transition_probe <Steam BF6 directory>\n");
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
    if (!router.refresh(error) || !router.push(
            "game/glacierflow/flow_mainmenu/ui/screens/bootflowlogoscreen",
            error)) {
        std::fprintf(stderr, "load: %s\n", error.c_str());
        return 1;
    }
    auto document = std::dynamic_pointer_cast<bf6_ui::DirectInstallScreenDocument>(
        router.current()->document);
    if (!document) return 1;

    std::map<ElementKey, const rime::Element*> elements;
    const auto& addresses = document->element_addresses();
    const auto& rows = document->screen().elements;
    for (size_t i = 0; i < rows.size() && i < addresses.size(); ++i)
        elements.emplace(key_of(addresses[i]), &rows[i]);

    int selected = 0;
    for (const bf6_ui::AuthoredEventEdge& edge : document->event_edges()) {
        if (!edge.occurrence.empty() ||
            edge.partition != document->identity().path) continue;
        const auto source_element = elements.find(
            {edge.partition, edge.occurrence, edge.source_instance});
        const auto target_element = elements.find(
            {edge.partition, edge.occurrence, edge.target_instance});
        if (source_element == elements.end()) continue;
        const bool movie = source_element->second->kind == rime::Kind::Movie;
        const bool skip = contains_skip(source_element->second->name);
        if (!movie && !skip) continue;
        ++selected;
        std::printf(
            "source=%d name=%s kind=%s source_event=0x%08X "
            "target=%d target_name=%s target_event=0x%08X mode=%d\n",
            edge.source_instance, source_element->second->name.c_str(),
            movie ? "movie" : "skip",
            edge.source_event, edge.target_instance,
            target_element == elements.end() ? "<nonvisual>" :
                target_element->second->name.c_str(),
            edge.target_event, edge.mode);
    }

    std::printf("sink_instance_7_edges:\n");
    for (const bf6_ui::AuthoredEventEdge& edge : document->event_edges()) {
        if (!edge.occurrence.empty() ||
            edge.partition != document->identity().path ||
            (edge.source_instance != 7 && edge.target_instance != 7)) continue;
        std::printf("  %d:0x%08X -> %d:0x%08X mode=%d\n",
                    edge.source_instance, edge.source_event,
                    edge.target_instance, edge.target_event, edge.mode);
    }

    const int property_count = bf6_rime_connections(
        source->context(), document->identity().path.c_str(), nullptr, 0);
    std::vector<bf6_rime_connection> properties(
        property_count > 0 ? static_cast<size_t>(property_count) : 0u);
    if (property_count > 0)
        bf6_rime_connections(source->context(), document->identity().path.c_str(),
                              properties.data(), property_count);
    std::printf("sink_instance_7_properties:\n");
    for (const bf6_rime_connection& edge : properties) {
        if (edge.source != 7 && edge.target != 7) continue;
        std::printf("  %d:0x%08X -> %d:0x%08X mode=%d\n",
                    edge.source, edge.source_field,
                    edge.target, edge.target_field, edge.mode);
    }

    // A one-bit mutation must not be accepted as an authored edge.
    bool mutation_rejected = false;
    if (!document->event_edges().empty()) {
        bf6_ui::AuthoredEventEdge fake = document->event_edges().front();
        fake.target_event ^= 1u;
        mutation_rejected = !router.dispatch_authored_event(fake, error);
    }
    const bool fake_route_absent = router.find(
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowlogoscreen__control__") == nullptr;
    std::printf("selected=%d mutation_rejected=%d fake_route_absent=%d\n",
                selected, mutation_rejected ? 1 : 0,
                fake_route_absent ? 1 : 0);
    return selected > 0 && mutation_rejected && fake_route_absent ? 0 : 1;
}
