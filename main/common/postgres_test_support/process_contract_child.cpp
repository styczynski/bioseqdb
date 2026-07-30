#include <cstdio>
#include <cstdlib>

namespace {

[[nodiscard]] const char* environment_value(const char* name) noexcept {
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return "<unset>";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 1 || argv[0] == nullptr) {
        return 125;
    }
    std::printf("argv0=%s\n", argv[0]);
    std::printf("PATH=%s\n", environment_value("PATH"));
    std::printf("PGHOST=%s\n", environment_value("PGHOST"));
    std::printf("PGDATA=%s\n", environment_value("PGDATA"));
    return std::ferror(stdout) == 0 ? 0 : 126;
}
