#include "authored_event_dispatch.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bf6_ui;

class Document final : public AuthoredScreenDocument {
public:
    Document()
    {
        identity_ = {"common/ui/test/screens/exact", 100};
        a_ = {"common/ui/test/widgets/button", {{"root", 3}}, 7};
        b_ = {"common/ui/test/widgets/button", {{"root", 4}}, 7};
        elements_ = {a_, b_};
        edges_ = {
            {a_.partition, a_.occurrence, 7, 8, 0x11111111u, 0x22222222u, 2},
            {a_.partition, a_.occurrence, 7, 9, 0x11111111u, 0x33333333u, 2},
            {b_.partition, b_.occurrence, 7, 8, 0x11111111u, 0x22222222u, 2},
            /* Same id on the target side is not an outgoing source. */
            {a_.partition, a_.occurrence, 12, 7, 0x44444444u, 0x11111111u, 2},
        };
    }
    const ScreenIdentity& identity() const override { return identity_; }
    bool contains(const ElementAddress& value) const override {
        for (const auto& row : elements_) if (row == value) return true;
        return false;
    }
    const std::vector<AuthoredEventEdge>& event_edges() const override {
        return edges_;
    }
    const ElementAddress& a() const { return a_; }
    const ElementAddress& b() const { return b_; }

private:
    ScreenIdentity identity_;
    ElementAddress a_, b_;
    std::vector<ElementAddress> elements_;
    std::vector<AuthoredEventEdge> edges_;
};

bool require(bool value, const char* text)
{
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", text);
    return false;
}
} // namespace

int main()
{
    bool ok = true;
    Document document;
    AuthoredEventBatch batch;
    std::string error;
    ok &= require(exact_focused_activation(
                      document, document.a(), 0x11111111u, batch, error),
                  "exact focused source did not fan out");
    ok &= require(batch.edges.size() == 2,
                  "exact source did not preserve its two authored edges");
    ok &= require(!batch.ordering_resolved && !batch.route_mutation_authorized,
                  "static graph invented ordering or route authority");

    AuthoredEventBatch other;
    ok &= require(exact_focused_activation(
                      document, document.b(), 0x11111111u, other, error) &&
                      other.edges.size() == 1,
                  "duplicate local id crossed occurrence boundary");
    const size_t other_occurrence_edges = other.edges.size();
    ok &= require(!exact_focused_activation(
                      document, document.a(), 0x11111110u, other, error),
                  "xor-one source event matched");
    ElementAddress shuffled = document.a();
    shuffled.occurrence = document.b().occurrence;
    shuffled.local_instance = 99;
    ok &= require(!exact_focused_activation(
                      document, shuffled, 0x11111111u, other, error),
                  "shuffled focus identity matched");

    const InterfaceEventOutput exact{
        document.a().partition, document.a().occurrence, 9, 0x33333333u};
    auto handoffs = exact_provider_handoffs(batch, {exact});
    ok &= require(handoffs.size() == 1,
                  "exact interface output was not reported as a handoff");
    InterfaceEventOutput mutated = exact;
    mutated.event ^= 1u;
    ok &= require(exact_provider_handoffs(batch, {mutated}).empty(),
                  "mutated interface output matched");
    ok &= require(batch.source.partition.find("/screens/") == std::string::npos,
                  "test accidentally smuggled a destination screen into event data");

    std::printf("fanout=%zu other_occurrence=%zu exact_handoffs=%zu "
                "xor_event=0 shuffled_focus=0 mutated_handoff=0 "
                "ordering=unresolved route=unresolved\n",
                batch.edges.size(), other_occurrence_edges, handoffs.size());
    return ok ? 0 : 1;
}
