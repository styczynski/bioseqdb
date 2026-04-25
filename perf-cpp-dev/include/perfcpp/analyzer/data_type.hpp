#pragma once

#include <cstdint>
#include <perfcpp/sample/sample.hpp>
#include <string>
#include <vector>

namespace perf::analyzer {
/**
 * The DataType projects a data object with members (attributes).
 */
class DataType
{
public:
  class Member
  {
  public:
    Member(std::string&& name, const std::size_t offset, const std::size_t size) noexcept
      : _name(std::move(name))
      , _offset(offset)
      , _size(size)
    {
    }
    Member(const Member&) = default;
    Member(Member&&) noexcept = default;
    ~Member() = default;
    Member& operator=(const Member&) = default;
    Member& operator=(Member&&) noexcept = default;

    [[nodiscard]] const std::string& name() const noexcept { return _name; }
    [[nodiscard]] std::size_t offset() const noexcept { return _offset; }
    [[nodiscard]] std::size_t size() const noexcept { return _size; }
    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return _samples; }
    [[nodiscard]] std::vector<Sample>& samples() noexcept { return _samples; }

  private:
    std::string _name;
    std::size_t _offset;
    std::size_t _size;

    std::vector<Sample> _samples;
  };

  DataType(std::string&& name, const std::size_t size)
    : _name(std::move(name))
    , _size(size)
  {
  }
  DataType(const DataType&) = default;
  DataType(DataType&&) noexcept = default;
  DataType(std::string&& new_name, const DataType& other)
    : _name(std::move(new_name))
    , _size(other._size)
    , _members(other._members)
  {
  }
  ~DataType() = default;

  DataType& operator=(const DataType&) = default;
  DataType& operator=(DataType&&) noexcept = default;

  /**
   * @return Name of the data type.
   */
  [[nodiscard]] const std::string& name() const noexcept { return _name; }

  /**
   * @return Size of the data type.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _size; }

  /**
   * @return List of all members.
   */
  [[nodiscard]] const std::vector<Member>& members() const noexcept { return _members; }

  /**
   * @return List of all members.
   */
  [[nodiscard]] std::vector<Member>& members() noexcept { return _members; }

  /**
   * Adds a member with the given name and size to the data type. A member can be, for example, an attribute of the data
   * type.
   *
   * @param member_name Name of the member.
   * @param size Size of the member.
   */
  void add(std::string&& member_name, const std::size_t size)
  {
    if (_members.empty()) {
      add(std::move(member_name), 0U, size);
    } else {
      add(std::move(member_name), _members.back().offset() + _members.back().size(), size);
    }
  }

  /**
   * Adds a member with the given name and size with a specified offset (relative to the beginning of the data object)
   * to the data type wit. A member can be, for example, an attribute of the data type.
   *
   * @param member_name Name of the member.
   * @param offset Offset of the member, relative to the data type.
   * @param size Size of the member.
   */
  void add(std::string&& member_name, const std::size_t offset, const std::size_t size)
  {
    _members.emplace_back(std::move(member_name), offset, size);
  }

  /**
   * Adds a member to the data type. Name and size will be derived from the type in the template.
   */
  template<typename T>
  void add()
  {
    add(std::string{ typeid(T).name() }, sizeof(T));
  }

  /**
   * Adds a member with a given name to the data type. The Size will be derived from the type in the template.
   *
   * @param name Name of the member.
   */
  template<typename T>
  void add(std::string&& name)
  {
    add(std::move(name), sizeof(T));
  }

  /**
   * Adds a member with a given name to the data type. The Size will be derived from the type in the template.
   *
   * @param name Name of the member.
   */
  template<typename T>
  void add(const std::string& name)
  {
    add(std::string{ name }, sizeof(T));
  }

  /**
   * Adds a member at a specific offset to the data type. Name and size will be derived from the type in the template.
   *
   * @param offset Offset relative to the data type.
   */
  template<typename T>
  void add(const std::size_t offset)
  {
    add(typeid(T).name(), offset, sizeof(T));
  }

  /**
   * Adds a member with a given name at a specific offset to the data type. The ize will be derived from the type in the
   * template.
   *
   * @param name
   * @param offset
   */
  template<typename T>
  void add(std::string&& name, const std::size_t offset)
  {
    add(std::move(name), offset, sizeof(T));
  }

private:
  std::string _name;
  std::size_t _size;
  std::vector<Member> _members;
};
}