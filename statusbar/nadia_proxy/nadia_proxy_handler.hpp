// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>

namespace statusbar::nadia_proxy {

/// Serves the generated blob with the per-deployment identity patched
/// in, routing non-identify CONTROL value traffic to the proxy. The proxy
/// holds one and its host serves descriptors through it.
class ProxyHandler : public nanoavb::DescriptorStorageHandler
{
  public:
    static constexpr std::uint16_t IDENTIFY_CONTROL = 0;

    using GetFn = std::function<std::size_t(std::uint16_t, std::span<std::uint8_t>)>;
    using SetFn = std::function<std::uint8_t(std::uint16_t, std::span<std::uint8_t const>)>;
    using PatchFn = std::function<void(std::uint16_t, atdecc::aem::DescriptorControl&)>;

    ProxyHandler(nanoavb::DescriptorStorage storage, ieee::Eui64 entity_id, ieee::Eui64 association_id)
        : nanoavb::DescriptorStorageHandler{storage}
        , entity_id_(entity_id)
        , association_id_(association_id)
    {}

    void set_hooks(GetFn get, SetFn set, PatchFn patch)
    {
        get_ = std::move(get);
        set_ = std::move(set);
        patch_ = std::move(patch);
    }

    auto on_get_entity(nanoavb::DescriptorId id, atdecc::aem::DescriptorEntity& desc) -> bool override
    {
        if (!nanoavb::DescriptorStorageHandler::on_get_entity(id, desc)) {
            return false;
        }
        desc.entity_id = entity_id_;
        desc.association_id = association_id_;
        return true;
    }

    /// READ_DESCRIPTOR shows the mirrored values as the CURRENT fields.
    auto on_get_control(nanoavb::DescriptorId id, atdecc::aem::DescriptorControl& desc) -> bool override
    {
        if (!nanoavb::DescriptorStorageHandler::on_get_control(id, desc)) {
            return false;
        }
        if (id.ref.descriptor_index != IDENTIFY_CONTROL && patch_) {
            patch_(id.ref.descriptor_index, desc);
        }
        return true;
    }

    auto on_set_descriptor_value(uint16_t command_type, nanoavb::DescriptorId id, std::span<uint8_t const> value)
        -> uint8_t override
    {
        if (command_type == atdecc::AEM_COMMAND_SET_CONTROL && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL &&
            id.ref.descriptor_index != IDENTIFY_CONTROL && set_) {
            return set_(id.ref.descriptor_index, value);
        }
        return nanoavb::DescriptorStorageHandler::on_set_descriptor_value(command_type, id, value);
    }

    auto on_get_descriptor_value(
        uint16_t command_type, nanoavb::DescriptorId id, std::span<uint8_t const> request, std::span<uint8_t> out)
        -> size_t override
    {
        if (command_type == atdecc::AEM_COMMAND_GET_CONTROL && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL &&
            id.ref.descriptor_index != IDENTIFY_CONTROL && get_) {
            return get_(id.ref.descriptor_index, out);
        }
        return nanoavb::DescriptorStorageHandler::on_get_descriptor_value(command_type, id, request, out);
    }

  private:
    ieee::Eui64 entity_id_;
    ieee::Eui64 association_id_;
    GetFn get_;
    SetFn set_;
    PatchFn patch_;
};

}  // namespace statusbar::nadia_proxy
