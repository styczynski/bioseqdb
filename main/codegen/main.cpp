#include <span>
#include <string_view>
#include <vector>

import bioseq.codegen;

int main(int argc, char** argv) {
    std::vector<std::string_view> a;
    a.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        a.emplace_back(argv[i]);
    }
    return bioseq::codegen::run(a);
}
