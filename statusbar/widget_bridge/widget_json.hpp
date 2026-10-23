// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// Minimal JSON for the widget-bridge WS subprotocol (docs/PLAN.md §12).
//
// The bridge speaks flat JSON objects over WebSocket text frames: every
// server push is one object per frame, every client request is one
// object per frame. This is deliberately not a general JSON library —
// it emits exactly what the subprotocol needs (objects, arrays,
// strings, doubles, integers, booleans) and scans exactly what the
// subprotocol accepts (one flat object of scalar fields plus flat
// arrays whose elements are all numbers or all strings — `values`,
// `options`, `labels`). Nesting beyond that is a protocol error,
// reported as such.
//
// Numbers are emitted with %.17g so a double round-trips exactly
// through JavaScript, whose numbers are the same IEEE 754 binary64.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::widget_bridge {

/// Append-only JSON emitter. The caller provides the string; nesting is
/// tracked so commas land where they belong. Keys are emitted verbatim
/// (the subprotocol's keys are all JSON-safe ASCII); values are escaped.
class JsonWriter
{
  public:
    explicit JsonWriter(std::string& out)
        : out_{out}
    {}

    void begin_object();
    void end_object();
    void begin_array(std::string_view key);
    void end_array();

    void field(std::string_view key, std::string_view value);  ///< escaped string
    /// A string literal would otherwise prefer the bool overload
    /// (pointer->bool is a standard conversion, string_view is not).
    void field(std::string_view key, char const* value) { field(key, std::string_view{value}); }
    void field(std::string_view key, double value);
    void field(std::string_view key, std::int64_t value);
    void field(std::string_view key, std::uint64_t value);
    void field(std::string_view key, bool value);
    void field_raw(std::string_view key, std::string_view raw);  ///< pre-formed JSON

    void element(double value);            ///< array element
    void element(std::string_view value);  ///< array element, escaped

    /// "0x%016llx" — entity_ids travel as hex strings, never as JSON
    /// numbers (binary64 cannot hold 64 significant bits).
    void field_hex64(std::string_view key, std::uint64_t value);

  private:
    void comma();

    std::string& out_;
    bool need_comma_ = false;
};

/// Escape @p text into a JSON string literal body (no surrounding
/// quotes): ", \, control characters. Invalid UTF-8 bytes pass through
/// unchanged — the sources here (AEM object names, GALAXY strings) are
/// treated as opaque.
void json_escape(std::string& out, std::string_view text);

/// One parsed message: a flat object of scalar fields plus flat arrays
/// (each all-numbers or all-strings). Field and array order preserved.
struct JsonMessage
{
    struct Field
    {
        std::string key;
        std::string string_value;  ///< unescaped; empty for non-strings
        double number_value = 0.0;
        bool bool_value = false;
        enum class Kind : std::uint8_t
        {
            string,
            number,
            boolean,
            null,
        } kind = Kind::null;
    };

    struct Array
    {
        std::string key;
        std::vector<double> numbers;       ///< numeric elements
        std::vector<std::string> strings;  ///< string elements (exclusive with numbers)
    };

    std::vector<Field> fields;
    std::vector<Array> arrays;

    /// The first array's key and numeric elements — the common
    /// one-array case (`values`) without an extra lookup.
    std::string array_key;
    std::vector<double> array_values;

    [[nodiscard]] Field const* find(std::string_view key) const noexcept;
    [[nodiscard]] Array const* array_of(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<std::string_view> string_of(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<double> number_of(std::string_view key) const noexcept;

    /// Parses "0x..." hex strings (the entity_id convention).
    [[nodiscard]] std::optional<std::uint64_t> hex64_of(std::string_view key) const noexcept;
};

/// Scan one flat JSON object. Returns nullopt on malformed input,
/// non-flat nesting, or a mixed-type array. Accepts arbitrary
/// whitespace; rejects trailing garbage.
[[nodiscard]] std::optional<JsonMessage> json_scan(std::string_view text);

}  // namespace statusbar::widget_bridge
