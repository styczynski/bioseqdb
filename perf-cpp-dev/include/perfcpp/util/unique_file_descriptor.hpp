#pragma once
#include <cstdint>
#include <unistd.h>
#include <utility>

namespace perf::util {
/**
 * The unique file descriptor is backed by a common file descriptor; however, it does not allow copying.
 * Destroying the unique file descriptor leads to closing the underyling file descriptor – comparable to a unique
 * pointer.
 */
class UniqueFileDescriptor
{
public:
  UniqueFileDescriptor() noexcept = default;

  explicit UniqueFileDescriptor(const std::int64_t file_descriptor) noexcept
    : _file_descriptor(static_cast<std::int32_t>(file_descriptor))
  {
  }

  UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept
    : _file_descriptor(std::exchange(other._file_descriptor, -1L))
  {
  }

  UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;

  /**
   * Closes the file descriptor underneath.
   */
  ~UniqueFileDescriptor()
  {
    if (has_value()) {
      ::close(value());
    }
  }

  UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept
  {
    _file_descriptor = std::exchange(other._file_descriptor, -1L);
    return *this;
  }

  UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;

  UniqueFileDescriptor& operator=(const std::int64_t file_descriptor) noexcept
  {
    _file_descriptor = static_cast<std::int32_t>(file_descriptor);
    return *this;
  }

  UniqueFileDescriptor& operator=(const std::int32_t file_descriptor) noexcept
  {
    _file_descriptor = file_descriptor;
    return *this;
  }

  /**
   * @return True, if the filter descriptor underneath is opened.
   */
  [[nodiscard]] bool has_value() const noexcept { return _file_descriptor > -1LL; }

  /**
   * @return The "real" filter descriptor.
   */
  [[nodiscard]] std::int32_t value() const noexcept { return _file_descriptor; }

  /**
   * Resets the file descriptor.
   */
  void reset() noexcept { _file_descriptor = -1LL; }

private:
  std::int32_t _file_descriptor{ -1LL };
};

/**
 * The file descriptor view grants access to a file descriptor without closing it when destroyed.
 * Logically, the view does not own the file descriptor; it can be outdated.
 */
class FileDescriptorView
{
public:
  FileDescriptorView() noexcept = default;

  explicit FileDescriptorView(const UniqueFileDescriptor& file_descriptor) noexcept
    : _file_descriptor(file_descriptor.value())
  {
  }
  FileDescriptorView(const FileDescriptorView&) noexcept = default;
  FileDescriptorView(FileDescriptorView&&) noexcept = default;

  ~FileDescriptorView() noexcept = default;

  FileDescriptorView& operator=(const FileDescriptorView&) noexcept = default;
  FileDescriptorView& operator=(FileDescriptorView&&) noexcept = default;

  /**
   * @return True, if the filter descriptor underneath is opened.
   */
  [[nodiscard]] bool has_value() const noexcept { return _file_descriptor > -1LL; }

  /**
   * @return The "real" filter descriptor.
   */
  [[nodiscard]] std::int32_t value() const noexcept { return _file_descriptor; }

private:
  std::int32_t _file_descriptor{ -1LL };
};
}