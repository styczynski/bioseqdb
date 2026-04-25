#include <cstring>
#include <perfcpp/exception.hpp>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sample/mmap_buffer.hpp>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/select.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

perf::MmapBufferOverflowWorker::MmapBufferOverflowWorker(MmapBuffer& mmap_buffer,
                                                         const util::UniqueFileDescriptor& counter_file_descriptor)
{
  /// Create an event file descriptor to cancel the thread when closing the sample buffer.
  this->_cancel_thread_file_descriptor = util::UniqueFileDescriptor{ ::eventfd(0, 0) };
  if (!this->_cancel_thread_file_descriptor.has_value()) {
    throw CannotCreateEventFileDescriptor{};
  }

  /// Create the thread that handles overflows in the mmap buffer and copies samples into a application-level buffer by
  /// triggering the mmap buffers handle_overflow() function.
  this->_overflow_handle_thread = std::thread(&MmapBufferOverflowWorker::run,
                                              std::ref(mmap_buffer),
                                              util::FileDescriptorView{ counter_file_descriptor },
                                              util::FileDescriptorView{ this->_cancel_thread_file_descriptor });
}

void
perf::MmapBufferOverflowWorker::run(perf::MmapBuffer& mmap_buffer,
                                    const perf::util::FileDescriptorView counter_file_descriptor,
                                    const perf::util::FileDescriptorView cancel_file_descriptor) noexcept
{
  do {
    /// Initialize the file descriptor set.
    auto file_descriptor_set = ::fd_set{};
    FD_ZERO(&file_descriptor_set);
    FD_SET(counter_file_descriptor.value(), &file_descriptor_set);
    FD_SET(cancel_file_descriptor.value(), &file_descriptor_set);

    const auto max_file_descriptor = std::max(counter_file_descriptor.value(), cancel_file_descriptor.value()) + 1;

    /// Block and wait for the perf file descriptor or the event file descriptor to notify.
    const auto select = ::select(max_file_descriptor, &file_descriptor_set, nullptr, nullptr, nullptr);

    if (select > 0) {
      /// If the cancel file descriptor is set, exit the loop and consequently the thread.
      if (FD_ISSET(cancel_file_descriptor.value(), &file_descriptor_set)) {
        return;
      }

      /// If the "normal" perf file descriptor is set, trigger the MmapBuffer to handle the overflow by copying all the
      /// data into an application-level buffer.
      if (FD_ISSET(counter_file_descriptor.value(), &file_descriptor_set)) {
        mmap_buffer.handle_overflow();
      }
    } else if (select == -1) {
      return;
    }
  } while (true);
}

void
perf::MmapBufferOverflowWorker::cancel()
{
  /// Notify the thread that copies data from the mmap-ed buffer into application-level buffers to cancel.
  if (this->_cancel_thread_file_descriptor.has_value()) {
    ::eventfd_write(this->_cancel_thread_file_descriptor.value(), 1);
  }

  /// Wait for the worker thread to return.
  this->_overflow_handle_thread.join();
}

perf::MmapBuffer::MmapBuffer(const util::UniqueFileDescriptor& file_descriptor, const std::uint64_t count_pages)
  : _count_pages(MmapBuffer::align_number_of_buffer_pages(count_pages))
{
  const auto is_handle_overflow = this->_count_pages != 1ULL;

  /// Open the mapped buffer; use write mode if needed because a separate thread will copy data into application-level
  /// buffers.
  const auto prod_flags = PROT_READ | (static_cast<decltype(PROT_WRITE)>(is_handle_overflow) * PROT_WRITE);
  this->_ringbuffer_header =
    static_cast<perf_event_mmap_page*>(::mmap(nullptr,
                                              this->_count_pages * HardwareInfo::memory_page_size(),
                                              prod_flags,
                                              MAP_SHARED,
                                              file_descriptor.value(),
                                              0));

  /// Notify the caller if buffer-allocation via ::mmap() failed.
  if (this->_ringbuffer_header == MAP_FAILED) {
    throw MmapError{ errno };
  }

  if (this->_ringbuffer_header == nullptr) {
    throw MmapNullError{};
  }

  /// When overflows need to be handled, create a worker.
  if (is_handle_overflow) {
    /// Reserve some space for overflow data.
    this->_overflow_data.reserve(32U);

    /// Start the overflow worker.
    this->_overflow_worker.emplace(*this, file_descriptor);
  }
}

perf::MmapBuffer::~MmapBuffer()
{
  /// Cancel the overflow worker and await its shutdown.
  if (this->_overflow_worker.has_value()) {
    this->_overflow_worker->cancel();
  }

  /// Close/un-map the mmap-ed buffer and set number of pages to zero, if any.
  if (auto* const user_level_buffer = std::exchange(this->_ringbuffer_header, nullptr); user_level_buffer != nullptr) {
    if (const auto user_level_buffer_pages = std::exchange(this->_count_pages, 0ULL); user_level_buffer_pages > 0ULL) {
      ::munmap(user_level_buffer, user_level_buffer_pages * HardwareInfo::memory_page_size());
    }
  }
}

#include <iostream>

std::optional<std::uint64_t>
perf::MmapBuffer::read_performance_monitoring_counter() const noexcept
{
  /// Read the counter without stopping/disabling it via the "rdpmc" instruction.
  /// This is only possible on x86 architectures.
  /// For more details see https://man7.org/linux/man-pages/man2/perf_event_open.2.html (section MMAP layout).

#if defined(__x86_64__) || defined(__i386__)
  /// Lock for sequentializing the read.
  auto lock = decltype(perf_event_mmap_page::lock){};

  /// Index of the physical counter.
  auto index = std::uint32_t{};

  /// Timing.
  auto enabled = std::uint64_t{};
  auto running = std::uint64_t{};

  /// Counter value.
  auto count = std::int64_t{};

  do {
    lock = this->_ringbuffer_header->lock;

    /// Memory fence.
    asm volatile("" ::: "memory");

    /// Hardware counter identifier.
    index = this->_ringbuffer_header->index;

    /// Offset that must be added to the value.
    count = this->_ringbuffer_header->offset;

    /// Read timing to scale the value in case the event was not counted the entire time.
    enabled = this->_ringbuffer_header->time_enabled;
    running = this->_ringbuffer_header->time_running;

    if (this->_ringbuffer_header->cap_user_rdpmc && index > 0U) {
      /// Read the hardware counter value.
      auto value = _rdpmc(index - 1U);

      /// Read the width of the value.
      const auto width = 64 - this->_ringbuffer_header->pmc_width;

      /// Adjust the value for the given width.
      value = (value << width) >> width;

      count += static_cast<std::int64_t>(value);
    } else {
      return std::nullopt;
    }

    asm volatile("" ::: "memory");
  } while (this->_ringbuffer_header->lock != lock);

  /// Scale the value if it was not counted the entire time.
  if (running > 0ULL && enabled > running) {
    count = static_cast<std::int64_t>(static_cast<double>(count) *
                                      (static_cast<double>(enabled) / static_cast<double>(running)));
  }

  return static_cast<std::uint64_t>(count);
#else
  return std::nullopt;
#endif
}

std::vector<std::vector<std::byte>>
perf::MmapBuffer::consume_data()
{
  /// Lock the sample buffers vector. The thread polling for buffer overflows might copy data at the moment (or wants to
  /// do so while we are reading the data),
  auto _ = std::scoped_lock{ this->_overflow_data_mutex };

  /// Move all the data out of the sample buffer into a buffer that will be passed to the caller of this function and
  /// create a new one that will be used for further sampling.
  auto data = std::exchange(this->_overflow_data, std::vector<std::vector<std::byte>>{});
  this->_overflow_data.reserve(data.capacity());

  /// Copy the current mmap buffer into the buffer that will be consumed.
  if (auto buffer = this->copy_data_from_ringbuffer(); !buffer.empty()) {
    data.push_back(std::move(buffer));
  }

  return data;
}

void
perf::MmapBuffer::handle_overflow()
{
  /// Since this function is called from another thread (the overflow worker thread), we need to protect the overflow
  /// data.
  auto _ = std::scoped_lock{ this->_overflow_data_mutex };

  /// Copy the ringbuffer and append to sample buffers if any data available.
  if (auto buffer = this->copy_data_from_ringbuffer(); !buffer.empty()) {
    this->_overflow_data.push_back(std::move(buffer));
  }
}

std::vector<std::byte>
perf::MmapBuffer::copy_data_from_ringbuffer()
{
  /// Fore more information about the perf ring buffer see:
  /// https://docs.kernel.org/userspace-api/perf_ring_buffer.html

  /// Read positions and size of the ringbuffer. Head and tail are offsets of data_start (allocated buffer + offset for
  /// metadata).
  const auto tail = this->_ringbuffer_header->data_tail;
  const auto head = __atomic_load_n(&this->_ringbuffer_header->data_head, __ATOMIC_ACQUIRE);
  asm volatile("" ::: "memory");

  /// Check if there is anything to read and cancel if not.
  if (tail == head) {
    return {};
  }

  const auto data_start = std::uintptr_t(this->_ringbuffer_header) + HardwareInfo::memory_page_size();
#ifndef PERFCPP_NO_MMAP_DATA_SIZE /// The "data_size" attribute was added in Linux 4.1.
  const auto data_size = this->_ringbuffer_header->data_size;
#else
  const auto data_size = (this->_count_pages - 1U) * HardwareInfo::memory_page_size();
#endif

  /// Align head and tail to the data size in case one or both are wrapped. Note: Both aligned values are offsets of
  /// data_start.
  const auto begin = head % data_size;
  const auto end = tail % data_size;

  /// When the tail is behind the head, we can read straightforward.
  if (end < begin) {
    /// Allocate space for the data in the buffer.
    const auto size = begin - end;
    auto buffer = std::vector<std::byte>(size);

    /// Copy the data from the tail into the buffer.
    const auto start = data_start + end;
    std::memcpy(buffer.data(), reinterpret_cast<std::byte*>(start), size);

    // Update the data_tail to the current head, marking the data as consumed.
    __sync_synchronize();
    __atomic_store_n(&this->_ringbuffer_header->data_tail, head, __ATOMIC_RELEASE);

    return buffer;
  }

  /// When the ringbuffer wrapped inbetween, we need to copy the first part from tail to end and the second part from
  /// start to head.
  /// Allocate space for the data in the buffer.
  const auto tail_rest_size = data_size - end;
  auto buffer = std::vector<std::byte>(tail_rest_size + begin);

  /// Copy the first part: from tail to end.
  const auto start_tail = data_start + end;
  std::memcpy(buffer.data(), reinterpret_cast<std::byte*>(start_tail), tail_rest_size);

  /// Copy the second part: from start to head.
  std::memcpy(buffer.data() + tail_rest_size, reinterpret_cast<std::byte*>(data_start), begin);

  // Update the data_tail to the current head, marking the data as consumed.
  __sync_synchronize();
  __atomic_store_n(&this->_ringbuffer_header->data_tail, head, __ATOMIC_RELEASE);

  return buffer;
}

std::uint64_t
perf::MmapBuffer::align_number_of_buffer_pages(std::uint64_t number_of_buffer_pages) noexcept
{
  if (number_of_buffer_pages == 1UL) {
    return 1UL;
  }

  /// Check if buffer pages is a power of two; if so, add one for the header as specified by the perf_event_open
  /// documentation (https://man7.org/linux/man-pages/man2/perf_event_open.2.html).
  if ((number_of_buffer_pages & (number_of_buffer_pages - 1ULL)) == 0ULL) {
    /// Add one page for the header.
    return number_of_buffer_pages + 1ULL;
  }

  /// Check if buffer pages minus one is a power if to (i.e., the number includes already the additional page for the
  /// header). If not, we align the number to the next power of two and add one page for the header.
  if (((number_of_buffer_pages - 1ULL) & (number_of_buffer_pages - 2ULL)) != 0ULL) {
    --number_of_buffer_pages;
    number_of_buffer_pages |= number_of_buffer_pages >> 1;
    number_of_buffer_pages |= number_of_buffer_pages >> 2;
    number_of_buffer_pages |= number_of_buffer_pages >> 4;
    number_of_buffer_pages |= number_of_buffer_pages >> 8;
    number_of_buffer_pages |= number_of_buffer_pages >> 16;
    number_of_buffer_pages |= number_of_buffer_pages >> 32;

    /// Add one as we decremented, plus one for the page.
    return number_of_buffer_pages + 2ULL;
  }

  /// The number is already a power of two plus one; everything is correct configured.
  return number_of_buffer_pages;
}