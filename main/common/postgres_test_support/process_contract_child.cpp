#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <unistd.h>

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
    if (argc == 4 && argv[1] != nullptr && argv[2] != nullptr
        && argv[3] != nullptr
        && std::string_view{argv[2]} == "-D"
        && (std::string_view{argv[1]} == "--hold"
            || std::string_view{argv[1]} == "--postmaster")) {
        if (std::string_view{argv[1]} == "--postmaster") {
            std::ofstream pidfile(
                std::filesystem::path{argv[3]} / "postmaster.pid",
                std::ios::trunc);
            pidfile << ::getpid() << '\n' << argv[3] << "\n1\n";
            pidfile.close();
            if (!pidfile) {
                return 126;
            }
        }
        if (std::fflush(stdout) != 0) {
            return 126;
        }
        for (;;) {
            static_cast<void>(::pause());
        }
    }
    return std::ferror(stdout) == 0 ? 0 : 126;
}
