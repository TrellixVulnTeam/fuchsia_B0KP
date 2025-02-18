// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Defining and handling of the monitor environment.
//!
//! # Summary
//!
//! The monitoring logic in the setting service is meant to be modular. Based on
//! product configuration, a variety of sources (resource monitors) and
//! sinks (data outlets) may participate in the monitoring environment. The
//! components in this mod are meant to aid capturing and passing this setup
//! to the appropriate handling component, and bringing up the environment as
//! directed. The separation of this responsibility from the main
//! resource-watching component promotes code reshare and modularity.

use crate::internal::monitor;
use crate::message::base::MessengerType;
use crate::monitor::base::{
    monitor::{self as base_monitor},
    Error,
};

/// `Actor` handles bringing up and controlling environment-specific components
/// surrounding monitoring, such as the resource monitors.
#[derive(Clone)]
pub struct Actor {
    messenger_factory: monitor::message::Factory,
    monitors: Vec<base_monitor::Generate>,
}

impl Actor {
    /// Starts up environment monitors and returns a TargetedMessenger that
    /// broadcasts to all monitors.
    pub async fn start_monitoring(&self) -> Result<monitor::message::TargetedMessenger, Error> {
        // Create unbound, broadcasting messenger to send messages to the monitors.
        let monitor_messenger = monitor::message::TargetedMessenger::new(
            self.messenger_factory
                .create(MessengerType::Unbound)
                .await
                .map_err(|_| {
                    Error::MessageSetupFailure("could not create monitor messenger".into())
                })?
                .0,
            monitor::message::Audience::Broadcast,
        );

        // Bring up each monitor.
        for monitor in &self.monitors {
            let (_, monitor_receptor) =
                self.messenger_factory.create(MessengerType::Unbound).await.map_err(|_| {
                    Error::MessageSetupFailure("could not create monitor receptor".into())
                })?;

            monitor(base_monitor::Context { receptor: monitor_receptor })
                .await
                .map_err(|_| Error::MonitorSetupFailure("could not create monitor".into()))?
        }

        Ok(monitor_messenger)
    }
}

/// `Builder` helps construct a monitoring environment in a step-wise fashion,
/// reutrning an [`Actor`] to control the resulting environment.
pub struct Builder {
    monitors: Vec<base_monitor::Generate>,
}

impl Builder {
    /// Returns a builder with no values set.
    pub fn new() -> Self {
        Self { monitors: vec![] }
    }

    /// Appends [`monitor::Generate`] to the set of monitors to participate in
    /// this environment.
    pub fn add_monitors(mut self, mut monitors: Vec<base_monitor::Generate>) -> Self {
        self.monitors.append(&mut monitors);
        self
    }

    /// Constructs the configuration.
    pub fn build(self) -> Actor {
        let monitor_messenger_factory = monitor::message::create_hub();
        Actor { messenger_factory: monitor_messenger_factory, monitors: self.monitors }
    }
}
