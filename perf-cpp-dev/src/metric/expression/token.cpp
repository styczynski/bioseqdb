#include <perfcpp/metric/expression/token.hpp>

std::string
perf::metric::expression::Token::TokenToStringVisitor::operator()(
  const perf::metric::expression::Operator_ metric_operator) const
{
  switch (metric_operator) {
    case Operator_::Plus:
      return "+";
    case Operator_::Minus:
      return "-";
    case Operator_::Times:
      return "*";
    case Operator_::Divide:
      return "/";
    default:
      return "<unknown operator>";
  }
}

std::string
perf::metric::expression::Token::TokenToStringVisitor::operator()(
  const perf::metric::expression::Token::Punctuation punctutation) const
{
  switch (punctutation) {
    case Token::Punctuation::LeftParentheses:
      return "(";
    case Token::Punctuation::RightParentheses:
      return ")";
    case Token::Punctuation::Comma:
      return ",";
    default:
      return "<unknown parenthesis>";
  }
}

std::string
perf::metric::expression::Token::to_string() const
{
  return std::visit(TokenToStringVisitor{}, this->_token);
}