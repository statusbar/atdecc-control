// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/widget_json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace statusbar::widget_bridge {

// ---- emit -------------------------------------------------------------------

void JsonWriter::comma()
{
    if (need_comma_) {
        out_.push_back(',');
    }
    need_comma_ = true;
}

void JsonWriter::begin_object()
{
    comma();
    out_.push_back('{');
    need_comma_ = false;
}

void JsonWriter::end_object()
{
    out_.push_back('}');
    need_comma_ = true;
}

void JsonWriter::begin_array(std::string_view key)
{
    comma();
    out_.push_back('"');
    out_.append(key);
    out_.append("\":[");
    need_comma_ = false;
}

void JsonWriter::end_array()
{
    out_.push_back(']');
    need_comma_ = true;
}

namespace {

void append_key(std::string& out, std::string_view key)
{
    out.push_back('"');
    out.append(key);
    out.append("\":");
}

void append_double(std::string& out, double value)
{
    char buf[32];
    auto const n = std::snprintf(buf, sizeof buf, "%.17g", value);
    // JSON has no inf/nan; the subprotocol substitutes null.
    if (n <= 0 || std::strstr(buf, "inf") != nullptr || std::strstr(buf, "nan") != nullptr) {
        out.append("null");
        return;
    }
    out.append(buf, std::size_t(n));
}

}  // namespace

void JsonWriter::field(std::string_view key, std::string_view value)
{
    comma();
    append_key(out_, key);
    out_.push_back('"');
    json_escape(out_, value);
    out_.push_back('"');
}

void JsonWriter::field(std::string_view key, double value)
{
    comma();
    append_key(out_, key);
    append_double(out_, value);
}

void JsonWriter::field(std::string_view key, std::int64_t value)
{
    comma();
    append_key(out_, key);
    char buf[24];
    auto const n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(value));
    out_.append(buf, std::size_t(n));
}

void JsonWriter::field(std::string_view key, std::uint64_t value)
{
    comma();
    append_key(out_, key);
    char buf[24];
    auto const n = std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(value));
    out_.append(buf, std::size_t(n));
}

void JsonWriter::field(std::string_view key, bool value)
{
    comma();
    append_key(out_, key);
    out_.append(value ? "true" : "false");
}

void JsonWriter::field_raw(std::string_view key, std::string_view raw)
{
    comma();
    append_key(out_, key);
    out_.append(raw);
}

void JsonWriter::field_hex64(std::string_view key, std::uint64_t value)
{
    comma();
    append_key(out_, key);
    char buf[24];
    auto const n = std::snprintf(buf, sizeof buf, "\"0x%016llx\"", static_cast<unsigned long long>(value));
    out_.append(buf, std::size_t(n));
}

void JsonWriter::element(double value)
{
    comma();
    append_double(out_, value);
}

void JsonWriter::element(std::string_view value)
{
    comma();
    out_.push_back('"');
    json_escape(out_, value);
    out_.push_back('"');
}

void json_escape(std::string& out, std::string_view text)
{
    for (char const c : text) {
        switch (c) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    auto const n = std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out.append(buf, std::size_t(n));
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
}

// ---- scan -------------------------------------------------------------------

namespace {

struct Scanner
{
    std::string_view text;
    std::size_t at = 0;

    void skip_ws()
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        }
    }

    [[nodiscard]] bool eof()
    {
        skip_ws();
        return at >= text.size();
    }

    [[nodiscard]] bool accept(char c)
    {
        skip_ws();
        if (at < text.size() && text[at] == c) {
            ++at;
            return true;
        }
        return false;
    }

    [[nodiscard]] char peek()
    {
        skip_ws();
        return at < text.size() ? text[at] : '\0';
    }

    /// A JSON string literal, unescaped. False on malformed input.
    [[nodiscard]] bool string(std::string& out)
    {
        if (!accept('"')) {
            return false;
        }
        while (at < text.size()) {
            char const c = text[at++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at >= text.size()) {
                return false;
            }
            char const esc = text[at++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    if (at + 4 > text.size()) {
                        return false;
                    }
                    char hex[5] = {text[at], text[at + 1], text[at + 2], text[at + 3], 0};
                    char* end = nullptr;
                    auto const code = std::strtoul(hex, &end, 16);
                    if (end != hex + 4) {
                        return false;
                    }
                    at += 4;
                    // BMP-only, encoded as UTF-8; the subprotocol's keys
                    // and values never need surrogate pairs.
                    if (code < 0x80) {
                        out.push_back(char(code));
                    } else if (code < 0x800) {
                        out.push_back(char(0xC0 | (code >> 6)));
                        out.push_back(char(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(char(0xE0 | (code >> 12)));
                        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool number(double& out)
    {
        skip_ws();
        auto const start = at;
        while (at < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[at])) != 0 || text[at] == '-' || text[at] == '+' || text[at] == '.' ||
                text[at] == 'e' || text[at] == 'E')) {
            ++at;
        }
        if (at == start) {
            return false;
        }
        std::string const lit{text.substr(start, at - start)};
        char* end = nullptr;
        out = std::strtod(lit.c_str(), &end);
        return end == lit.c_str() + lit.size();
    }

    [[nodiscard]] bool literal(std::string_view word)
    {
        skip_ws();
        if (text.substr(at, word.size()) == word) {
            at += word.size();
            return true;
        }
        return false;
    }
};

}  // namespace

JsonMessage::Field const* JsonMessage::find(std::string_view key) const noexcept
{
    for (auto const& f : fields) {
        if (f.key == key) {
            return &f;
        }
    }
    return nullptr;
}

JsonMessage::Array const* JsonMessage::array_of(std::string_view key) const noexcept
{
    for (auto const& a : arrays) {
        if (a.key == key) {
            return &a;
        }
    }
    return nullptr;
}

std::optional<std::string_view> JsonMessage::string_of(std::string_view key) const noexcept
{
    auto const* f = find(key);
    if (f == nullptr || f->kind != Field::Kind::string) {
        return std::nullopt;
    }
    return std::string_view{f->string_value};
}

std::optional<double> JsonMessage::number_of(std::string_view key) const noexcept
{
    auto const* f = find(key);
    if (f == nullptr || f->kind != Field::Kind::number) {
        return std::nullopt;
    }
    return f->number_value;
}

std::optional<std::uint64_t> JsonMessage::hex64_of(std::string_view key) const noexcept
{
    auto const s = string_of(key);
    if (!s || s->size() < 3 || s->size() > 18 || !s->starts_with("0x")) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (char const c : s->substr(2)) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        } else {
            return std::nullopt;
        }
        v = (v << 4) | std::uint64_t(digit);
    }
    return v;
}

std::optional<JsonMessage> json_scan(std::string_view text)
{
    Scanner s{text};
    JsonMessage msg;
    if (!s.accept('{')) {
        return std::nullopt;
    }
    if (!s.accept('}')) {
        while (true) {
            JsonMessage::Field field;
            if (!s.string(field.key)) {
                return std::nullopt;
            }
            if (!s.accept(':')) {
                return std::nullopt;
            }
            char const c = s.peek();
            if (c == '"') {
                field.kind = JsonMessage::Field::Kind::string;
                if (!s.string(field.string_value)) {
                    return std::nullopt;
                }
                msg.fields.push_back(std::move(field));
            } else if (c == '[') {
                (void)s.accept('[');
                JsonMessage::Array array;
                array.key = field.key;
                if (!s.accept(']')) {
                    // First element decides the array's type; the rest
                    // must match it (all numbers or all strings).
                    bool const strings = s.peek() == '"';
                    while (true) {
                        if (strings) {
                            std::string text;
                            if (!s.string(text)) {
                                return std::nullopt;
                            }
                            array.strings.push_back(std::move(text));
                        } else {
                            double v = 0;
                            if (!s.number(v)) {
                                return std::nullopt;
                            }
                            array.numbers.push_back(v);
                        }
                        if (s.accept(']')) {
                            break;
                        }
                        if (!s.accept(',')) {
                            return std::nullopt;
                        }
                        if ((s.peek() == '"') != strings) {
                            return std::nullopt;  // mixed-type array
                        }
                    }
                }
                if (msg.arrays.empty()) {
                    msg.array_key = array.key;
                    msg.array_values = array.numbers;
                }
                msg.arrays.push_back(std::move(array));
            } else if (c == 't' || c == 'f') {
                field.kind = JsonMessage::Field::Kind::boolean;
                if (s.literal("true")) {
                    field.bool_value = true;
                } else if (s.literal("false")) {
                    field.bool_value = false;
                } else {
                    return std::nullopt;
                }
                msg.fields.push_back(std::move(field));
            } else if (c == 'n') {
                if (!s.literal("null")) {
                    return std::nullopt;
                }
                field.kind = JsonMessage::Field::Kind::null;
                msg.fields.push_back(std::move(field));
            } else if (c == '{') {
                return std::nullopt;  // flat objects only
            } else {
                field.kind = JsonMessage::Field::Kind::number;
                if (!s.number(field.number_value)) {
                    return std::nullopt;
                }
                msg.fields.push_back(std::move(field));
            }
            if (s.accept('}')) {
                break;
            }
            if (!s.accept(',')) {
                return std::nullopt;
            }
        }
    }
    if (!s.eof()) {
        return std::nullopt;  // trailing garbage
    }
    return msg;
}

}  // namespace statusbar::widget_bridge
