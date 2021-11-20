#include <doctest/doctest.h>
#include <bioseq_lib/bioseq_lib.h>
#include <bioseq_lib/version.h>

#include <string>

TEST_CASE("BioSeqDBLib") {
  using namespace bioseq_lib;

  BioSeqDBLib bioseq_lib("Tests");

  CHECK(bioseq_lib.greet(LanguageCode::EN) == "Hello, Tests!");
  CHECK(bioseq_lib.greet(LanguageCode::DE) == "Hallo Tests!");
  CHECK(bioseq_lib.greet(LanguageCode::ES) == "¡Hola Tests!");
  CHECK(bioseq_lib.greet(LanguageCode::FR) == "Bonjour Tests!");
}

TEST_CASE("BioSeqDBLib version") {
  static_assert(std::string_view(GREETER_VERSION) == std::string_view("1.0"));
  CHECK(std::string(GREETER_VERSION) == std::string("1.0"));
}
