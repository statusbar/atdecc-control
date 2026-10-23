// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"

namespace statusbar::galaxy_proxy {

/// Serves the M5 blob with the per-deployment identity patched in. The
/// proxy holds one and its host serves descriptors through it.
class ProxyHandler : public nanoavb::DescriptorStorageHandler
{
  public:
    ProxyHandler(nanoavb::DescriptorStorage storage, ieee::Eui64 entity_id, ieee::Eui64 association_id)
        : nanoavb::DescriptorStorageHandler{storage}
        , entity_id_(entity_id)
        , association_id_(association_id)
    {}

    auto on_get_entity(nanoavb::DescriptorId id, atdecc::aem::DescriptorEntity& desc) -> bool override
    {
        if (!nanoavb::DescriptorStorageHandler::on_get_entity(id, desc)) {
            return false;
        }
        desc.entity_id = entity_id_;
        desc.association_id = association_id_;
        return true;
    }

  private:
    ieee::Eui64 entity_id_;
    ieee::Eui64 association_id_;
};

}  // namespace statusbar::galaxy_proxy
