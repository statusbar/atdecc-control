// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// Offline mode (docs/PLAN.md §12.7): a ControllerService with no network
// at all. The "discovered entity" is an in-process AEM model served from
// a `.aem` blob — the same DescriptorStorageHandler + AemEntityModel the
// proxy serves with — so the ENTIRE widget bridge pipeline (crawl,
// projection, labels, the live-value sweep, SET round trips with stored
// state, matrices, selectors) runs unchanged against a generated model,
// or a crawl captured into the M7 model store (a real device's model,
// browsable and editable on a laptop with no AVB network).
//
// SETs persist in the handler's RAM state for the life of the process:
// every connected browser sees every change, exactly as online. Command
// completions and descriptor responses are queued and delivered from
// tick(), preserving the seam's asynchronous contract.

#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_model.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace statusbar::widget_bridge {

class OfflineControllerService final : public atdecc_tools::ControllerService
{
  public:
    /// @p blob is a DescriptorStorage image (`.aem`); the service owns a
    /// copy. Invalid blobs leave the service constructed but entity-less
    /// (valid() false).
    explicit OfflineControllerService(std::vector<std::uint8_t> blob);
    ~OfflineControllerService() override;

    [[nodiscard]] auto valid() const noexcept -> bool { return handler_ != nullptr; }

    /// The in-process model — the offline scene endpoints read and write
    /// state through these (docs/PLAN §12.8).
    [[nodiscard]] auto handler() noexcept -> nanoavb::DescriptorStorageHandler& { return *handler_; }
    [[nodiscard]] auto model() noexcept -> nanoavb::AemEntityModel& { return *model_; }
    [[nodiscard]] auto entity_id() const noexcept -> ieee::Eui64 { return entity_.adpdu.entity_id; }

    // ---- ControllerService ------------------------------------------------
    void set_sink(atdecc_tools::ControllerServiceSink sink) override;
    void start() override;
    void tick(std::int64_t now_ns) override;
    [[nodiscard]] auto pollable() noexcept -> net::Pollable* override { return nullptr; }

    void discover_all() override {}
    [[nodiscard]] auto find_entity(ieee::Eui64 const& id) const -> atdecc::DiscoveredEntity const* override;

    auto send_aem_command(
        ieee::Eui64 const& target,
        std::uint16_t command_code,
        std::span<std::uint8_t const> payload,
        atdecc::AemCommandCompletion completion) -> bool override;
    [[nodiscard]] auto aem_inflight_count() const -> std::size_t override { return pending_.size(); }

    auto read_descriptor(ieee::Eui64 const& target, std::uint16_t desc_type, std::uint16_t desc_index) -> bool override;
    auto register_unsolicited(ieee::Eui64 const& target, atdecc::AemCommandCompletion completion) -> bool override;

    auto set_identify(ieee::Eui64 const&, bool, atdecc::AemCommandCompletion) -> bool override { return false; }
    auto get_counters(ieee::Eui64 const&, std::uint16_t, std::uint16_t) -> bool override { return false; }
    auto set_stream_format(ieee::Eui64 const&, std::uint16_t, std::uint16_t, std::uint64_t) -> bool override { return false; }
    auto start_streaming(ieee::Eui64 const&, std::uint16_t, std::uint16_t) -> bool override { return false; }
    auto stop_streaming(ieee::Eui64 const&, std::uint16_t, std::uint16_t) -> bool override { return false; }
    auto set_clock_source(ieee::Eui64 const&, std::uint16_t, std::uint16_t, atdecc::AemCommandCompletion) -> bool override
    {
        return false;
    }
    auto get_clock_source(ieee::Eui64 const&, std::uint16_t, atdecc::AemCommandCompletion) -> bool override { return false; }
    auto set_signal_selector(
        ieee::Eui64 const& target,
        std::uint16_t desc_index,
        std::uint16_t signal_type,
        std::uint16_t signal_index,
        std::uint16_t signal_output,
        atdecc::AemCommandCompletion completion) -> bool override;
    auto get_signal_selector(ieee::Eui64 const& target, std::uint16_t desc_index, atdecc::AemCommandCompletion completion)
        -> bool override;
    auto connect_stream(ieee::Eui64 const&, std::uint16_t, ieee::Eui64 const&, std::uint16_t) -> bool override { return false; }
    auto disconnect_stream(ieee::Eui64 const&, std::uint16_t, ieee::Eui64 const&, std::uint16_t) -> bool override { return false; }
    auto connect_tx_stream(ieee::Eui64 const&, std::uint16_t, ieee::Eui64 const&, std::uint16_t) -> bool override { return false; }
    auto disconnect_tx_stream(ieee::Eui64 const&, std::uint16_t, ieee::Eui64 const&, std::uint16_t) -> bool override
    {
        return false;
    }
    auto get_rx_state(ieee::Eui64 const&, std::uint16_t) -> bool override { return false; }
    auto get_tx_state(ieee::Eui64 const&, std::uint16_t) -> bool override { return false; }

  private:
    struct Pending
    {
        std::uint16_t command{0};
        std::vector<std::uint8_t> sent_payload;
        std::vector<std::uint8_t> response;
        std::uint8_t status{0};
        atdecc::AemCommandCompletion completion{};
        bool is_read_descriptor{false};
    };

    /// Serve @p command against the model now; queue the outcome for tick().
    void queue_command(std::uint16_t command, std::span<std::uint8_t const> payload, atdecc::AemCommandCompletion completion);

    std::vector<std::uint8_t> blob_;
    std::unique_ptr<nanoavb::DescriptorStorageHandler> handler_;
    std::unique_ptr<nanoavb::AemEntityModel> model_;
    atdecc::DiscoveredEntity entity_{};
    atdecc_tools::ControllerServiceSink sink_{};
    std::deque<Pending> pending_;
    bool announced_ = false;
};

}  // namespace statusbar::widget_bridge
