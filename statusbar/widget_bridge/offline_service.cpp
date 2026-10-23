// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/offline_service.hpp"

#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <array>
#include <utility>

namespace statusbar::widget_bridge {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using ieee::Eui64;

namespace {
constexpr std::size_t MAX_RESPONSE = 2048;  ///< descriptors incl. trailers fit comfortably
}

OfflineControllerService::OfflineControllerService(std::vector<std::uint8_t> blob)
    : blob_{std::move(blob)}
{
    auto storage = DescriptorStorage::create(std::span<std::uint8_t const>{blob_});
    if (!storage.has_value()) {
        return;
    }
    handler_ = std::make_unique<nanoavb::DescriptorStorageHandler>(*storage);
    handler_->manage_object_names();  // SET_NAME renames stick (RAM, like SETs)
    model_ = std::make_unique<nanoavb::AemEntityModel>(*handler_, *storage);

    // The "discovered" entity, from the blob's own ENTITY descriptor.
    auto const entity_blob = storage->get_descriptor(0, DESCRIPTOR_ENTITY, 0);
    if (!entity_blob.has_value() || entity_blob->size() < DescriptorEntity::LENGTH) {
        handler_.reset();
        model_.reset();
        return;
    }
    DescriptorEntity desc{};
    span_load_padded(desc, *entity_blob);
    // Generated blobs author entity_id zero (a live entity patches its
    // own in at serve time); offline synthesizes a stable identity from
    // the model id so clients never see the invalid all-zero EUI-64.
    auto entity_id = desc.entity_id;
    if (entity_id.to_uint64() == 0) {
        entity_id.from_uint64(desc.entity_model_id.to_uint64());
    }
    entity_.adpdu.init_entity_available(entity_id, 31);
    entity_.adpdu.entity_model_id = desc.entity_model_id;
    entity_.valid = true;
}

OfflineControllerService::~OfflineControllerService() = default;

void OfflineControllerService::set_sink(atdecc_tools::ControllerServiceSink sink)
{
    sink_ = std::move(sink);
}

void OfflineControllerService::start()
{
    if (valid() && !announced_ && sink_.on_entity_added) {
        announced_ = true;
        sink_.on_entity_added(entity_);
    }
}

auto OfflineControllerService::find_entity(Eui64 const& id) const -> DiscoveredEntity const*
{
    if (valid() && entity_.adpdu.entity_id == id) {
        return &entity_;
    }
    return nullptr;
}

void OfflineControllerService::queue_command(
    std::uint16_t command, std::span<std::uint8_t const> payload, atdecc::AemCommandCompletion completion)
{
    Pending pending;
    pending.command = command;
    pending.sent_payload.assign(payload.begin(), payload.end());
    pending.completion = std::move(completion);

    std::array<std::uint8_t, MAX_RESPONSE> out{};
    switch (command) {
        case AEM_COMMAND_SET_CONTROL:
        case AEM_COMMAND_SET_MATRIX:
        case AEM_COMMAND_SET_SIGNAL_SELECTOR: {
            auto const r = model_->apply_set_descriptor_value(command, payload, out);
            pending.status = r.status;
            pending.response.assign(out.begin(), out.begin() + long(r.size));
            break;
        }
        case AEM_COMMAND_GET_CONTROL:
        case AEM_COMMAND_GET_MATRIX:
        case AEM_COMMAND_GET_SIGNAL_SELECTOR: {
            auto const n = model_->get_descriptor_value_for_wire(command, payload, out);
            pending.status = n > 0 ? AEM_STATUS_SUCCESS : AEM_STATUS_NO_SUCH_DESCRIPTOR;
            pending.response.assign(out.begin(), out.begin() + long(n));
            break;
        }
        case AEM_COMMAND_SET_NAME: {
            pending.status = model_->apply_set_name(payload);
            if (pending.status == AEM_STATUS_SUCCESS) {
                pending.response.assign(payload.begin(), payload.end());  // SET_NAME echoes the command
            }
            break;
        }
        case AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
            pending.status = AEM_STATUS_SUCCESS;  // there is nobody else to hear from
            break;
        default:
            pending.status = AEM_STATUS_NOT_IMPLEMENTED;
            break;
    }
    pending_.push_back(std::move(pending));
}

auto OfflineControllerService::send_aem_command(
    Eui64 const& target, std::uint16_t command_code, std::span<std::uint8_t const> payload, atdecc::AemCommandCompletion completion)
    -> bool
{
    if (!valid() || target != entity_.adpdu.entity_id) {
        return false;
    }
    queue_command(command_code, payload, std::move(completion));
    return true;
}

auto OfflineControllerService::read_descriptor(Eui64 const& target, std::uint16_t desc_type, std::uint16_t desc_index) -> bool
{
    if (!valid() || target != entity_.adpdu.entity_id) {
        return false;
    }
    Pending pending;
    pending.is_read_descriptor = true;
    pending.command = AEM_COMMAND_READ_DESCRIPTOR;

    AemReadDescriptorCommandPayload sent{};
    sent.descriptor_type = desc_type;
    sent.descriptor_index = desc_index;
    pending.sent_payload.resize(AemReadDescriptorCommandPayload::LENGTH);
    span_store(std::span<std::uint8_t>{pending.sent_payload}, sent);

    std::array<std::uint8_t, MAX_RESPONSE> out{};
    auto const n = model_->get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = desc_type, .descriptor_index = desc_index},
        std::span<std::uint8_t>{out}.subspan(AemReadDescriptorResponsePayload::LENGTH));
    pending.status = n > 0 ? AEM_STATUS_SUCCESS : AEM_STATUS_NO_SUCH_DESCRIPTOR;
    pending.response.assign(out.begin(), out.begin() + long(AemReadDescriptorResponsePayload::LENGTH + n));
    pending_.push_back(std::move(pending));
    return true;
}

auto OfflineControllerService::register_unsolicited(Eui64 const& target, atdecc::AemCommandCompletion completion) -> bool
{
    if (!valid() || target != entity_.adpdu.entity_id) {
        return false;
    }
    queue_command(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, {}, std::move(completion));
    return true;
}

auto OfflineControllerService::set_signal_selector(
    Eui64 const& target,
    std::uint16_t desc_index,
    std::uint16_t signal_type,
    std::uint16_t signal_index,
    std::uint16_t signal_output,
    atdecc::AemCommandCompletion completion) -> bool
{
    if (!valid() || target != entity_.adpdu.entity_id) {
        return false;
    }
    AemSignalSelectorPayload payload{};
    payload.descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
    payload.descriptor_index = desc_index;
    payload.source.signal_type = signal_type;
    payload.source.signal_index = signal_index;
    payload.source.signal_output = signal_output;
    queue_command(AEM_COMMAND_SET_SIGNAL_SELECTOR, make_const_span(payload), std::move(completion));
    return true;
}

auto OfflineControllerService::get_signal_selector(
    Eui64 const& target, std::uint16_t desc_index, atdecc::AemCommandCompletion completion) -> bool
{
    if (!valid() || target != entity_.adpdu.entity_id) {
        return false;
    }
    AemControlPayloadHeader header{};
    header.descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
    header.descriptor_index = desc_index;
    queue_command(AEM_COMMAND_GET_SIGNAL_SELECTOR, make_const_span(header), std::move(completion));
    return true;
}

void OfflineControllerService::tick(std::int64_t /*now_ns*/)
{
    // Deliver everything queued, in order — the seam's asynchronous
    // contract, one tick late instead of one network round trip.
    while (!pending_.empty()) {
        auto pending = std::move(pending_.front());
        pending_.pop_front();
        if (pending.is_read_descriptor) {
            if (sink_.on_aem_response) {
                sink_.on_aem_response(
                    entity_.adpdu.entity_id,
                    pending.command,
                    pending.status,
                    make_const_span(pending.sent_payload),
                    make_const_span(pending.response));
            }
            continue;
        }
        if (pending.completion) {
            pending.completion(atdecc::AemCommandResult{
                .delivery = atdecc::AemCommandDelivery::Responded,
                .status = pending.status,
                .command_type = pending.command,
                .target_entity_id = entity_.adpdu.entity_id,
                .sent_payload = make_const_span(pending.sent_payload),
                .response = make_const_span(pending.response)});
        }
    }
}

}  // namespace statusbar::widget_bridge
