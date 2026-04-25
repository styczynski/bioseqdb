#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <perfcpp/metric/expression/token.hpp>
#include <string>
#include <utility>

namespace perf::metric::expression {
/**
 * The Tokenizer translates a given input string into a queue of tokens.
 */
class Tokenizer
{
public:
  explicit Tokenizer(std::string&& input)
    : _input(std::move(input))
  {
  }

  Tokenizer(const Tokenizer&) = delete;
  Tokenizer(Tokenizer&&) = delete;

  ~Tokenizer() = default;

  Tokenizer& operator=(const Tokenizer&) = delete;
  Tokenizer& operator=(Tokenizer&&) = delete;

  /**
   * @return The original input.
   */
  [[nodiscard]] const std::string& input() const noexcept { return _input; }

  /**
   * Tokenizes the input string.
   * @return Queue of tokens.
   */
  [[nodiscard]] std::optional<Token> next();

private:
  /// The expression to tokenize.
  const std::string _input;

  std::size_t _position{ 0U };

  /**
   * @return Position after skipping all whitespaces.
   */
  [[nodiscard]] std::size_t skip_whitespaces() const noexcept;

  /**
   * Reads a constant number (e.g., 13.37) from the input string, starting at the given position.
   *
   * @param begin Position within the input string.
   * @return A tuple (token containing the constant, new position).
   */
  [[nodiscard]] std::pair<double, std::size_t> read_constant(std::size_t begin) const;

  /**
   * Reads an identifier (e.g., a hardware counter name) from the input string, starting at the given position.
   *
   * @param begin Position within the input string.
   * @return A tuple (token containing the identifier, new position).
   */
  [[nodiscard]] std::pair<std::string, std::size_t> read_identifier(std::size_t begin) const;

  /**
   * Reads an operator (e.g., +) from the given char.
   *
   * @param current_char Current char from input string.
   * @return The metric operator.
   */
  [[nodiscard]] Operator_ read_operator(char current_char) const;

  /**
   * Checks if the given char is an escape character.
   *
   * @param current_char Current character.
   * @return True, if the given char is an escape character.
   */
  [[nodiscard]] static bool is_escape_char(char current_char) noexcept
  {
    return current_char == '\'' || current_char == '`';
  }

  /**
   * Checks if the given char could belong to an identifier (alphanumerical chars, _, ., etc.).
   *
   * @param char_ Char to check.
   * @return True, if the char could belong to an identifier.
   */
  [[nodiscard]] static bool is_identifier_char(const char char_) noexcept
  {
    return (std::isalnum(char_) != 0) || char_ == '_' || char_ == '.';
  }

  /**
   * Checks if the given char is a scientific 'e'.
   *
   * @param char_ Char to check.
   * @return True, if the char could is a scientific 'e'.
   */
  [[nodiscard]] static bool is_scientific_e(const char char_) noexcept { return char_ == 'e' || char_ == 'E'; }
};
}