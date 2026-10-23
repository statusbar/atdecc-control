// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Live model-store round trip (docs/PLAN.md §6): crawl a running proxy's
// full descriptor tree over AECP — ENTITY -> CONFIGURATION -> every
// (type, index) in the configuration's descriptor_counts — persist the
// raw wire bytes into an AEM model store, and verify the loop closes:
//
//   1. the wire fingerprint equals the locally generated model's
//      fingerprint (per-unit fields are canonicalized, so the proxy's
//      patched entity_id does not perturb identity);
//   2. store() keys the blob under that fingerprint and load_blob() serves
//      every crawled descriptor back byte-identical;
//   3. the stored symbol table is non-empty and list() shows the model.
//
//   statusbar-galaxy-model-verify <interface> <entity-id-hex> <store-dir>
//       [--foreign | --nadia <shape> | --nadia-discover <nadia-host>]
//
// --foreign: the target is not a statusbar proxy (e.g. the real GALAXY
// itself) — skip the generated-model identity gate and verify only the
// crawl + store round trip, exercising the store's vendor-neutral
// byte-exactness against a third-party device.
//
// --nadia: the target is the nadia-cp proxy — regenerate its model from
// the given shape (the k=v,... form the proxy logs at startup) and gate
// the wire fingerprint against it. --nadia-discover probes the shape
// from the Nadia's own text port instead, exactly as the proxy's
// --discover does, so proxy and gate derive from the same live design.

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/controlplane_devices/nadia_ascii.hpp"
#include "statusbar/controlplane_devices/nadia_discover.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nadia_proxy/nadia_aem_model.hpp"
#include "statusbar/nanoavb/nanoavb_aem_model_store.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using ieee::Eui48;
using ieee::Eui64;

namespace {

constexpr Eui64 VERIFY_CONTROLLER_ENTITY_ID(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x07);

std::int64_t monotonic_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Eui64 eui64_from(std::uint64_t v)
{
    return Eui64{
        std::uint8_t(v >> 56),
        std::uint8_t(v >> 48),
        std::uint8_t(v >> 40),
        std::uint8_t(v >> 32),
        std::uint8_t(v >> 24),
        std::uint8_t(v >> 16),
        std::uint8_t(v >> 8),
        std::uint8_t(v)};
}

std::uint16_t be16_at(std::span<std::uint8_t const> b, std::size_t off)
{
    if (off + 1 >= b.size()) {
        return 0;
    }
    ieee::doublet_t v{};
    span_load(v, b.subspan(off, sizeof v));
    return v.get();
}

/// Full-tree AECP crawler on the nanoavb controller (the monitor tool's
/// --enumerate walk, self-contained): descriptor reads are queued and
/// drained from tick() under an in-flight cap — never sent from inside
/// the response callback, which re-enters the controller.
class Crawler : public net::Pollable
{
  public:
    Crawler(std::string const& interface_name, Eui64 target, int& exit_code, std::vector<nanoavb::AemRawDescriptor>& out)
        : target_{target}
        , exit_code_{exit_code}
        , out_{out}
        , controller_{VERIFY_CONTROLLER_ENTITY_ID}
    {
        (void)context_.open(interface_name, avtp::AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC);
        nanoavb::AemControllerEntityCallbacks callbacks;
        callbacks.send_atdecc_multicast = [this](std::span<std::uint8_t const> packet) -> bool {
            return context_.send(&ATDECC_MULTICAST_MAC, packet).has_value();
        };
        callbacks.send_atdecc_unicast = [this](Eui48 const& dest, std::span<std::uint8_t const> packet) -> bool {
            return context_.send(&dest, packet).has_value();
        };
        callbacks.on_aem_response = [this](
                                        Eui64 /*t*/,
                                        std::uint16_t cmd,
                                        std::uint8_t status,
                                        std::span<std::uint8_t const> /*sent*/,
                                        std::span<std::uint8_t const> data) { on_response(cmd, status, data); };
        callbacks.on_aem_timeout = [this](Eui64 /*t*/, std::uint16_t /*cmd*/) {
            fprintf(stderr, "FAIL: READ_DESCRIPTOR timeout mid-crawl\n");
            finish(1);
        };
        controller_.set_callbacks(std::move(callbacks));
        controller_.start();
        controller_.discover(target_);
    }

    [[nodiscard]] bool valid() const noexcept { return context_.fd() >= 0; }
    [[nodiscard]] int fd() const noexcept override { return context_.fd(); }
    [[nodiscard]] bool finished() const noexcept override { return done_; }

    void on_ready(std::int64_t now_ns) override
    {
        Eui48 src{};
        Eui48 dst{};
        while (true) {
            auto result = context_.recv(&src, &dst, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            auto payload = std::span<std::uint8_t const>{payload_buf_.data(), std::size_t(*result)};
            if (payload.empty()) {
                continue;
            }
            if (payload[0] == avtp::AvtpSubtype::adp) {
                if (payload.size() >= AdpDu::LENGTH) {
                    AdpDu adp{};
                    span_load(adp, payload);
                    controller_.receive_adp(adp, src, now_ns);
                }
            } else if (payload[0] == avtp::AvtpSubtype::aecp) {
                controller_.receive_aecp(payload, now_ns);
            }
        }
    }

    void tick(std::int64_t now_ns) override
    {
        controller_.tick(now_ns);
        if (start_ns_ == 0) {
            start_ns_ = now_ns;
        }
        if (!entity_requested_ && controller_.find_entity(target_) != nullptr) {
            entity_requested_ = true;
            enqueue(DESCRIPTOR_ENTITY, 0);
        }
        while (outstanding_ < MAX_INFLIGHT && !queue_.empty()) {
            auto [type, index] = queue_.front();
            if (!controller_.read_descriptor(target_, type, index)) {
                break;  // controller busy; retry next tick
            }
            queue_.pop_front();
            ++outstanding_;
        }
        if (!done_ && seen_entity_ && seen_config_ && queue_.empty() && outstanding_ == 0) {
            finish(0);
        }
        if (!done_ && (now_ns - start_ns_) > TIMEOUT_NS) {
            fprintf(
                stderr,
                "FAIL: crawl deadline (%zu descriptors, %zu queued, %d outstanding)\n",
                out_.size(),
                queue_.size(),
                outstanding_);
            finish(1);
        }
    }

  private:
    static constexpr int MAX_INFLIGHT = 4;
    static constexpr std::int64_t TIMEOUT_NS = 60'000'000'000;

    void finish(int code)
    {
        exit_code_ = code;
        done_ = true;
    }

    void enqueue(std::uint16_t type, std::uint16_t index)
    {
        if (queued_.insert({type, index}).second) {
            queue_.push_back({type, index});
        }
    }

    void on_response(std::uint16_t cmd, std::uint8_t status, std::span<std::uint8_t const> data)
    {
        if (cmd != AEM_COMMAND_READ_DESCRIPTOR) {
            return;
        }
        if (outstanding_ > 0) {
            --outstanding_;
        }
        if (status != AEM_STATUS_SUCCESS || data.size() < AemReadDescriptorResponsePayload::LENGTH + 4) {
            fprintf(stderr, "FAIL: READ_DESCRIPTOR status %u\n", status);
            finish(1);
            return;
        }
        auto const body = data.subspan(AemReadDescriptorResponsePayload::LENGTH);
        auto const dtype = be16_at(body, 0);
        auto const dindex = be16_at(body, 2);
        out_.push_back({0, dtype, dindex, {body.begin(), body.end()}});

        if (dtype == DESCRIPTOR_ENTITY) {
            seen_entity_ = true;
            auto const n_configs = be16_at(body, 308);
            for (std::uint16_t c = 0; c < n_configs; ++c) {
                enqueue(DESCRIPTOR_CONFIGURATION, c);
            }
        } else if (dtype == DESCRIPTOR_CONFIGURATION) {
            seen_config_ = true;
            auto const counts = be16_at(body, 70);
            auto off = be16_at(body, 72);
            if (off < 74) {
                off = 74;
            }
            for (std::uint16_t i = 0; i < counts; ++i) {
                std::size_t const base = std::size_t(off) + std::size_t(i) * 4;
                auto const type = be16_at(body, base);
                auto const count = be16_at(body, base + 2);
                if (type == DESCRIPTOR_ENTITY || type == DESCRIPTOR_CONFIGURATION) {
                    continue;
                }
                for (std::uint16_t k = 0; k < count; ++k) {
                    enqueue(type, k);
                }
            }
        } else if (dtype == DESCRIPTOR_LOCALE && body.size() >= DescriptorLocale::LENGTH) {
            // Chase the locale's STRINGS: real devices (the GALAXY, JOYNED
            // interfaces) count their LOCALE but not the STRINGS behind it,
            // which the standard reaches only through base_strings — without
            // this a counts-driven crawl stores a model whose localized
            // names dangle. enqueue() dedupes against models that do count
            // their STRINGS.
            auto const n_strings = be16_at(body, 68);
            auto const base_strings = be16_at(body, 70);
            for (std::uint16_t k = 0; k < n_strings; ++k) {
                enqueue(DESCRIPTOR_STRINGS, std::uint16_t(base_strings + k));
            }
        }
    }

    Eui64 target_;
    int& exit_code_;
    std::vector<nanoavb::AemRawDescriptor>& out_;
    net::RawnetContext context_{};
    nanoavb::NanoAvbAemController controller_;
    std::array<std::uint8_t, 2048> payload_buf_{};
    std::deque<std::pair<std::uint16_t, std::uint16_t>> queue_;
    std::set<std::pair<std::uint16_t, std::uint16_t>> queued_;
    int outstanding_{0};
    std::int64_t start_ns_{0};
    bool entity_requested_{false};
    bool seen_entity_{false};
    bool seen_config_{false};
    bool done_{false};
};

int fail(char const* what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/// Zeroes every CONTROL descriptor's CURRENT value fields, in place. The
/// nadia proxy serves live mirrored values there (spec-correct
/// READ_DESCRIPTOR behavior), so the identity gate must compare model
/// structure, not the moment's fader positions — both the wire and the
/// generated set get the same canonicalization.
void canonicalize_control_currents(std::vector<nanoavb::AemRawDescriptor>& descs)
{
    for (auto& d : descs) {
        if (d.descriptor_type != DESCRIPTOR_CONTROL || d.bytes.size() < DescriptorControl::LENGTH) {
            continue;
        }
        DescriptorControl c{};
        span_load_padded(c, std::span<std::uint8_t const>(d.bytes));
        auto const bits = unpack_control_value_type(std::uint16_t(c.control_value_type.get()));
        auto const elem = control_value_element_size(bits.value_type);
        if (elem == 0) {
            continue;
        }
        bool const array = is_array_value_type(bits.value_type);
        std::size_t const off = c.values_offset.get();
        for (std::uint16_t v = 0; v < c.number_of_values.get(); ++v) {
            std::size_t const at = off +
                (array ? 4 * elem + 4 + std::size_t(v) * elem
                       : std::size_t(v) * linear_entry_size(elem) + linear_entry_current_offset(elem));
            if (at + elem > d.bytes.size()) {
                break;
            }
            std::fill_n(d.bytes.begin() + long(at), elem, std::uint8_t(0));
        }
    }
}

int verify(
    std::vector<nanoavb::AemRawDescriptor> const& wire,
    std::string const& store_dir,
    std::vector<nanoavb::AemRawDescriptor> const& generated,
    bool foreign)
{
    // 1. Wire identity == generated identity (proxy targets only), with
    // CONTROL current values canonicalized on both sides — the store
    // round trip below stays byte-exact on the raw wire bytes.
    auto wire_canonical = wire;
    auto generated_canonical = generated;
    canonicalize_control_currents(wire_canonical);
    canonicalize_control_currents(generated_canonical);
    auto const wire_fp = nanoavb::aem_fingerprint(wire_canonical);
    auto const generated_fp = nanoavb::aem_fingerprint(generated_canonical);
    // The store keys by the RAW wire fingerprint (live currents included).
    auto const store_key = nanoavb::aem_fingerprint(wire);
    printf("wire fingerprint      %s\n", wire_fp.c_str());
    if (!foreign) {
        printf("generated fingerprint %s\n", generated_fp.c_str());
    }
    bool identity_ok = foreign || (wire.size() == generated.size() && wire_fp == generated_fp);
    if (!identity_ok) {
        // Persist the crawl anyway (below) so the mismatch can be diffed
        // offline, and name the differing descriptors here.
        fprintf(stderr, "wire %zu vs generated %zu descriptors\n", wire.size(), generated.size());
        int shown = 0;
        for (auto const& w : wire_canonical) {
            for (auto const& g : generated_canonical) {
                if (g.descriptor_type == w.descriptor_type && g.descriptor_index == w.descriptor_index &&
                    g.configuration == w.configuration && g.bytes != w.bytes && shown < 10) {
                    std::size_t at = 0;
                    while (at < std::min(g.bytes.size(), w.bytes.size()) && g.bytes[at] == w.bytes[at]) {
                        ++at;
                    }
                    fprintf(
                        stderr,
                        "differs: type 0x%04x index %u (wire %zu vs gen %zu bytes, first delta at %zu)\n",
                        w.descriptor_type,
                        w.descriptor_index,
                        w.bytes.size(),
                        g.bytes.size(),
                        at);
                    ++shown;
                }
            }
        }
    }

    // 2. Store round trip: every wire descriptor comes back byte-identical.
    nanoavb::AemModelStore store(store_dir);
    auto stored_fp = store.store(wire);
    if (!stored_fp || *stored_fp != store_key) {
        return fail("store() failed or keyed under a different fingerprint");
    }
    if (!store.contains(store_key)) {
        return fail("contains() lost the stored model");
    }
    auto blob = store.load_blob(store_key);
    if (!blob) {
        return fail("load_blob() failed");
    }
    auto storage = DescriptorStorage::create(*blob);
    if (!storage) {
        return fail("stored blob does not parse");
    }
    for (auto const& d : wire) {
        auto got = storage->get_descriptor(d.configuration, d.descriptor_type, d.descriptor_index);
        if (!got || got->size() != d.bytes.size() || !std::equal(got->begin(), got->end(), d.bytes.begin())) {
            return fail("a stored descriptor is not byte-identical to the wire bytes");
        }
    }

    // 3. Derived artifacts.
    auto symbols = store.load_symbols(store_key);
    if (!symbols || symbols->empty()) {
        return fail("stored symbol table missing or empty");
    }
    auto listing = store.list();
    if (std::find(listing.begin(), listing.end(), store_key) == listing.end()) {
        return fail("list() does not show the stored fingerprint");
    }

    if (!identity_ok) {
        return fail("wire identity differs from the generated model (store round trip itself was byte-exact)");
    }
    printf(
        "PASS%s: %zu descriptors round-tripped byte-identical through %s (%zu symbols)\n",
        foreign ? " (foreign model)" : "",
        wire.size(),
        store_dir.c_str(),
        symbols->size());
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool const foreign = argc == 5 && std::string_view(argv[4]) == "--foreign";
    bool const nadia_shape_mode = argc == 6 && std::string_view(argv[4]) == "--nadia";
    bool const nadia_discover_mode = argc == 6 && std::string_view(argv[4]) == "--nadia-discover";
    if (argc != 4 && !foreign && !nadia_shape_mode && !nadia_discover_mode) {
        fprintf(
            stderr,
            "usage: statusbar-galaxy-model-verify <interface> <entity-id-hex> <store-dir>\n"
            "         [--foreign | --nadia <shape k=v,...> | --nadia-discover <nadia-host>]\n");
        return 2;
    }
    auto const target = eui64_from(std::strtoull(argv[2], nullptr, 16));

    // The identity gate's locally generated model: the galaxy proxy's by
    // default, the nadia proxy's for --nadia/--nadia-discover, none for
    // --foreign.
    std::vector<nanoavb::AemRawDescriptor> generated;
    if (nadia_shape_mode || nadia_discover_mode) {
        controlplane_devices::NadiaShape shape;
        if (nadia_shape_mode) {
            if (!controlplane_devices::parse_nadia_shape(argv[5], shape)) {
                return fail("bad --nadia shape (want k=v,... as the proxy logs it)");
            }
        } else {
            controlplane_devices::GalaxyClient probe;
            if (!probe.connect(argv[5], controlplane_devices::NADIA_ASCII_PORT)) {
                return fail("cannot reach the Nadia text port for --nadia-discover");
            }
            shape = controlplane_devices::discover_nadia_shape(probe, 128, stderr);
        }
        auto ns = controlplane_devices::build_nadia_namespace(shape);
        auto model = nadia_proxy::build_nadia_proxy_aem_model(ns);
        generated.reserve(model.descriptors.size());
        for (auto const& d : model.descriptors) {
            generated.push_back({d.configuration, d.descriptor_type, d.descriptor_index, d.bytes});
        }
    } else if (!foreign) {
        auto ns = controlplane_devices::build_galaxy_816_namespace();
        auto model = galaxy_proxy::build_galaxy_proxy_aem_model(ns);
        generated.reserve(model.descriptors.size());
        for (auto const& d : model.descriptors) {
            generated.push_back({d.configuration, d.descriptor_type, d.descriptor_index, d.bytes});
        }
    }

    int exit_code = 1;
    std::vector<nanoavb::AemRawDescriptor> wire;
    auto crawler = std::make_unique<Crawler>(argv[1], target, exit_code, wire);
    if (!crawler->valid()) {
        return fail("cannot open interface (needs cap_net_raw or root)");
    }
    auto& stop = itc::install_stop_signal();
    net::MessageReactor reactor{stop, monotonic_ns, 100};
    reactor.add(std::move(crawler));
    reactor.run();
    if (exit_code != 0) {
        return exit_code;
    }
    printf("crawled %zu descriptors\n", wire.size());
    return verify(wire, argv[3], generated, foreign);
}
