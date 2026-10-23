// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/widget_json.hpp"

#include "statusbar/test/test.hpp"

#include <limits>
#include <string>

using namespace statusbar::widget_bridge;

TEST(widget_json, writer_emits_flat_objects)
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "hello");
    w.field_hex64("entity", 0x70B3D5EDC2001234ULL);
    w.field("control", std::int64_t{42});
    w.field("live", true);
    w.field("value", 0.5);
    w.begin_array("values");
    w.element(1.0);
    w.element(-2.5);
    w.end_array();
    w.end_object();
    EXPECT_EQ(
        out, std::string{R"({"op":"hello","entity":"0x70b3d5edc2001234","control":42,"live":true,"value":0.5,"values":[1,-2.5]})"});
}

TEST(widget_json, writer_escapes_and_handles_nonfinite)
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("name", "a\"b\\c\nd\te\x01");
    w.field("bad", std::numeric_limits<double>::quiet_NaN());
    w.end_object();
    EXPECT_EQ(out, std::string{R"({"name":"a\"b\\c\nd\te\u0001","bad":null})"});
}

TEST(widget_json, double_round_trips_17g)
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("v", 0.1);
    w.end_object();
    auto const msg = json_scan(out);
    EXPECT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->number_of("v") == 0.1);
}

TEST(widget_json, scans_the_subprotocol_shapes)
{
    auto const msg = json_scan(R"(  {"op":"set","entity":"0xABCDEF0102030405","control":7,"values":[0, 1.5, -3e2]} )");
    EXPECT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->string_of("op") == "set");
    EXPECT_TRUE(msg->hex64_of("entity") == 0xABCDEF0102030405ULL);
    EXPECT_TRUE(msg->number_of("control") == 7.0);
    EXPECT_EQ(msg->array_key, std::string{"values"});
    EXPECT_EQ(msg->array_values.size(), 3U);
    EXPECT_TRUE(msg->array_values[2] == -300.0);

    auto const empty = json_scan("{}");
    EXPECT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->fields.empty());

    auto const escaped = json_scan(R"({"s":"a\"\\\nAé"})");
    EXPECT_TRUE(escaped.has_value());
    EXPECT_TRUE(escaped->string_of("s") == "a\"\\\nA\xc3\xa9");

    auto const booleans = json_scan(R"({"a":true,"b":false,"c":null})");
    EXPECT_TRUE(booleans.has_value());
    EXPECT_TRUE(booleans->find("a")->bool_value);
    EXPECT_FALSE(booleans->find("b")->bool_value);
    EXPECT_TRUE(booleans->find("c")->kind == JsonMessage::Field::Kind::null);
}

TEST(widget_json, scans_multiple_and_string_arrays)
{
    // The widget-message shape: numeric options beside string labels.
    auto const msg = json_scan(R"({"op":"widget","options":[0,1,2],"labels":["OFF","LOW","HIGH"],"value":1})");
    EXPECT_TRUE(msg.has_value());
    EXPECT_EQ(msg->arrays.size(), 2U);
    EXPECT_EQ(msg->array_key, std::string{"options"});  // first array, back-compat
    EXPECT_EQ(msg->array_values.size(), 3U);
    auto const* labels = msg->array_of("labels");
    EXPECT_TRUE(labels != nullptr);
    EXPECT_TRUE(labels != nullptr && labels->strings.size() == 3 && labels->strings[1] == "LOW");
    auto const* options = msg->array_of("options");
    EXPECT_TRUE(options != nullptr && options->numbers.size() == 3 && options->strings.empty());

    auto const empty = json_scan(R"({"a":[],"b":[1]})");
    EXPECT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->array_of("a") != nullptr && empty->array_of("a")->numbers.empty());
}

TEST(widget_json, rejects_what_the_subprotocol_forbids)
{
    EXPECT_FALSE(json_scan("").has_value());
    EXPECT_FALSE(json_scan("[1,2]").has_value());
    EXPECT_FALSE(json_scan(R"({"a":{"b":1}})").has_value());  // nested object
    EXPECT_FALSE(json_scan(R"({"a":[1,"x"]})").has_value());  // mixed-type array
    EXPECT_FALSE(json_scan(R"({"a":["x",1]})").has_value());  // mixed-type array
    EXPECT_FALSE(json_scan(R"({"a":[[1]]})").has_value());    // nested array
    EXPECT_FALSE(json_scan(R"({"a":1)").has_value());         // unterminated
    EXPECT_FALSE(json_scan(R"({"a":1} extra)").has_value());  // trailing garbage
    EXPECT_FALSE(json_scan(R"({"a":tru})").has_value());      // bad literal
    EXPECT_FALSE(json_scan(R"({"a":"\q"})").has_value());     // bad escape
    EXPECT_FALSE(json_scan(R"({"a" 1})").has_value());        // missing colon
}

TEST(widget_json, hex64_rejects_malformed)
{
    auto const msg = json_scan(R"({"a":"0x12g4","b":"1234","c":"0x","d":"0x00000000000000001"})");
    EXPECT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->hex64_of("a").has_value());
    EXPECT_FALSE(msg->hex64_of("b").has_value());
    EXPECT_FALSE(msg->hex64_of("c").has_value());
    EXPECT_FALSE(msg->hex64_of("d").has_value());
    EXPECT_FALSE(msg->hex64_of("missing").has_value());
}

//
// Test Runner
//

TEST_MAIN(statusbar_widget_bridge, widget_json_test)
