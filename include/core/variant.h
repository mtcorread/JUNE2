#pragma once

#include <string>
#include <variant>
#include <vector>

namespace june {

// Variant type for dynamic properties
using PropertyValue =
    std::variant<std::monostate,       // None/null
                 bool,                 // Boolean
                 int32_t,              // Integer
                 double,               // Double
                 std::string,          // String
                 std::vector<int32_t>,     // List of ints (e.g. social_contacts)
                 std::vector<std::string>  // List of strings (e.g. unit names)
                 >;

// Helper functions for PropertyValue
inline bool isNull(const PropertyValue& v) {
  return std::holds_alternative<std::monostate>(v);
}

template <typename T>
inline bool holds(const PropertyValue& v) {
  return std::holds_alternative<T>(v);
}

template <typename T>
inline const T& get(const PropertyValue& v) {
  return std::get<T>(v);
}

template <typename T>
inline T getOr(const PropertyValue& v, const T& defaultVal) {
  if (std::holds_alternative<T>(v)) {
    return std::get<T>(v);
  }
  return defaultVal;
}

}  // namespace june
