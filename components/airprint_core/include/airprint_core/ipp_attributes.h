// ============================================================================
// ipp_attributes.h - Small, explicit registry for the first IPP/AirPrint slice.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_ATTRIBUTES_H
#define IJPD_AIRPRINT_CORE_IPP_ATTRIBUTES_H

#include <cstddef>

#include "airprint_core/ipp_types.h"

namespace ijpd {
namespace airprint {

struct IppAttributeSpec {
    const char* name;
    IppValueTag value_tag;
};

const IppAttributeSpec* find_attribute_spec(const char* name,
                                            size_t name_length) noexcept;
bool attribute_name_equals(const IppAttributeView& attribute,
                           const char* name) noexcept;

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_ATTRIBUTES_H
