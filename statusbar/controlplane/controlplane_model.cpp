// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_model.hpp"

namespace statusbar::controlplane {

ControlPointId ModelBuilder::add(ControlPointDesc const& d)
{
    auto h = ControlPointId(model_.control_type_.size());
    model_.control_type_.push_back(d.control_type);
    model_.value_type_.push_back(d.value_type);
    model_.units_.push_back(d.units);
    model_.multiplier_.push_back(d.multiplier);
    model_.min_.push_back(d.min);
    model_.max_.push_back(d.max);
    model_.step_.push_back(d.step);
    model_.def_.push_back(d.def);
    model_.localized_.push_back(d.localized);
    model_.flags_.push_back(d.flags);
    model_.aem_.push_back(d.aem);
    return h;
}

std::shared_ptr<Model const> ModelBuilder::build()
{
    auto m = std::make_shared<Model>(std::move(model_));
    model_ = Model{};
    return m;
}

}  // namespace statusbar::controlplane
