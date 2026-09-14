#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bf6_core.h"
#include "rime.h"

namespace rime_list {

/* The public MenuWeaponScreen contract.  These ids are dynamic Rime property
 * ids (case-sensitive djb2-xor), not EBX reflection hashes. */
constexpr uint32_t kGridViewData = 0x2B6D8E5Fu;
constexpr uint32_t kGridItemCollectionData = 0xED5A88EEu;
constexpr const char* kDataListTypeGuid =
    "2420ee41-1c4d-38bd-4203-777f31cf9c9a";
constexpr const char* kGridItemDbdPartition =
    "common/ui/metacore/metacustomization/assets/databindings/"
    "metacustomization_griditemdbd";
constexpr const char* kHeaderInfoDbdPartition =
    "common/ui/metacore/metacustomization/assets/databindings/"
    "metacustomization_headerinfodbd";
constexpr const char* kGridInfoPartition =
    "common/ui/metacore/metacustomization/widgets/"
    "metacustomization_gridinfo";
constexpr const char* kGridItemCellPartition =
    "common/ui/metacore/metacustomization/widgets/"
    "metacustomization_griditemcell";
constexpr const char* kWeaponInfoHeaderPartition =
    "common/ui/weapons/widgets/weaponinfoheader";
constexpr const char* kArchetypesDbdPartition =
    "common/ui/weaponcustomization/assets/databindings/archetypesdbd";
constexpr const char* kWeaponTagCellPartition =
    "common/ui/weapons/widgets/menuweapontagcell";
/* Universal-panel and weapon-navigation assets are deliberately named here,
 * rather than reduced to copied GUIDs.  Their partition GUIDs and DBD fields
 * are read from the mounted install by the runtime resolver/test. */
constexpr const char* kUniversalPanelDbdPartition =
    "common/ui/universalmenu/assets/databindings/um_universalpaneldatadbd";
constexpr const char* kCategoryLabelDbdPartition =
    "common/ui/universalmenu/assets/databindings/um_categorylabeldbd";
constexpr const char* kCategoryLabelCellPartition =
    "common/ui/universalmenu/widgets/universalpanellistcells/"
    "um_categorylabelcell";
constexpr const char* kInteractableButtonDbdPartition =
    "common/ui/universalmenu/assets/databindings/"
    "um_interactablebuttondbd";
constexpr const char* kLabelButtonCellPartition =
    "common/ui/componentlibrary/components/buttons/cl_labelbuttoncell";
constexpr const char* kRectangleGlowLabelCellPartition =
    "common/ui/componentlibrary/components/labels/"
    "cl_celllabel_rectangleglow";
constexpr const char* kGridIconCellPartition =
    "common/ui/metacore/metacustomization/widgets/"
    "metacustomization_gridiconcell";
constexpr const char* kDividerVerticalCellPartition =
    "common/ui/componentlibrary/components/borders/cl_dividerverticalcell";
constexpr const char* kCellTagDbdPartition =
    "common/ui/componentlibrary/shared/assets/dbd/cb_celltagdbd";
constexpr const char* kGridIconCellDbdPartition =
    "common/ui/metacore/metacustomization/assets/databindings/"
    "metacustomization_gridiconcelldbd";
constexpr const char* kDividerCellDbdPartition =
    "common/ui/componentlibrary/shared/assets/dbd/cb_dividercelldbd";
constexpr const char* kNavigationDataDbdPartition =
    "common/ui/universalmenu/assets/databindings/um_navigationdatadbd";
constexpr const char* kNavigationDataCellPartition =
    "common/ui/universalmenu/widgets/universalpanellistcells/"
    "um_navigationdatacell";
constexpr const char* kCollapseButtonDbdPartition =
    "common/ui/universalmenu/assets/databindings/um_collapsebuttondata";
constexpr const char* kCollapseButtonCellPartition =
    "common/ui/universalmenu/widgets/visual/"
    "um_collapsebuttonlistcell";
constexpr const char* kWeaponCollectionColumnDbdPartition =
    "common/ui/weaponcustomization/assets/databindings/"
    "weaponcollectioncolumndbd";
constexpr const char* kWeaponNavigationGeneratorPartition =
    "common/ui/weapons/uiviewmodel/um_weaponnavigationlistgenerator";
constexpr const char* kBackButtonPartition =
    "common/ui/universalmenu/widgets/visual/um_backbutton";
constexpr const char* kAttachmentDbdPartition =
    "common/ui/weaponcustomization/assets/databindings/"
    "weaponcustomizationattachmentdbd";
constexpr const char* kAttachmentCellPartition =
    "common/ui/weaponcustomization/widgets/weaponattachmentselectioncell";
constexpr const char* kIconizedAttributesDbdPartition =
    "common/ui/weaponcustomization/assets/databindings/"
    "iconizedattributesdbd";
constexpr const char* kNumericalStatsDbdPartition =
    "common/ui/weaponcustomization/assets/databindings/"
    "numericalstatsdbd";
constexpr const char* kIconizedAttributesCellPartition =
    "common/ui/weaponcustomization/widgets/weaponattributesiconizedcell";
constexpr const char* kAttributesCellPartition =
    "common/ui/weaponcustomization/widgets/weaponattributescell";
constexpr const char* kAttributesDeltaCellPartition =
    "common/ui/weaponcustomization/widgets/weaponattributesdelta";
constexpr const char* kExtendedAttributesCellPartition =
    "common/ui/weaponcustomization/widgets/weaponextendedattributescell";

struct FieldSpec {
    uint32_t id;
    const char* name;
};

const FieldSpec* root_fields(size_t* count = nullptr);
const FieldSpec* item_fields(size_t* count = nullptr);
const FieldSpec* header_fields(size_t* count = nullptr);
const FieldSpec* root_field(uint32_t id);
const FieldSpec* item_field(uint32_t id);
const FieldSpec* header_field(uint32_t id);

struct DbdField {
    std::string name;
    uint32_t property_id = 0;
    uint32_t provider_id = 0;
    uint64_t type_signature = 0;
};

struct DbdContract {
    std::string data_name;
    std::vector<DbdField> fields;
};

const DbdField* contract_field(const DbdContract& contract,
                               uint32_t property_id);
const DbdField* contract_field(const DbdContract& contract,
                               const char* property_name);

/* Read the DBD definition from the mounted install.  provider_id is BF6's
 * exact qualified runtime pin: djb2-xor(DataName + "." + PropertyName).
 * type_signature is the shipped TypeRef payload retained verbatim; opaque
 * signatures are not relabelled as strings/textures by this layer. */
bool load_item_contract(bf6_ctx* ctx, DbdContract& out,
                        const char* partition = kGridItemDbdPartition);

/* Generic form used by every runtime DataBinding Definition.  The item-name
 * wrapper above remains for source compatibility. */
bool load_contract(bf6_ctx* ctx, DbdContract& out, const char* partition);

/* Values are deliberately tagged by the caller.  The DBD establishes the
 * field names but does not prove that Icon/Texture/CellData are strings (they
 * are not), so this adapter never coerces an opaque runtime object into text.
 * A missing entry stays missing; in particular favorite/lock/package/filter
 * state is never defaulted or inferred. */
enum class ValueKind { Null, Bool, Int, UInt, Real, String, Opaque };

struct Value {
    ValueKind kind = ValueKind::Null;
    bool boolean = false;
    int64_t integer = 0;
    uint64_t unsigned_integer = 0;
    double real = 0.0;
    std::string string;
    const void* opaque = nullptr;
};

struct Entry {
    uint32_t field = 0;
    Value value;
};

class Record {
public:
    /* Unknown/fabricated ids are rejected.  Repeated ids replace their prior
     * explicit value, matching a provider snapshot rather than accumulating
     * stale state across frames. */
    bool put(uint32_t field, const Value& value);
    /* Contract-scoped form: accept only a property present in the DBD read
     * from this mounted install.  This is the path for HeaderInfo and future
     * provider records whose field set is not the GridItem catalogue. */
    bool put(const DbdContract& contract, uint32_t field, const Value& value);
    const Entry* find(uint32_t field) const;
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

struct DataList {
    int row = -1;
    int instance = -1;
    int parent = -1;
    std::string partition;
    std::string name;
    std::string item_template;
    std::vector<std::string> item_templates;
    float item_spacing = 0.f;
    int orientation = -1;          // inherited Rime stack axis: 0 H, 1 V
    int size_distribution = -1;
    int flow_direction = -1;       // Default/Reverse/TextDirection, not axis
    int space_distribution = -1;
    int preserve_fit_content = -1;
};

struct ContractReport {
    int interface_descriptors = 0;
    int exact_root_wires = 0;
    int bottom_cap_wires = 0;
    int wrong_target_wires = 0;
    std::vector<DataList> lists;
};

/* Find one exact list in a live expanded tree.  Duplicate matches reject the
 * query: selecting the first same-named list would be the list equivalent of
 * widening a texture lookup to a sibling bundle. */
bool find_unique_list(const bf6_rime_node* rows, int row_count,
                      const char* owner_partition, const char* list_name,
                      DataList& out, int* matches = nullptr);

/* Inspect the mounted game's own graph.  `rows` must be the live expanded
 * bf6_rime_tree result for `partition`; no exported TSV/JSON participates. */
ContractReport inspect(bf6_ctx* ctx, const char* partition,
                       const bf6_rime_node* rows, int row_count,
                       uint32_t root_property = kGridViewData);

struct RouteReport {
    int supplied = 0;
    int declared = 0;
    int connected = 0;
    int unrouted = 0;
    std::vector<uint32_t> declared_fields;
    std::vector<uint32_t> connected_fields;
    std::vector<uint32_t> unrouted_fields;
    struct Evidence {
        uint32_t field = 0;
        uint32_t provider_field = 0;
        std::string partition;
        int interface_declarations = 0;
        int source_wires = 0;
        int target_wires = 0;
        int target_widget_wires = 0;
    };
    std::vector<Evidence> evidence;
};

/* Measure whether supplied item fields occur on the shipped item widget's
 * authored property graph.  The adapter derives BF6's qualified provider pin
 * from the mounted DBD and requires that exact pin on the source side of a
 * shipped connection.  Downstream occurrences of the short property id do
 * not count as provider evidence.  This is not an element-name writer. */
RouteReport route(bf6_ctx* ctx, const char* item_partition,
                  const Record& record);

/* Required null control: deterministically perturb every token before testing
 * it against the same live descriptor and connection graph. */
RouteReport route_shuffled_control(bf6_ctx* ctx, const char* item_partition,
                                   const Record& record);

/* Route a record through a live graph while requiring its qualified provider
 * pin to terminate on a widget reference to `target_widget_partition`.
 * Passing nullptr for the target preserves the source-pin-only census used by
 * the older GridItem proof. */
RouteReport route_contract(bf6_ctx* ctx, const char* dbd_partition,
                           const char* graph_partition,
                           const char* target_widget_partition,
                           const Record& record);
RouteReport route_contract_shuffled_control(
    bf6_ctx* ctx, const char* dbd_partition, const char* graph_partition,
    const char* target_widget_partition, const Record& record);

struct BindReport {
    int supplied = 0;
    int declared = 0;
    int applied_fields = 0;
    int applied_targets = 0;
    int unsupported_values = 0;
    int unrouted = 0;
    int ambiguous = 0;
};

/* Apply one provider snapshot to one already-loaded live item template.  Only
 * primitive values represented by bf6_rime_interface_field are accepted.
 * Null and Opaque deliberately remain unresolved; notably this prevents an
 * image pointer or runtime collection object being coerced into a filename.
 * Binding is transactional: if any supplied field is unsupported, unrouted,
 * or ambiguous, the input Screen remains unchanged.  The shuffled form is a
 * required negative control. */
BindReport bind_record(rime::Screen& item, const char* graph_partition,
                       const DbdContract& contract, const Record& record);
BindReport bind_record_shuffled_control(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record);

/* Transaction variant for cells whose shipped graph combines two or more
 * provider fields before any one can reach paint. Every provider endpoint
 * must exist uniquely, all values must be primitive, and the fully supplied
 * graph must reach an unambiguous terminal before the Screen is committed. */
BindReport bind_record_transaction(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record);
BindReport bind_record_transaction_shuffled_control(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record);

/* Bind and append one item at a caller-selected cell returned by
 * rime::uniform_grid_layout.  Ordering and virtualization remain with the
 * caller because they are view-model state, not properties of the DBD. */
bool append_bound_grid_item(
    const rime::Screen& item_template,
    const rime::UniformGridLayout& layout,
    const DbdContract& contract, const Record& record,
    const char* graph_partition, float cell_x, float cell_y,
    int runtime_state, std::vector<rime::Element>& out,
    BindReport* binding = nullptr);

} // namespace rime_list
