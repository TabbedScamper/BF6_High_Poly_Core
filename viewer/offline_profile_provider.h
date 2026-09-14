#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bf6_core.h"
#include "rime_list_provider.h"

namespace offline_profile {

/* Explicit provenance for synthetic state.  These values stand in only for
 * account/network services that do not exist in the offline viewer.  They are
 * never used for names, art, layout, weapon data, or any other shipped value. */
constexpr const char* kProviderName = "offline-profile-placeholder";
constexpr const char* kDisplayName = "OFFLINE";

struct Evidence {
    std::string field;
    uint32_t property_id = 0;
    uint64_t type_signature = 0;
    int real_routes = 0;
    int shuffled_routes = 0;
};

struct Report {
    int candidate_rules = 0;
    int explicit_values_preserved = 0;
    int missing_contract_fields = 0;
    int type_mismatches = 0;
    int unrouted = 0;
    int applied = 0;
    int shuffled_control_routes = 0;
    std::vector<Evidence> evidence;
};

/* Append only placeholders that satisfy all four gates:
 *
 *   exact DBD partition -> exact current-install field name -> expected
 *   current-install TypeRef signature -> real graph route with zero shuffled
 *   routes.
 *
 * Existing values win.  This lets direct game data or local viewer state
 * replace a placeholder without changing call order.  A field that does not
 * reach the supplied graph remains absent rather than making the transactional
 * Rime binder reject the entire record. */
Report append_routed(bf6_ctx* ctx, const char* dbd_partition,
                     const char* graph_partition,
                     const rime_list::DbdContract& contract,
                     rime_list::Record& record);

} // namespace offline_profile
