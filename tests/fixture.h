// A private dbus-daemon per test, the backend serving on it, and a separate
// client connection: the tests never touch the user's session bus. The
// daemon dies with the test process (PR_SET_PDEATHSIG), even on REQUIRE's exit.
#pragma once

#include "check.h"
#include "broportal/backend.h"

#include <csignal>
#include <cstdio>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bstest {

struct PrivateBus {
    pid_t pid = -1;
    std::string address;

    PrivateBus() {
        int fds[2];
        if (pipe(fds) != 0) return;
        pid = fork();
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
            execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1",
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        close(fds[1]);
        char buf[512];
        ssize_t n = read(fds[0], buf, sizeof buf - 1);
        close(fds[0]);
        if (n > 0) {
            buf[n] = 0;
            address = buf;
            while (!address.empty() && (address.back() == '\n' || address.back() == ' ')) address.pop_back();
        }
    }
    ~PrivateBus() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
    PrivateBus(const PrivateBus&) = delete;
    PrivateBus& operator=(const PrivateBus&) = delete;
    bool ok() const { return !address.empty(); }
};

// Backend started and dispatching on its own thread, plus a client connection.
struct PortalFixture {
    PrivateBus bus;
    broportal::BackendConfig config;
    std::unique_ptr<broportal::PortalBackend> backend;
    std::unique_ptr<broportal::dbus::Bus> client;

    explicit PortalFixture(const char* test_name, bool start = true) {
        if (!bus.ok()) skip(test_name, "dbus-daemon could not be started (is it installed?)");
        std::string err;
        backend = broportal::PortalBackend::create_on_address(bus.address, config, &err);
        if (!backend) {
            fail(__FILE__, __LINE__, "create_on_address: " + err);
            std::exit(1);
        }
        client = broportal::dbus::Bus::open_address(bus.address, &err);
        if (!client) {
            fail(__FILE__, __LINE__, "client open_address: " + err);
            std::exit(1);
        }
        if (start) this->start();
    }

    void start() {
        std::string err;
        if (!backend->start(&err)) {
            fail(__FILE__, __LINE__, "backend start: " + err);
            std::exit(1);
        }
        if (!backend->run_in_background()) {
            fail(__FILE__, __LINE__, "run_in_background failed");
            std::exit(1);
        }
    }

    ~PortalFixture() {
        if (backend) {
            backend->stop_background();
            backend->stop();
        }
    }

    // Calls a method on the backend's object, as xdg-desktop-portal would.
    bool call(const std::string& iface, const std::string& member,
              std::function<void(broportal::dbus::Message&)> args,
              std::function<void(broportal::dbus::Message&)> reply, std::string* err = nullptr) {
        std::string e;
        bool ok = client->call_method(config.bus_name, config.object_path, iface, member, std::move(args),
                                      std::move(reply), &e);
        if (err) *err = e;
        return ok;
    }

    // A portal-style call answering (u response, a{sv} results).
    struct Reply {
        bool called = false;
        uint32_t code = 999;
        broportal::VariantMap results;
        std::string error;
    };
    Reply request(const std::string& iface, const std::string& member,
                  std::function<void(broportal::dbus::Message&)> args) {
        Reply r;
        r.called = call(
            iface, member, std::move(args),
            [&](broportal::dbus::Message& reply) {
                reply.read_uint32(&r.code);
                reply.read_variant_map(&r.results);
            },
            &r.error);
        if (!r.called) std::fprintf(stderr, "%s.%s: %s\n", iface.c_str(), member.c_str(), r.error.c_str());
        return r;
    }

    // gdbus as an independent client: the same call, encoded by GLib from its
    // text form. Returns gdbus's stdout ("" when it failed).
    std::string gdbus(const std::string& iface_method, const std::string& args) const {
        std::string cmd = "gdbus call --address '" + bus.address + "' --dest " + config.bus_name +
                          " --object-path " + config.object_path + " --method " + iface_method + " " + args +
                          " 2>&1";
        std::string out;
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return out;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        int st = pclose(p);
        if (st != 0) {
            std::fprintf(stderr, "gdbus failed: %s\n", out.c_str());
            return {};
        }
        return out;
    }
};

inline bool have_gdbus() {
    return std::system("command -v gdbus >/dev/null 2>&1") == 0;
}

struct CmdResult {
    int status = -1;
    std::string out;
};

// Runs `sh -c cmd` while dispatching `server` on this thread, so a blocking
// client call (gdbus, busctl) can be answered by a connection that only this
// thread may touch. Returns the command's exit status and stdout+stderr.
inline CmdResult run_while_dispatching(broportal::dbus::Bus& server, const std::string& cmd,
                                       std::chrono::seconds timeout = std::chrono::seconds(10)) {
    CmdResult res;
    int fds[2];
    if (pipe(fds) != 0) return res;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool done = false;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        server.wait(10000);
        while (server.process() > 0) {
        }
        done = waitpid(pid, &status, WNOHANG) == pid;
    }
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) res.out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    res.status = (done && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return res;
}

}  // namespace bstest
