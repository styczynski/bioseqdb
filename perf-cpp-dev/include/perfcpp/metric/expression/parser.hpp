#pragma once

#include <memory>
#include <perfcpp/exception.hpp>
#include <perfcpp/metric/expression/expression.hpp>
#include <perfcpp/metric/expression/tokenizer.hpp>
#include <string>

namespace perf::metric::expression {
/**
 * The parser translates an expression (string) to an executable metric expression.
 */
class Parser
{
  class TokenVisitor;
  friend TokenVisitor;

public:
  explicit Parser(std::string&& input)
    : _tokenizer(std::move(input))
    , _current_token(_tokenizer.next())
  {
  }

  Parser(const Parser&) = delete;
  Parser(Parser&&) = delete;

  ~Parser() = default;

  Parser& operator=(const Parser&) = delete;
  Parser& operator=(Parser&&) = delete;

  /**
   * Builds an evaluable expression from the given expression-string.
   *
   * @return Evaluable expression.
   */
  [[nodiscard]] std::unique_ptr<ExpressionInterface> parse();

private:
  Tokenizer _tokenizer;

  std::optional<Token> _current_token;

  /**
   * Consume the current token by moving to the next one.
   */
  void consume() { this->_current_token = this->_tokenizer.next(); }

  /**
   * Verifies that the current token is equal to the expected punctuation. Throws an exception otherwise.
   * If the current token is equal to the expected punctuation, the current token is consumed.
   *
   * @param expected_punctuation Expected punctuation.
   */
  void consume(const Token::Punctuation expected_punctuation)
  {
    if (!(this->_current_token.has_value() && this->_current_token.value() == expected_punctuation)) {
      throw CannotParseMetricExpressionError{ this->_tokenizer.input() };
    }

    consume();
  }

  /**
   * Parses the input and returns a parsed expression.
   *
   * @return The parsed expression.
   */
  [[nodiscard]] std::unique_ptr<ExpressionInterface> parse_expression() { return this->parse_additive_expression(); }

  /**
   * Parses an additive expression (with "+" and "-" operations).
   *
   * @return The parsed expression.
   */
  [[nodiscard]] std::unique_ptr<ExpressionInterface> parse_additive_expression();

  /**
   * Parses a multiplicative expression (with "*" and "/" operations).
   *
   * @return The parsed expression.
   */
  [[nodiscard]] std::unique_ptr<ExpressionInterface> parse_multiplicative_expression();

  /**
   * Parse numbers, identifiers, functions, and parenthesized expressions.
   *
   * @return The parsed expression.
   */
  [[nodiscard]] std::unique_ptr<ExpressionInterface> parse_primary();

  /**
   * Creates a function with the given name and arguments.
   *
   * @param function_name Name of the function.
   * @param arguments Arguments of the function.
   * @return Function expression.
   */
  [[nodiscard]] std::unique_ptr<ExpressionInterface> build_function(
    std::string&& function_name,
    std::vector<std::unique_ptr<ExpressionInterface>>&& arguments) const;

  /**
   * Visits a token and translates it into an expression, called by the parse_primary() function.
   * This may include recursive calls for parenthesized expressions.
   */
  class TokenVisitor
  {
  public:
    explicit TokenVisitor(Parser& parser) noexcept
      : _parser(parser)
    {
    }

    TokenVisitor(const TokenVisitor&) = delete;
    TokenVisitor(TokenVisitor&&) = delete;

    ~TokenVisitor() noexcept = default;

    TokenVisitor& operator=(const TokenVisitor&) = delete;
    TokenVisitor& operator=(TokenVisitor&&) = delete;

    [[nodiscard]] std::unique_ptr<ExpressionInterface> operator()(std::string& identifier);
    [[nodiscard]] std::unique_ptr<ExpressionInterface> operator()(double constant);
    [[nodiscard]] std::unique_ptr<ExpressionInterface> operator()(Operator_ metric_operator);
    [[nodiscard]] std::unique_ptr<ExpressionInterface> operator()(Token::Punctuation punctutation);

  private:
    /// The calling parser to consume tokens and continue parsing.
    Parser& _parser;
  };
};
}