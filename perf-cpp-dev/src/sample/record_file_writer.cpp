#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <perfcpp/sample/decoder.hpp>
#include <perfcpp/sample/record_file_writer.hpp>
#include <perfcpp/sampler.hpp>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

void
perf::RecordFileWriter::write(const SampleRecordingValues& sampler_values,
                              const std::vector<Sampler::SampleCounter>& sample_counters,
                              const std::vector<std::vector<std::vector<std::byte>>>& sample_data,
                              const std::string_view file_name)
{
  auto output_file_stream = std::ofstream{ std::string{ file_name }, std::ios::binary | std::ios::trunc };
  if (!output_file_stream.is_open()) {
    return;
  }

  auto output_stream = BinaryStream<std::ofstream>{ std::move(output_file_stream) };

  /// Calculate total sample data size.
  const auto sample_size = RecordFileWriter::calculate_size(sample_data);

  /// Read any sample_id field that will be used for generating MMAP2 and COMM records since these records are not
  /// written by the kernel when we start the sampling AFTER starting the executable (where the sampling is embedded).
  const auto [process_id, thread_id, timestamp, sample_id, stream_id, cpu_id] =
    RecordFileWriter::read_first_sample_id(sampler_values, sample_data);

  /// Read all modules from /proc/-system
  auto modules = util::SymbolResolver::read_modules();

  /// Test if any module has a build id. If so, we will enable the appropriate feature in the perf data.
  auto build_ids = std::optional<std::string>{};
  if (std::any_of(modules.begin(), modules.end(), [](const auto& module) { return !module.build_id().empty(); })) {
    /// Generate the modules build ids.
    build_ids = RecordFileWriter::generate_build_ids_records(modules);
  }

  /// Write MMAP2 and COMM records to dedicated buffers.
  auto mmap2_samples = RecordFileWriter::generate_module_records(
    std::move(modules), process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);
  auto comm_sample =
    RecordFileWriter::generate_comm_records(process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);

  /// Get modules with build IDs and memory mappings.
  auto header = FileHeader{};
  header.size = sizeof(FileHeader);

  /// Attributes section comes first after header and includes all attributes of all counters.
  header.attributes.offset = sizeof(FileHeader);
  header.attributes.size = sizeof(AttributeFileSection) * sample_counters.size();

  /// Data section comes after attributes.
  header.data.offset = header.attributes.offset + header.attributes.size;
  header.data.size = sample_size + mmap2_samples.size() + comm_sample.size();

  /// Event types section (size is empty by default).
  header.event_types.offset = header.data.offset + header.data.size;

  /// Set feature bit for build ID.
  if (build_ids.has_value()) {
    RecordFileWriter::set_feature_bit(header.features, HEADER_BUILD_ID);
  }

  /// Write the header.
  output_stream << header;

  /// Write attributes with proper file structure.
  for (const auto& counter : sample_counters) {
    const auto event_index = 0U + static_cast<std::uint8_t>(counter.has_intel_auxiliary_event());
    const auto& perf_event_attribute = counter.group().member(event_index).perf_event_attribute();

    /// Create attribute file section (offset and size are empty by default).
    auto attribute_section = AttributeFileSection{};
    attribute_section.attr = perf_event_attribute;

    output_stream << attribute_section;
  }

  /// Write the pre-computed MMAP2 and COMM records from buffer.
  output_stream << std::move(mmap2_samples) << std::move(comm_sample);

  /// Write the sample data.
  for (const auto& counter_data : sample_data) {
    for (const auto& buffer : counter_data) {
      output_stream << buffer;
    }
  }

  /// Write feature sections. The perf format requires all feature section headers (FileSection structs)
  /// to be written contiguously first, followed by all feature data. perf reads all headers in a single
  /// contiguous read (perf_header__process_sections in tools/perf/util/header.c).
  if (build_ids.has_value()) {
    auto build_id_section = FileSection{};
    build_id_section.offset = static_cast<std::uint64_t>(output_stream.position()) + sizeof(FileSection);
    build_id_section.size = build_ids->size();

    /// Write section header, then data.
    output_stream << build_id_section << std::move(build_ids.value());
  }
}

std::optional<std::string>
perf::RecordFileWriter::generate_build_ids_records(const std::vector<util::SymbolResolver::Module>& modules)
{
  if (modules.empty()) {
    return std::nullopt;
  }

  auto output_stream = BinaryStream{ std::ostringstream{ std::ios::binary } };

  for (const auto& module : modules) {
    if (!module.build_id().empty()) {
      /// Build ID feature format based on perf_record_header_build_id
      const auto filename_length = module.path().length() + 1U; /// +1 for null terminator
      const auto aligned_filename_length = (filename_length + PERF_FILE_ALIGNMENT - 1U) & ~(PERF_FILE_ALIGNMENT - 1U);

      /// perf_event_header for build ID record
      output_stream << static_cast<std::uint32_t>(PERF_RECORD_HEADER_BUILD_ID)
                    << static_cast<std::uint16_t>(PERF_RECORD_MISC_USER)
                    << static_cast<std::uint16_t>(BUILD_ID_HEADER_SIZE + BUILD_ID_PID_SIZE + BUILD_ID_PADDED_SIZE +
                                                  aligned_filename_length);

      /// Write the PID (4 bytes).
      output_stream << static_cast<std::uint32_t>(::getpid());

      /// Write the build ID (padded to 24 bytes as per perf format).
      const auto copy_size = std::min<std::size_t>(module.build_id().size(), BUILD_ID_PADDED_SIZE);
      auto build_id_padded = std::vector<std::uint8_t>(
        module.build_id().begin(), module.build_id().begin() + static_cast<std::int32_t>(copy_size));
      build_id_padded.resize(BUILD_ID_PADDED_SIZE);
      output_stream << std::move(build_id_padded);

      /// Write the filename with padding.
      output_stream << module.path() << '\0';

      /// Add padding to align to PERF_FILE_ALIGNMENT bytes.
      if (const auto padding = aligned_filename_length - filename_length; padding > 0U) {
        output_stream << std::string(padding, '\0');
      }
    }
  }

  return output_stream.to_string();
}

void
perf::RecordFileWriter::set_feature_bit(std::array<std::uint64_t, FEATURE_BITMAP_SIZE>& bitmap,
                                        const std::uint8_t bit_index)
{
  if (const auto long_index = static_cast<std::size_t>(bit_index / 64U); long_index < bitmap.size()) {
    const auto bit_offset = bit_index % 64;
    bitmap.at(long_index) |= (1ULL << bit_offset);
  }
}

std::string
perf::RecordFileWriter::generate_module_records(std::vector<util::SymbolResolver::Module>&& modules,
                                                const std::optional<std::uint32_t> process_id,
                                                const std::optional<std::uint32_t> thread_id,
                                                const std::optional<std::uint64_t> timestamp,
                                                const std::optional<std::uint64_t> sample_id,
                                                const std::optional<std::uint64_t> stream_id,
                                                const std::optional<std::uint32_t> cpu_id)
{
  auto output_stream = BinaryStream<std::ostringstream>{};

  /// Calculate the size of the sample id field which is shared among all generated MMAP2 entries.
  const auto sample_id_size =
    RecordFileWriter::calculate_sample_id_all_size(process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);

  for (const auto& module : modules) {
    /// Calculate the size needed for this MMAP record.
    const auto filename_length = module.path().length() + /* null terminator */ 1U;
    const auto aligned_filename_length = (filename_length + 7U) & /* 8-byte align */ ~7UL;
    /// MMAP2 record size: header + pid + tid + addr + len + pgoff + maj + min + ino + ino_generation + prot + flags +
    /// filename
    const auto record_size = sizeof(perf_event_header) + sizeof(std::uint32_t) + sizeof(std::uint32_t) +
                             sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint64_t) +
                             sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
                             sizeof(std::uint64_t) + sizeof(std::uint32_t) + sizeof(std::uint32_t) +
                             aligned_filename_length + sample_id_size;

    /// Create and write the record header.
    auto header = perf_event_header{};
    header.type = PERF_RECORD_MMAP2;
    header.misc = PERF_RECORD_MISC_USER;
    header.size = static_cast<std::uint16_t>(record_size);
    output_stream << header;

    /// Write PID and TID.
    output_stream << process_id.value_or(static_cast<std::uint32_t>(::getpid()))
                  << thread_id.value_or(static_cast<std::uint32_t>(::getpid()));

    /// Write memory mapping information.
    output_stream << module.start() << (module.end() - module.start()) << module.offset();

    /// Write MMAP2 additional fields: device_major, device_minor, ino, ino_generation.
    /// Get real filesystem information using stat().
    auto device_major = 0U;
    auto device_minor = 0U;
    auto ino = static_cast<std::uint64_t>(0UL);

    if (struct stat file_stat{}; ::stat(module.path().c_str(), &file_stat) == 0) {
      device_major = static_cast<std::uint32_t>(::major(file_stat.st_dev));
      device_minor = static_cast<std::uint32_t>(::minor(file_stat.st_dev));
      ino = static_cast<std::uint64_t>(file_stat.st_ino);
    }

    output_stream << device_major << device_minor << ino
                  << static_cast<std::uint64_t>(0UL)

                  /// Write MMAP2 additional fields: prot, flags.
                  << static_cast<std::uint32_t>(PROT_READ | PROT_EXEC)
                  << static_cast<std::uint32_t>(MAP_PRIVATE)

                  /// Write the filename with padding.
                  << module.path() << '\0';

    /// Add padding to align to 8 bytes.
    if (const auto padding = aligned_filename_length - filename_length; padding > 0U) {
      output_stream << std::string(padding, '\0');
    }

    /// Write sample_id values.
    RecordFileWriter::write_sample_id(output_stream, process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);
  }

  return output_stream.to_string();
}

std::string
perf::RecordFileWriter::generate_comm_records(const std::optional<std::uint32_t> process_id,
                                              const std::optional<std::uint32_t> thread_id,
                                              const std::optional<std::uint64_t> timestamp,
                                              const std::optional<std::uint64_t> sample_id,
                                              const std::optional<std::uint64_t> stream_id,
                                              const std::optional<std::uint32_t> cpu_id)
{
  auto output_stream = BinaryStream<std::ostringstream>{};

  /// Create COMM record for process name
  std::string comm_name;
  if (auto process_name = util::SymbolResolver::read_process_name(); process_name.has_value()) {
    comm_name = std::move(process_name.value());
  } else {
    comm_name = "unknown";
  }

  const auto comm_length = comm_name.length() + /* null terminator */ 1U;
  const auto aligned_comm_length = (comm_length + 7U) & /* 8-byte aligned */ ~7UL;

  const auto record_size =
    sizeof(perf_event_header) + sizeof(std::uint32_t) + sizeof(std::uint32_t) + aligned_comm_length +
    RecordFileWriter::calculate_sample_id_all_size(process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);

  /// Create and write the COMM record header
  auto header = perf_event_header{};
  header.type = PERF_RECORD_COMM;
  header.misc = PERF_RECORD_MISC_COMM_EXEC | PERF_RECORD_MISC_USER;
  header.size = static_cast<std::uint16_t>(record_size);
  output_stream << header;

  /// Write PID and TID.
  const auto pid = process_id.value_or(static_cast<std::uint32_t>(::getpid()));
  output_stream << pid << thread_id.value_or(pid);

  /// Write the command name with null terminator
  output_stream << comm_name << '\0';

  /// Add padding to align to 8 bytes
  if (const auto padding = aligned_comm_length - comm_length; padding > 0U) {
    output_stream << std::string(padding, '\0');
  }

  /// Write sample_id field.
  RecordFileWriter::write_sample_id(output_stream, process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);

  return output_stream.to_string();
}

std::uint64_t
perf::RecordFileWriter::calculate_size(const std::vector<std::vector<std::vector<std::byte>>>& sample_data) noexcept
{
  return std::accumulate(sample_data.begin(), sample_data.end(), 0ULL, [](const auto sum, const auto& counter_data) {
    return sum + std::accumulate(counter_data.begin(),
                                 counter_data.end(),
                                 0ULL,
                                 [](const auto buffer_sum, const auto& buffer) { return buffer_sum + buffer.size(); });
  });
}

std::tuple<std::optional<std::uint32_t>,
           std::optional<std::uint32_t>,
           std::optional<std::uint64_t>,
           std::optional<std::uint64_t>,
           std::optional<std::uint64_t>,
           std::optional<std::uint32_t>>
perf::RecordFileWriter::read_first_sample_id(const SampleRecordingValues& sampler_values,
                                             const std::vector<std::vector<std::vector<std::byte>>>& sample_data)
{
  /// If none of the values was sampled, we can cancel early without scanning.
  if (sampler_values.is_set(SampleRecordingValues::Field::Id) ||
      sampler_values.is_set(SampleRecordingValues::Field::ThreadId) ||
      sampler_values.is_set(SampleRecordingValues::Field::Timestamp) ||
      sampler_values.is_set(SampleRecordingValues::Field::StreamId) ||
      sampler_values.is_set(SampleRecordingValues::Field::CpuId)) {
    for (const auto& sample_counter : sample_data) {
      /// Scan all the buffers to find the first sample_id.
      for (const auto& buffer : sample_counter) {
        auto iterator = reinterpret_cast<std::uintptr_t>(buffer.data());
        const auto end = iterator + buffer.size();

        /// Scan over all samples stored in the user-level buffer.
        while (iterator < end) {
          auto entry = SampleIterator{ iterator };

          if (entry.size() == 0U) {
            break;
          }

          if (entry.is_sample_event()) {
            auto process_id = std::optional<std::uint32_t>{};
            auto thread_id = std::optional<std::uint32_t>{};
            auto timestamp = std::optional<std::uint64_t>{};
            auto sample_id = std::optional<std::uint64_t>{};
            auto stream_id = std::optional<std::uint64_t>{};
            auto cpu_id = std::optional<std::uint32_t>{};

            if (sampler_values.is_set(SampleRecordingValues::Field::Id)) {
              sample_id = entry.read<std::uint64_t>();
            }

            /// Instruction pointer is not needed and consequently ignored.
            if (sampler_values.is_set(SampleRecordingValues::Field::LogicalInstructionPointer)) {
              entry.skip<std::uint64_t>();
            }

            if (sampler_values.is_set(SampleRecordingValues::Field::ThreadId)) {
              process_id = entry.read<std::uint32_t>();
              thread_id = entry.read<std::uint32_t>();
            }

            if (sampler_values.is_set(SampleRecordingValues::Field::Timestamp)) {
              timestamp = entry.read<std::uint64_t>();
            }

            if (sampler_values.is_set(SampleRecordingValues::Field::StreamId)) {
              stream_id = entry.read<std::uint64_t>();
            }

            if (sampler_values.is_set(SampleRecordingValues::Field::LogicalMemoryAddress)) {
              entry.skip<std::uint64_t>();
            }

            if (sampler_values.is_set(SampleRecordingValues::Field::CpuId)) {
              cpu_id = entry.read<std::uint32_t>();
            }

            /// If any of these values was found, return the sample_id values.
            return std::make_tuple(process_id, thread_id, timestamp, sample_id, stream_id, cpu_id);
          }

          /// Go to the next sample.
          iterator += entry.size();
        }
      }
    }
  }

  return std::make_tuple(std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
}

std::uint64_t
perf::RecordFileWriter::calculate_sample_id_all_size(std::optional<std::uint32_t> process_id,
                                                     std::optional<std::uint32_t> thread_id,
                                                     std::optional<std::uint64_t> timestamp,
                                                     std::optional<std::uint64_t> sample_id,
                                                     std::optional<std::uint64_t> stream_id,
                                                     std::optional<std::uint32_t> cpu_id)
{
  return (sizeof(std::uint64_t) * static_cast<std::uint64_t>(process_id.has_value() || thread_id.has_value())) +
         (sizeof(std::uint64_t) * static_cast<std::uint64_t>(timestamp.has_value())) +
         (sizeof(std::uint64_t) * static_cast<std::uint64_t>(sample_id.has_value())) +
         (sizeof(std::uint64_t) * static_cast<std::uint64_t>(stream_id.has_value())) +
         (sizeof(std::uint64_t) * static_cast<std::uint64_t>(cpu_id.has_value())) +
         (sizeof(std::uint64_t) * static_cast<std::uint64_t>(sample_id.has_value()));
}

void
perf::RecordFileWriter::write_sample_id(BinaryStream<std::ostringstream>& output_stream,
                                        std::optional<std::uint32_t> process_id,
                                        std::optional<std::uint32_t> thread_id,
                                        std::optional<std::uint64_t> timestamp,
                                        std::optional<std::uint64_t> sample_id,
                                        std::optional<std::uint64_t> stream_id,
                                        std::optional<std::uint32_t> cpu_id)
{
  if (process_id.has_value() || thread_id.has_value()) {
    const auto pid = process_id.value_or(static_cast<std::uint32_t>(::getpid()));
    output_stream << pid << thread_id.value_or(pid);
  }

  if (timestamp.has_value()) {
    output_stream << timestamp.value_or(0ULL);
  }

  if (sample_id.has_value()) {
    output_stream << sample_id.value_or(0ULL);
  }

  if (stream_id.has_value()) {
    output_stream << stream_id.value_or(0ULL);
  }

  if (cpu_id.has_value()) {
    output_stream << cpu_id.value_or(0U) << static_cast<std::uint32_t>(0);
  }

  if (sample_id.has_value()) {
    output_stream << sample_id.value_or(0ULL);
  }
}
