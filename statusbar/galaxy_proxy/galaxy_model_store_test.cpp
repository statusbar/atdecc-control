// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// M7 against the real generated model: the galaxy_proxy descriptor set
// fingerprints deterministically, derives a unique orphan-free symbol
// table, and round-trips through the model store.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/nanoavb/nanoavb_aem_model_store.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <filesystem>
#include <set>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;
using namespace statusbar::galaxy_proxy;

namespace {

std::vector<AemRawDescriptor> raws_of(GalaxyAemModel const& model)
{
    std::vector<AemRawDescriptor> out;
    out.reserve(model.descriptors.size());
    for (auto const& d : model.descriptors) {
        out.push_back({d.configuration, d.descriptor_type, d.descriptor_index, d.bytes});
    }
    return out;
}

}  // namespace

TEST(galaxy_model_store, fingerprint_symbols_and_store)
{
    auto ns = controlplane_devices::build_galaxy_816_namespace();
    auto model = build_galaxy_proxy_aem_model(ns);
    auto raws = raws_of(model);

    // Deterministic generation => stable fingerprint across builds.
    auto model2 = build_galaxy_proxy_aem_model(ns);
    EXPECT_TRUE(aem_fingerprint(raws) == aem_fingerprint(raws_of(model2)));

    // Unique, orphan-free symbols across all 1121 descriptors.
    auto symbols = derive_aem_symbols(raws);
    EXPECT_EQ(symbols.size(), raws.size());
    std::set<std::string> unique;
    for (auto const& s : symbols) {
        EXPECT_FALSE(s.orphan);
        EXPECT_TRUE(unique.insert(s.symbol).second);
    }
    // Spot checks: strip-scoped controls and shared matrix signals.
    EXPECT_TRUE(unique.contains("cfg0/au0/cb0/ctl:gain/0"));    // input 1 strip
    EXPECT_TRUE(unique.contains("cfg0/au0/cb0/ctl:enable/0"));  // its first bypass
    EXPECT_TRUE(unique.contains("cfg0/au0/mx:gain/0"));
    EXPECT_TRUE(unique.contains("cfg0/au0/mxsig47"));
    EXPECT_TRUE(unique.contains("cfg0/avbif0"));
    EXPECT_TRUE(unique.contains("cfg0/au0/sel0"));

    // Store round trip on the real model.
    auto dir = (std::filesystem::temp_directory_path() / ("galaxy-store-" + std::to_string(::getpid()))).string();
    std::filesystem::remove_all(dir);
    AemModelStore store(dir);
    auto fp = store.store(raws);
    EXPECT_TRUE(fp.has_value());
    auto loaded = store.load_symbols(*fp);
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), symbols.size());
    std::filesystem::remove_all(dir);
}

//
// Test Runner
//

TEST_MAIN(statusbar_galaxy_proxy, galaxy_model_store_test)
