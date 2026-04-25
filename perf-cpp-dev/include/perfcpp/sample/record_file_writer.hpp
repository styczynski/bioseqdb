#pragma once

#include <array>
#include <cstdint>
#include <linux/perf_event.h>
#include <optional>
#include <perfcpp/sample/recording_values.hpp>
#include <perfcpp/sampler.hpp>
#include <perfcpp/util/symbol_resolver.hpp>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace perf {
/**
 * Writes recorded samples by the Sampler instances to a perf.data file format, specified here:
 * https://lwn.net/Articles/644919/
 */
class RecordFileWriter
{
private:
  static constexpr auto PERF_RECORD_HEADER_BUILD_ID = 67UL;

  /// Magic number for perf.data files ("PERFILE2")
  static constexpr auto MAGIC = 0x32454c4946524550LL;

  /// Size of feature bitmap (256 bits = 4 unsigned longs on 64-bit systems)
  static constexpr std::size_t FEATURE_BITS = 256;

  /// Size of the bitmap using 8-byte fields.
  static constexpr auto FEATURE_BITMAP_SIZE = (FEATURE_BITS + 63U) / 64U;

  /// Header feature constant (matches Linux kernel's tools/perf/util/header.h enum)
  static constexpr std::uint8_t HEADER_BUILD_ID = 2U;

  /// Standard 8-byte alignment for perf.data records
  static constexpr auto PERF_FILE_ALIGNMENT = 8U;

  /// 4-byte alignment for build ID entries
  static constexpr auto BUILD_ID_ALIGNMENT = 4U;

  /// Build ID padded to 4-byte boundary
  static constexpr auto BUILD_ID_PADDED_SIZE = 24U;

  /// perf_event_header size
  static constexpr auto BUILD_ID_HEADER_SIZE = 8U;

  /// PID field size
  static constexpr auto BUILD_ID_PID_SIZE = 4U;

  /**
   * File section descriptor
   */
  struct FileSection
  {
    std::uint64_t offset{ 0UL };
    std::uint64_t size{ 0UL };
  };

  /**
   * Main perf.data file header
   */
  struct FileHeader
  {
    std::uint64_t magic{ MAGIC };
    std::uint64_t size{ 0U };
    std::uint64_t attribute_size{ sizeof(perf_event_attr) + sizeof(FileSection) };
    FileSection attributes;
    FileSection data;
    FileSection event_types;
    std::array<std::uint64_t, FEATURE_BITMAP_SIZE> features{};
  };

  /**
   * Attribute file section (perf_event_attr + ids)
   */
  struct AttributeFileSection
  {
    perf_event_attr attr{};
    FileSection ids;
  };

  /**
   * Stream that allows to shift specific values in using binary format. The data will be written binary, not
   * human-readable (os usually done using the << operator on streams).
   *
   * @tparam S Type of the ostream.
   */
  template<class S>
  class BinaryStream
  {
  public:
    BinaryStream()
      : BinaryStream(S{ std::ios::binary })
    {
    }

    explicit BinaryStream(S&& stream) noexcept
      : _stream(std::move(stream))
    {
    }
    BinaryStream(const BinaryStream&) = delete;
    BinaryStream(BinaryStream&&) noexcept = default;
    ~BinaryStream() = default;
    BinaryStream& operator=(const BinaryStream&) = delete;
    BinaryStream& operator=(BinaryStream&&) noexcept = default;

    BinaryStream& operator<<(const FileHeader& header)
    {
      write(header);
      return *this;
    }
    BinaryStream& operator<<(const AttributeFileSection& attribute_file_section)
    {
      write(attribute_file_section);
      return *this;
    }
    BinaryStream& operator<<(const FileSection& file_section)
    {
      write(file_section);
      return *this;
    }
    BinaryStream& operator<<(const perf_event_header& perf_event_header)
    {
      write(perf_event_header);
      return *this;
    }

    BinaryStream& operator<<(const char value)
    {
      write(value);
      return *this;
    }
    BinaryStream& operator<<(const std::uint16_t value)
    {
      write(value);
      return *this;
    }
    BinaryStream& operator<<(const std::uint32_t value)
    {
      write(value);
      return *this;
    }
    BinaryStream& operator<<(const std::uint64_t value)
    {
      write(value);
      return *this;
    }

    BinaryStream& operator<<(const std::string& data)
    {
      write(data.data(), data.size());
      return *this;
    }

    BinaryStream& operator<<(std::string&& data)
    {
      write(data.data(), data.size());
      return *this;
    }

    BinaryStream& operator<<(const std::vector<std::byte>& data)
    {
      write(data.data(), data.size());
      return *this;
    }

    BinaryStream& operator<<(std::vector<std::uint8_t>&& data)
    {
      write(data.data(), data.size());
      return *this;
    }

    /**
     * @return The current write position in the buffer.
     */
    [[nodiscard]] std::ostream::pos_type position() noexcept { return _stream.tellp(); }

    /**
     * @return The buffer as a string that can be moved to further buffers.
     */
    [[nodiscard]] std::string to_string() { return _stream.str(); }

  private:
    /// The output stream to write into.
    S _stream;

    template<typename T>
    void write(const T& data)
    {
      _stream.write(reinterpret_cast<const char*>(&data), sizeof(data));
    }

    template<typename T>
    void write(const T* data, const std::size_t size)
    {
      _stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
  };

public:
  /**
   * Writes the given samples (including the given sample values) for the given counter into the provided file. The
   * format follows the perf-data format; the resulting file can be read by `perf report`.
   *
   * @param sampler_values Values that are included into the samples.
   * @param sample_counters List of counters that where sampled.
   * @param sample_data List of data that was sampled on different counters.
   * @param file_name Name of the file to write the perf data to.
   */
  static void write(const SampleRecordingValues& sampler_values,
                    const std::vector<Sampler::SampleCounter>& sample_counters,
                    const std::vector<std::vector<std::vector<std::byte>>>& sample_data,
                    std::string_view file_name);

private:
  /**
   * Writes build ID headers.
   *
   * @param modules List of modules with build IDs.
   * @return Written build ids.
   */
  static std::optional<std::string> generate_build_ids_records(
    const std::vector<util::SymbolResolver::Module>& modules);

  /**
   * Writes MMAP records for process memory mappings.
   *
   * @param modules List of memory mapped modules.
   * @param process_id Id of the process, if included into samples.
   * @param thread_id Id of the thread, if included into samples.
   * @param timestamp Timestamp.
   * @param sample_id Id of a sample, if included into samples.
   * @param stream_id Id of a stream, if included into samples.
   * @param cpu_id Id of a CPU, if included into samples.
   * @return Modules written as MMAP2 records.
   */
  [[nodiscard]] static std::string generate_module_records(std::vector<util::SymbolResolver::Module>&& modules,
                                                           std::optional<std::uint32_t> process_id,
                                                           std::optional<std::uint32_t> thread_id,
                                                           std::optional<std::uint64_t> timestamp,
                                                           std::optional<std::uint64_t> sample_id,
                                                           std::optional<std::uint64_t> stream_id,
                                                           std::optional<std::uint32_t> cpu_id);

  /**
   * Writes COMM records for process name identification.
   *
   * @param process_id Id of the process, if included into samples.
   * @param thread_id Id of the thread, if included into samples.
   * @param timestamp Timestamp.
   * @param sample_id Id of a sample, if included into samples.
   * @param stream_id Id of a stream, if included into samples.
   * @param cpu_id Id of a CPU, if included into samples.
   * @return COMM data written as record.
   */
  [[nodiscard]] static std::string generate_comm_records(std::optional<std::uint32_t> process_id,
                                                         std::optional<std::uint32_t> thread_id,
                                                         std::optional<std::uint64_t> timestamp,
                                                         std::optional<std::uint64_t> sample_id,
                                                         std::optional<std::uint64_t> stream_id,
                                                         std::optional<std::uint32_t> cpu_id);

  /**
   * Sets a feature bit in the feature bitmap.
   *
   * @param bitmap Feature bitmap array.
   * @param bit_index Index of the bit to set.
   */
  static void set_feature_bit(std::array<std::uint64_t, FEATURE_BITMAP_SIZE>& bitmap, std::uint8_t bit_index);

  /**
   * Calculates the size of all samples.
   *
   * @param sample_data Raw samples.
   * @return Size needed for all samples (flattened).
   */
  [[nodiscard]] static std::uint64_t calculate_size(
    const std::vector<std::vector<std::vector<std::byte>>>& sample_data) noexcept;

  /**
   * Reads the first sample_id from the sampler values. This will be used to generate sample_id entries for MMAP2 and
   * COMM entries that are necessary since we start sampling only after execution.
   * @param sampler_values Values recorded by the sampler; the actual fields present in the sample_id depend on the
   * recorded values.
   * @param sample_data The sample data to find the first sample_id from.
   * @return A tuple (process_id, thread_id, sample_id, stream_id, cpu_id).
   */
  [[nodiscard]] static std::tuple<std::optional<std::uint32_t>,
                                  std::optional<std::uint32_t>,
                                  std::optional<std::uint64_t>,
                                  std::optional<std::uint64_t>,
                                  std::optional<std::uint64_t>,
                                  std::optional<std::uint32_t>>
  read_first_sample_id(const SampleRecordingValues& sampler_values,
                       const std::vector<std::vector<std::vector<std::byte>>>& sample_data);

  /**
   * Calculates the size of the sample_id field that is appended to MMAP2 and COMM samples.
   *
   * @param process_id Id of the process.
   * @param thread_id Id of the thread.
   * @param timestamp Timestamp.
   * @param sample_id Id of the sample.
   * @param stream_id Id of the stream.
   * @param cpu_id Id of the CPU.
   * @return Size of the sample_id field.
   */
  [[nodiscard]] static std::uint64_t calculate_sample_id_all_size(std::optional<std::uint32_t> process_id,
                                                                  std::optional<std::uint32_t> thread_id,
                                                                  std::optional<std::uint64_t> timestamp,
                                                                  std::optional<std::uint64_t> sample_id,
                                                                  std::optional<std::uint64_t> stream_id,
                                                                  std::optional<std::uint32_t> cpu_id);

  /**
   * Writes sample_id values to the output stream.
   *
   * @param output_stream Output file stream.
   * @param process_id Id of the process, if included into samples.
   * @param thread_id Id of the thread, if included into samples.
   * @param timestamp Timestamp.
   * @param sample_id Id of a sample, if included into samples.
   * @param stream_id Id of a stream, if included into samples.
   * @param cpu_id Id of a CPU, if included into samples.
   */
  static void write_sample_id(BinaryStream<std::ostringstream>& output_stream,
                              std::optional<std::uint32_t> process_id,
                              std::optional<std::uint32_t> thread_id,
                              std::optional<std::uint64_t> timestamp,
                              std::optional<std::uint64_t> sample_id,
                              std::optional<std::uint64_t> stream_id,
                              std::optional<std::uint32_t> cpu_id);
};
}