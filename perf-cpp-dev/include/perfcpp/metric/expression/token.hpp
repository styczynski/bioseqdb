#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace perf::metric::expression {
/**
 * Representation of the supported operators.
 */
enum class Operator_ : std::uint8_t
{
  Plus,
  Minus,
  Times,
  Divide
};

/**
 * A token represents a single constant, identifier, operator (like +,-, etc.), or punctutations.
 */
class Token
{
public:
  enum class Punctuation : std::uint8_t
  {
    LeftParentheses,
    RightParentheses,
    Comma,
  };

  /**
   * Token can be an identifier, a constant number, an operator, or a punctutation.
   */
  using token_t = std::variant<std::string, double, Operator_, Punctuation>;

  Token(const Token&) = default;
  Token(Token&&) noexcept = default;

  explicit Token(const Operator_ operator_)
    : _token(operator_)
  {
  }

  explicit Token(const double number)
    : _token(number)
  {
  }

  explicit Token(std::string&& text)
    : _token(std::move(text))
  {
  }

  explicit Token(const Punctuation punctutation)
    : _token(punctutation)
  {
  }

  ~Token() = default;

  Token& operator=(const Token&) noexcept = default;
  Token& operator=(Token&&) noexcept = default;

  /**
   * @return True, if this token is a left parenthesis.
   */
  [[nodiscard]] bool is_left_parenthesis() const
  {
    return std::holds_alternative<Punctuation>(_token) && std::get<Punctuation>(_token) == Punctuation::LeftParentheses;
  }

  /**
   * @return True, if this token is a right parenthesis.
   */
  [[nodiscard]] bool is_right_parenthesis() const
  {
    return std::holds_alternative<Punctuation>(_token) &&
           std::get<Punctuation>(_token) == Punctuation::RightParentheses;
  }

  /**
   * @return True, if this token is a comma.
   */
  [[nodiscard]] bool is_comma() const
  {
    return std::holds_alternative<Punctuation>(_token) && std::get<Punctuation>(_token) == Punctuation::Comma;
  }

  /**
   * @return True, if this token is an additive operator.
   */
  [[nodiscard]] bool is_additive_operator() const
  {
    return std::holds_alternative<Operator_>(_token) &&
           (std::get<Operator_>(_token) == Operator_::Plus || std::get<Operator_>(_token) == Operator_::Minus);
  }

  /**
   * @return True, if this token is a multiplicative operator.
   */
  [[nodiscard]] bool is_multiplicative_operator() const
  {
    return std::holds_alternative<Operator_>(_token) &&
           (std::get<Operator_>(_token) == Operator_::Divide || std::get<Operator_>(_token) == Operator_::Times);
  }

  /**
   * @return The operator inside the token.
   */
  [[nodiscard]] Operator_ operator_() const { return std::get<Operator_>(_token); }

  /**
   * @return Ownership of the underlying token data.
   */
  [[nodiscard]] token_t& data() noexcept { return _token; }

  [[nodiscard]] bool operator==(const Punctuation punctuation) const
  {
    return std::holds_alternative<Punctuation>(_token) && std::get<Punctuation>(_token) == punctuation;
  }

  /**
   * @return A text representation of this token.
   */
  [[nodiscard]] std::string to_string() const;

private:
  token_t _token;

  /**
   * Visits a token and translates it into an std::string.
   */
  class TokenToStringVisitor
  {
  public:
    [[nodiscard]] std::string operator()(const std::string& identifier) const { return identifier; }

    [[nodiscard]] std::string operator()(const double constant) const { return std::to_string(constant); }

    [[nodiscard]] std::string operator()(Operator_ metric_operator) const;

    [[nodiscard]] std::string operator()(Token::Punctuation punctutation) const;
  };
};
}