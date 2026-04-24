#pragma once

#if __has_include(<glaze/glaze.hpp>)

#include <glaze/glaze.hpp>

#include "bencher/bencher.hpp"

namespace bencher
{
   struct stage_result
   {
      std::string name{};
      std::vector<performance_metrics> results{};
   };

   [[nodiscard]] inline std::string to_json(const stage& s)
   {
      stage_result output{s.name, s.results};
      return glz::write_json(output).value_or("{}");
   }

   [[nodiscard]] inline std::string to_json_pretty(const stage& s)
   {
      stage_result output{s.name, s.results};
      return glz::write<glz::opts{.prettify = true}>(output).value_or("{}");
   }
}

#endif
