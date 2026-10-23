// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// discover_nadia_shape against a scripted fake device. The two designs
// mirror what real units serve: the RZ's loaded design (2026-09) answers
// `level` on populated strips and leaves `address` empty, while the
// pre-design behavior populated `address` — discovery must count either.

#include "statusbar/controlplane_devices/nadia_discover.hpp"

#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar::controlplane_devices;

namespace {

/// One fake CASL text port: answers `get <family...> <lo>-<hi> <leaf>`
/// with populated values for indices <= the design's count for
/// (family, leaf), and empty values beyond — like the real device.
class FakeNadia
{
  public:
    /// design maps "family leaf" (e.g. "output level") to the populated
    /// count; unlisted pairs answer empty for every index.
    explicit FakeNadia(std::map<std::string, int> design)
        : design_(std::move(design))
    {}

    bool start()
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 1) != 0) {
            return false;
        }
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    ~FakeNadia()
    {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        if (conn_fd_ >= 0) {
            ::shutdown(conn_fd_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
        if (conn_fd_ >= 0) {
            ::close(conn_fd_);
        }
    }

    std::uint16_t port = 0;

  private:
    void serve()
    {
        conn_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (conn_fd_ < 0) {
            return;
        }
        std::string pending;
        char buf[4096];
        for (;;) {
            auto n = ::read(conn_fd_, buf, sizeof buf);
            if (n <= 0) {
                return;
            }
            pending.append(buf, std::size_t(n));
            std::size_t at = 0;
            for (;;) {
                auto nl = pending.find('\n', at);
                if (nl == std::string::npos) {
                    break;
                }
                handle(pending.substr(at, nl - at));
                at = nl + 1;
            }
            pending.erase(0, at);
        }
    }

    void handle(std::string const& line)
    {
        // get <family words...> <lo>-<hi> <leaf>
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        if (verb != "get") {
            return;
        }
        std::vector<std::string> words;
        std::string w;
        while (in >> w) {
            words.push_back(w);
        }
        if (words.size() < 3) {
            return;
        }
        // The range token sits anywhere in the command ("vras 1-16 reverb
        // duration"): family = the words before it, leaf = the words after.
        int lo = 0;
        int hi = 0;
        std::size_t range_at = words.size();
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (std::sscanf(words[i].c_str(), "%d-%d", &lo, &hi) == 2 && words[i].find('-') != std::string::npos) {
                range_at = i;
                break;
            }
        }
        if (range_at == words.size()) {
            return;
        }
        std::string family;
        for (std::size_t i = 0; i < range_at; ++i) {
            family += (i ? " " : "") + words[i];
        }
        std::string leaf;
        for (std::size_t i = range_at + 1; i < words.size(); ++i) {
            leaf += (i > range_at + 1 ? " " : "") + words[i];
        }
        auto it = design_.find(family + " " + leaf);
        int const populated = it == design_.end() ? 0 : it->second;
        std::string out;
        for (int i = lo; i <= hi; ++i) {
            out += "Got (req-1): X " + std::to_string(i) + " Leaf =";
            if (i <= populated) {
                out += " 1.0";
            }
            out += "\r\n";
        }
        (void)!::write(conn_fd_, out.data(), out.size());
    }

    std::map<std::string, int> design_;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    std::thread thread_;
};

NadiaShape discover(FakeNadia& fake)
{
    GalaxyClient client;
    EXPECT_TRUE(client.connect("127.0.0.1", fake.port));
    return discover_nadia_shape(client, 8, nullptr);
}

}  // namespace

TEST(nadia_discover, counts_level_populated_design)
{
    // The RZ's loaded design: outputs and vgroups answer `level`,
    // `address` is empty everywhere, EQ bands live on the outputs.
    FakeNadia fake({
        {"output level", 3},
        {"vgroup level", 2},
        {"output 1 parametric frequency", 4},
    });
    EXPECT_TRUE(fake.start());
    auto shape = discover(fake);
    EXPECT_EQ(shape.inputs, 0u);
    EXPECT_EQ(shape.buses, 0u);
    EXPECT_EQ(shape.outputs, 3u);
    EXPECT_EQ(shape.listens, 0u);
    EXPECT_EQ(shape.vgroups, 2u);
    EXPECT_EQ(shape.eq_bands, 4u);
}

TEST(nadia_discover, counts_design_level_families)
{
    FakeNadia fake({
        {"output level", 2},
        {"output 1 parametric frequency", 3},
        {"vras reverb duration", 2},
        {"vras 1 input level", 3},
        {"vras 1 return level", 2},
        {"vras 1 earlyreflection level", 4},
        {"monitor isolate", 5},
        {"agc threshold", 1},
    });
    EXPECT_TRUE(fake.start());
    auto shape = discover(fake);
    EXPECT_EQ(shape.outputs, 2u);
    EXPECT_EQ(shape.vras, 2u);
    EXPECT_EQ(shape.vras_inputs, 3u);
    EXPECT_EQ(shape.vras_returns, 2u);
    EXPECT_EQ(shape.vras_outputs, 0u);  // opt-in only
    EXPECT_EQ(shape.reflections, 4u);
    EXPECT_EQ(shape.monitors, 5u);
    EXPECT_EQ(shape.agcs, 1u);
    EXPECT_FALSE(shape.master);  // fake serves no unindexed master address
}

TEST(nadia_discover, parses_design_level_shape_keys)
{
    NadiaShape shape;
    EXPECT_TRUE(parse_nadia_shape(
        "outputs=128,vras=12,vras_inputs=16,vras_returns=16,vras_outputs=8,reflections=16,monitors=24,agcs=16,master=1", shape));
    EXPECT_EQ(shape.vras, 12u);
    EXPECT_EQ(shape.vras_outputs, 8u);
    EXPECT_EQ(shape.monitors, 24u);
    EXPECT_TRUE(shape.master);
}

TEST(nadia_discover, falls_back_to_address_leaf)
{
    // The pre-design behavior: `address` populated, `level` empty.
    FakeNadia fake({
        {"input address", 2},
        {"input 1 parametric frequency", 3},
    });
    EXPECT_TRUE(fake.start());
    auto shape = discover(fake);
    EXPECT_EQ(shape.inputs, 2u);
    EXPECT_EQ(shape.outputs, 0u);
    EXPECT_EQ(shape.vgroups, 0u);
    EXPECT_EQ(shape.eq_bands, 3u);
}

TEST_MAIN(statusbar_controlplane_devices, nadia_discover_test)
