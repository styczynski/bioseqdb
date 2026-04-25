#include <numeric>
#include <perfcpp/exception.hpp>
#include <perfcpp/util/table.hpp>
#include <sstream>

void
perf::util::Table::add(std::vector<perf::util::Table::Header>&& header_row)
{
  const auto count_columns =
    std::accumulate(header_row.begin(), header_row.end(), 0U, [](const auto count, const auto& header) {
      return count + header.span();
    });

  if (!this->_count_columns.has_value()) {
    this->_count_columns = count_columns;
  } else if (count_columns != this->_count_columns.value()) {
    throw CannotAddHeaderToTable{ count_columns, this->_count_columns.value() };
  }

  _header_row.push_back(std::move(header_row));
}

void
perf::util::Table::add(perf::util::Table::Row&& row)
{
  const auto count_columns = row.columns().size();

  if (!this->_count_columns.has_value()) {
    this->_count_columns = count_columns;
  } else if (count_columns != this->_count_columns.value()) {
    throw CannotAddRowToTable{ count_columns, this->_count_columns.value() };
  }

  /// Add the row.
  this->_rows.push_back(std::move(row));
}

std::string
perf::util::Table::to_string() const
{
  if (!this->_count_columns.has_value()) {
    return "";
  }

  /// Calculate the max size for each column. Also take the latest header into account.
  auto column_max_sizes = std::vector<std::size_t>{};
  column_max_sizes.resize(this->_count_columns.value(), 0U);
  for (const auto& row : this->_rows) {
    for (auto column_id = 0UL; column_id < row.columns().size(); ++column_id) {
      column_max_sizes[column_id] = std::max(column_max_sizes[column_id], row.columns()[column_id].size());
    }
  }

  if (!this->_header_row.empty()) {
    auto column_id = 0U;
    for (const auto& header : this->_header_row.back()) {
      if (header.span() == 1U) {
        column_max_sizes[column_id] = std::max(column_max_sizes[column_id], header.text().size());
      }
      column_id += header.span();
    }
  }

  /// Calculate the separator by any of the headers having a separator. Plus, calculate the alignment by using any
  /// header that spans over only a single column.
  auto is_column_separator = std::vector<bool>(this->_count_columns.value(), false);
  auto column_alignment = std::vector<Alignment>(this->_count_columns.value(), Alignment::Left);
  for (const auto& header_row : this->_header_row) {
    auto column_id = 0U;
    for (const auto& header : header_row) {
      is_column_separator[column_id] = is_column_separator[column_id] || header.has_separator();

      if (header.span() == 1U) {
        column_alignment[column_id] = header.alignment();
      }

      column_id += header.span();
    }
  }

  auto stream = std::stringstream{};

  /// Print the headers.
  for (const auto& header_row : this->_header_row) {
    /// Add offset at the beginning of the row.
    if (this->_offset > 0U) {
      stream << std::string(this->_offset, ' ');
    }

    /// Print all columns.
    auto column_id = 0ULL;
    for (const auto& header : header_row) {
      /// Add a space left.
      if (column_id > 0ULL) {
        stream << " ";
      }

      /// Add left column separator.
      if (is_column_separator[column_id]) {
        stream << '|';
      }

      stream << " ";

      /// Content of "normal" columns that do not span over multiple columns.
      if (header.span() == 1U) {
        Table::print_text_aligned(stream, column_alignment[column_id], header.text(), column_max_sizes[column_id]);
      }

      /// Content of columns that span over multiple columns.
      else {
        /// Aggregate the size of this cell as the size of all spanned columns plus separators.
        auto span_columns_sizes = column_max_sizes[column_id];
        for (auto i = column_id + 1U; i < column_id + header.span(); ++i) {
          span_columns_sizes += /* size of the spanned column */ column_max_sizes[i] +
                                /* empty space for each column */ 2U +
                                /* separator, if present */ static_cast<std::size_t>(is_column_separator[i]);
        }

        /// Calculate empty spaces to the left and to the right.
        const auto total_spaces = span_columns_sizes - header.text().size();
        const auto left_spaces = total_spaces / 2U;
        const auto right_spaces = total_spaces - left_spaces;

        stream << std::string(left_spaces, ' ') << header.text() << std::string(right_spaces, ' ');
      }

      column_id += header.span();
    }

    stream << "\n";
  }

  /// Print the rows.
  for (const auto& row : this->_rows) {

    /// Add offset at the beginning of the row.
    if (this->_offset > 0U) {
      stream << std::string(this->_offset, ' ');
    }

    /// Print all columns.
    for (auto column_id = 0U; column_id < row.columns().size(); ++column_id) {
      /// Add a space left.
      if (column_id > 0ULL) {
        stream << " ";
      }

      /// Add left column separator.
      if (is_column_separator[column_id]) {
        stream << '|';
      }

      stream << " ";

      /// Content of "normal" columns that do not span over multiple columns.
      Table::print_text_aligned(
        stream, column_alignment[column_id], row.columns()[column_id], column_max_sizes[column_id]);
    }

    stream << "\n";
  }

  return stream.str();
}

void
perf::util::Table::print_text_aligned(std::stringstream& stream,
                                      perf::util::Table::Alignment alignment,
                                      const std::string& text,
                                      const std::size_t column_size)
{
  const auto excess_size = column_size - text.size();

  switch (alignment) {
    case Alignment::Left:
      /// Add content and empty spaces to the right..
      stream << text << std::string(excess_size, ' ');
      break;
    case Alignment::Right:
      /// Add empty spaces to the left and content.
      stream << std::string(excess_size, ' ') << text;
      break;
    case Alignment::Center:
      /// Calculate the number of empty spaces to the left and to the right.
      const auto left_spaces = excess_size / 2U;
      const auto right_spaces = excess_size - left_spaces;

      /// Add empty spaces to the left, content, and empty spaces to the right.
      stream << std::string(left_spaces, ' ') << text << std::string(right_spaces, ' ');
      break;
  }
}