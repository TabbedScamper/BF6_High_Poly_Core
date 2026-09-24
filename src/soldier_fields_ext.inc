/* soldier_fields_ext.inc - the C entry point to the soldier's named state fields
 * (soldier_fields.h). Included into bf6_core.cpp beside the other *_ext.inc files. */
#include "soldier_fields.h"

int bf6_soldier_fields_load(bf6_ctx* c)
{
    if (!c) return 0;
    std::string err;
    /* The type schema opens lazily; without it no field layout resolves. */
    if (!ensure_types(c, err)) {
        std::fprintf(stderr, "soldier fields: no type schema: %s\n", err.c_str());
        return 0;
    }
    if (!bf6::SoldierFields::get().load(c->src, *c->types, err)) {
        if (!err.empty()) std::fprintf(stderr, "soldier fields: %s\n", err.c_str());
        return 0;
    }
    return (int)bf6::SoldierFields::get().size();
}

void bf6_soldier_field_set(const char* name, const float* v, int32_t n)
{
    if (name && v && n > 0) bf6::SoldierFields::get().set_live(name, v, (int)n);
}
