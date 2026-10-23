// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/nadia_mirror.hpp"

#include "statusbar/controlplane_devices/nadia_ascii.hpp"

#include <algorithm>

namespace statusbar::controlplane_devices {

using controlplane::ControlPointId;

NadiaMirror::NadiaMirror(NadiaNamespace ns)
    : ns_(std::move(ns))
    , instance_(ns_.model, controlplane::Role::controller)
    , values_(ns_.size())
    , reported_(ns_.size())
    , populated_(ns_.size())
{}

std::optional<ControlPointId> NadiaMirror::consume_line(std::string_view line, std::uint64_t now_ns)
{
    auto parsed = parse_nadia_line(line);
    switch (parsed.kind) {
        case NadiaLineKind::empty:
            return std::nullopt;
        case NadiaLineKind::other:
            ++other_count_;
            return std::nullopt;
        case NadiaLineKind::value:
            break;
    }
    if (parsed.unknown_address) {
        ++unknown_address_count_;
        return std::nullopt;
    }
    ++value_report_count_;
    auto h = ns_.resolve_name(parsed.name);
    if (!h) {
        std::string name(parsed.name);
        if (std::find(unknown_names_.begin(), unknown_names_.end(), name) == unknown_names_.end()) {
            unknown_names_.push_back(std::move(name));
        }
        return std::nullopt;
    }
    store(*h, parsed.value, now_ns);
    return h;
}

void NadiaMirror::record_local_set(ControlPointId h, std::string_view value, std::uint64_t now_ns)
{
    store(h, value, now_ns);
}

void NadiaMirror::store(ControlPointId h, std::string_view value, std::uint64_t now_ns)
{
    values_[h].assign(value);
    reported_.set(h);
    if (value.empty()) {
        populated_.reset(h);
        return;
    }
    populated_.set(h);
    if (auto scalar = nadia_scalar_of(ns_.kinds[h], ns_.model->min(h), value)) {
        instance_.set_reported(h, controlplane::value_bits(*scalar), now_ns);
    }
}

std::optional<double> NadiaMirror::scalar_of(ControlPointId h) const
{
    if (!populated_.test(h)) {
        return std::nullopt;
    }
    return nadia_scalar_of(ns_.kinds[h], ns_.model->min(h), values_[h]);
}

controlplane::Bitset NadiaMirror::missing() const
{
    controlplane::Bitset out(ns_.size());
    out.set_all();
    reported_.for_each_set([&](ControlPointId h) { out.reset(h); });
    return out;
}

}  // namespace statusbar::controlplane_devices
