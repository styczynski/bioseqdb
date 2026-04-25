#include <algorithm>
#include <perfcpp/metric/expression/function.hpp>
#include <perfcpp/metric/expression/parser.hpp>

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::parse()
{
  auto expression = this->parse_expression();
  if (this->_current_token.has_value()) {
    throw CannotParseMetricExpressionError{ this->_tokenizer.input() };
  }

  return expression;
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::parse_additive_expression()
{
  /// Parse the left of a binary expression.
  auto left_expression = this->parse_multiplicative_expression();

  while (this->_current_token.has_value() && this->_current_token->is_additive_operator()) {
    const auto operator_ = this->_current_token->operator_();

    /// Consume the operator token and move to the next token.
    this->consume();

    /// Parse the right of a binary expression.
    auto right_expression = this->parse_multiplicative_expression();

    /// Build the binary (additive) expression.
    switch (operator_) {
      case Operator_::Plus:
        left_expression = std::make_unique<AdditionExpression>(std::move(left_expression), std::move(right_expression));
        break;
      case Operator_::Minus:
        left_expression =
          std::make_unique<SubtractionExpression>(std::move(left_expression), std::move(right_expression));
        break;
      default:
        throw CannotParseMetricExpressionError{ this->_tokenizer.input() };
    }
  }

  return left_expression;
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::parse_multiplicative_expression()
{
  /// Parse the left of a binary expression.
  auto left_expression = this->parse_primary();

  while (this->_current_token.has_value() && this->_current_token->is_multiplicative_operator()) {
    const auto operator_ = this->_current_token->operator_();

    /// Consume the operator token and move to the next token.
    this->consume();

    /// Parse the right of a binary expression.
    auto right_expression = this->parse_primary();

    /// Build the binary (multiplicative) expression.
    switch (operator_) {
      case Operator_::Times:
        left_expression = std::make_unique<MultiplyExpression>(std::move(left_expression), std::move(right_expression));
        break;
      case Operator_::Divide:
        left_expression = std::make_unique<DivideExpression>(std::move(left_expression), std::move(right_expression));
        break;
      default:
        throw CannotParseMetricExpressionError{ this->_tokenizer.input() };
    }
  }

  return left_expression;
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::parse_primary()
{
  if (!this->_current_token.has_value()) {
    throw CannotParseMetricExpressionError{ this->_tokenizer.input() };
  }

  return std::visit(TokenVisitor{ *this }, this->_current_token->data());
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::build_function(std::string&& function_name,
                                                 std::vector<std::unique_ptr<ExpressionInterface>>&& arguments) const
{
  /// Transform the function name into lower characters.
  std::transform(function_name.begin(), function_name.end(), function_name.begin(), [](const auto character) {
    return std::tolower(character);
  });

  /// Turn the function name and arguments into a function expression.
  if (function_name == "d_ratio" || function_name == "ratio") {
    if (arguments.size() == 2U) {
      return std::make_unique<DRatioFunction>(std::move(arguments[0U]), std::move(arguments[1U]));
    }

    throw CannotParseMetricExpressionUnexpectedFunctionArgumentsError{
      this->_tokenizer.input(), function_name, 2U, arguments.size()
    };
  }

  /// Turn the function name and arguments into a function expression.
  if (function_name == "sum") {
    if (arguments.size() >= 2U) {
      return std::make_unique<SumFunction>(std::move(arguments));
    }

    throw CannotParseMetricExpressionUnexpectedFunctionArgumentsError{
      this->_tokenizer.input(), function_name, 2U, arguments.size()
    };
  }

  throw CannotParseMetricExpressionUnknownFunctionError{ this->_tokenizer.input(), function_name };
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::TokenVisitor::operator()(std::string& identifier)
{
  /// Move the identifier to a new variable since it is consumed next.
  auto name = std::move(identifier);

  /// Consume the identifier.
  this->_parser.consume();

  /// Check if the next token is a opening parenthesis which indicates a function call,
  if (this->_parser._current_token.has_value() && this->_parser._current_token->is_left_parenthesis()) {
    /// Consume the left parentheses.
    this->_parser.consume();

    auto arguments = std::vector<std::unique_ptr<ExpressionInterface>>{};

    /// Consume arguments if the parenthesis is not closed immediately.
    if (this->_parser._current_token.has_value() && !this->_parser._current_token->is_right_parenthesis()) {
      arguments.push_back(this->_parser.parse_expression());

      while (this->_parser._current_token.has_value() && this->_parser._current_token->is_comma()) {
        /// Consume the comma.
        this->_parser.consume();

        /// Parse the argument.
        auto argument = this->_parser.parse_expression();
        if (argument == nullptr) {
          throw CannotParseMetricExpressionError{ this->_parser._tokenizer.input() };
        }
        arguments.push_back(std::move(argument));
      }
    }

    this->_parser.consume(Token::Punctuation::RightParentheses);

    return this->_parser.build_function(std::move(name), std::move(arguments));
  }

  return std::make_unique<IdentifierExpression>(std::move(name));
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::TokenVisitor::operator()(const double constant)
{
  /// Consume the constant.
  this->_parser.consume();

  return std::make_unique<ConstantExpression>(constant);
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::TokenVisitor::operator()(const perf::metric::expression::Operator_ metric_operator)
{
  /// Handle an expression starting with "-".
  if (metric_operator == Operator_::Minus) {
    /// Consume the "-" operator.
    this->_parser.consume();

    /// Creating an expression subtracting the operand from zero.
    auto operand = this->_parser.parse_primary();
    return std::make_unique<SubtractionExpression>(std::make_unique<ConstantExpression>(0), std::move(operand));
  }

  /// Handle an expression starting with "+".
  if (metric_operator == Operator_::Plus) {
    /// Consume the "+" operator.
    this->_parser.consume();

    /// Just ignore the "+" operator.
    return this->_parser.parse_primary();
  }

  return nullptr;
}

std::unique_ptr<perf::metric::expression::ExpressionInterface>
perf::metric::expression::Parser::TokenVisitor::operator()(const Token::Punctuation punctutation)
{
  /// Handle an expression starting with "(".
  if (punctutation == Token::Punctuation::LeftParentheses) {
    /// Consume the left parentheses.
    this->_parser.consume();

    /// Parse the expression within parentheses.
    auto expression = this->_parser.parse_expression();

    /// The expression must end with a ")" – consume it (and verify).
    this->_parser.consume(Token::Punctuation::RightParentheses);

    return expression;
  }

  return nullptr;
}