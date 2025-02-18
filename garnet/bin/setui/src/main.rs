// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
    fuchsia_syslog::{self as syslog, fx_log_info},
    futures::lock::Mutex,
    settings::agent::base::BlueprintHandle as AgentBlueprintHandle,
    settings::config::base::{get_default_agent_types, AgentType},
    settings::config::default_settings::DefaultSetting,
    settings::handler::device_storage::StashDeviceStorageFactory,
    settings::switchboard::base::get_default_setting_types,
    settings::AgentConfiguration,
    settings::EnabledPoliciesConfiguration,
    settings::EnabledServicesConfiguration,
    settings::EnvironmentBuilder,
    settings::ServiceConfiguration,
    settings::ServiceFlags,
    std::collections::HashSet,
    std::sync::Arc,
};

const STASH_IDENTITY: &str = "settings_service";

fn main() -> Result<(), Error> {
    let executor = fasync::Executor::new()?;

    syslog::init_with_tags(&["setui-service"]).expect("Can't init logger");
    fx_log_info!("Starting setui-service...");

    let storage_factory = StashDeviceStorageFactory::create(
        STASH_IDENTITY,
        connect_to_service::<fidl_fuchsia_stash::StoreMarker>().unwrap(),
    );

    let default_enabled_service_configuration =
        EnabledServicesConfiguration::with_services(get_default_setting_types());

    // By default, no policies are enabled.
    let default_enabled_policy_configuration =
        EnabledPoliciesConfiguration::with_policies(HashSet::default());

    let enabled_service_configuration = DefaultSetting::new(
        Some(default_enabled_service_configuration),
        "/config/data/service_configuration.json",
    )
    .get_default_value()
    .expect("no default enabled service configuration");

    let enabled_policy_configuration = DefaultSetting::new(
        Some(default_enabled_policy_configuration),
        "/config/data/policy_configuration.json",
    )
    .get_default_value()
    .expect("no default enabled policy configuration");

    let flags =
        DefaultSetting::new(Some(ServiceFlags::default()), "/config/data/service_flags.json")
            .get_default_value()
            .expect("no default service flags");

    let agent_types = DefaultSetting::new(
        Some(AgentConfiguration { agent_types: get_default_agent_types() }),
        "/config/data/agent_configuration.json",
    )
    .get_default_value()
    .expect("no default agent types");

    let configuration = ServiceConfiguration::from(
        agent_types,
        enabled_service_configuration,
        enabled_policy_configuration,
        flags,
    );

    // EnvironmentBuilder::spawn returns a future that can be awaited for the
    // result of the startup. Since main is a synchronous function, we cannot
    // block here and therefore continue without waiting for the result.
    EnvironmentBuilder::new(Arc::new(Mutex::new(storage_factory)))
        .configuration(configuration)
        .agent_mapping(<AgentBlueprintHandle as From<AgentType>>::from)
        .spawn(executor)
}
