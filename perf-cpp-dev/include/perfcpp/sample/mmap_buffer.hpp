#pragma once

#include <cstddef>
#include <cstdint>
#include <linux/perf_event.h>
#include <mutex>
#include <perfcpp/util/unique_file_descriptor.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace perf {
class MmapBuffer;

/**
 * The MmapBufferOverflowWorker is responsible for handling an extra thread that waits for overflows to happen and then
 * triggers the MmapBuffer to handle the overflow.
 */
class MmapBufferOverflowWorker
{
public:
  MmapBufferOverflowWorker(MmapBuffer& mmap_buffer, const util::UniqueFileDescriptor& counter_file_descriptor);
  ~MmapBufferOverflowWorker() = default;

  MmapBufferOverflowWorker(MmapBufferOverflowWorker&&) = delete;
  MmapBufferOverflowWorker(const MmapBufferOverflowWorker&) = delete;

  MmapBufferOverflowWorker& operator=(const MmapBufferOverflowWorker&) = delete;
  MmapBufferOverflowWorker& operator=(MmapBufferOverflowWorker&&) = delete;

  /**
   * Cancels the worker thread and awaits its shutdown.
   */
  void cancel();

private:
  /// Thread to run for handling overflows.
  std::thread _overflow_handle_thread;

  /// File descriptor to communicate with the thread in case of canceling the worker.
  util::UniqueFileDescriptor _cancel_thread_file_descriptor;

  /**
   * Worker function that waits for an overflow within the mmap-ed buffer–calling the mmap buffer to handle the
   * overflow–and a cancel signal to shut down the worker thread.
   *
   * @param mmap_buffer Mmap buffer that will be triggered to handle the overflow.
   * @param counter_file_descriptor File descriptor of the counter that is used to mmap the buffer; used to await the
   * overflow.
   * @param cancel_file_descriptor File descriptor used to communicate canceling the worker thread.
   */
  static void run(MmapBuffer& mmap_buffer,
                  util::FileDescriptorView counter_file_descriptor,
                  util::FileDescriptorView cancel_file_descriptor) noexcept;
};

class MmapBuffer
{
public:
  explicit MmapBuffer(const util::UniqueFileDescriptor& file_descriptor, std::uint64_t count_pages = 1ULL);
  ~MmapBuffer();

  MmapBuffer(const MmapBuffer&) = delete;
  MmapBuffer(MmapBuffer&&) = delete;

  MmapBuffer& operator=(const MmapBuffer&) = delete;
  MmapBuffer& operator=(MmapBuffer&&) = delete;

  /**
   * Reads a performance monitoring counter value from the mmap-ed buffer via the `rdpmc` instruction.
   *
   * @return PMC value read via `rdpmc` from the buffer.
   */
  [[nodiscard]] std::optional<std::uint64_t> read_performance_monitoring_counter() const noexcept;

  /**
   * @return The entire data from the buffer, including all data copied from overflows. This will consume the data,
   * i.e., the caller owns the data.
   */
  [[nodiscard]] std::vector<std::vector<std::byte>> consume_data();

  /**
   * Copies the data from the mmap-ed buffer into a specific application-level buffer.
   * Overflow handling (i.e., this function) will be triggered by the overflow handler.
   */
  void handle_overflow();

  [[nodiscard]] explicit operator bool() const noexcept { return _ringbuffer_header != nullptr; }

private:
  /// First page of the mmap-ed buffer; pointing to the buffer's header.
  perf_event_mmap_page* _ringbuffer_header{ nullptr };

  /// Number of pages allocated via mmap.
  std::uint64_t _count_pages{ 0ULL };

  /// List of data copied from the mmap-ed buffer after an overflow happened.
  std::vector<std::vector<std::byte>> _overflow_data;

  /// Worker that waits for overflows and triggers the overflow handling.
  std::optional<MmapBufferOverflowWorker> _overflow_worker{ std::nullopt };

  /// Mutex for accessing the overflow data.
  alignas(64) std::mutex _overflow_data_mutex;

  /**
   * Copies the sample data from the mmap-ed buffer into a vector and sets the tail of the mmap-ed buffer accordingly.
   *
   * @return Data copied from the buffer.
   */
  [[nodiscard]] std::vector<std::byte> copy_data_from_ringbuffer();

  /**
   * Aligns the number of buffer pages to a number that is a power of two plus one for the header.
   *
   * @param number_of_buffer_pages Current number of buffer pages.
   * @return An aligned number that is a power of two plus one. Nothing changes if the number is already aligned.
   */
  [[nodiscard]] static std::uint64_t align_number_of_buffer_pages(std::uint64_t number_of_buffer_pages) noexcept;
};
}