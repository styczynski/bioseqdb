#include "benchmark/bar_chart.hpp"
#include "benchmark/benchmark.hpp"
#include "benchmark/diagnostics.hpp"
#include "benchmark/file.hpp"

int main()
{
   bencher::stage stage{"stage_name"};
   stage.run("Rocket", [] {
      double x{};
      for (size_t i = 0; i < 100000; ++i) {
         if (i % 31 == 0 && i % 13 == 0) {
            x += std::sin(double(i));
            bencher::do_not_optimize(x);
         }
      }
      return 100000;
   });

   stage.run("Aircraft", [] {
      double x{};
      for (size_t i = 0; i < 100000; ++i) {
         if (i % 31 == 0 && i % 13 == 0) {
            x += std::sin(double(i));
            bencher::do_not_optimize(x);
         }
      }
      return 100000;
   });

   stage.run("Truck", [] {
      double x{};
      for (size_t i = 0; i < 100000; ++i) {
         x += std::sin(double(i));
         bencher::do_not_optimize(x);
      }
      return 100000;
   });

   bencher::print_results(stage);

   bencher::save_file(bencher::to_markdown(stage), "results.md");

   bencher::save_file(bencher::bar_chart(stage), "results.svg");

   return 0;
}
