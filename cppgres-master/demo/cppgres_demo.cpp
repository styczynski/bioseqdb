#include <cppgres.hpp>

extern "C" {
PG_MODULE_MAGIC;
}

postgres_function(demo_len, ([](std::string_view t) { return t.length(); }));


#if __has_include(<generator>)
#include <generator>
#endif
#if !defined(__cpp_lib_generator) || (__cpp_lib_generator < 202107L)
#include "__generator.hpp"
#endif

std::generator<int64_t> prime_generator(std::size_t count) {
  // Yield the first prime.
  if (count >= 1) {
    co_yield 2;
  }

  int counter = 1;
  // Check for primes among odd numbers starting from 3.
  for (unsigned int num = 3; counter < count; num += 2) {
    bool isPrime = true;
    unsigned int limit = static_cast<unsigned int>(std::sqrt(num));
    for (unsigned int i = 2; i <= limit; ++i) {
      if (num % i == 0) {
        isPrime = false;
        break;
      }
    }
    if (isPrime) {
      co_yield num;
      counter++;
    }
  }
}

postgres_function(demo_srf, ([](int64_t t) {
                    return prime_generator(t);
                  }));
