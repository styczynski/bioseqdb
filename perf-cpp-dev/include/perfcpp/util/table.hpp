#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace perf::util {
class Table
{
public:
  enum class Alignment : std::uint8_t
  {
    Left,
    Center,
    Right
  };

  class Header
  {
  public:
    Header(std::string&& text, const std::uint8_t span, const bool has_separator) noexcept
      : _text(std::move(text))
      , _span(span)
      , _has_separator(has_separator)
    {
    }
    explicit Header(std::string&& text,
                    const Alignment alignment = Alignment::Right,
                    const bool has_separator = false) noexcept
      : _text(std::move(text))
      , _alignment(alignment)
      , _has_separator(has_separator)
    {
    }
    Header(const Header&) = default;
    Header(Header&&) noexcept = default;
    ~Header() = default;
    Header& operator=(const Header&) = default;
    Header& operator=(Header&&) noexcept = default;

    [[nodiscard]] const std::string& text() const noexcept { return _text; }
    [[nodiscard]] Alignment alignment() const noexcept { return _alignment; }
    [[nodiscard]] std::uint8_t span() const noexcept { return _span; }
    [[nodiscard]] bool has_separator() const noexcept { return _has_separator; }

  private:
    std::string _text;
    Alignment _alignment{ Alignment::Left };
    std::uint8_t _span{ 1U };
    bool _has_separator;
  };

  class Row
  {
  public:
    Row() { _columns.reserve(32U); }
    Row(const Row&) = default;
    Row(Row&&) noexcept = default;
    ~Row() = default;
    Row& operator=(const Row&) = default;
    Row& operator=(Row&&) noexcept = default;

    void add(std::string&& column) { _columns.push_back(std::move(column)); }

    Row& operator<<(std::string&& column)
    {
      _columns.emplace_back(std::move(column));
      return *this;
    }

    Row& operator<<(const std::string& column)
    {
      _columns.emplace_back(column);
      return *this;
    }

    Row& operator<<(const std::size_t column)
    {
      _columns.emplace_back(std::to_string(column));
      return *this;
    }

    Row& operator<<(const std::uint32_t column)
    {
      _columns.emplace_back(std::to_string(column));
      return *this;
    }

    Row& operator<<(const std::uint16_t column)
    {
      _columns.emplace_back(std::to_string(column));
      return *this;
    }

    Row& operator<<(const std::uint8_t column)
    {
      _columns.emplace_back(std::to_string(column));
      return *this;
    }

    Row& operator<<(const float column)
    {
      _columns.emplace_back(std::to_string(column));
      return *this;
    }

    Row& operator<<(const double column)
    {
      _columns.emplace_back(std::to_string(column));
      return *this;
    }

    [[nodiscard]] const std::vector<std::string>& columns() const noexcept { return _columns; }
    [[nodiscard]] std::vector<std::string>& columns() noexcept { return _columns; }

  private:
    std::vector<std::string> _columns;
  };

  Table() = default;
  explicit Table(const std::uint64_t offset)
    : _offset(offset)
  {
  }
  Table(const Table&) = default;
  Table(Table&&) noexcept = default;
  ~Table() = default;
  Table& operator=(const Table&) = default;
  Table& operator=(Table&&) noexcept = default;

  /**
   * Reserve space for the given number of rows.
   * @param count_rows Number of rows to reserve.
   */
  void reserve(const std::size_t count_rows) { _rows.reserve(count_rows); }

  /**
   * Adds a header row to the table.
   * @param header_row Header row to add.
   */
  void add(std::vector<Header>&& header_row);

  /**
   * Adds the given row to the table.
   * @param row Row to add.
   */
  void add(Row&& row);

  /**
   * @return Turns the table into a printable string.
   */
  [[nodiscard]] std::string to_string() const;

private:
  std::optional<std::uint16_t> _count_columns{ std::nullopt };

  /// Headers
  std::vector<std::vector<Header>> _header_row;

  /// Rows
  std::vector<Row> _rows;

  /// Offset of each row in number of empty spaces.
  std::uint64_t _offset{ 0U };

  void static print_text_aligned(std::stringstream& stream,
                                 Alignment alignment,
                                 const std::string& text,
                                 std::size_t column_size);
};
}