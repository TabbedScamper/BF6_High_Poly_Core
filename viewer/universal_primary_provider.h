#pragma once

#include <string>
#include <vector>

#include "bf6_core.h"

namespace universal_primary {

constexpr const char* kPanelDataPartition =
    "common/ui/universalmenu/menucomponents/mainmenucomponent/paneldata/"
    "primary/playgroup_primarypaneldata";
constexpr const char* kCellPartition =
    "common/ui/universalmenu/widgets/visual/um_primarybuttonlistcell";

struct Entry {
    int input = -1;
    int localized_instance = -1;
    std::string label;
};

struct Report {
    int connections = 0;
    int localized_sources = 0;
    int complete_paths = 0;
    int ambiguous_paths = 0;
    int collections = 0;
    int input_hash_control_matches = 0;
    int fake_partition_connections = 0;
    bool event_collection_found = false;
    bool plain_collection_found = false;
};

struct Navigation {
    // Exact graph collections, before any standalone-host visibility policy.
    std::vector<Entry> event_entries;
    std::vector<Entry> plain_entries;
    Report report;
};

// Follows LocalizedStringEntity -> ButtonData -> ThemeButtonData -> InputN
// in the mounted playgroup graph. No instance number or copied label table is
// used. Both event and non-event collections are retained because the online
// kill-switch which selects between them is not available to an offline host.
bool read(bf6_ctx* context, Navigation& out);

// The current standalone policy omits graph placeholders whose service-side
// visibility cannot be evaluated. This never mutates the recovered graph and
// is kept separate so callers cannot mistake policy for shipped state.
std::vector<Entry> visible_offline(const Navigation& navigation,
                                   bool event_available);

} // namespace universal_primary
