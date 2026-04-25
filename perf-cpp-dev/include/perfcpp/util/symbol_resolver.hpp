#pragma once
#include <cstdint>
#include <elf.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace perf::util {
/**
 * The SymbolResolver resolves instruction pointers to symbolic names by parsing ELF files
 * and mapping addresses to function symbols within loaded modules.
 */
class SymbolResolver
{
public:
  /**
   * The Module represents a loaded executable or library with its memory mapping information.
   */
  class Module
  {
  public:
    Module(std::string&& name,
           const std::uintptr_t start,
           const std::uintptr_t end,
           const std::size_t offset,
           std::string&& path,
           std::string&& permission) noexcept
      : _name(std::move(name))
      , _start(start)
      , _end(end)
      , _offset(offset)
      , _path(std::move(path))
      , _permissions(std::move(permission))
    {
    }

    Module(std::string&& name,
           const std::uintptr_t start,
           const std::uintptr_t end,
           const std::size_t offset,
           std::string&& path,
           std::string&& permission,
           std::vector<std::uint8_t>&& build_id) noexcept
      : _name(std::move(name))
      , _start(start)
      , _end(end)
      , _offset(offset)
      , _path(std::move(path))
      , _permissions(std::move(permission))
      , _build_id(std::move(build_id))
    {
    }

    Module(const Module&) = default;
    Module(Module&&) noexcept = default;
    ~Module() = default;
    Module& operator=(const Module&) = default;
    Module& operator=(Module&&) noexcept = default;

    /**
     * @return The name of the module.
     */
    [[nodiscard]] const std::string& name() const noexcept { return _name; }

    /**
     * @return The start address of the module in memory.
     */
    [[nodiscard]] std::uintptr_t start() const noexcept { return _start; }

    /**
     * @return The end address of the module in memory.
     */
    [[nodiscard]] std::uintptr_t end() const noexcept { return _end; }

    /**
     * @return The offset of the module mapping.
     */
    [[nodiscard]] std::size_t offset() const noexcept { return _offset; }

    /**
     * @return The file path of the module.
     */
    [[nodiscard]] const std::string& path() const noexcept { return _path; }

    /**
     * @return The memory permissions of the module.
     */
    [[nodiscard]] const std::string& permission() const noexcept { return _permissions; }

    /**
     * @return Build ID as a vector of bytes.
     */
    [[nodiscard]] const std::vector<std::uint8_t>& build_id() const noexcept { return _build_id; }

    /**
     * Compares two modules for equality based on their paths.
     *
     * @param other Module to compare with.
     * @return True if both modules have the same path, false otherwise.
     */
    [[nodiscard]] bool operator==(const Module& other) const { return _path == other._path; }

  private:
    /// Name of the module.
    std::string _name;

    /// Start address of the module in memory.
    std::uintptr_t _start;

    /// End address of the module in memory.
    std::uintptr_t _end;

    /// Offset of the module mapping.
    std::uintptr_t _offset;

    /// File path of the module.
    std::string _path;

    /// Memory permissions of the module.
    std::string _permissions;

    /// Build ID of the module.
    std::vector<std::uint8_t> _build_id;
  };

  /**
   * The ModuleHash provides a hash function for Module objects based on their file path.
   */
  class ModuleHash
  {
  public:
    /**
     * Computes the hash of a module based on its path.
     *
     * @param module Module to hash.
     * @return Hash value of the module's path.
     */
    std::size_t operator()(const Module& module) const { return std::hash<std::string>()(module.path()); }
  };

  class Symbol
  {
  public:
    Symbol(std::string&& name, const std::uintptr_t address, const std::size_t size) noexcept
      : _name(std::move(name))
      , _address(address)
      , _size(size)
    {
    }
    Symbol(const Symbol&) = default;
    Symbol(Symbol&&) noexcept = default;
    ~Symbol() = default;
    Symbol& operator=(const Symbol&) = default;
    Symbol& operator=(Symbol&&) noexcept = default;

    [[nodiscard]] const std::string& name() const noexcept { return _name; }
    [[nodiscard]] std::uintptr_t address() const noexcept { return _address; }
    [[nodiscard]] std::size_t size() const noexcept { return _size; }

    [[nodiscard]] bool is_in_range(const std::uintptr_t address) const noexcept
    {
      return address >= _address && address < (_address + _size);
    }

    [[nodiscard]] bool operator==(const Symbol& other) const noexcept { return _address == other._address; }

    [[nodiscard]] std::string to_string() const
    {
      auto name = _name;
      return name.append(" (")
        .append(std::to_string(_address))
        .append("--")
        .append(std::to_string(_address + size()))
        .append(")");
    }

  private:
    std::string _name;
    std::uintptr_t _address;
    std::size_t _size;
  };

  class ResolvedSymbol
  {
  public:
    ResolvedSymbol(const Module& module, const Symbol& symbol, const std::size_t offset) noexcept
      : _module(module)
      , _symbol(symbol)
      , _offset(offset)
    {
    }
    ResolvedSymbol(const ResolvedSymbol&) = default;
    ResolvedSymbol(ResolvedSymbol&&) noexcept = default;
    ~ResolvedSymbol() noexcept = default;
    ResolvedSymbol& operator=(const ResolvedSymbol&) = delete;
    ResolvedSymbol& operator=(ResolvedSymbol&&) noexcept = delete;

    [[nodiscard]] const Module& module() const noexcept { return _module; }
    [[nodiscard]] const Symbol& symbol() const noexcept { return _symbol; }
    [[nodiscard]] std::size_t offset() const noexcept { return _offset; }

    [[nodiscard]] std::string to_string() const
    {
      auto str = std::string{ "[" };
      return str.append(_module.name()).append("] ").append(_symbol.name()).append("+").append(std::to_string(_offset));
    }

  private:
    const Module& _module;
    const Symbol& _symbol;
    const std::size_t _offset;
  };

  SymbolResolver();
  SymbolResolver(const SymbolResolver&) = default;
  SymbolResolver(SymbolResolver&&) noexcept = default;
  ~SymbolResolver() = default;
  SymbolResolver& operator=(const SymbolResolver&) = default;
  SymbolResolver& operator=(SymbolResolver&&) noexcept = default;

  /**
   * Resolves the symbol the given instruction points to.
   *
   * @param logical_instruction_pointer Logical instruction pointer.
   * @return The resolved symbol, if the symbol can be resolved.
   */
  [[nodiscard]] std::optional<ResolvedSymbol> resolve(std::uintptr_t logical_instruction_pointer) noexcept;

  /**
   * Parses the /proc/self/maps table.
   *
   * @return List of all modules found in /proc/self/maps.
   */
  [[nodiscard]] static std::vector<Module> read_modules();

  /**
   * Parses the symbol table for the given module (path).
   *
   * @param module Module to lookup.
   * @return List of all symbols linked to the module.
   */
  [[nodiscard]] static std::vector<Symbol> parse_symbol_table(const SymbolResolver::Module& module);

  /**
   * Reads the process name from /proc/self/comm.
   * @return Name of the process, if it can be read.
   */
  [[nodiscard]] static std::optional<std::string> read_process_name();

private:
  /// List of all modules and linked symbols.
  std::unordered_map<Module, std::vector<Symbol>, ModuleHash> _modules;

  /// Cache for already resolved symbols.
  std::unordered_map<std::uintptr_t, SymbolResolver::ResolvedSymbol> _resolved_symbols;

  /**
   * Resolves the symbol within the given module (and symbols).
   *
   * @param module Module the instruction points to.
   * @param symbols Symbols of the module.
   * @param logical_instruction_pointer Instruction pointer to resolve.
   * @return Symbol if it can be resolved.
   */
  [[nodiscard]] static std::optional<ResolvedSymbol> resolve(const Module& module,
                                                             const std::vector<Symbol>& symbols,
                                                             std::uintptr_t logical_instruction_pointer) noexcept;

  /**
   * Demangles the given symbol name,
   *
   * @param symbol_name Symbol name.
   * @return Demangled symbol name.
   */
  [[nodiscard]] static std::string demangle_symbol_name(std::string&& symbol_name);

  /**
   * Scans the section header table for the symbol table and string table.
   *
   * @param section_header_table Pointer to the section header table.
   * @param size Size of the section header table.
   * @return Pointer pair (symbol table, string table).
   */
  [[nodiscard]] static std::pair<const Elf64_Shdr*, const Elf64_Shdr*> find_symbol_and_string_tables(
    const Elf64_Shdr* section_header_table,
    std::uint16_t size) noexcept;

  /**
   * Extracts symbols from a given symbol table section.
   *
   * @param elf_data Pointer to the memory-mapped ELF file.
   * @param symbol_table Pointer to the symbol table section header.
   * @param string_table Pointer to the string table section header.
   * @return Vector of extracted symbols.
   */
  [[nodiscard]] static std::vector<Symbol> extract_symbols_from_table(void* elf_data,
                                                                      const Elf64_Shdr* symbol_table,
                                                                      const Elf64_Shdr* string_table);

  /**
   * Extracts the build ID from an ELF file.
   *
   * @param path Path of the module.
   * @return Build ID as a vector of bytes, or empty if not found.
   */
  [[nodiscard]] static std::vector<std::uint8_t> extract_build_id(const std::string& path) noexcept;
};
}