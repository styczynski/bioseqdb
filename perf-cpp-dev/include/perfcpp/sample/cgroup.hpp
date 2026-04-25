#pragma once

#include <cstdint>
#include <string>

namespace perf {
class CGroup
{
public:
  CGroup(const std::uint64_t id, std::string&& path) noexcept
    : _id(id)
    , _path(std::move(path))
  {
  }
  ~CGroup() = default;

  CGroup(const CGroup&) noexcept = default;
  CGroup(CGroup&&) noexcept = default;

  CGroup& operator=(const CGroup&) noexcept = default;
  CGroup& operator=(CGroup&&) noexcept = default;

  /**
   * @return Id of the CGgroup (as found in samples).
   */
  [[nodiscard]] std::uint64_t id() const noexcept { return _id; }

  /**
   * @return Path of the CGroup.
   */
  [[nodiscard]] const std::string& path() const noexcept { return _path; }

private:
  std::uint64_t _id;
  std::string _path;
};
}