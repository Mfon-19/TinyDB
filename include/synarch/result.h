#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace synarch {

using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;

struct ResultSet {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

}  // namespace synarch
