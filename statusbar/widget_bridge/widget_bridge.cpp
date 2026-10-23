// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/widget_bridge.hpp"

#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_symbols.hpp"
#include "statusbar/widget_bridge/widget_json.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace statusbar::widget_bridge {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::atdecc_tools;
using ieee::Eui64;

auto EntityWidgets::find(std::uint16_t control_index) -> WidgetSpec*
{
    for (auto& w : widgets) {
        if (!w.is_selector && !w.is_matrix && w.control_index == control_index) {
            return &w;
        }
    }
    return nullptr;
}

auto EntityWidgets::find_selector(std::uint16_t selector_index) -> WidgetSpec*
{
    for (auto& w : widgets) {
        if (w.is_selector && w.control_index == selector_index) {
            return &w;
        }
    }
    return nullptr;
}

auto EntityWidgets::find_matrix(std::uint16_t matrix_index) -> WidgetSpec*
{
    for (auto& w : widgets) {
        if (w.is_matrix && w.control_index == matrix_index) {
            return &w;
        }
    }
    return nullptr;
}

WidgetBridge::WidgetBridge(std::unique_ptr<ControllerService> service, WidgetBridgeConfig config)
    : ctrl_{std::move(service)}
    , http_{config.bind, config.limits, config.site}
    , ws_path_{std::move(config.ws_path)}
    , labels_{config.labels}  // 10 bytes covers the largest server frame header (unmasked, 64-bit
    // length); a little extra slack costs one short frame, never a wedge.
    , max_ws_payload_{config.limits.max_response_head > 16 ? config.limits.max_response_head - 16 : 0}
{
    (void)http_.add_ws_route(ws_path_, *this);
}

auto WidgetBridge::valid() const -> bool
{
    return http_.valid();
}

// ---- pump -------------------------------------------------------------------

void WidgetBridge::pump(std::int64_t now_ns)
{
    for (auto const& event : ctrl_.drain_events()) {
        handle_event(event, now_ns);
    }
    reconcile_entities(now_ns);
    pump_control_sweeps(now_ns);
    pump_avb_info(now_ns);
    flush_outboxes();
}

void WidgetBridge::refresh_entity_values(std::int64_t now_ns)
{
    for (auto& [entity_id, entity] : entities_) {
        for (auto& w : entity.widgets) {
            if (w.is_matrix) {
                w.grid_next_row = 0;
                w.grid_loaded = false;
                request_matrix_chunk(entity_id, w, now_ns);
            } else if (w.is_selector) {
                ControllerAction action{};
                action.kind = ControllerActionKind::GetSignalSelector;
                action.request.talker_entity_id = entity_id;
                action.request.desc_index = w.control_index;
                ctrl_.dispatch(action, now_ns);
            }
        }
        start_control_sweep(entity);
    }
}

void WidgetBridge::start_control_sweep(EntityWidgets& entity)
{
    entity.sweep_queue.clear();
    entity.sweep_in_flight = -1;
    entity.sweep_retries = 0;
    for (auto const& w : entity.widgets) {
        if (w.is_selector || w.is_matrix) {
            continue;  // selectors and matrices have their own reads
        }
        auto const kind = w.projection.kind;
        if (kind == WidgetKind::text || kind == WidgetKind::opaque) {
            continue;  // no value decoder for these in v1
        }
        entity.sweep_queue.push_back(w.control_index);
    }
}

void WidgetBridge::pump_control_sweeps(std::int64_t now_ns)
{
    for (auto& [entity_id, entity] : entities_) {
        if (entity.sweep_in_flight >= 0 || entity.sweep_queue.empty()) {
            continue;
        }
        entity.sweep_in_flight = entity.sweep_queue.front();
        entity.sweep_queue.pop_front();
        ControllerAction action{};
        action.kind = ControllerActionKind::GetControl;
        action.request.talker_entity_id = entity_id;
        action.request.desc_index = std::uint16_t(entity.sweep_in_flight);
        ctrl_.dispatch(action, now_ns);
    }
}

void WidgetBridge::handle_event(ControllerEvent const& event, std::int64_t now_ns)
{
    if (auto const* completed = std::get_if<CommandCompletedEvent>(&event)) {
        if (completed->command_type == AEM_COMMAND_GET_AVB_INFO) {
            if (completed->ok()) {
                handle_avb_info_response(completed->entity_id, completed->response);
            }
            return;
        }
        if (completed->command_type == AEM_COMMAND_READ_DESCRIPTOR) {
            if (completed->ok()) {
                handle_interface_descriptor_response(completed->entity_id, completed->response);
            }
            return;
        }
        if (completed->command_type == AEM_COMMAND_GET_SIGNAL_SELECTOR ||
            completed->command_type == AEM_COMMAND_SET_SIGNAL_SELECTOR) {
            if (completed->ok()) {
                handle_selector_response(completed->entity_id, completed->response);
            }
            return;
        }
        if (completed->command_type == AEM_COMMAND_GET_MATRIX || completed->command_type == AEM_COMMAND_SET_MATRIX) {
            if (completed->ok()) {
                handle_matrix_response(
                    completed->entity_id, completed->response, completed->command_type == AEM_COMMAND_GET_MATRIX);
            }
            return;
        }
        if (completed->command_type == AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION && completed->ok()) {
            // Registration confirmed: every later change arrives as an
            // unsolicited push, so grids re-read NOW form a consistent
            // snapshot. (The build-time load can race a freshly started
            // entity that is still applying its own initial state — seen
            // live against the proxy mid-dump.)
            auto const entity_it = entities_.find(completed->entity_id);
            if (entity_it != entities_.end()) {
                for (auto& w : entity_it->second.widgets) {
                    if (w.is_matrix) {
                        w.grid_next_row = 0;
                        w.grid_loaded = false;
                        request_matrix_chunk(completed->entity_id, w, now_ns);
                    }
                }
                // ...and sweep every control's live value the same way.
                start_control_sweep(entity_it->second);
                // ...and read each AVB interface's live gPTP state, plus
                // the descriptor itself: a proxy mirrors its gPTP port
                // fields in after it starts advertising, so the crawl's
                // copy may be the pre-mirror one.
                for (auto& iface : entity_it->second.interfaces) {
                    request_avb_info(completed->entity_id, iface, now_ns);
                    request_interface_descriptor(completed->entity_id, iface, now_ns);
                }
            }
            return;
        }
        if (completed->command_type == AEM_COMMAND_SET_NAME && completed->ok()) {
            handle_name_response(completed->entity_id, completed->response);
            return;
        }
        if (completed->command_type != AEM_COMMAND_GET_CONTROL && completed->command_type != AEM_COMMAND_SET_CONTROL) {
            return;
        }
        if (completed->command_type == AEM_COMMAND_GET_CONTROL) {
            // Sweep bookkeeping. A successful response names its control
            // in the payload header; a failure names nothing, so with one
            // in flight the awaited index is retried (bounded) or dropped.
            auto const entity_it = entities_.find(completed->entity_id);
            if (entity_it != entities_.end() && entity_it->second.sweep_in_flight >= 0) {
                auto& entity = entity_it->second;
                if (completed->ok() && completed->response.size() >= AemControlPayloadHeader::LENGTH) {
                    AemControlPayloadHeader hdr{};
                    span_load(hdr, make_const_span(completed->response).first(AemControlPayloadHeader::LENGTH));
                    if (hdr.descriptor_index.get() == std::uint16_t(entity.sweep_in_flight)) {
                        entity.sweep_in_flight = -1;
                        entity.sweep_retries = 0;
                    }
                } else if (!completed->ok()) {
                    ++entity.sweep_retries;
                    if (entity.sweep_retries >= 3) {
                        entity.sweep_in_flight = -1;  // give this control up
                        entity.sweep_retries = 0;
                    } else {
                        entity.sweep_queue.push_front(std::uint16_t(entity.sweep_in_flight));
                        entity.sweep_in_flight = -1;
                    }
                }
            }
        }
        if (completed->ok()) {
            handle_control_response(
                completed->entity_id,
                completed->aem_status,
                completed->response,
                completed->command_type == AEM_COMMAND_GET_CONTROL ? ValueSource::get_result : ValueSource::set_echo);
        } else {
            // A refused or lost SET/GET: tell the clients which control so a
            // widget can snap back to its confirmed state.
            std::int64_t control_index = -1;
            if (completed->response.size() >= AemControlPayloadHeader::LENGTH) {
                AemControlPayloadHeader hdr{};
                span_load(hdr, make_const_span(completed->response).first(AemControlPayloadHeader::LENGTH));
                control_index = hdr.descriptor_index.get();
                // The error is the answer a client refresh gets.
                auto const entity_it = entities_.find(completed->entity_id);
                if (entity_it != entities_.end()) {
                    if (auto* const spec = entity_it->second.find(hdr.descriptor_index.get()); spec != nullptr) {
                        spec->refresh_pending = false;
                    }
                }
            }
            char const* const reason = completed->delivery == atdecc::AemCommandDelivery::Responded ? "rejected" : "unreachable";
            broadcast(message_error(reason, completed->entity_id, control_index));
        }
        return;
    }
    if (auto const* unsolicited = std::get_if<UnsolicitedEvent>(&event)) {
        if (unsolicited->command_type == AEM_COMMAND_SET_CONTROL && unsolicited->aem_status == AEM_STATUS_SUCCESS) {
            handle_control_response(
                unsolicited->entity_id, unsolicited->aem_status, unsolicited->response, ValueSource::unsolicited);
        }
        if (unsolicited->command_type == AEM_COMMAND_SET_SIGNAL_SELECTOR && unsolicited->aem_status == AEM_STATUS_SUCCESS) {
            handle_selector_response(unsolicited->entity_id, unsolicited->response);
        }
        if (unsolicited->command_type == AEM_COMMAND_SET_MATRIX && unsolicited->aem_status == AEM_STATUS_SUCCESS) {
            handle_matrix_response(unsolicited->entity_id, unsolicited->response, false);
        }
        if (unsolicited->command_type == AEM_COMMAND_SET_NAME && unsolicited->aem_status == AEM_STATUS_SUCCESS) {
            handle_name_response(unsolicited->entity_id, unsolicited->response);
        }
        if (unsolicited->command_type == AEM_COMMAND_GET_AVB_INFO && unsolicited->aem_status == AEM_STATUS_SUCCESS) {
            handle_avb_info_response(unsolicited->entity_id, unsolicited->response);
            // An entity that pushes its AVB info may have changed the
            // descriptor's gPTP fields with it (the proxies do, when their
            // mirror reads the device): re-read it.
            auto const entity_it = entities_.find(unsolicited->entity_id);
            if (entity_it != entities_.end() && unsolicited->response.size() >= AemAvbInfoPayload::LENGTH) {
                AemAvbInfoPayload p{};
                span_load(p, make_const_span(unsolicited->response).first(AemAvbInfoPayload::LENGTH));
                auto const index = p.descriptor_index.get();
                for (auto const& iface : entity_it->second.interfaces) {
                    if (iface.index == index) {
                        request_interface_descriptor(unsolicited->entity_id, iface, now_ns);
                    }
                }
            }
        }
        return;
    }
}

void WidgetBridge::handle_control_response(
    Eui64 const& entity_id, std::uint8_t aem_status, std::span<std::uint8_t const> response, ValueSource source)
{
    (void)aem_status;
    if (response.size() < AemControlPayloadHeader::LENGTH) {
        return;
    }
    AemControlPayloadHeader hdr{};
    span_load(hdr, response.first(AemControlPayloadHeader::LENGTH));
    if (hdr.descriptor_type.get() != DESCRIPTOR_CONTROL) {
        return;
    }
    auto const entity_it = entities_.find(entity_id);
    if (entity_it == entities_.end()) {
        return;
    }
    auto* const spec = entity_it->second.find(hdr.descriptor_index.get());
    if (spec == nullptr) {
        return;
    }
    auto const values = decode_control_values(spec->descriptor, response.subspan(AemControlPayloadHeader::LENGTH));
    if (!values.has_value()) {
        return;
    }
    // The sweep and unsolicited streams would otherwise flood clients
    // with values they already hold; a SET's confirmation never
    // suppresses (the setter deserves the echo even when nothing moved),
    // and neither does the GET result a client asked for by name — a
    // refresh that answers with silence is indistinguishable from one
    // that was lost.
    bool const requested = source == ValueSource::get_result && spec->refresh_pending;
    if (source == ValueSource::get_result) {
        spec->refresh_pending = false;
    }
    if (source != ValueSource::set_echo && !requested && spec->projection.current == *values) {
        return;
    }
    spec->projection.current = *values;
    broadcast_scoped(entity_id, *spec, message_value(entity_id, *spec));
}

void WidgetBridge::handle_selector_response(Eui64 const& entity_id, std::span<std::uint8_t const> response)
{
    // GET/SET_SIGNAL_SELECTOR responses and unsolicited notifications all
    // carry AemSignalSelectorPayload (reserved doublet may be absent).
    if (response.size() < AemSignalSelectorPayload::LENGTH - 2) {
        return;
    }
    AemSignalSelectorPayload payload{};
    span_load_padded(payload, response);
    if (payload.descriptor_type.get() != DESCRIPTOR_SIGNAL_SELECTOR) {
        return;
    }
    auto const entity_it = entities_.find(entity_id);
    if (entity_it == entities_.end()) {
        return;
    }
    auto* const spec = entity_it->second.find_selector(payload.descriptor_index.get());
    if (spec == nullptr) {
        return;
    }
    spec->projection.current.clear();
    for (std::size_t i = 0; i < spec->sources.size(); ++i) {
        if (spec->sources[i] == payload.source) {
            spec->projection.current.push_back(double(i));
            break;
        }
    }
    broadcast_scoped(entity_id, *spec, message_value(entity_id, *spec));
}

void WidgetBridge::request_matrix_chunk(Eui64 const& entity_id, WidgetSpec& spec, std::int64_t now_ns)
{
    if (spec.grid_next_row >= spec.matrix_height) {
        spec.grid_loaded = true;
        return;
    }
    // Row-sliced region that fits the AECP payload budget (~450 bytes of
    // values after the two headers).
    auto const elem = control_value_element_size(spec.matrix_value_type);
    auto const rows_per_chunk =
        std::max<std::uint16_t>(1, std::uint16_t(450 / std::max<std::size_t>(1, std::size_t(spec.matrix_width) * elem)));
    auto const rows = std::min<std::uint16_t>(rows_per_chunk, std::uint16_t(spec.matrix_height - spec.grid_next_row));

    ControllerAction action{};
    action.kind = ControllerActionKind::GetMatrix;
    action.request.talker_entity_id = entity_id;
    action.request.desc_index = spec.control_index;
    auto push16 = [&](std::uint16_t v) {
        action.request.control_values.push_back(std::uint8_t(v >> 8));
        action.request.control_values.push_back(std::uint8_t(v & 0xFF));
    };
    push16(0);                   // column
    push16(spec.grid_next_row);  // row
    push16(spec.matrix_width);   // width
    push16(rows);                // height
    push16(0);                   // rep off, horizontal, count 0 = whole region
    push16(0);                   // item_offset
    ctrl_.dispatch(action, now_ns);
}

void WidgetBridge::handle_matrix_response(Eui64 const& entity_id, std::span<std::uint8_t const> response, bool from_get)
{
    // 4-byte descriptor header + 12-byte region header + values.
    constexpr std::size_t REGION_HEADER = 12;
    if (response.size() < AemControlPayloadHeader::LENGTH + REGION_HEADER) {
        return;
    }
    AemControlPayloadHeader hdr{};
    span_load(hdr, response.first(AemControlPayloadHeader::LENGTH));
    if (hdr.descriptor_type.get() != DESCRIPTOR_MATRIX) {
        return;
    }
    auto const entity_it = entities_.find(entity_id);
    if (entity_it == entities_.end()) {
        return;
    }
    auto* const spec = entity_it->second.find_matrix(hdr.descriptor_index.get());
    if (spec == nullptr || spec->matrix_width == 0) {
        return;
    }

    auto const region = response.subspan(AemControlPayloadHeader::LENGTH);
    auto read16 = [&](std::size_t at) { return std::uint16_t((region[at] << 8) | region[at + 1]); };
    auto const column = read16(0);
    auto const row = read16(2);
    auto const width = read16(4);
    auto const height = read16(6);
    auto const rep_dir_count = read16(8);
    auto const item_offset = read16(10);
    bool const rep = (rep_dir_count & 0x8000U) != 0;
    auto const direction = std::uint16_t((rep_dir_count >> 14) & 0x1U);
    auto const count = std::uint16_t(rep_dir_count & 0x3FFFU);

    auto const values = decode_linear_elements(spec->matrix_value_type, region.subspan(REGION_HEADER));
    if (!values.has_value() || values->empty() || width == 0 || height == 0 || column + width > spec->matrix_width ||
        row + height > spec->matrix_height) {
        return;
    }
    auto& grid = spec->projection.current;
    if (grid.size() != std::size_t(spec->matrix_width) * spec->matrix_height) {
        grid.assign(std::size_t(spec->matrix_width) * spec->matrix_height, spec->projection.default_value);
    }

    // Walk the region in `direction` order (Clause 7-146), skipping
    // item_offset cells; with rep the values repeat to fill the region.
    std::size_t const region_cells = std::size_t(width) * height;
    std::size_t const limit = (count == 0) ? region_cells : count;
    std::size_t value_at = 0;
    for (std::size_t i = item_offset; i < region_cells; ++i) {
        if (!rep && value_at >= values->size()) {
            break;
        }
        if (value_at >= limit) {
            break;
        }
        std::uint16_t col;
        std::uint16_t r;
        if (direction == 0) {
            col = std::uint16_t(column + (i % width));
            r = std::uint16_t(row + (i / width));
        } else {
            r = std::uint16_t(row + (i % height));
            col = std::uint16_t(column + (i / height));
        }
        grid[(std::size_t(r) * spec->matrix_width) + col] = (*values)[value_at % values->size()];
        ++value_at;
    }

    // Fan the region out as contiguous runs (full-width regions collapse
    // to one run; anything else goes row by row).
    if (column == 0 && width == spec->matrix_width && direction == 0 && !rep && item_offset == 0) {
        auto const at = std::size_t(row) * spec->matrix_width;
        auto const n = std::min<std::size_t>(value_at, std::size_t(width) * height);
        broadcast(message_matrix_run(entity_id, *spec, at, std::span<double const>{grid.data() + at, n}));
    } else {
        for (std::uint16_t r = row; r < row + height; ++r) {
            auto const at = (std::size_t(r) * spec->matrix_width) + column;
            broadcast_scoped(
                entity_id, *spec, message_matrix_run(entity_id, *spec, at, std::span<double const>{grid.data() + at, width}));
        }
    }

    // A GET chunk advances the initial load cursor.
    if (from_get && !spec->grid_loaded && row == spec->grid_next_row) {
        spec->grid_next_row = std::uint16_t(row + height);
        request_matrix_chunk(entity_id, *spec, 0);
    }
}

void WidgetBridge::reconcile_entities(std::int64_t now_ns)
{
    auto const display = ctrl_.get_display_entities();

    std::set<Eui64> live;
    for (auto const& info : display) {
        live.insert(info.entity_id);
        if (!entities_.contains(info.entity_id) && !pending_.contains(info.entity_id)) {
            pending_.insert(info.entity_id);
        }
    }

    // Departures: announce and drop (pending ones just drop).
    for (auto it = entities_.begin(); it != entities_.end();) {
        if (!live.contains(it->first)) {
            broadcast(message_departed(it->first));
            it = entities_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(pending_, [&](Eui64 const& id) { return !live.contains(id); });
    std::erase_if(strings_deadline_, [&](auto const& entry) { return !live.contains(entry.first); });

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (try_build_entity(*it, now_ns)) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

auto WidgetBridge::try_build_entity(Eui64 const& entity_id, std::int64_t now_ns) -> bool
{
    auto const raws = ctrl_.cached_descriptors(entity_id);

    // Localized-name resolution (Clause 7.3.7): the localized string is
    // the manufacturer's fixed name and object_name the user-relabelable
    // sticker, so the localized string wins whenever it is authored —
    // the GALAXY and JOYNED devices name nearly everything this way and
    // leave object_name empty. The first LOCALE's base_strings anchors
    // the reference's 13-bit STRINGS index.
    std::uint16_t strings_base = 0;
    std::size_t locale_strings = 0;  ///< STRINGS the first LOCALE advertises
    std::size_t strings_cached = 0;
    for (auto const& raw : raws) {
        if (raw.type == DESCRIPTOR_LOCALE && locale_strings == 0 && raw.data.size() >= DescriptorLocale::LENGTH) {
            DescriptorLocale locale{};
            span_load_padded(locale, make_const_span(raw.data));
            strings_base = locale.base_strings.get();
            locale_strings = locale.number_of_strings.get();
        } else if (raw.type == DESCRIPTOR_STRINGS) {
            ++strings_cached;
        }
    }
    auto localized_of = [&](std::uint16_t localized_description) -> std::string {
        auto const ref = parse_localized_description(localized_description);
        if (!ref.has_value()) {
            return {};
        }
        auto const index = std::uint16_t(strings_base + ref->strings_descriptor_index);
        for (auto const& raw : raws) {
            if (raw.type == DESCRIPTOR_STRINGS && raw.index == index && raw.data.size() >= DescriptorStrings::LENGTH) {
                DescriptorStrings str{};
                span_load_padded(str, make_const_span(raw.data));
                return std::string{str.as_string_array()[ref->string_offset]->as_string_view()};
            }
        }
        return {};
    };
    // The display preference: object_name (the user's SET_NAME sticker)
    // when non-blank, else the localized manufacturer name. Both travel
    // in the widget spec so the site can toggle and show either.
    auto display_name = [&](std::uint16_t localized_description, std::string_view object_name) -> std::string {
        if (!object_name.empty()) {
            return std::string{object_name};
        }
        return localized_of(localized_description);
    };

    std::span<std::uint8_t const> entity_bytes;
    std::span<std::uint8_t const> config_bytes;
    std::vector<RawDescriptor const*> controls;
    std::vector<RawDescriptor const*> selectors;
    std::vector<RawDescriptor const*> matrices;
    // CONTROL_BLOCKs are the AEM grouping structure (a name spanning
    // [base_control, base_control + number_of_controls)); the UI's
    // channel hierarchy comes straight from them.
    struct BlockRange
    {
        std::string name;
        std::uint16_t base;
        std::uint16_t count;
    };
    std::vector<BlockRange> blocks;
    for (auto const& raw : raws) {
        if (raw.type == DESCRIPTOR_ENTITY && raw.index == 0) {
            entity_bytes = make_const_span(raw.data);
        } else if (raw.type == DESCRIPTOR_CONFIGURATION && raw.index == 0) {
            config_bytes = make_const_span(raw.data);
        } else if (raw.type == DESCRIPTOR_CONTROL) {
            controls.push_back(&raw);
        } else if (raw.type == DESCRIPTOR_SIGNAL_SELECTOR) {
            selectors.push_back(&raw);
        } else if (raw.type == DESCRIPTOR_MATRIX) {
            matrices.push_back(&raw);
        } else if (raw.type == DESCRIPTOR_CONTROL_BLOCK && raw.data.size() >= DescriptorControlBlock::MINIMUM_LENGTH) {
            DescriptorControlBlock block{};
            span_load_padded(block, make_const_span(raw.data));
            blocks.push_back(
                {display_name(block.localized_description.get(), block.object_name.as_string_view()),
                 block.base_control.get(),
                 block.number_of_controls.get()});
        }
    }
    auto group_of = [&](std::uint16_t control_index) -> std::string {
        for (auto const& block : blocks) {
            if (control_index >= block.base && control_index < block.base + block.count) {
                return block.name;
            }
        }
        return {};
    };
    if (entity_bytes.size() < DescriptorEntity::LENGTH || config_bytes.size() < DescriptorConfiguration::LENGTH) {
        return false;
    }

    // The CONFIGURATION's descriptor_counts table says how many CONTROLs
    // and SIGNAL_SELECTORs the crawl will eventually cache; wait for all.
    DescriptorConfiguration config{};
    span_load_padded(config, config_bytes);
    std::size_t advertised_controls = 0;
    std::size_t advertised_selectors = 0;
    std::size_t advertised_matrices = 0;
    auto const counts = std::min<std::size_t>(config.descriptor_counts_count.get(), DescriptorConfiguration::MAX_DESCRIPTOR_COUNTS);
    for (std::size_t i = 0; i < counts; ++i) {
        if (config.descriptor_counts[i].descriptor_type.get() == DESCRIPTOR_CONTROL) {
            advertised_controls = config.descriptor_counts[i].count.get();
        }
        if (config.descriptor_counts[i].descriptor_type.get() == DESCRIPTOR_SIGNAL_SELECTOR) {
            advertised_selectors = config.descriptor_counts[i].count.get();
        }
        if (config.descriptor_counts[i].descriptor_type.get() == DESCRIPTOR_MATRIX) {
            advertised_matrices = config.descriptor_counts[i].count.get();
        }
    }
    std::size_t advertised_blocks = 0;
    std::size_t advertised_locales = 0;
    for (std::size_t i = 0; i < counts; ++i) {
        if (config.descriptor_counts[i].descriptor_type.get() == DESCRIPTOR_CONTROL_BLOCK) {
            advertised_blocks = config.descriptor_counts[i].count.get();
        }
        if (config.descriptor_counts[i].descriptor_type.get() == DESCRIPTOR_LOCALE) {
            advertised_locales = config.descriptor_counts[i].count.get();
        }
    }
    if (controls.size() < advertised_controls || selectors.size() < advertised_selectors || matrices.size() < advertised_matrices ||
        blocks.size() < advertised_blocks) {
        return false;
    }

    // Names prefer the localized strings, so also wait for an advertised
    // LOCALE and its chased STRINGS — grace-bounded once everything else
    // is cached, building anyway when it lapses (a refusing device would
    // otherwise never appear) with object_name fallbacks. Waiting for
    // the LOCALE itself matters: the GALAXY counts LOCALE after its
    // CONTROLs, so the main gate passes before the LOCALE is read and
    // locale_strings would still be zero here.
    std::size_t locales_cached = 0;
    for (auto const& raw : raws) {
        if (raw.type == DESCRIPTOR_LOCALE) {
            ++locales_cached;
        }
    }
    bool const strings_pending = locales_cached < advertised_locales || (locale_strings > 0 && strings_cached < locale_strings);
    if (strings_pending) {
        auto const [it, first] = strings_deadline_.try_emplace(entity_id, now_ns + STRINGS_GRACE_NS);
        if (first || now_ns < it->second) {
            return false;
        }
    }
    strings_deadline_.erase(entity_id);

    DescriptorEntity entity_desc{};
    span_load_padded(entity_desc, entity_bytes);

    EntityWidgets entity;
    entity.entity_id = entity_id;
    entity.entity_model_id = entity_desc.entity_model_id.to_uint64();
    entity.name = std::string{entity_desc.entity_name.as_string_view()};
    if (entity.name.empty()) {
        entity.name = std::string{ieee::to_string(entity_id).view()};
    }
    // Extra ENTITY-descriptor detail for the client header. vendor/model
    // are localized-string refs (resolved through this entity's LOCALE/
    // STRINGS); the rest are inline AtdeccStrings / counts.
    entity.vendor = localized_of(entity_desc.vendor_name_string.get());
    entity.model = localized_of(entity_desc.model_name_string.get());
    entity.firmware = std::string{entity_desc.firmware_version.as_string_view()};
    entity.serial = std::string{entity_desc.serial_number.as_string_view()};
    entity.group = std::string{entity_desc.group_name.as_string_view()};
    entity.talker_sources = entity_desc.talker_stream_sources.get();
    entity.listener_sinks = entity_desc.listener_stream_sinks.get();
    entity.configurations = entity_desc.configurations_count.get();
    entity.current_config = entity_desc.current_configuration.get();

    // AVB_INTERFACEs: the static half of the interface panel. The live
    // half (GET_AVB_INFO) is read once unsolicited registration confirms.
    for (auto const& raw : raws) {
        if (raw.type != DESCRIPTOR_AVB_INTERFACE || raw.data.size() < DescriptorAvbInterface::MINIMUM_LENGTH) {
            continue;
        }
        DescriptorAvbInterface d{};
        span_load_padded(d, make_const_span(raw.data));
        InterfaceInfo iface{};
        iface.index = raw.index;
        iface.name = display_name(d.localized_description.get(), d.object_name.as_string_view());
        fill_interface_fields(iface, d);
        entity.interfaces.push_back(std::move(iface));
    }
    std::sort(entity.interfaces.begin(), entity.interfaces.end(), [](InterfaceInfo const& a, InterfaceInfo const& b) {
        return a.index < b.index;
    });

    // Stable ids: the nanoavb symbol table over the full cached model.
    std::vector<nanoavb::AemRawDescriptor> symbol_input;
    symbol_input.reserve(raws.size());
    for (auto const& raw : raws) {
        symbol_input.push_back({0, raw.type, raw.index, raw.data});
    }
    auto const symbols = nanoavb::derive_aem_symbols(symbol_input);
    auto symbol_of = [&](std::uint16_t type, std::uint16_t index) -> std::string {
        for (std::size_t i = 0; i < symbol_input.size(); ++i) {
            if (symbol_input[i].descriptor_type == type && symbol_input[i].descriptor_index == index && i < symbols.size()) {
                return symbols[i].symbol;
            }
        }
        return {};
    };
    // A source's display label: the referenced descriptor's localized
    // string, else its object_name (every named descriptor keeps
    // object_name at offset 4 and localized_description right after it),
    // else "TYPE[i]".
    auto name_of_descriptor = [&](std::uint16_t type, std::uint16_t index) -> std::string {
        for (auto const& raw : raws) {
            if (raw.type == type && raw.index == index && raw.data.size() >= 4 + AtdeccString::LENGTH + 2) {
                AtdeccString name{};
                std::memcpy(&name, raw.data.data() + 4, sizeof name);
                auto const at = 4 + AtdeccString::LENGTH;
                auto const localized = std::uint16_t((raw.data[at] << 8) | raw.data[at + 1]);
                auto resolved = display_name(localized, name.as_string_view());
                if (!resolved.empty()) {
                    return resolved;
                }
            }
        }
        return {};
    };

    for (auto const* raw : controls) {
        WidgetSpec spec;
        spec.control_index = raw->index;
        span_load_padded(spec.descriptor, make_const_span(raw->data));
        spec.projection = project_control(spec.descriptor);
        spec.symbol = symbol_of(DESCRIPTOR_CONTROL, raw->index);
        spec.mfr_name = localized_of(spec.descriptor.localized_description.get());
        spec.authored_name = std::string{spec.descriptor.object_name.as_string_view()};
        spec.name = display_name(spec.descriptor.localized_description.get(), spec.descriptor.object_name.as_string_view());
        if (spec.name.empty()) {
            spec.name = spec.symbol;
        }
        spec.group = group_of(raw->index);
        // A multi-value linear control names its values through the
        // per-entry localized_string refs; ship them only when at least
        // one is authored (single-value controls name the control itself).
        if (spec.projection.count > 1) {
            bool any = false;
            for (auto const ref : spec.projection.value_string_refs) {
                spec.value_labels.push_back(localized_of(ref));
                any = any || !spec.value_labels.back().empty();
            }
            if (!any) {
                spec.value_labels.clear();
            }
        }
        apply_labels(entity.entity_model_id, spec);
        entity.widgets.push_back(std::move(spec));
    }

    for (auto const* raw : selectors) {
        if (raw->data.size() < DescriptorSignalSelector::LENGTH) {
            continue;
        }
        DescriptorSignalSelector desc{};
        span_load_padded(desc, make_const_span(raw->data));

        WidgetSpec spec;
        spec.control_index = raw->index;
        spec.is_selector = true;
        spec.symbol = symbol_of(DESCRIPTOR_SIGNAL_SELECTOR, raw->index);
        spec.mfr_name = localized_of(desc.localized_description.get());
        spec.authored_name = std::string{desc.object_name.as_string_view()};
        spec.name = display_name(desc.localized_description.get(), desc.object_name.as_string_view());
        if (spec.name.empty()) {
            spec.name = spec.symbol;
        }

        // The authored source trailer; options index into it.
        auto const offset = std::max<std::size_t>(desc.sources_offset.get(), DescriptorSignalSelector::LENGTH);
        for (std::size_t i = 0; i < desc.number_of_sources.get(); ++i) {
            auto const at = offset + (i * SignalSource::LENGTH);
            if (at + SignalSource::LENGTH > raw->data.size()) {
                break;
            }
            SignalSource source{};
            span_load(source, make_const_span(raw->data).subspan(at, SignalSource::LENGTH));
            spec.sources.push_back(source);
        }

        auto& p = spec.projection;
        p.kind = WidgetKind::select;
        p.count = 1;
        p.read_only = spec.sources.empty();  // unauthored selector: nothing to choose
        for (std::size_t i = 0; i < spec.sources.size(); ++i) {
            auto const& source = spec.sources[i];
            p.options.push_back(double(i));
            auto label = name_of_descriptor(source.signal_type.get(), source.signal_index.get());
            if (label.empty()) {
                label =
                    std::format("{}[{}]", descriptor_type_name(source.signal_type.get()), std::size_t{source.signal_index.get()});
            }
            p.option_labels.push_back(std::move(label));
            if (source == desc.current_signal) {
                p.current.assign(1, double(i));
            }
        }
        apply_labels(entity.entity_model_id, spec);
        entity.widgets.push_back(std::move(spec));
    }

    for (auto const* raw : matrices) {
        if (raw->data.size() < DescriptorMatrix::LENGTH) {
            continue;
        }
        DescriptorMatrix desc{};
        span_load_padded(desc, make_const_span(raw->data));

        WidgetSpec spec;
        spec.control_index = raw->index;
        spec.is_matrix = true;
        spec.symbol = symbol_of(DESCRIPTOR_MATRIX, raw->index);
        spec.mfr_name = localized_of(desc.localized_description.get());
        spec.authored_name = std::string{desc.object_name.as_string_view()};
        spec.name = display_name(desc.localized_description.get(), desc.object_name.as_string_view());
        if (spec.name.empty()) {
            spec.name = spec.symbol;
        }
        spec.matrix_width = desc.width.get();
        spec.matrix_height = desc.height.get();
        auto const bits = unpack_control_value_type(desc.control_value_type.get());
        spec.matrix_value_type = bits.value_type;

        // The value_details trailer is a linear entry (min/max/step/
        // default + unit) — stage it into a CONTROL shape and reuse the
        // linear projection for the cell metadata.
        DescriptorControl staged{};
        staged.control_value_type = desc.control_value_type.get();
        staged.number_of_values = desc.number_of_values.get();
        staged.values_offset = std::uint16_t(DescriptorControl::LENGTH);
        auto const values_at = std::max<std::size_t>(desc.values_offset.get(), DescriptorMatrix::LENGTH);
        if (raw->data.size() > values_at) {
            auto const n = std::min<std::size_t>(raw->data.size() - values_at, DescriptorControl::MAX_VALUE_DETAILS);
            std::memcpy(staged.value_details.data(), raw->data.data() + values_at, n);
        }
        auto const cell = project_control(staged);
        spec.projection.kind = WidgetKind::matrix;
        spec.projection.value_type = bits.value_type;
        spec.projection.read_only = bits.read_only;
        spec.projection.minimum = cell.minimum;
        spec.projection.maximum = cell.maximum;
        spec.projection.step = cell.step;
        spec.projection.default_value = cell.default_value;
        spec.projection.unit_code = cell.unit_code;
        spec.projection.unit_multiplier = cell.unit_multiplier;
        spec.projection.current.assign(std::size_t(spec.matrix_width) * spec.matrix_height, cell.default_value);
        if (labels_ != nullptr) {
            spec.row_labels = labels_->matrix_row_labels(entity.entity_model_id, spec.control_index, spec.matrix_height);
        }
        entity.widgets.push_back(std::move(spec));
    }

    std::sort(entity.widgets.begin(), entity.widgets.end(), [](WidgetSpec const& a, WidgetSpec const& b) {
        auto const rank = [](WidgetSpec const& w) { return w.is_matrix ? 2 : (w.is_selector ? 1 : 0); };
        if (rank(a) != rank(b)) {
            return rank(a) < rank(b);  // controls, then selectors, then matrices
        }
        return a.control_index < b.control_index;
    });

    auto const [it, inserted] = entities_.insert_or_assign(entity_id, std::move(entity));
    (void)inserted;

    // Live updates from other controllers, from now on.
    ControllerAction reg{};
    reg.kind = ControllerActionKind::RegisterUnsolicited;
    reg.request.talker_entity_id = entity_id;
    ctrl_.dispatch(reg, now_ns);

    // Start the chunked grid loads (one region in flight per matrix).
    for (auto& w : it->second.widgets) {
        if (w.is_matrix) {
            request_matrix_chunk(entity_id, w, now_ns);
        }
    }

    broadcast(message_entity(it->second));
    for (auto const& iface : it->second.interfaces) {
        broadcast(message_interface(it->second, iface));
    }
    if (it->second.jumbo()) {
        // A rebuilt jumbo entity invalidates whatever groups clients had
        // open (indices may have renumbered) — they re-open on demand.
        for (auto& [slot, client] : clients_) {
            client.open_groups.erase(entity_id.to_uint64());
        }
        for (auto const& frame : message_groups(it->second)) {
            broadcast(frame);
        }
    } else {
        for (auto const& w : it->second.widgets) {
            broadcast(message_widget(it->second, w));
        }
    }
    broadcast(message_entity_done(it->second));
    return true;
}

void WidgetBridge::apply_labels(std::uint64_t entity_model_id, WidgetSpec& spec)
{
    auto& p = spec.projection;
    if (labels_ == nullptr) {
        return;
    }

    if (spec.is_selector) {
        // Selector options are source-list positions; the provider labels
        // them positionally (option value == position).
        if (p.options.empty()) {
            return;
        }
        auto const enumerated = labels_->options_for(entity_model_id, spec.control_index, true);
        if (enumerated.empty()) {
            return;
        }
        for (std::size_t position = 0; position < p.options.size() && position < p.option_labels.size(); ++position) {
            for (auto const& option : enumerated) {
                if (option.value == double(position)) {
                    p.option_labels[position] = option.label;
                    break;
                }
            }
        }
        return;
    }

    if (p.count != 1) {
        return;
    }
    // Only single-value integer linear projections (slider, toggle, or a
    // grid-derived select) can carry a schema enumeration; selector-family
    // controls already own their options.
    bool const linear_integer = p.value_type <= atdecc::aem::CONTROL_LINEAR_UINT64;
    if (!linear_integer || p.kind == WidgetKind::opaque || p.kind == WidgetKind::text) {
        return;
    }
    auto const enumerated = labels_->options_for(entity_model_id, spec.control_index, false);
    if (enumerated.empty()) {
        return;
    }
    // An authoritative enumeration beats the range heuristic: this
    // control is a select over exactly these values, however wide the
    // range looked (the GALAXY's delay-integration type has 162).
    p.kind = WidgetKind::select;
    p.options.clear();
    p.option_labels.clear();
    p.options.reserve(enumerated.size());
    p.option_labels.reserve(enumerated.size());
    for (auto const& option : enumerated) {
        p.options.push_back(option.value);
        p.option_labels.push_back(option.label);
    }
}

// ---- WS ---------------------------------------------------------------------

void WidgetBridge::on_ws_open(std::size_t slot, http::HttpRequest const& request)
{
    (void)request;
    clients_[slot];  // create
    send_to(slot, message_hello());
    replay_entities_to(slot);
}

void WidgetBridge::on_ws_closed(std::size_t slot)
{
    clients_.erase(slot);
}

void WidgetBridge::on_ws_message(std::size_t slot, std::span<std::uint8_t const> payload, bool is_text)
{
    if (!is_text) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const msg = json_scan(std::string_view{reinterpret_cast<char const*>(payload.data()), payload.size()});
    if (!msg.has_value()) {
        send_to(slot, message_error("malformed message", Eui64{}, -1));
        flush_outboxes();
        return;
    }
    auto const op = msg->string_of("op").value_or("");
    auto const entity = msg->hex64_of("entity");
    auto const control = msg->number_of("control");
    auto const d = msg->string_of("d").value_or("ctl");
    auto const address = d == "sel" ? WidgetAddress::selector : (d == "mx" ? WidgetAddress::matrix : WidgetAddress::control);
    auto const cell = std::int64_t(msg->number_of("at").value_or(-1.0));

    if (op == "refresh") {
        replay_entities_to(slot);
    } else if ((op == "open" || op == "close") && entity.has_value() && msg->string_of("group").has_value()) {
        Eui64 group_target{};
        group_target.from_uint64(*entity);
        handle_client_group(slot, group_target, std::string(*msg->string_of("group")), op == "open");
    } else if (op == "get" && entity.has_value() && control.has_value()) {
        handle_client_get(slot, *entity, std::int64_t(*control), address, 0);
    } else if (op == "set" && entity.has_value() && control.has_value() && msg->array_of("values") != nullptr) {
        auto const& values = msg->array_of("values")->numbers;
        handle_client_set(slot, *entity, std::int64_t(*control), address, cell, values, 0);
    } else if (op == "rename" && entity.has_value() && control.has_value() && msg->string_of("name").has_value()) {
        handle_client_rename(slot, *entity, std::int64_t(*control), address, *msg->string_of("name"), 0);
    } else {
        send_to(slot, message_error("unknown or incomplete op", Eui64{}, -1));
    }
    flush_outboxes();
}

void WidgetBridge::handle_client_get(
    std::size_t slot, std::uint64_t entity_id, std::int64_t control_index, WidgetAddress address, std::int64_t now_ns)
{
    Eui64 id{};
    id.from_uint64(entity_id);
    auto const it = entities_.find(id);
    WidgetSpec* spec = nullptr;
    if (it != entities_.end()) {
        switch (address) {
            case WidgetAddress::control:
                spec = it->second.find(std::uint16_t(control_index));
                break;
            case WidgetAddress::selector:
                spec = it->second.find_selector(std::uint16_t(control_index));
                break;
            case WidgetAddress::matrix:
                spec = it->second.find_matrix(std::uint16_t(control_index));
                break;
        }
    }
    if (spec == nullptr) {
        send_to(slot, message_error("unknown control", id, control_index));
        return;
    }
    if (address == WidgetAddress::matrix) {
        // A matrix "get" restarts the chunked grid refresh.
        spec->grid_next_row = 0;
        spec->grid_loaded = false;
        request_matrix_chunk(id, *spec, now_ns);
        return;
    }
    if (address == WidgetAddress::control) {
        spec->refresh_pending = true;  // answer this one even if unchanged
    }
    ControllerAction action{};
    action.kind = address == WidgetAddress::selector ? ControllerActionKind::GetSignalSelector : ControllerActionKind::GetControl;
    action.request.talker_entity_id = id;
    action.request.desc_index = std::uint16_t(control_index);
    ctrl_.dispatch(action, now_ns);
}

void WidgetBridge::handle_client_set(
    std::size_t slot,
    std::uint64_t entity_id,
    std::int64_t control_index,
    WidgetAddress address,
    std::int64_t cell,
    std::span<double const> values,
    std::int64_t now_ns)
{
    Eui64 id{};
    id.from_uint64(entity_id);
    auto const it = entities_.find(id);
    WidgetSpec* spec = nullptr;
    if (it != entities_.end()) {
        switch (address) {
            case WidgetAddress::control:
                spec = it->second.find(std::uint16_t(control_index));
                break;
            case WidgetAddress::selector:
                spec = it->second.find_selector(std::uint16_t(control_index));
                break;
            case WidgetAddress::matrix:
                spec = it->second.find_matrix(std::uint16_t(control_index));
                break;
        }
    }
    if (spec == nullptr) {
        send_to(slot, message_error("unknown control", id, control_index));
        return;
    }
    if (spec->projection.read_only) {
        send_to(slot, message_error("read-only control", id, control_index));
        return;
    }

    if (address == WidgetAddress::matrix) {
        // One crosspoint: `at` addresses the cell (row * width + column),
        // one value, range-checked against the cell metadata.
        auto const cells = std::size_t(spec->matrix_width) * spec->matrix_height;
        if (values.size() != 1 || cell < 0 || std::size_t(cell) >= cells || values[0] < spec->projection.minimum ||
            values[0] > spec->projection.maximum) {
            send_to(slot, message_error("cell or value out of range", id, control_index));
            return;
        }
        auto const encoded = encode_linear_element(spec->matrix_value_type, values[0]);
        if (!encoded.has_value()) {
            send_to(slot, message_error("unsupported matrix value type", id, control_index));
            return;
        }
        ControllerAction action{};
        action.kind = ControllerActionKind::SetMatrix;
        action.request.talker_entity_id = id;
        action.request.desc_index = std::uint16_t(control_index);
        auto push16 = [&](std::uint16_t v) {
            action.request.control_values.push_back(std::uint8_t(v >> 8));
            action.request.control_values.push_back(std::uint8_t(v & 0xFF));
        };
        push16(std::uint16_t(std::size_t(cell) % spec->matrix_width));  // column
        push16(std::uint16_t(std::size_t(cell) / spec->matrix_width));  // row
        push16(1);                                                      // width
        push16(1);                                                      // height
        push16(1);                                                      // rep off, horizontal, count 1
        push16(0);                                                      // item_offset
        for (auto const b : *encoded) {
            action.request.control_values.push_back(b);
        }
        ctrl_.dispatch(action, now_ns);
        return;
    }

    if (address == WidgetAddress::selector) {
        // The value is an index into the authored source list.
        if (values.size() != 1 || values[0] < 0.0 || values[0] >= double(spec->sources.size()) ||
            values[0] != double(std::int64_t(values[0]))) {
            send_to(slot, message_error("not an authored source index", id, control_index));
            return;
        }
        auto const& source = spec->sources[std::size_t(values[0])];
        ControllerAction action{};
        action.kind = ControllerActionKind::SetSignalSelector;
        action.request.talker_entity_id = id;
        action.request.desc_index = std::uint16_t(control_index);
        action.request.signal_type = source.signal_type.get();
        action.request.signal_index = source.signal_index.get();
        action.request.signal_output = source.signal_output.get();
        ctrl_.dispatch(action, now_ns);
        return;
    }

    auto const encoded = encode_control_values(spec->descriptor, values);
    if (!encoded.has_value()) {
        send_to(slot, message_error("values out of range or wrong shape", id, control_index));
        return;
    }
    ControllerAction action{};
    action.kind = ControllerActionKind::SetControl;
    action.request.talker_entity_id = id;
    action.request.desc_index = std::uint16_t(control_index);
    action.request.control_values = *encoded;
    ctrl_.dispatch(action, now_ns);
    // The confirming `value` broadcast comes from the SET_CONTROL
    // response — the device is the source of truth, not the request.
}

void WidgetBridge::handle_client_rename(
    std::size_t slot,
    std::uint64_t entity_id,
    std::int64_t control_index,
    WidgetAddress address,
    std::string_view name,
    std::int64_t now_ns)
{
    Eui64 id{};
    id.from_uint64(entity_id);
    auto const it = entities_.find(id);
    WidgetSpec* spec = nullptr;
    if (it != entities_.end()) {
        switch (address) {
            case WidgetAddress::control:
                spec = it->second.find(std::uint16_t(control_index));
                break;
            case WidgetAddress::selector:
                spec = it->second.find_selector(std::uint16_t(control_index));
                break;
            case WidgetAddress::matrix:
                spec = it->second.find_matrix(std::uint16_t(control_index));
                break;
        }
    }
    if (spec == nullptr) {
        send_to(slot, message_error("unknown widget", id, control_index));
        return;
    }
    // SET_NAME on name_index 0 (object_name); an empty name asks the
    // entity to clear back to its authored name. The confirming widget
    // broadcast comes from the SET_NAME response, like every other SET.
    ControllerAction action{};
    action.kind = ControllerActionKind::SetName;
    action.request.talker_entity_id = id;
    action.request.desc_type =
        spec->is_selector ? DESCRIPTOR_SIGNAL_SELECTOR : (spec->is_matrix ? DESCRIPTOR_MATRIX : DESCRIPTOR_CONTROL);
    action.request.desc_index = spec->control_index;
    auto const n = std::min<std::size_t>(name.size(), AtdeccString::LENGTH);
    for (std::size_t i = 0; i < n; ++i) {
        action.request.control_values.push_back(std::uint8_t(name[i]));
    }
    ctrl_.dispatch(action, now_ns);
}

void WidgetBridge::handle_name_response(Eui64 const& entity_id, std::span<std::uint8_t const> response)
{
    // The SET_NAME echo: 8-byte name header + the 64-byte name.
    if (response.size() < AemNameCommandPayload::LENGTH + AtdeccString::LENGTH) {
        return;
    }
    AemNameCommandPayload hdr{};
    span_load(hdr, response.first(AemNameCommandPayload::LENGTH));
    if (hdr.name_index.get() != 0) {
        return;
    }
    auto const entity_it = entities_.find(entity_id);
    if (entity_it == entities_.end()) {
        return;
    }
    WidgetSpec* spec = nullptr;
    switch (hdr.descriptor_type.get()) {
        case DESCRIPTOR_CONTROL:
            spec = entity_it->second.find(hdr.descriptor_index.get());
            break;
        case DESCRIPTOR_SIGNAL_SELECTOR:
            spec = entity_it->second.find_selector(hdr.descriptor_index.get());
            break;
        case DESCRIPTOR_MATRIX:
            spec = entity_it->second.find_matrix(hdr.descriptor_index.get());
            break;
        default:
            break;
    }
    if (spec == nullptr) {
        return;
    }
    AtdeccString name{};
    std::memcpy(&name, response.data() + AemNameCommandPayload::LENGTH, sizeof name);
    // An empty SET_NAME cleared the entity back to its authored name (the
    // response echoes the command, so the effective name is ours to know).
    auto const object_name = std::string{name.as_string_view()};
    auto const effective = object_name.empty() ? spec->authored_name : object_name;
    if (!spec->is_selector && !spec->is_matrix) {
        spec->descriptor.object_name = AtdeccString{effective};
    }
    // Re-resolve the display preference — sticker, else manufacturer
    // name, else the symbol — and fan the updated spec out. Unchanged
    // stays quiet: a rename arrives twice (the completion AND the
    // entity's unsolicited echo of the same command), and the second
    // would otherwise re-broadcast an identical spec.
    auto const display = !effective.empty() ? effective : (!spec->mfr_name.empty() ? spec->mfr_name : spec->symbol);
    if (display == spec->name) {
        return;
    }
    spec->name = display;
    broadcast_scoped(entity_id, *spec, message_widget(entity_it->second, *spec));
}

// ---- outbound ---------------------------------------------------------------

void WidgetBridge::send_to(std::size_t slot, std::string message)
{
    auto const it = clients_.find(slot);
    if (it != clients_.end()) {
        it->second.outbox.push_back(std::move(message));
    }
}

void WidgetBridge::broadcast(std::string const& message)
{
    for (auto& [slot, client] : clients_) {
        client.outbox.push_back(message);
    }
}

void WidgetBridge::flush_outboxes()
{
    for (auto& [slot, client] : clients_) {
        while (!client.outbox.empty()) {
            auto const& next = client.outbox.front();
            if (next.size() > max_ws_payload_) {
                // ws_send refuses an over-tx-buffer payload permanently, not
                // just while draining — retrying would wedge this client's
                // outbox forever (and spin the reactor). Drop it instead.
                fprintf(stderr, "[bridge] dropped %zu-byte ws message (> %zu tx capacity)\n", next.size(), max_ws_payload_);
                client.outbox.pop_front();
                continue;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            auto const bytes = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(next.data()), next.size()};
            if (!http_.ws_send(slot, bytes, true)) {
                break;  // still draining — retry next pump, order preserved
            }
            client.outbox.pop_front();
        }
    }
}

void WidgetBridge::replay_entities_to(std::size_t slot)
{
    for (auto const& [id, entity] : entities_) {
        send_to(slot, message_entity(entity));
        for (auto const& iface : entity.interfaces) {
            send_to(slot, message_interface(entity, iface));
            if (iface.live) {
                send_to(slot, message_avb_info(entity, iface));
            }
        }
        if (entity.jumbo()) {
            if (auto client = clients_.find(slot); client != clients_.end()) {
                client->second.open_groups.erase(entity.entity_id.to_uint64());
            }
            for (auto& frame : message_groups(entity)) {
                send_to(slot, std::move(frame));
            }
        } else {
            for (auto const& w : entity.widgets) {
                send_to(slot, message_widget(entity, w));
            }
        }
        send_to(slot, message_entity_done(entity));
    }
}

void WidgetBridge::broadcast_scoped(Eui64 const& entity_id, WidgetSpec const& spec, std::string const& message)
{
    auto const it = entities_.find(entity_id);
    if (it == entities_.end() || !it->second.jumbo()) {
        broadcast(message);
        return;
    }
    for (auto& [slot, client] : clients_) {
        auto const open = client.open_groups.find(entity_id.to_uint64());
        if (open != client.open_groups.end() && open->second.count(spec.group) != 0) {
            send_to(slot, message);
        }
    }
}

void WidgetBridge::replay_group_to(std::size_t slot, EntityWidgets const& entity, std::string const& group)
{
    for (auto const& w : entity.widgets) {
        if (w.group == group) {
            send_to(slot, message_widget(entity, w));
            if (!w.projection.current.empty()) {
                send_to(slot, message_value(entity.entity_id, w));
            }
        }
    }
    send_to(slot, message_entity_done(entity));
}

void WidgetBridge::handle_client_group(std::size_t slot, Eui64 const& entity_id, std::string const& group, bool open)
{
    auto const it = entities_.find(entity_id);
    if (it == entities_.end() || !it->second.jumbo()) {
        send_to(slot, message_error("unknown or non-jumbo entity", entity_id, -1));
        return;
    }
    auto client = clients_.find(slot);
    if (client == clients_.end()) {
        return;
    }
    if (open) {
        client->second.open_groups[entity_id.to_uint64()].insert(group);
        replay_group_to(slot, it->second, group);
    } else {
        client->second.open_groups[entity_id.to_uint64()].erase(group);
    }
}

// ---- messages ---------------------------------------------------------------

auto WidgetBridge::message_hello() const -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "hello");
    w.field("version", std::int64_t{1});
    w.field("entities", std::int64_t(entities_.size()));
    if (scene_endpoints_) {
        w.field("scenes", true);
    }
    w.end_object();
    return out;
}

//
// AVB interfaces: static detail from the crawl, live GET_AVB_INFO
//

void WidgetBridge::request_avb_info(Eui64 const& entity_id, InterfaceInfo& iface, std::int64_t now_ns)
{
    ControllerAction action{};
    action.kind = ControllerActionKind::GetAvbInfo;
    action.request.talker_entity_id = entity_id;
    action.request.desc_index = iface.index;
    ctrl_.dispatch(action, now_ns);
    iface.last_poll_ns = now_ns;
}

void WidgetBridge::fill_interface_fields(InterfaceInfo& iface, DescriptorAvbInterface const& d)
{
    iface.mac = d.mac_address;
    iface.clock_identity = d.clock_identity;
    iface.interface_flags = d.interface_flags.get();
    iface.priority1 = static_cast<std::uint8_t>(d.priority1);
    iface.clock_class = static_cast<std::uint8_t>(d.clock_class);
    iface.offset_scaled_log_variance = d.offset_scaled_log_variance.get();
    iface.clock_accuracy = static_cast<std::uint8_t>(d.clock_accuracy);
    iface.priority2 = static_cast<std::uint8_t>(d.priority2);
    iface.domain_number = static_cast<std::uint8_t>(d.domain_number);
    iface.log_sync_interval = static_cast<std::int8_t>(static_cast<std::uint8_t>(d.log_sync_interval));
    iface.log_announce_interval = static_cast<std::int8_t>(static_cast<std::uint8_t>(d.log_announce_interval));
    iface.log_pdelay_interval = static_cast<std::int8_t>(static_cast<std::uint8_t>(d.log_pdelay_interval));
    iface.port_number = d.port_number.get();
}

void WidgetBridge::request_interface_descriptor(Eui64 const& entity_id, InterfaceInfo const& iface, std::int64_t now_ns)
{
    ControllerAction action{};
    action.kind = ControllerActionKind::ReadDescriptor;
    action.request.talker_entity_id = entity_id;
    action.request.desc_type = DESCRIPTOR_AVB_INTERFACE;
    action.request.desc_index = iface.index;
    ctrl_.dispatch(action, now_ns);
}

void WidgetBridge::handle_interface_descriptor_response(Eui64 const& entity_id, std::span<std::uint8_t const> response)
{
    // AemReadDescriptorResponsePayload header, then the descriptor, which
    // names its own type and index.
    if (response.size() < AemReadDescriptorResponsePayload::LENGTH + DescriptorAvbInterface::MINIMUM_LENGTH) {
        return;
    }
    auto const payload = response.subspan(AemReadDescriptorResponsePayload::LENGTH);
    DescriptorAvbInterface d{};
    span_load_padded(d, payload);
    if (d.descriptor_type.get() != DESCRIPTOR_AVB_INTERFACE) {
        return;
    }
    auto const entity_it = entities_.find(entity_id);
    if (entity_it == entities_.end()) {
        return;
    }
    auto& entity = entity_it->second;
    auto const index = d.descriptor_index.get();
    auto const it = std::find_if(
        entity.interfaces.begin(), entity.interfaces.end(), [index](InterfaceInfo const& i) { return i.index == index; });
    if (it == entity.interfaces.end()) {
        return;
    }
    InterfaceInfo& iface = *it;
    InterfaceInfo const before = iface;
    fill_interface_fields(iface, d);
    bool const changed = before.mac != iface.mac || before.clock_identity != iface.clock_identity ||
        before.interface_flags != iface.interface_flags || before.priority1 != iface.priority1 ||
        before.clock_class != iface.clock_class || before.offset_scaled_log_variance != iface.offset_scaled_log_variance ||
        before.clock_accuracy != iface.clock_accuracy || before.priority2 != iface.priority2 ||
        before.domain_number != iface.domain_number || before.log_sync_interval != iface.log_sync_interval ||
        before.log_announce_interval != iface.log_announce_interval || before.log_pdelay_interval != iface.log_pdelay_interval ||
        before.port_number != iface.port_number;
    if (changed) {
        broadcast(message_interface(entity, iface));
    }
}

void WidgetBridge::pump_avb_info(std::int64_t now_ns)
{
    // The live state is cheap to poll (one small command per interface) and
    // most third-party devices never push it, so re-read it periodically;
    // entities that do push unsolicited GET_AVB_INFO arrive sooner anyway.
    for (auto& [entity_id, entity] : entities_) {
        for (auto& iface : entity.interfaces) {
            if (iface.last_poll_ns != 0 && now_ns - iface.last_poll_ns >= AVB_INFO_POLL_NS) {
                request_avb_info(entity_id, iface, now_ns);
            }
        }
    }
}

void WidgetBridge::handle_avb_info_response(Eui64 const& entity_id, std::span<std::uint8_t const> response)
{
    if (response.size() < AemAvbInfoPayload::LENGTH) {
        return;
    }
    AemAvbInfoPayload p{};
    span_load(p, response.first(AemAvbInfoPayload::LENGTH));
    auto const entity_it = entities_.find(entity_id);
    if (entity_it == entities_.end()) {
        return;
    }
    auto& entity = entity_it->second;
    auto const index = p.descriptor_index.get();
    auto const it = std::find_if(
        entity.interfaces.begin(), entity.interfaces.end(), [index](InterfaceInfo const& i) { return i.index == index; });
    if (it == entity.interfaces.end()) {
        return;
    }
    InterfaceInfo& iface = *it;
    InterfaceInfo const before = iface;
    iface.live = true;
    iface.gptp_grandmaster_id = p.gptp_grandmaster_id.to_eui64();
    iface.propagation_delay = p.propagation_delay.get();
    iface.gptp_domain_number = static_cast<std::uint8_t>(p.gptp_domain_number);
    iface.avb_flags = static_cast<std::uint8_t>(p.flags);
    iface.msrp_mappings.clear();
    auto const count = p.msrp_mappings_count.get();
    for (std::size_t i = 0; i < count; ++i) {
        auto const at = AemAvbInfoPayload::LENGTH + (i * 4);
        if (at + 4 > response.size()) {
            break;
        }
        iface.msrp_mappings.push_back(
            {.traffic_class = response[at],
             .priority = response[at + 1],
             .vlan_id = static_cast<std::uint16_t>((response[at + 2] << 8U) | response[at + 3])});
    }
    bool const changed = !before.live || before.gptp_grandmaster_id != iface.gptp_grandmaster_id ||
        before.propagation_delay != iface.propagation_delay || before.gptp_domain_number != iface.gptp_domain_number ||
        before.avb_flags != iface.avb_flags || before.msrp_mappings.size() != iface.msrp_mappings.size();
    if (changed) {
        broadcast(message_avb_info(entity, iface));
    }
}

auto WidgetBridge::message_interface(EntityWidgets const& e, InterfaceInfo const& iface) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "interface");
    w.field_hex64("entity", e.entity_id.to_uint64());
    w.field("interface", std::int64_t{iface.index});
    w.field("name", iface.name);
    w.field(
        "mac",
        std::format(
            "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            iface.mac.value[0],
            iface.mac.value[1],
            iface.mac.value[2],
            iface.mac.value[3],
            iface.mac.value[4],
            iface.mac.value[5]));
    w.field_hex64("clock_id", iface.clock_identity.to_uint64());
    w.field("flags", std::int64_t{iface.interface_flags});
    w.field("priority1", std::int64_t{iface.priority1});
    w.field("priority2", std::int64_t{iface.priority2});
    w.field("clock_class", std::int64_t{iface.clock_class});
    w.field("clock_accuracy", std::int64_t{iface.clock_accuracy});
    w.field("log_variance", std::int64_t{iface.offset_scaled_log_variance});
    w.field("domain", std::int64_t{iface.domain_number});
    w.field("log_sync", std::int64_t{iface.log_sync_interval});
    w.field("log_announce", std::int64_t{iface.log_announce_interval});
    w.field("log_pdelay", std::int64_t{iface.log_pdelay_interval});
    w.field("port", std::int64_t{iface.port_number});
    w.end_object();
    return out;
}

auto WidgetBridge::message_avb_info(EntityWidgets const& e, InterfaceInfo const& iface) -> std::string
{
    using namespace atdecc::aem::avb_info_flags;
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "avb_info");
    w.field_hex64("entity", e.entity_id.to_uint64());
    w.field("interface", std::int64_t{iface.index});
    w.field_hex64("gm", iface.gptp_grandmaster_id.to_uint64());
    w.field("prop_delay", std::int64_t{iface.propagation_delay});
    w.field("domain", std::int64_t{iface.gptp_domain_number});
    w.field("as_capable", (iface.avb_flags & AS_CAPABLE) != 0);
    w.field("gptp_enabled", (iface.avb_flags & GPTP_ENABLED) != 0);
    w.field("srp_enabled", (iface.avb_flags & SRP_ENABLED) != 0);
    if ((iface.avb_flags & AVTP_DOWN_VALID) != 0) {
        w.field("avtp_down", (iface.avb_flags & AVTP_DOWN) != 0);
    }
    // The MSRP mappings as three parallel arrays (traffic class, PCP, VLAN):
    // the wire protocol's flat-array convention, one element per SR class.
    w.begin_array("msrp_tc");
    for (auto const& m : iface.msrp_mappings) {
        w.element(double(m.traffic_class));
    }
    w.end_array();
    w.begin_array("msrp_pcp");
    for (auto const& m : iface.msrp_mappings) {
        w.element(double(m.priority));
    }
    w.end_array();
    w.begin_array("msrp_vlan");
    for (auto const& m : iface.msrp_mappings) {
        w.element(double(m.vlan_id));
    }
    w.end_array();
    w.end_object();
    return out;
}

auto WidgetBridge::message_entity(EntityWidgets const& e) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "entity");
    w.field_hex64("entity", e.entity_id.to_uint64());
    w.field_hex64("model", e.entity_model_id);
    w.field("name", e.name);
    // Optional ENTITY detail — omit blanks so the client shows only what
    // the device actually authored.
    if (!e.vendor.empty()) {
        w.field("vendor", e.vendor);
    }
    if (!e.model.empty()) {
        w.field("model_name", e.model);
    }
    if (!e.firmware.empty()) {
        w.field("firmware", e.firmware);
    }
    if (!e.serial.empty()) {
        w.field("serial", e.serial);
    }
    if (!e.group.empty()) {
        w.field("group", e.group);
    }
    if (e.talker_sources > 0) {
        w.field("talkers", std::int64_t(e.talker_sources));
    }
    if (e.listener_sinks > 0) {
        w.field("listeners", std::int64_t(e.listener_sinks));
    }
    if (e.configurations > 0) {
        w.field("configs", std::int64_t(e.configurations));
        w.field("config", std::int64_t(e.current_config));
    }
    w.field("widgets", std::int64_t(e.widgets.size()));
    w.end_object();
    return out;
}

auto WidgetBridge::message_widget(EntityWidgets const& e, WidgetSpec const& spec) -> std::string
{
    auto const& p = spec.projection;
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "widget");
    w.field_hex64("entity", e.entity_id.to_uint64());
    w.field("control", std::int64_t{spec.control_index});
    if (spec.is_selector) {
        w.field("d", "sel");
    } else if (spec.is_matrix) {
        w.field("d", "mx");
    }
    if (!spec.group.empty()) {
        w.field("group", spec.group);
    }
    if (!spec.is_selector && !spec.is_matrix) {
        // The control_type EUI-64 — clients pick "primary" channel
        // controls (GAIN, MUTE) from it generically.
        w.field_hex64("ct", spec.descriptor.control_type.to_uint64());
    }
    w.field("symbol", spec.symbol);
    w.field("name", spec.name);
    if (!spec.mfr_name.empty()) {
        w.field("mfr", spec.mfr_name);
    }
    w.field("kind", widget_kind_name(p.kind));
    w.field("ro", p.read_only);
    if (p.kind == WidgetKind::slider || p.kind == WidgetKind::toggle || p.kind == WidgetKind::meter ||
        p.kind == WidgetKind::matrix || p.kind == WidgetKind::identify) {
        w.field("min", p.minimum);
        w.field("max", p.maximum);
        w.field("step", p.step);
        w.field("default", p.default_value);
        w.field("unit", std::int64_t{p.unit_code});
        w.field("unit_multiplier", std::int64_t{p.unit_multiplier});
    }
    if (p.kind == WidgetKind::slider && !spec.value_labels.empty()) {
        // Multi-value sliders: one label per entry of `values`.
        w.begin_array("labels");
        for (auto const& label : spec.value_labels) {
            w.element(std::string_view{label});
        }
        w.end_array();
    }
    if (p.kind == WidgetKind::matrix) {
        w.field("width", std::int64_t{spec.matrix_width});
        w.field("height", std::int64_t{spec.matrix_height});
        if (!spec.row_labels.empty()) {
            w.begin_array("rows");
            for (auto const& label : spec.row_labels) {
                w.element(std::string_view{label});
            }
            w.end_array();
        }
    }
    if (p.kind == WidgetKind::select) {
        w.begin_array("options");
        for (auto const v : p.options) {
            w.element(v);
        }
        w.end_array();
        if (!p.option_labels.empty()) {
            w.begin_array("labels");
            for (auto const& label : p.option_labels) {
                w.element(std::string_view{label});
            }
            w.end_array();
        }
    }
    if (p.kind == WidgetKind::text) {
        w.field("text", p.text);
    }
    if (!p.current.empty()) {
        // JsonMessage allows one array per message; `options` wins for
        // selects, whose single current value travels as `value`.
        if (p.kind == WidgetKind::select) {
            w.field("value", p.current.front());
        } else {
            w.begin_array("values");
            for (auto const v : p.current) {
                w.element(v);
            }
            w.end_array();
        }
    }
    w.end_object();
    return out;
}

auto WidgetBridge::message_entity_done(EntityWidgets const& e) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "entity_done");
    w.field_hex64("entity", e.entity_id.to_uint64());
    w.end_object();
    return out;
}

auto WidgetBridge::message_departed(Eui64 const& entity_id) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "departed");
    w.field_hex64("entity", entity_id.to_uint64());
    w.end_object();
    return out;
}

auto WidgetBridge::message_groups(EntityWidgets const& e) const -> std::vector<std::string>
{
    // Group names in first-widget order with their widget counts; the
    // block-less remainder rides under "" (the client's Misc bucket).
    std::vector<std::pair<std::string, std::size_t>> groups;
    std::map<std::string_view, std::size_t> at;  // views into groups' keys
    for (auto const& w : e.widgets) {
        auto const [it, inserted] = at.try_emplace(w.group, groups.size());
        if (inserted) {
            groups.push_back({w.group, 1});
        } else {
            ++groups[it->second].second;
        }
    }
    // A jumbo device's summary (the nadia-CP proxy: ~600 CONTROL_BLOCKs)
    // can far exceed the per-connection tx buffer, and ws_send refuses an
    // oversize payload permanently — so the summary ships as a sequence
    // of frames the buffer can always hold; the client accumulates until
    // "last". target stays well under the frame budget so the entry that
    // crosses it still fits.
    std::size_t const target = std::min<std::size_t>(2048, max_ws_payload_ > 512 ? max_ws_payload_ - 256 : 512);
    std::vector<std::string> frames;
    std::size_t i = 0;
    std::int64_t seq = 0;
    while (i < groups.size() || frames.empty()) {
        std::string out;
        JsonWriter w{out};
        w.begin_object();
        w.field("op", "groups");
        w.field_hex64("entity", e.entity_id.to_uint64());
        w.field("seq", seq++);
        w.begin_array("groups");
        while (i < groups.size() && out.size() < target) {
            w.begin_object();
            w.field("name", groups[i].first);
            w.field("count", std::int64_t(groups[i].second));
            w.end_object();
            ++i;
        }
        w.end_array();
        w.field("last", i >= groups.size());
        w.end_object();
        frames.push_back(std::move(out));
    }
    return frames;
}

auto WidgetBridge::message_value(Eui64 const& entity_id, WidgetSpec const& spec) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "value");
    w.field_hex64("entity", entity_id.to_uint64());
    w.field("control", std::int64_t{spec.control_index});
    if (spec.is_selector) {
        w.field("d", "sel");
    } else if (spec.is_matrix) {
        w.field("d", "mx");
    }
    w.begin_array("values");
    for (auto const v : spec.projection.current) {
        w.element(v);
    }
    w.end_array();
    w.end_object();
    return out;
}

auto WidgetBridge::message_matrix_run(
    Eui64 const& entity_id, WidgetSpec const& spec, std::size_t at, std::span<double const> values) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "value");
    w.field_hex64("entity", entity_id.to_uint64());
    w.field("control", std::int64_t{spec.control_index});
    w.field("d", "mx");
    w.field("at", std::int64_t(at));
    w.begin_array("values");
    for (auto const v : values) {
        w.element(v);
    }
    w.end_array();
    w.end_object();
    return out;
}

auto WidgetBridge::message_error(std::string_view reason, Eui64 const& entity_id, std::int64_t control_index) -> std::string
{
    std::string out;
    JsonWriter w{out};
    w.begin_object();
    w.field("op", "error");
    w.field("reason", reason);
    if (entity_id.to_uint64() != 0) {
        w.field_hex64("entity", entity_id.to_uint64());
    }
    if (control_index >= 0) {
        w.field("control", control_index);
    }
    w.end_object();
    return out;
}

}  // namespace statusbar::widget_bridge
