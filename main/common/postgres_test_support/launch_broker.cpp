#include <spawn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>

extern char** environ;

namespace {

[[nodiscard]] int parse_control_descriptor(std::string_view text) noexcept {
    int descriptor = -1;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), descriptor);
    if (error != std::errc{} || end != text.data() + text.size()
        || descriptor <= STDERR_FILENO) {
        return -1;
    }
    return descriptor;
}

[[nodiscard]] bool wait_for_release(int descriptor) noexcept {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISFIFO(status.st_mode)) {
        static_cast<void>(::close(descriptor));
        return false;
    }
    char release = '\0';
    ssize_t received = -1;
    do {
        received = ::read(descriptor, &release, sizeof(release));
    } while (received < 0 && errno == EINTR);
    const bool closed = ::close(descriptor) == 0;
    return received == 1 && release == 'G' && closed;
}

[[noreturn]] void mirror_signal(int signal_number) noexcept {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    static_cast<void>(sigemptyset(&action.sa_mask));
    static_cast<void>(
        ::sigaction(signal_number, &action, nullptr));
    sigset_t unblocked;
    static_cast<void>(sigemptyset(&unblocked));
    static_cast<void>(sigaddset(&unblocked, signal_number));
    static_cast<void>(
        ::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr));
    static_cast<void>(::kill(::getpid(), signal_number));
    ::_exit(128 + signal_number);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        return 125;
    }
    const int control_descriptor =
        parse_control_descriptor(argv[1]);
    if (control_descriptor < 0) {
        return 125;
    }
    const pid_t broker_group = ::getpgrp();
    if (broker_group <= 1 || broker_group != ::getpid()) {
        static_cast<void>(::close(control_descriptor));
        return 125;
    }
    if (!wait_for_release(control_descriptor)) {
        return 125;
    }

    posix_spawnattr_t attributes{};
    int status = ::posix_spawnattr_init(&attributes);
    if (status != 0) {
        std::fprintf(
            stderr, "launch broker attribute initialization failed: %s\n",
            std::strerror(status));
        return 127;
    }
    status = ::posix_spawnattr_setflags(
        &attributes, POSIX_SPAWN_SETPGROUP);
    if (status == 0) {
        status = ::posix_spawnattr_setpgroup(
            &attributes, broker_group);
    }

    pid_t target = -1;
    if (status == 0) {
        status = ::posix_spawn(
            &target, argv[2], nullptr, &attributes,
            argv + 3, environ);
    }
    static_cast<void>(::posix_spawnattr_destroy(&attributes));
    if (status != 0) {
        std::fprintf(
            stderr, "launch broker posix_spawn failed for %s: %s\n",
            argv[2], std::strerror(status));
        return 127;
    }

    int target_status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(target, &target_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != target) {
        std::fprintf(
            stderr, "launch broker waitpid failed for %s: %s\n",
            argv[2], std::strerror(errno));
        return 126;
    }
    if (WIFEXITED(target_status)) {
        return WEXITSTATUS(target_status);
    }
    if (WIFSIGNALED(target_status)) {
        mirror_signal(WTERMSIG(target_status));
    }
    return 126;
}
