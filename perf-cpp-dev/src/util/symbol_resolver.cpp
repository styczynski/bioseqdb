#include "perfcpp/sample/record_file_writer.hpp"

#include <algorithm>
#include <cstring>
#include <cxxabi.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <perfcpp/exception.hpp>
#include <perfcpp/util/symbol_resolver.hpp>
#include <perfcpp/util/unique_file_descriptor.hpp>
#include <regex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

perf::util::SymbolResolver::SymbolResolver()
{
  this->_resolved_symbols.reserve(1ULL << 10);

  for (auto& module : util::SymbolResolver::read_modules()) {
    if (auto symbols = util::SymbolResolver::parse_symbol_table(module); !symbols.empty()) {
      this->_modules.insert(std::make_pair(std::move(module), std::move(symbols)));
    }
  }
}

std::optional<perf::util::SymbolResolver::ResolvedSymbol>
perf::util::SymbolResolver::resolve(const std::uintptr_t logical_instruction_pointer) noexcept
{
  /// Query the cache.
  if (auto iterator = this->_resolved_symbols.find(logical_instruction_pointer);
      iterator != this->_resolved_symbols.end()) {
    return iterator->second;
  }

  // Resolve the symbol and store in the cache, if it could be resolved,
  for (const auto& [module, symbols] : this->_modules) {
    if (logical_instruction_pointer >= module.start() && logical_instruction_pointer < module.end()) {
      /// Resolve the symbol.
      auto symbol = util::SymbolResolver::resolve(module, symbols, logical_instruction_pointer);

      /// Store the symbol in the cache.
      if (symbol != std::nullopt) {
        this->_resolved_symbols.insert(std::make_pair(logical_instruction_pointer, symbol.value()));
      }

      return symbol;
    }
  }

  return std::nullopt;
}

std::optional<perf::util::SymbolResolver::ResolvedSymbol>
perf::util::SymbolResolver::resolve(const perf::util::SymbolResolver::Module& module,
                                    const std::vector<Symbol>& symbols,
                                    std::uintptr_t logical_instruction_pointer) noexcept
{
  const auto relative_address = logical_instruction_pointer - module.start() + module.offset();

  /// Find the closest symbol.
  auto closest_symbol_iterator =
    std::upper_bound(symbols.begin(), symbols.end(), relative_address, [](const auto address, const auto& symbol) {
      return address < symbol.address();
    });

  if (closest_symbol_iterator == symbols.begin()) {
    return std::nullopt;
  }

  /// Get the symbol at or before our address.
  --closest_symbol_iterator;

  // Check if we're within the symbol's range
  if (!closest_symbol_iterator->is_in_range(relative_address)) {
    return std::nullopt;
  }

  return ResolvedSymbol{ module, *closest_symbol_iterator, relative_address - closest_symbol_iterator->address() };
}

std::vector<perf::util::SymbolResolver::Module>
perf::util::SymbolResolver::read_modules()
{
  auto modules_stream = std::ifstream{ "/proc/self/maps" };
  if (!modules_stream.is_open()) {
    return {};
  }

  auto modules = std::vector<Module>{};
  modules.reserve(32U);

  std::string line;
  const auto map_regex =
    std::regex{ R"(([0-9a-f]+)-([0-9a-f]+)\s+([rwxp-]+)\s+([0-9a-f]+)\s+[0-9a-f]+:[0-9a-f]+\s+\d+\s*(.*)?)" };

  while (std::getline(modules_stream, line)) {
    std::smatch match;
    if (std::regex_match(line, match, map_regex)) {
      auto path = match[5].str();
      auto permission = match[3].str();

      if (permission.find('x') != std::string::npos && !path.empty() && path.front() == '/') {
        /// Turn path into module name.
        const auto pos = path.find_last_of('/');
        auto module_name = (pos != std::string::npos) ? path.substr(pos + 1U) : path;

        /// Extract build ID for this module.
        auto build_id = util::SymbolResolver::extract_build_id(path);

        modules.emplace_back(std::move(module_name),
                             std::stoull(match[1].str(), nullptr, 16),
                             std::stoull(match[2].str(), nullptr, 16),
                             std::stoull(match[4].str(), nullptr, 16),
                             std::move(path),
                             std::move(permission),
                             std::move(build_id));
      }
    }
  }

  return modules;
}

std::optional<std::string>
perf::util::SymbolResolver::read_process_name()
{
  if (auto comm_file = std::ifstream{ "/proc/self/comm" }; comm_file.is_open()) {
    std::string name;
    std::getline(comm_file, name);

    return name;
  }

  return std::nullopt;
}

std::vector<perf::util::SymbolResolver::Symbol>
perf::util::SymbolResolver::extract_symbols_from_table(void* elf_data,
                                                       const Elf64_Shdr* symbol_table,
                                                       const Elf64_Shdr* string_table)
{
  const auto* symbols = reinterpret_cast<const Elf64_Sym*>(static_cast<char*>(elf_data) + symbol_table->sh_offset);
  const auto* strings = static_cast<char*>(elf_data) + string_table->sh_offset;
  const auto symbols_size = symbol_table->sh_size / sizeof(Elf64_Sym);

  auto extracted_symbols = std::vector<Symbol>{};
  extracted_symbols.reserve(symbols_size);

  /// Read symbols and transform to Symbol instances.
  for (auto i = 0ULL; i < symbols_size; ++i) {
    const auto symbol_type = ELF64_ST_TYPE(symbols[i].st_info);
    /// Include regular functions (STT_FUNC) and indirect functions (STT_GNU_IFUNC).
    if ((symbol_type == STT_FUNC || symbol_type == STT_GNU_IFUNC) && symbols[i].st_name != 0U) {
      if (auto mangled_name = std::string(strings + symbols[i].st_name); !mangled_name.empty()) {
        /// Demangle C++ symbol names for better readability.
        auto demangled_name = util::SymbolResolver::demangle_symbol_name(std::move(mangled_name));
        extracted_symbols.emplace_back(std::move(demangled_name), symbols[i].st_value, symbols[i].st_size);
      }
    }
  }

  return extracted_symbols;
}

std::vector<perf::util::SymbolResolver::Symbol>
perf::util::SymbolResolver::parse_symbol_table(const perf::util::SymbolResolver::Module& module)
{
  const auto file_descriptor = util::UniqueFileDescriptor{ ::open(module.path().c_str(), O_RDONLY) };
  if (!file_descriptor.has_value()) {
    throw CannotReadSymbolsForModule{ module.name(), module.path() };
  }

  struct stat stat_{};
  if (::fstat(file_descriptor.value(), &stat_) < 0) {
    throw CannotReadFstatForModule{ module.name(), module.path() };
  }

  const auto stat_size = static_cast<std::size_t>(stat_.st_size);

  /// MMap ELF data.
  auto* elf_data = ::mmap(nullptr, stat_size, PROT_READ, MAP_PRIVATE, file_descriptor.value(), 0);
  if (elf_data == MAP_FAILED) {
    throw CannotReadElfForModule{ module.name(), module.path() };
  }
  const auto* elf_header = static_cast<const Elf64_Ehdr*>(elf_data);

  /// Verify ELF magic.
  if (std::memcmp(&elf_header->e_ident[0], ELFMAG, SELFMAG) != 0) {
    ::munmap(elf_data, stat_size);
    throw CannotVerifyElfMagicForModule{ module.name(), module.path() };
  }

  const auto* section_header_table =
    reinterpret_cast<const Elf64_Shdr*>(static_cast<const char*>(elf_data) + elf_header->e_shoff);

  auto extracted_symbols = std::vector<Symbol>{};

  /// Extract symbols from SYMTAB (static symbol table) if available.
  const auto [symtab_table, symtab_strings] =
    util::SymbolResolver::find_symbol_and_string_tables(section_header_table, elf_header->e_shnum);
  if (symtab_table != nullptr && symtab_strings != nullptr) {
    extracted_symbols = util::SymbolResolver::extract_symbols_from_table(elf_data, symtab_table, symtab_strings);
  }

  /// Extract symbols from DYNSYM (dynamic symbol table) if available and merge with existing symbols.
  for (auto i = 0U; i < elf_header->e_shnum; ++i) {
    if (section_header_table[i].sh_type == SHT_DYNSYM) {
      const auto* dynsym_table = &section_header_table[i];
      const auto* dynsym_strings = &section_header_table[section_header_table[i].sh_link];
      auto dynsym_symbols = util::SymbolResolver::extract_symbols_from_table(elf_data, dynsym_table, dynsym_strings);
      extracted_symbols.insert(extracted_symbols.end(),
                               std::make_move_iterator(dynsym_symbols.begin()),
                               std::make_move_iterator(dynsym_symbols.end()));
      break;
    }
  }

  /// Unmap ELF data.
  ::munmap(elf_data, stat_size);

  /// Sort symbols by address.
  std::sort(extracted_symbols.begin(), extracted_symbols.end(), [](const Symbol& first, const Symbol& second) {
    return first.address() < second.address();
  });

  /// Remove duplicate symbols at the same address.
  auto unique_end = std::unique(extracted_symbols.begin(),
                                extracted_symbols.end(),
                                [](const Symbol& a, const Symbol& b) { return a.address() == b.address(); });
  extracted_symbols.erase(unique_end, extracted_symbols.end());

  return extracted_symbols;
}

std::string
perf::util::SymbolResolver::demangle_symbol_name(std::string&& symbol_name)
{
  auto status = 0;
  auto demangled_name = std::unique_ptr<char, void (*)(void*)>(
    abi::__cxa_demangle(symbol_name.c_str(), nullptr, nullptr, &status), std::free);

  /// Return demangled name if successful, otherwise return the original mangled name.
  return (status == 0 && demangled_name) ? std::string{ demangled_name.get() } : symbol_name;
}

std::pair<const Elf64_Shdr*, const Elf64_Shdr*>
perf::util::SymbolResolver::find_symbol_and_string_tables(const Elf64_Shdr* section_header_table,
                                                          const std::uint16_t size) noexcept
{
  /// First, try to find the static symbol table (SHT_SYMTAB) which contains all symbols.
  for (auto i = 0U; i < size; ++i) {
    if (section_header_table[i].sh_type == SHT_SYMTAB) {
      return std::make_pair(&section_header_table[i], &section_header_table[section_header_table[i].sh_link]);
    }
  }

  /// If SHT_SYMTAB is not found (stripped binary/library), fallback to dynamic symbol table (SHT_DYNSYM).
  for (auto i = 0U; i < size; ++i) {
    if (section_header_table[i].sh_type == SHT_DYNSYM) {
      return std::make_pair(&section_header_table[i], &section_header_table[section_header_table[i].sh_link]);
    }
  }

  return std::make_pair(nullptr, nullptr);
}

std::vector<std::uint8_t>
perf::util::SymbolResolver::extract_build_id(const std::string& path) noexcept
{
  const auto file_descriptor = util::UniqueFileDescriptor{ ::open(path.c_str(), O_RDONLY) };
  if (!file_descriptor.has_value()) {
    return {};
  }

  struct stat stat_{};
  if (::fstat(file_descriptor.value(), &stat_) < 0) {
    return {};
  }

  const auto stat_size = static_cast<std::size_t>(stat_.st_size);

  /// MMap ELF data.
  auto* elf_data = ::mmap(nullptr, stat_size, PROT_READ, MAP_PRIVATE, file_descriptor.value(), 0);
  if (elf_data == MAP_FAILED) {
    return {};
  }

  const auto* elf_header = static_cast<const Elf64_Ehdr*>(elf_data);

  /// Verify ELF magic.
  if (std::memcmp(&elf_header->e_ident[0], ELFMAG, SELFMAG) != 0) {
    ::munmap(elf_data, stat_size);
    return {};
  }

  /// Look for build ID in note sections.
  const auto* section_header_table =
    reinterpret_cast<const Elf64_Shdr*>(static_cast<const char*>(elf_data) + elf_header->e_shoff);

  std::vector<std::uint8_t> build_id;

  for (auto i = 0U; i < elf_header->e_shnum; ++i) {
    if (section_header_table[i].sh_type == SHT_NOTE) {
      const auto* note_data = static_cast<const char*>(elf_data) + section_header_table[i].sh_offset;
      const auto note_size = section_header_table[i].sh_size;

      /// Parse notes in this section.
      auto offset = 0ULL;
      while (offset + sizeof(Elf64_Nhdr) < note_size) {
        const auto* note_header = reinterpret_cast<const Elf64_Nhdr*>(note_data + offset);

        /// Align name size and desc size to 4-byte boundaries.
        const auto name_size_aligned = (note_header->n_namesz + 3) & ~static_cast<Elf64_Word>(3);
        const auto desc_size_aligned = (note_header->n_descsz + 3) & ~static_cast<Elf64_Word>(3);

        if (offset + sizeof(Elf64_Nhdr) + name_size_aligned + desc_size_aligned > note_size) {
          break;
        }

        const auto* name = note_data + offset + sizeof(Elf64_Nhdr);
        const auto* desc = name + name_size_aligned;

        /// Check if this is a build ID note (NT_GNU_BUILD_ID = 3).
        if (note_header->n_type == 3 && note_header->n_namesz == 4 && std::strncmp(name, "GNU", 3) == 0 &&
            note_header->n_descsz > 0) {
          build_id.assign(desc, desc + note_header->n_descsz);
          break;
        }

        offset += sizeof(Elf64_Nhdr) + name_size_aligned + desc_size_aligned;
      }

      if (!build_id.empty()) {
        break;
      }
    }
  }

  /// Unmap ELF data.
  ::munmap(elf_data, stat_size);

  return build_id;
}