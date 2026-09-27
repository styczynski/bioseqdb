import bioseq.highway.gcc_canary;

extern "C" int printf(const char*, ...);

int main() {
  const auto result =
      bioseq::highway_gcc_canary::run_imported_binder_canary();
  if (!result.ok || result.first_target == result.second_target
      || result.first_name == nullptr || result.second_name == nullptr
      || result.checksum != 976U) {
    return 1;
  }
  printf(
      "bioseq-highway-gcc-canary: first=%s(0x%llx) "
      "second=%s(0x%llx) checksum=%u\n",
      result.first_name,
      static_cast<unsigned long long>(result.first_target),
      result.second_name,
      static_cast<unsigned long long>(result.second_target),
      static_cast<unsigned int>(result.checksum));
  return 0;
}
