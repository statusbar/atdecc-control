// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/widget_controls.hpp"

#include "statusbar/atdecc/atdecc_aem_control_accessors.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"

#include <cmath>
#include <type_traits>

namespace statusbar::widget_bridge {

using namespace statusbar::atdecc::aem;

auto widget_kind_name(WidgetKind kind) noexcept -> std::string_view
{
    switch (kind) {
        case WidgetKind::slider:
            return "slider";
        case WidgetKind::toggle:
            return "toggle";
        case WidgetKind::select:
            return "select";
        case WidgetKind::text:
            return "text";
        case WidgetKind::meter:
            return "meter";
        case WidgetKind::matrix:
            return "matrix";
        case WidgetKind::identify:
            return "identify";
        case WidgetKind::opaque:
            return "opaque";
    }
    return "opaque";
}

namespace {

/// Calls f.template operator()<T>() with the element type of a linear or
/// selector value_type. Returns false for non-numeric types.
template <typename F>
auto dispatch_element_type(std::uint16_t value_type, F&& f) -> bool
{
    switch (value_type) {
        case CONTROL_LINEAR_INT8:
        case CONTROL_SELECTOR_INT8:
            f.template operator()<std::int8_t>();
            return true;
        case CONTROL_LINEAR_UINT8:
        case CONTROL_SELECTOR_UINT8:
            f.template operator()<std::uint8_t>();
            return true;
        case CONTROL_LINEAR_INT16:
        case CONTROL_SELECTOR_INT16:
            f.template operator()<std::int16_t>();
            return true;
        case CONTROL_LINEAR_UINT16:
        case CONTROL_SELECTOR_UINT16:
            f.template operator()<std::uint16_t>();
            return true;
        case CONTROL_LINEAR_INT32:
        case CONTROL_SELECTOR_INT32:
            f.template operator()<std::int32_t>();
            return true;
        case CONTROL_LINEAR_UINT32:
        case CONTROL_SELECTOR_UINT32:
            f.template operator()<std::uint32_t>();
            return true;
        case CONTROL_LINEAR_INT64:
        case CONTROL_SELECTOR_INT64:
            f.template operator()<std::int64_t>();
            return true;
        case CONTROL_LINEAR_UINT64:
        case CONTROL_SELECTOR_UINT64:
            f.template operator()<std::uint64_t>();
            return true;
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_SELECTOR_FLOAT:
            f.template operator()<float>();
            return true;
        case CONTROL_LINEAR_DOUBLE:
        case CONTROL_SELECTOR_DOUBLE:
            f.template operator()<double>();
            return true;
        default:
            return false;
    }
}

/// The array-family counterpart of dispatch_element_type.
template <typename F>
auto dispatch_array_element_type(std::uint16_t value_type, F&& f) -> bool
{
    switch (value_type) {
        case CONTROL_ARRAY_INT8:
            f.template operator()<std::int8_t>();
            return true;
        case CONTROL_ARRAY_UINT8:
            f.template operator()<std::uint8_t>();
            return true;
        case CONTROL_ARRAY_INT16:
            f.template operator()<std::int16_t>();
            return true;
        case CONTROL_ARRAY_UINT16:
            f.template operator()<std::uint16_t>();
            return true;
        case CONTROL_ARRAY_INT32:
            f.template operator()<std::int32_t>();
            return true;
        case CONTROL_ARRAY_UINT32:
            f.template operator()<std::uint32_t>();
            return true;
        case CONTROL_ARRAY_INT64:
            f.template operator()<std::int64_t>();
            return true;
        case CONTROL_ARRAY_UINT64:
            f.template operator()<std::uint64_t>();
            return true;
        case CONTROL_ARRAY_FLOAT:
            f.template operator()<float>();
            return true;
        case CONTROL_ARRAY_DOUBLE:
            f.template operator()<double>();
            return true;
        default:
            return false;
    }
}

}  // namespace

auto project_control(DescriptorControl const& d) -> ControlProjection
{
    ControlProjection out;
    auto const bits = unpack_control_value_type(d.control_value_type.get());
    out.value_type = bits.value_type;
    out.read_only = bits.read_only;
    out.count = d.number_of_values.get();

    if (is_linear_value_type(bits.value_type)) {
        bool decoded = false;
        bool integral = false;
        (void)dispatch_element_type(bits.value_type, [&]<typename T>() {
            auto const first = linear_entry<T>(d, 0);
            if (!first.has_value()) {
                return;
            }
            out.minimum = double(first->minimum());
            out.maximum = double(first->maximum());
            out.step = double(first->step());
            out.default_value = double(first->default_value());
            out.unit_code = first->unit.code.get();
            out.unit_multiplier = first->unit.signed_multiplier();
            out.current.reserve(out.count);
            out.value_string_refs.reserve(out.count);
            for (std::uint16_t i = 0; i < out.count; ++i) {
                auto const e = linear_entry<T>(d, i);
                out.current.push_back(e.has_value() ? double(e->current()) : 0.0);
                out.value_string_refs.push_back(e.has_value() ? e->localized_string.get() : std::uint16_t{0});
            }
            integral = std::is_integral_v<T>;
            decoded = true;
        });
        if (decoded) {
            out.kind = WidgetKind::slider;
            // Single-value integer controls with a small, exact state grid
            // widget better than a continuous slider does: exactly two
            // reachable values (0/1, or the AEM 0/255 mute-and-identify
            // convention, step == span) is a toggle; a handful of steps
            // (a mode, a slope, a delay type) is a select over the grid.
            if (out.count == 1 && integral && out.step > 0.0 && out.maximum > out.minimum) {
                double const steps = (out.maximum - out.minimum) / out.step;
                bool const exact = std::abs(steps - std::round(steps)) < 1e-9;
                auto const states = exact ? std::llround(steps) + 1 : 0;
                if (states == 2) {
                    out.kind = WidgetKind::toggle;
                } else if (states >= 3 && states <= MAX_SELECT_STATES) {
                    out.kind = WidgetKind::select;
                    out.options.reserve(std::size_t(states));
                    for (long long i = 0; i < states; ++i) {
                        out.options.push_back(out.minimum + (double(i) * out.step));
                    }
                }
            }
            if (out.count == 1 && out.minimum == 0.0 && out.maximum == 1.0) {
                out.kind = WidgetKind::toggle;  // 0/1 even with a degenerate step
            }
            // The standard IDENTIFY control (Milan mandates one per
            // entity) is momentary, not a state: write max to wink the
            // device's LED and it self-resets per reset_time. Shape-typed
            // by control_type, so it wins over the toggle heuristic.
            if (out.count == 1 && !out.read_only && d.control_type == CONTROL_TYPE_IDENTIFY) {
                out.kind = WidgetKind::identify;
            }
        }
        return out;
    }

    if (is_selector_value_type(bits.value_type) && bits.value_type != CONTROL_SELECTOR_STRING) {
        bool decoded = false;
        (void)dispatch_element_type(bits.value_type, [&]<typename T>() {
            auto const view = selector_view<T>(d);
            if (!view.has_value()) {
                return;
            }
            out.current.push_back(double(view->current));
            out.default_value = double(view->default_value);
            out.options.reserve(view->options.size());
            for (auto const v : view->options) {
                out.options.push_back(double(v));
            }
            out.unit_code = view->unit.code.get();
            out.unit_multiplier = view->unit.signed_multiplier();
            decoded = true;
        });
        if (decoded) {
            out.kind = WidgetKind::select;
        }
        return out;
    }

    if (is_array_value_type(bits.value_type)) {
        // Meter banks and other value arrays: a read-only display bank in
        // v1 (arrays are never settable through the bridge).
        bool decoded = false;
        (void)dispatch_array_element_type(bits.value_type, [&]<typename T>() {
            auto const view = array_view<T>(d);
            if (!view.has_value()) {
                return;
            }
            out.minimum = double(view->minimum);
            out.maximum = double(view->maximum);
            out.step = double(view->step);
            out.default_value = double(view->default_value);
            out.unit_code = view->unit.code.get();
            out.unit_multiplier = view->unit.signed_multiplier();
            out.current.reserve(view->current.size());
            for (auto const v : view->current) {
                out.current.push_back(double(v));
            }
            decoded = true;
        });
        if (decoded) {
            out.kind = WidgetKind::meter;
            out.read_only = true;
        }
        return out;
    }

    if (bits.value_type == CONTROL_UTF8) {
        if (auto const text = control_utf8(d); text.has_value()) {
            out.text = *text;
            out.kind = WidgetKind::text;
            out.read_only = true;  // v1: UTF8 controls are display-only
        }
        return out;
    }

    return out;  // opaque
}

auto encode_control_values(DescriptorControl const& d, std::span<double const> values)
    -> std::optional<statusbar::sg14::inplace_vector<std::uint8_t, 508>>
{
    auto const bits = unpack_control_value_type(d.control_value_type.get());
    if (bits.read_only) {
        return std::nullopt;
    }

    statusbar::sg14::inplace_vector<std::uint8_t, 508> out;
    bool ok = false;

    if (is_linear_value_type(bits.value_type)) {
        if (values.size() != d.number_of_values.get()) {
            return std::nullopt;
        }
        (void)dispatch_element_type(bits.value_type, [&]<typename T>() {
            for (std::size_t i = 0; i < values.size(); ++i) {
                auto const entry = linear_entry<T>(d, i);
                if (!entry.has_value()) {
                    return;
                }
                double const v = values[i];
                if (std::isnan(v) || v < double(entry->minimum()) || v > double(entry->maximum())) {
                    return;
                }
                std::array<std::uint8_t, sizeof(T)> bytes{};
                detail::encode_linear_field(T(v), bytes);
                for (auto const b : bytes) {
                    out.push_back(b);
                }
            }
            ok = out.size() == values.size() * sizeof(T);
        });
        return ok ? std::optional{out} : std::nullopt;
    }

    if (is_selector_value_type(bits.value_type) && bits.value_type != CONTROL_SELECTOR_STRING) {
        if (values.size() != 1) {
            return std::nullopt;
        }
        (void)dispatch_element_type(bits.value_type, [&]<typename T>() {
            auto const view = selector_view<T>(d);
            if (!view.has_value()) {
                return;
            }
            double const v = values[0];
            bool listed = false;
            for (auto const option : view->options) {
                listed = listed || double(option) == v;
            }
            if (!listed) {
                return;
            }
            std::array<std::uint8_t, sizeof(T)> bytes{};
            detail::encode_linear_field(T(v), bytes);
            for (auto const b : bytes) {
                out.push_back(b);
            }
            ok = true;
        });
        return ok ? std::optional{out} : std::nullopt;
    }

    return std::nullopt;
}

auto decode_control_values(DescriptorControl const& d, std::span<std::uint8_t const> body) -> std::optional<std::vector<double>>
{
    auto const bits = unpack_control_value_type(d.control_value_type.get());
    std::vector<double> out;
    bool ok = false;

    // Bodies are accepted with trailing padding: some devices (the JOYNED
    // MU16 pads a 1 x UINT8 IDENTIFY to 13 bytes) answer GET/SET_CONTROL
    // with more than number_of_values x element and dropping those left
    // every identify click unconfirmed. Too short is still rejected.
    if (is_linear_value_type(bits.value_type)) {
        (void)dispatch_element_type(bits.value_type, [&]<typename T>() {
            std::size_t const n = d.number_of_values.get();
            if (body.size() < n * sizeof(T)) {
                return;
            }
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                std::array<std::uint8_t, sizeof(T)> bytes{};
                std::memcpy(bytes.data(), body.data() + (i * sizeof(T)), sizeof(T));
                out.push_back(double(detail::decode_linear_field<T>(bytes)));
            }
            ok = true;
        });
        return ok ? std::optional{std::move(out)} : std::nullopt;
    }

    if (is_selector_value_type(bits.value_type) && bits.value_type != CONTROL_SELECTOR_STRING) {
        (void)dispatch_element_type(bits.value_type, [&]<typename T>() {
            if (body.size() < sizeof(T)) {
                return;
            }
            std::array<std::uint8_t, sizeof(T)> bytes{};
            std::memcpy(bytes.data(), body.data(), sizeof(T));
            out.push_back(double(detail::decode_linear_field<T>(bytes)));
            ok = true;
        });
        return ok ? std::optional{std::move(out)} : std::nullopt;
    }

    if (is_array_value_type(bits.value_type)) {
        // GET/SET/unsolicited bodies for arrays carry the current values,
        // number_of_values x element — the same shape as linear.
        (void)dispatch_array_element_type(bits.value_type, [&]<typename T>() {
            std::size_t const n = d.number_of_values.get();
            if (body.size() < n * sizeof(T)) {
                return;
            }
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                std::array<std::uint8_t, sizeof(T)> bytes{};
                std::memcpy(bytes.data(), body.data() + (i * sizeof(T)), sizeof(T));
                out.push_back(double(detail::decode_linear_field<T>(bytes)));
            }
            ok = true;
        });
        return ok ? std::optional{std::move(out)} : std::nullopt;
    }

    return std::nullopt;
}

auto decode_linear_elements(std::uint16_t value_type, std::span<std::uint8_t const> bytes) -> std::optional<std::vector<double>>
{
    if (!is_linear_value_type(value_type)) {
        return std::nullopt;
    }
    std::vector<double> out;
    bool ok = false;
    (void)dispatch_element_type(value_type, [&]<typename T>() {
        if (bytes.size() % sizeof(T) != 0) {
            return;
        }
        std::size_t const n = bytes.size() / sizeof(T);
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::array<std::uint8_t, sizeof(T)> b{};
            std::memcpy(b.data(), bytes.data() + (i * sizeof(T)), sizeof(T));
            out.push_back(double(detail::decode_linear_field<T>(b)));
        }
        ok = true;
    });
    return ok ? std::optional{std::move(out)} : std::nullopt;
}

auto encode_linear_element(std::uint16_t value_type, double value)
    -> std::optional<statusbar::sg14::inplace_vector<std::uint8_t, 8>>
{
    if (!is_linear_value_type(value_type) || std::isnan(value)) {
        return std::nullopt;
    }
    statusbar::sg14::inplace_vector<std::uint8_t, 8> out;
    bool ok = false;
    (void)dispatch_element_type(value_type, [&]<typename T>() {
        std::array<std::uint8_t, sizeof(T)> b{};
        detail::encode_linear_field(T(value), b);
        for (auto const byte : b) {
            out.push_back(byte);
        }
        ok = true;
    });
    return ok ? std::optional{out} : std::nullopt;
}

}  // namespace statusbar::widget_bridge
