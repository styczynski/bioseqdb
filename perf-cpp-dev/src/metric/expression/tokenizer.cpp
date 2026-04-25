#include <perfcpp/exception.hpp>
#include <perfcpp/metric/expression/tokenizer.hpp>

std::optional<perf::metric::expression::Token>
perf::metric::expression::Tokenizer::next()
{
  this->_position = this->skip_whitespaces();

  /// Check if reached the end.
  if (this->_position >= this->_input.size()) {
    return std::nullopt;
  }

  const auto current_character = this->_input[this->_position];

  /// Read binary operator.
  if (current_character == '+' || current_character == '-' || current_character == '*' || current_character == '/') {
    const auto operator_ = this->read_operator(current_character);
    ++this->_position;
    return Token{ operator_ };
  }

  /// Read left parentheses.
  if (current_character == '(') {
    ++this->_position;
    return Token{ Token::Punctuation::LeftParentheses };
  }

  /// Read right parentheses.
  if (current_character == ')') {
    ++this->_position;
    return Token{ Token::Punctuation::RightParentheses };
  }

  /// Read left parentheses.
  if (current_character == ',') {
    ++this->_position;
    return Token{ Token::Punctuation::Comma };
  }

  /// Check if the next character is a constant number (obviously a digit indicates a number – and so does a ".").
  /// If so, return a number token.
  if (std::isdigit(current_character) != 0 || current_character == '.') {
    auto [constant, new_position] = Tokenizer::read_constant(this->_position);

    /// The constant tokenizer calculates the next position.
    this->_position = new_position;

    return Token{ constant };
  }

  /// Check if the next character is an alphabetical char, which indicates an identifier.
  /// Additionally, identifiers can start with single quotes to escape, for example, - operators as part of the
  /// identifier (e.g., the hardware counter "L1-cache-miss"). If so, return an identifier token.
  if (std::isalpha(current_character) != 0 || Tokenizer::is_escape_char(current_character)) {
    auto [identifier, new_position] = Tokenizer::read_identifier(this->_position);

    /// The identifier tokenizer calculates the next position.
    this->_position = new_position;

    return Token{ std::move(identifier) };
  }

  throw CannotParseMetricExpressionError{ this->_input };
}

std::size_t
perf::metric::expression::Tokenizer::skip_whitespaces() const noexcept
{
  auto position = this->_position;

  while (position < this->_input.length() && std::isspace(this->_input[position]) != 0) {
    ++position;
  }

  return position;
}

std::pair<double, std::size_t>
perf::metric::expression::Tokenizer::read_constant(const std::size_t begin) const
{
  /// We know that the current character (this->_position) is a digit; otherwise, this function wouldn't have been
  /// called.
  auto position = begin + 1ULL;

  /// Read all digits.
  while (position < this->_input.length() && std::isdigit(this->_input[position]) != 0) {
    ++position;
  }

  /// When hitting a (decimal) point, continue reading.
  if (position < this->_input.length() && this->_input[position] == '.') {
    ++position;

    /// Again, read all digits after the decimal point.
    while (position < this->_input.length() && std::isdigit(this->_input[position]) != 0) {
      ++position;
    }
  }

  /// When hitting a scientific e, continue reading.
  if (position < this->_input.length() && Tokenizer::is_scientific_e(this->_input[position])) {
    ++position;

    /// Include possible +/- sign after scientific e.
    position += static_cast<std::size_t>(position < this->_input.length() &&
                                         (this->_input[position] == '+' || this->_input[position] == '-'));

    /// Again, read all digits after the scientific e.
    while (position < this->_input.length() && std::isdigit(this->_input[position]) != 0) {
      ++position;
    }
  }

  /// Extract the number.
  const auto number = this->_input.substr(begin, position - begin);
  try {
    /// Parse the number to decimal.
    return std::make_pair(std::stod(number), position);
  } catch (std::invalid_argument& e) {
    throw CannotParseMetricExpressionError{ this->_input,
                                            std::string{ "Cannot parse number (" }.append(e.what()).append(")") };
  }
}

std::pair<std::string, std::size_t>
perf::metric::expression::Tokenizer::read_identifier(const std::size_t begin) const
{
  /// We know that the current character (this->_position) is alphabetical; otherwise, this function wouldn't have been
  /// called.
  auto count = 1ULL;

  /// If the identifier starts with a single quote, we scan until we find the "ending" single quote.
  const auto is_start_with_escape_char = Tokenizer::is_escape_char(this->_input[begin]);
  if (is_start_with_escape_char) {
    while ((begin + count) < this->_input.size() && !Tokenizer::is_escape_char(this->_input[begin + count])) {
      ++count;
    }

    /// If we reached the end of the string, the quote was never closed.
    if ((begin + count) == this->_input.size()) {
      throw CannotParseMetricExpressionError{ this->_input, "Open quote was never closed" };
    }
  }

  /// Otherwise, we read all characters that are alphabetical or numerical(e.g., L2Cache).
  else {
    while ((begin + count) < this->_input.size() && Tokenizer::is_identifier_char(this->_input[begin + count])) {
      ++count;
    }
  }

  /// Increase the position by the number of scanned chars; skip the closing single quite if given.
  const auto new_position = begin + count + static_cast<std::uint64_t>(is_start_with_escape_char);

  /// Return the identifier; remove single quotes if given.
  auto identifier = this->_input.substr(begin + static_cast<std::uint64_t>(is_start_with_escape_char),
                                        count - static_cast<std::uint64_t>(is_start_with_escape_char));
  return std::make_pair(std::move(identifier), new_position);
}

perf::metric::expression::Operator_
perf::metric::expression::Tokenizer::read_operator(const char current_char) const
{
  /// Translate the given char into an operator, if it is one.
  switch (current_char) {
    case '+':
      return Operator_::Plus;
    case '-':
      return Operator_::Minus;
    case '*':
      return Operator_::Times;
    case '/':
      return Operator_::Divide;

      /// We could not tokenize a number, a sequence of chars. or an operator. This is an error.
    default:
      throw CannotParseMetricExpressionError{ this->_input, "Unknown operator token" };
  }
}