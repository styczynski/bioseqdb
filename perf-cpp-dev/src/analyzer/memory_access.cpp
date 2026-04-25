#include <algorithm>
#include <numeric>
#include <perfcpp/analyzer/memory_access.hpp>
#include <perfcpp/exception.hpp>
#include <perfcpp/util/table.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

void
perf::analyzer::MemoryAccess::add(perf::analyzer::DataType&& data_type)
{
  const auto data_type_iterator = this->find(data_type.name());
  if (data_type_iterator == this->_data_type_instances.end()) {
    this->_data_type_instances.emplace_back(std::move(data_type),
                                            std::unordered_map<std::string, std::vector<std::uintptr_t>>{});
  } else {
    throw DataTypeAlreadyRegisteredError{ data_type.name() };
  }
}

void
perf::analyzer::MemoryAccess::annotate(const std::string_view data_type_name,
                                       const std::uintptr_t data_object,
                                       const std::string& instance_name)
{
  if (auto type_iterator = this->find(data_type_name); type_iterator != this->_data_type_instances.end()) {
    auto& type_instances = type_iterator->second;

    /// Check if the data object already contains the tag.
    if (auto tag_iterator = type_instances.find(instance_name); tag_iterator != type_instances.end()) {
      tag_iterator->second.push_back(data_object);
    } else {
      /// If the tag did not exist, add it as a new map tag -> [addresses].
      auto instances = std::vector<std::uintptr_t>{};
      instances.reserve(2048U);
      instances.push_back(data_object);
      type_instances.insert(std::make_pair(instance_name, std::move(instances)));
    }
  } else {
    throw DataTypeNotRegisteredError{ data_type_name };
  }
}

std::vector<std::pair<perf::analyzer::DataType, std::unordered_map<std::string, std::vector<std::uintptr_t>>>>::iterator
perf::analyzer::MemoryAccess::find(std::string_view data_type_name) noexcept
{
  return std::find_if(this->_data_type_instances.begin(),
                      this->_data_type_instances.end(),
                      [data_type_name](const auto& data_type_and_tags) {
                        return std::get<0>(data_type_and_tags).name() == data_type_name;
                      });
}

perf::analyzer::MemoryAccessResult
perf::analyzer::MemoryAccess::map(const SampleResult& sample_result)
{
  /// Copy of all data types; the result will contain a copy since we add the samples to the members.
  auto data_types = std::vector<DataType>{};
  data_types.reserve(this->_data_type_instances.size());

  /// List of all registered instances and the linked data type.
  auto registered_addresses = std::vector<std::pair<std::uintptr_t, std::reference_wrapper<DataType>>>{};

  /// Unfold the list of (DataType, [instance addresses]) into a list of [(instance address, DataType)] to perform a
  /// lower bound search for each sample.
  for (const auto& [data_type, tags] : this->_data_type_instances) {

    for (const auto& [tag, addresses] : tags) {
      auto data_typ_tag_name =
        tag.empty() ? data_type.name() : std::string{ data_type.name() }.append("::").append(tag);

      /// Copy the data type, if not already done.
      auto& tagged_data_type = data_types.emplace_back(std::move(data_typ_tag_name), data_type);

      /// Fill up the empty spaces in the data type.
      MemoryAccess::add_empty_attributes(tagged_data_type);

      /// Unfold the instance addresses into the registered_addresses list.
      std::transform(
        addresses.cbegin(),
        addresses.cend(),
        std::back_inserter(registered_addresses),
        [&tagged_data_type](const auto address) { return std::make_pair(address, std::ref(tagged_data_type)); });
    }
  }

  if (registered_addresses.empty()) {
    return MemoryAccessResult{};
  }

  /// Sort the addresses to perform a lower bound search.
  std::sort(registered_addresses.begin(), registered_addresses.end(), DataTypeInstanceComp{});

  /// Scan the samples and annotate each sample to the member of a data type instance the sample may belong to.
  for (const auto& sample : sample_result) {
    if (const auto memory_address = sample.data_access().logical_memory_address().value_or(0ULL);
        memory_address > 0ULL) {

      /// For every sampled address, find the potentially linked data type instance.
      auto data_type_instance = std::lower_bound(
        registered_addresses.begin(), registered_addresses.end(), memory_address, DataTypeInstanceComp{});

      /// The lower bound will find the first data type instance that is not less than the sampled memory address.
      /// Thus, we have to check if (a) there is a potential data type instance and (b) not all elements are greater.
      if (data_type_instance != registered_addresses.begin()) {

        /// Go back to the potential instance (we found the first that is greater than the potential start address).
        --data_type_instance;

        /// Calculate the offset within the data type (address - start of the instance)
        const auto offset = memory_address - data_type_instance->first;

        /// Verify that the address maps to that object instance.
        if (offset < data_type_instance->second.get().size()) {
          /// Find the member that the sample may linked to and append the sample to the member's samples.
          for (auto& member : data_type_instance->second.get().members()) {
            if (member.offset() <= offset && offset < (member.offset() + member.size())) {
              member.samples().emplace_back(sample);
              break;
            }
          }
        }
      }
    }
  }

  return MemoryAccessResult{ std::move(data_types) };
}

void
perf::analyzer::MemoryAccess::add_empty_attributes(perf::analyzer::DataType& data_type)
{
  auto& members = data_type.members();
  if (members.empty()) {
    return;
  }

  auto size = members.size();
  for (auto i = 0U; i < size - 1U; ++i) {

    /// Check if there is a whole between two members i and i+1.
    const auto distance = members[i + 1U].offset() - (members[i].offset() + members[i].size());

    /// If there is a whole, add a new member indicating that whole.
    if (distance > 0U) {
      members.insert(members.begin() + (i + 1U),
                     DataType::Member("/* unknown */", members[i].offset() + members[i].size(), distance));
      ++i;
      ++size;
    }
  }

  /// Repeat the step for the last member and the size of the data type.
  const auto& last_member = members.back();
  const auto distance = data_type.size() - (last_member.offset() + last_member.size());
  if (distance > 0U) {
    members.emplace_back("/* unknown */", last_member.offset() + last_member.size(), distance);
  }
}

std::string
perf::analyzer::MemoryAccessResult::to_string() const
{
  /// Checks if any sample mapped to the data type is of a specific data access type.
  auto has_any_access_type = [](const DataType& data_type, const DataAccess::AccessType type) {
    return std::find_if(data_type.members().begin(), data_type.members().end(), [type](const auto& member) {
             return std::find_if(member.samples().begin(), member.samples().end(), [type](const auto& sample) {
                      return sample.data_access().type().has_value() && sample.data_access().type().value() == type;
                    }) != member.samples().end();
           }) != data_type.members().end();
  };

  auto data_types = std::vector<std::tuple<std::string, std::size_t, util::Table, std::size_t>>{};

  for (const auto& data_type : this->_data_types) {
    if (data_type.members().empty()) {
      continue;
    }

    auto name = data_type.name();

    const auto has_load = has_any_access_type(data_type, DataAccess::AccessType::Load);
    const auto has_software_prefetch = has_any_access_type(data_type, DataAccess::AccessType::SoftwarePrefetch);
    const auto has_store = has_any_access_type(data_type, DataAccess::AccessType::Store);

    auto count_samples = 0ULL;

    /// Create the data type table.
    auto table = util::Table{ 2U };
    table.reserve(data_type.members().size() + 3U);

    /// Access type headers.
    auto access_type_headers = std::vector<util::Table::Header>{ util::Table::Header{ "", 3U, false } };
    auto category_headers = std::vector<util::Table::Header>{ util::Table::Header{ "", 3U, false } };
    auto row_headers = std::vector<util::Table::Header>{ util::Table::Header{ "" },
                                                         util::Table::Header{ "", util::Table::Alignment::Left },
                                                         util::Table::Header{ "samples" } };

    if (has_load) {
      access_type_headers.emplace_back("loads",
                                       10U + (static_cast<std::uint8_t>(HardwareInfo::is_amd()) * 4U) +
                                         static_cast<std::uint8_t>(HardwareInfo::is_intel()),
                                       true);

      category_headers.emplace_back("", 1U, true);
      category_headers.emplace_back(
        "latency", static_cast<std::uint8_t>(2U + static_cast<std::uint8_t>(HardwareInfo::is_amd())), true);
      category_headers.emplace_back(
        "cache hits", static_cast<std::uint8_t>(3U + static_cast<std::uint8_t>(HardwareInfo::is_intel())), true);
      category_headers.emplace_back("RAM hits", 2U, true);
      if (HardwareInfo::is_amd()) {
        category_headers.emplace_back("MAB", 2U, true);
      }
      category_headers.emplace_back(
        "TLB", static_cast<std::uint8_t>(2U + static_cast<std::uint8_t>(HardwareInfo::is_amd())), true);

      row_headers.emplace_back("count");
      row_headers.emplace_back("cache");
      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("uOp");
        row_headers.emplace_back("dTLB");
      } else {
        row_headers.emplace_back("instr.");
      }

      row_headers.emplace_back("L1d");
      if (HardwareInfo::is_intel()) {
        row_headers.emplace_back("LFB");
      }
      row_headers.emplace_back("L2");
      row_headers.emplace_back("L3");

      row_headers.emplace_back("local");
      row_headers.emplace_back("remote");

      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("no alloc.");
        row_headers.emplace_back("slots");
      }

      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("dTLB");
        row_headers.emplace_back("STLB");
      } else {
        row_headers.emplace_back("hit");
      }
      row_headers.emplace_back("miss");
    }

    if (has_software_prefetch) {
      access_type_headers.emplace_back("software prefetches",
                                       10U + (static_cast<std::uint8_t>(HardwareInfo::is_amd()) * 3U) +
                                         static_cast<std::uint8_t>(HardwareInfo::is_intel()),
                                       true);

      category_headers.emplace_back("", 1U, true);
      category_headers.emplace_back("latency", 2U, true);
      category_headers.emplace_back(
        "cache hits", static_cast<std::uint8_t>(3U + static_cast<std::uint8_t>(HardwareInfo::is_intel())), true);
      category_headers.emplace_back("RAM hits", 2U, true);
      if (HardwareInfo::is_amd()) {
        category_headers.emplace_back("MAB", 2U, true);
      }
      category_headers.emplace_back(
        "TLB", static_cast<std::uint8_t>(2U + static_cast<std::uint8_t>(HardwareInfo::is_amd())), true);

      row_headers.emplace_back("count");
      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("uOp");
        row_headers.emplace_back("dTLB");
      } else {
        row_headers.emplace_back("cache");
        row_headers.emplace_back("instr.");
      }

      row_headers.emplace_back("L1d");
      if (HardwareInfo::is_intel()) {
        row_headers.emplace_back("LFB");
      }
      row_headers.emplace_back("L2");
      row_headers.emplace_back("L3");

      row_headers.emplace_back("local");
      row_headers.emplace_back("remote");

      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("no alloc.");
        row_headers.emplace_back("slots");
      }

      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("dTLB");
        row_headers.emplace_back("STLB");
      } else {
        row_headers.emplace_back("hit");
      }
      row_headers.emplace_back("miss");
    }

    if (has_store) {
      access_type_headers.emplace_back("stores", 2U + (static_cast<std::uint8_t>(HardwareInfo::is_amd()) * 1U), true);

      category_headers.emplace_back("", 1U, true);
      category_headers.emplace_back("latency", 1U + static_cast<std::uint8_t>(HardwareInfo::is_amd()), true);

      row_headers.emplace_back("count");
      if (HardwareInfo::is_amd()) {
        row_headers.emplace_back("uOp");
        row_headers.emplace_back("dTLB");
      } else {
        row_headers.emplace_back("instr.");
      }
    }

    table.add(std::move(access_type_headers));
    table.add(std::move(category_headers));
    table.add(std::move(row_headers));

    /// Add member attributes to table.
    for (const auto& member : data_type.members()) {

      const auto statistics = std::accumulate(member.samples().cbegin(),
                                              member.samples().cend(),
                                              MemberStatistic{},
                                              [](auto& current, const auto& sample) { return current += sample; });

      auto row = util::Table::Row{};
      auto member_offset = std::to_string(member.offset()).append(": ");
      auto member_name = std::string{ member.name() }.append(" (").append(std::to_string(member.size())).append("B)");
      row << member_offset << member_name << member.samples().size();

      /// Loads
      if (has_load) {
        row << statistics.loads().count() << statistics.loads().average_cache_latency()
            << statistics.loads().average_instruction_latency();
        if (HardwareInfo::is_amd()) {
          row << statistics.loads().average_dtlb_latency();
        }
        row << statistics.loads().count_l1_hits();
        if (HardwareInfo::is_intel()) {
          row << statistics.loads().count_mhb_hits();
        }
        row << statistics.loads().count_l2_hits() << statistics.loads().count_l3_hits()
            << statistics.loads().count_local_ram_hits() << statistics.loads().count_remote_ram_hits();

        if (HardwareInfo::is_amd()) {
          row << statistics.loads().count_mhb_hits() << statistics.loads().average_alloc_mab_entries();
        }

        row << statistics.loads().dtlb_hits();
        if (HardwareInfo::is_amd()) {
          row << statistics.loads().stlb_hits();
        }
        row << statistics.loads().stlb_misses();
      }

      /// Software Prefetches
      if (has_software_prefetch) {
        row << statistics.software_prefetches().count();
        if (HardwareInfo::is_amd()) {
          row << statistics.software_prefetches().average_instruction_latency()
              << statistics.software_prefetches().average_dtlb_latency();
        } else {
          row << statistics.software_prefetches().average_cache_latency()
              << statistics.software_prefetches().average_instruction_latency();
        }
        row << statistics.software_prefetches().count_l1_hits();
        if (HardwareInfo::is_intel()) {
          row << statistics.software_prefetches().count_mhb_hits();
        }
        row << statistics.software_prefetches().count_l2_hits() << statistics.software_prefetches().count_l3_hits()
            << statistics.software_prefetches().count_local_ram_hits()
            << statistics.software_prefetches().count_remote_ram_hits();

        if (HardwareInfo::is_amd()) {
          row << statistics.software_prefetches().count_mhb_hits()
              << statistics.software_prefetches().average_alloc_mab_entries();
        }

        row << statistics.software_prefetches().dtlb_hits();
        if (HardwareInfo::is_amd()) {
          row << statistics.software_prefetches().stlb_hits();
        }
        row << statistics.software_prefetches().stlb_misses();
      }

      /// Stores
      if (has_store) {
        row << statistics.stores().count() << statistics.stores().average_instruction_latency();
        if (HardwareInfo::is_amd()) {
          row << statistics.stores().average_dtlb_latency();
        }
      }

      table.add(std::move(row));
      count_samples += member.samples().size();
    }

    data_types.emplace_back(std::move(name), data_type.size(), std::move(table), count_samples);
  }

  /// Sort data type (instances) by number of samples to show types with more samples first.
  std::sort(data_types.begin(), data_types.end(), [](const auto& left, const auto& right) {
    return std::get<3>(left) > std::get<3>(right);
  });

  /// Print the data types with members as table.
  auto stream = std::stringstream{};
  for (auto data_type_id = 0U; data_type_id < data_types.size(); ++data_type_id) {
    const auto& [name, size, table, _] = data_types[data_type_id];

    if (data_type_id > 0U) {
      stream << "\n";
    }

    stream << "DataType " << name << " (" << size << "B)" << " {\n" << table.to_string() << "}\n";
  }

  return stream.str();
}

std::string
perf::analyzer::MemoryAccessResult::to_json() const
{
  auto stream = std::stringstream{};
  stream << "[";

  for (auto data_type_index = 0U; data_type_index < this->_data_types.size(); ++data_type_index) {
    const auto& data_type = this->_data_types[data_type_index];

    if (data_type_index != 0U) {
      stream << ",";
    }
    stream << R"({ "name":")" << data_type.name() << R"(", "size": )" << data_type.size() << ", \"members\": [";

    for (auto member_index = 0U; member_index < data_type.members().size(); ++member_index) {
      const auto& member = data_type.members()[member_index];

      const auto statistics = std::accumulate(member.samples().cbegin(),
                                              member.samples().cend(),
                                              MemberStatistic{},
                                              [](auto& current, const auto& sample) { return current += sample; });

      if (member_index != 0U) {
        stream << ",";
      }

      stream << "{" << "\"name\":" << "\"" << member.name() << "\"," << "\"offset\":" << member.offset() << ","
             << "\"size\":" << member.size() << "," << "\"samples\":" << member.samples().size() << ",";

      /// Loads
      stream << "\"loads\": {" << "\"count\":" << statistics.loads().count() << ','

             << "\"latency\":{" << (HardwareInfo::is_amd() ? "\"cache-miss\":" : "\"cache\":")
             << statistics.loads().average_cache_latency() << ','
             << (HardwareInfo::is_amd() ? "\"uop\":" : "\"instruction\":")
             << statistics.loads().average_instruction_latency();
      if (HardwareInfo::is_amd()) {
        stream << ",\"dtlb\":" << statistics.loads().average_dtlb_latency();
      }
      stream << "}," << "\"cache-hits\":{" << "\"l1d\":" << statistics.loads().count_l1_hits() << ','
             << "\"l2\":" << statistics.loads().count_l2_hits() << ','
             << "\"l3\":" << statistics.loads().count_l3_hits() << ','
             << "\"mhb\":" << statistics.loads().count_mhb_hits() << "},"

             << "\"ram\":{" << "\"local\":" << statistics.loads().count_local_ram_hits() << ','
             << "\"remote\":" << statistics.loads().count_remote_ram_hits() << "},"

             << "\"tlb\":{" << "\"dtlb\":" << statistics.loads().dtlb_hits() << ','
             << "\"stlb\":" << statistics.loads().stlb_hits() << ',' << "\"miss\":" << statistics.loads().stlb_misses()
             << '}' << "},";

      /// Software prefetches
      stream << "\"software-prefetches\": {" << "\"count\":" << statistics.software_prefetches().count() << ','

             << "\"latency\":{" << (HardwareInfo::is_amd() ? "\"cache-miss\":" : "\"cache\":")
             << statistics.software_prefetches().average_cache_latency() << ','
             << (HardwareInfo::is_amd() ? "\"uop\":" : "\"instruction\":")
             << statistics.software_prefetches().average_instruction_latency();
      if (HardwareInfo::is_amd()) {
        stream << ",\"dtlb\":" << statistics.software_prefetches().average_dtlb_latency();
      }
      stream << "}," << "\"cache-hits\":{" << "\"l1d\":" << statistics.software_prefetches().count_l1_hits() << ','
             << "\"l2\":" << statistics.software_prefetches().count_l2_hits() << ','
             << "\"l3\":" << statistics.software_prefetches().count_l3_hits() << ','
             << "\"mhb\":" << statistics.software_prefetches().count_mhb_hits() << "},"

             << "\"ram\":{" << "\"local\":" << statistics.software_prefetches().count_local_ram_hits() << ','
             << "\"remote\":" << statistics.software_prefetches().count_remote_ram_hits() << "},"

             << "\"tlb\":{" << "\"dtlb\":" << statistics.software_prefetches().dtlb_hits() << ','
             << "\"stlb\":" << statistics.software_prefetches().stlb_hits() << ','
             << "\"miss\":" << statistics.software_prefetches().stlb_misses() << '}' << "},";

      /// Stores
      stream << "\"stores\": {" << "\"count\":" << statistics.software_prefetches().count() << ','

             << "\"latency\":{" << (HardwareInfo::is_amd() ? "\"uop\":" : "\"instruction\":")
             << statistics.software_prefetches().average_instruction_latency();
      if (HardwareInfo::is_amd()) {
        stream << ",\"cache\":" << statistics.software_prefetches().average_cache_latency()
               << ",\"dtlb\":" << statistics.software_prefetches().average_dtlb_latency();
      }
      stream << "}" << "}" << "}";
    }

    stream << "]}";
  }

  stream << "]";
  return stream.str();
}

std::string
perf::analyzer::MemoryAccessResult::to_csv(const std::string_view data_type_name,
                                           const char delimiter,
                                           const bool print_header) const
{
  auto stream = std::stringstream{};

  auto cell = [&stream, delimiter](auto&& v, const bool is_write_delimiter = true) {
    if (is_write_delimiter) {
      stream << delimiter;
    }
    stream << v;
  };

  if (print_header) {
    std::vector<std::string> header = { "name",
                                        "offset",
                                        "size",
                                        "samples",

                                        "loads-count" };

    if (HardwareInfo::is_amd()) {
      header.insert(header.end(), { "loads-latency-cache-miss", "loads-latency-uop", "loads-latency-dtlb" });
    } else {
      header.insert(header.end(), { "loads-latency-cache", "loads-latency-instruction" });
    }

    header.insert(header.end(),
                  { "loads-cache-hits-l1d",
                    "loads-cache-hits-l2",
                    "loads-cache-hits-l3",
                    "loads-cache-hits-mhb",
                    "loads-ram-local",
                    "loads-ram-remote",
                    "loads-tlb-dtlb",
                    "loads-tlb-stlb",
                    "loads-tlb-miss",

                    "software-prefetches-count" });

    if (HardwareInfo::is_amd()) {
      header.insert(header.end(),
                    { "software-prefetches-latency-cache-miss",
                      "software-prefetches-latency-uop",
                      "software-prefetches-latency-dtlb" });
    } else {
      header.insert(header.end(), { "software-prefetches-latency-cache", "software-prefetches-latency-instruction" });
    }

    header.insert(header.end(),
                  { "software-prefetches-cache-hits-l1d",
                    "software-prefetches-cache-hits-l2",
                    "software-prefetches-cache-hits-l3",
                    "software-prefetches-cache-hits-mhb",
                    "software-prefetches-ram-local",
                    "software-prefetches-ram-remote",
                    "software-prefetches-tlb-dtlb",
                    "software-prefetches-tlb-stlb",
                    "software-prefetches-tlb-miss",

                    "stores-count",
                    "stores-latency-instruction" });

    if (HardwareInfo::is_amd()) {
      header.insert(header.end(), { "stores-latency-cache", "stores-latency-dtlb" });
    }

    /* write header row */
    for (auto index = 0U; index < header.size(); ++index) {
      cell(header[index], index > 0U);
    }
    stream << '\n';
  }

  if (auto data_type_iterator =
        std::find_if(this->_data_types.cbegin(),
                     this->_data_types.cend(),
                     [&data_type_name](const auto& data_type) { return data_type.name() == data_type_name; });
      data_type_iterator != this->_data_types.cend()) {
    for (const auto& member : data_type_iterator->members()) {

      const auto statistics = std::accumulate(member.samples().cbegin(),
                                              member.samples().cend(),
                                              MemberStatistic{},
                                              [](auto& current, const auto& sample) { return current += sample; });

      /* member */
      cell(member.name(), false);
      cell(member.offset());
      cell(member.size());
      cell(member.samples().size());

      /* loads */
      cell(statistics.loads().count());
      cell(statistics.loads().average_cache_latency());
      cell(statistics.loads().average_instruction_latency());
      if (HardwareInfo::is_amd()) {
        cell(statistics.loads().average_dtlb_latency());
      }
      cell(statistics.loads().count_l1_hits());
      cell(statistics.loads().count_l2_hits());
      cell(statistics.loads().count_l3_hits());
      cell(statistics.loads().count_mhb_hits());
      cell(statistics.loads().count_local_ram_hits());
      cell(statistics.loads().count_remote_ram_hits());
      cell(statistics.loads().dtlb_hits());
      cell(statistics.loads().stlb_hits());
      cell(statistics.loads().stlb_misses());

      /* software prefetches */
      cell(statistics.software_prefetches().count());
      cell(statistics.software_prefetches().average_cache_latency());
      cell(statistics.software_prefetches().average_instruction_latency());
      if (HardwareInfo::is_amd()) {
        cell(statistics.software_prefetches().average_dtlb_latency());
      }
      cell(statistics.software_prefetches().count_l1_hits());
      cell(statistics.software_prefetches().count_l2_hits());
      cell(statistics.software_prefetches().count_l3_hits());
      cell(statistics.software_prefetches().count_mhb_hits());
      cell(statistics.software_prefetches().count_local_ram_hits());
      cell(statistics.software_prefetches().count_remote_ram_hits());
      cell(statistics.software_prefetches().dtlb_hits());
      cell(statistics.software_prefetches().stlb_hits());
      cell(statistics.software_prefetches().stlb_misses());

      /* stores */
      cell(statistics.stores().count());
      cell(statistics.stores().average_instruction_latency());
      if (HardwareInfo::is_amd()) {
        cell(statistics.stores().average_cache_latency());
        cell(statistics.stores().average_dtlb_latency());
      }

      stream << '\n';
    }
  }

  return stream.str();
}