// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::cobalt_metrics::{get_cobalt_metrics_instance, CobaltMetrics};
use crate::error::PowerManagerError;
use crate::log_if_err;
use crate::message::{Message, MessageReturn};
use crate::node::Node;
use crate::shutdown_request::{RebootReason, ShutdownRequest};
use crate::temperature_handler::TemperatureFilter;
use crate::thermal_limiter;
use crate::types::{Celsius, Nanoseconds, Seconds, ThermalLoad, Watts};
use crate::utils::get_current_timestamp;
use anyhow::{format_err, Error};
use async_trait::async_trait;
use fuchsia_async as fasync;
use fuchsia_inspect::{self as inspect, HistogramProperty, LinearHistogramParams, Property};
use futures::{
    future::{FutureExt, LocalBoxFuture},
    stream::FuturesUnordered,
    StreamExt,
};
use log::*;
use serde_derive::Deserialize;
use serde_json as json;
use std::cell::{Cell, RefCell, RefMut};
use std::collections::{HashMap, VecDeque};
use std::rc::Rc;

/// Node: ThermalPolicy
///
/// Summary: Implements the closed loop thermal control policy for the system
///
/// Handles Messages: N/A
///
/// Sends Messages:
///     - ReadTemperature
///     - SetMaxPowerConsumption
///     - SystemShutdown
///     - UpdateThermalLoad
///     - FileCrashReport
///
/// FIDL dependencies: N/A

pub struct ThermalPolicyBuilder<'a> {
    config: ThermalConfig,
    inspect_root: Option<&'a inspect::Node>,
    thermal_metrics: Option<Box<dyn CobaltMetrics>>,
}

impl<'a> ThermalPolicyBuilder<'a> {
    pub fn new(config: ThermalConfig) -> Self {
        Self { config, inspect_root: None, thermal_metrics: None }
    }

    pub fn new_from_json(json_data: json::Value, nodes: &HashMap<String, Rc<dyn Node>>) -> Self {
        #[derive(Deserialize)]
        struct ControllerConfig {
            sample_interval: f64,
            filter_time_constant: f64,
            target_temperature: f64,
            e_integral_min: f64,
            e_integral_max: f64,
            sustainable_power: f64,
            proportional_gain: f64,
            integral_gain: f64,
        }

        #[derive(Deserialize)]
        struct NodeConfig {
            thermal_shutdown_temperature: f64,
            controller_params: ControllerConfig,
            throttle_end_delay: f64,
        }

        #[derive(Deserialize)]
        struct Dependencies {
            crash_report_handler_node: String,
            cpu_control_nodes: Vec<String>,
            system_power_handler_node: String,
            temperature_handler_node: String,
            thermal_limiter_node: String,
        }

        #[derive(Deserialize)]
        struct JsonData {
            config: NodeConfig,
            dependencies: Dependencies,
        }

        let data: JsonData = json::from_value(json_data).unwrap();
        let thermal_config = ThermalConfig {
            temperature_node: nodes[&data.dependencies.temperature_handler_node].clone(),
            cpu_control_nodes: data
                .dependencies
                .cpu_control_nodes
                .iter()
                .map(|node| nodes[node].clone())
                .collect(),
            sys_pwr_handler: nodes[&data.dependencies.system_power_handler_node].clone(),
            thermal_limiter_node: nodes[&data.dependencies.thermal_limiter_node].clone(),
            crash_report_handler: nodes[&data.dependencies.crash_report_handler_node].clone(),
            policy_params: ThermalPolicyParams {
                controller_params: ThermalControllerParams {
                    sample_interval: Seconds(data.config.controller_params.sample_interval),
                    filter_time_constant: Seconds(
                        data.config.controller_params.filter_time_constant,
                    ),
                    target_temperature: Celsius(data.config.controller_params.target_temperature),
                    e_integral_min: data.config.controller_params.e_integral_min,
                    e_integral_max: data.config.controller_params.e_integral_max,
                    sustainable_power: Watts(data.config.controller_params.sustainable_power),
                    proportional_gain: data.config.controller_params.proportional_gain,
                    integral_gain: data.config.controller_params.integral_gain,
                },
                thermal_shutdown_temperature: Celsius(data.config.thermal_shutdown_temperature),
                throttle_end_delay: Seconds(data.config.throttle_end_delay),
            },
        };
        Self::new(thermal_config)
    }

    #[cfg(test)]
    fn with_inspect_root(mut self, root: &'a inspect::Node) -> Self {
        self.inspect_root = Some(root);
        self
    }

    #[cfg(test)]
    fn with_thermal_metrics(mut self, thermal_metrics: Box<dyn CobaltMetrics>) -> Self {
        self.thermal_metrics = Some(thermal_metrics);
        self
    }

    pub fn build<'b>(
        self,
        futures_out: &FuturesUnordered<LocalBoxFuture<'b, ()>>,
    ) -> Result<Rc<ThermalPolicy>, Error> {
        // Create default values
        let inspect_root = self.inspect_root.unwrap_or(inspect::component::inspector().root());
        let thermal_metrics =
            self.thermal_metrics.unwrap_or(Box::new(get_cobalt_metrics_instance()));

        let node = Rc::new(ThermalPolicy {
            state: ThermalState {
                temperature_filter: TemperatureFilter::new(
                    self.config.temperature_node.clone(),
                    self.config.policy_params.controller_params.filter_time_constant,
                ),
                prev_timestamp: Cell::new(Nanoseconds(0)),
                max_time_delta: Cell::new(Seconds(0.0)),
                error_integral: Cell::new(0.0),
                thermal_load: Cell::new(ThermalLoad(0)),
                throttling_state: Cell::new(ThrottlingState::ThrottlingInactive),
                throttle_end_deadline: Cell::new(None),
            },
            inspect: InspectData::new(inspect_root, "ThermalPolicy".to_string(), &self.config),
            config: self.config,
            thermal_metrics,
        });

        futures_out.push(node.clone().periodic_thermal_loop());
        Ok(node)
    }
}

pub struct ThermalPolicy {
    config: ThermalConfig,
    state: ThermalState,

    /// A struct for managing Component Inspection data.
    inspect: InspectData,

    /// Metrics collection for thermals.
    thermal_metrics: Box<dyn CobaltMetrics>,
}

/// A struct to store all configurable aspects of the ThermalPolicy node
pub struct ThermalConfig {
    /// The node to provide temperature readings for the thermal control loop. It is expected that
    /// this node responds to the ReadTemperature message.
    pub temperature_node: Rc<dyn Node>,

    /// The nodes used to impose limits on CPU power state. There will be one node for each CPU
    /// power domain (e.g., big.LITTLE). It is expected that these nodes respond to the
    /// SetMaxPowerConsumption message.
    pub cpu_control_nodes: Vec<Rc<dyn Node>>,

    /// The node to handle system power state changes (e.g., shutdown). It is expected that this
    /// node responds to the SystemShutdown message.
    pub sys_pwr_handler: Rc<dyn Node>,

    /// The node which will impose thermal limits on external clients according to the thermal
    /// load of the system. It is expected that this node responds to the UpdateThermalLoad
    /// message.
    pub thermal_limiter_node: Rc<dyn Node>,

    /// The node used for filing a crash report. It is expected that this node responds to the
    /// FileCrashReport message.
    pub crash_report_handler: Rc<dyn Node>,

    /// All parameter values relating to the thermal policy itself
    pub policy_params: ThermalPolicyParams,
}

/// A struct to store all configurable aspects of the thermal policy itself
pub struct ThermalPolicyParams {
    /// The thermal control loop parameters
    pub controller_params: ThermalControllerParams,

    /// If temperature reaches or exceeds this value, the policy will command a system shutdown
    pub thermal_shutdown_temperature: Celsius,

    /// Time to wait after throttling ends to officially declare a throttle event complete.
    pub throttle_end_delay: Seconds,
}

/// A struct to store the tunable thermal control loop parameters
#[derive(Clone, Debug)]
pub struct ThermalControllerParams {
    /// The interval at which to run the thermal control loop
    pub sample_interval: Seconds,

    /// Time constant for the low-pass filter used for smoothing the temperature input signal
    pub filter_time_constant: Seconds,

    /// Target temperature for the PI control calculation
    pub target_temperature: Celsius,

    /// Minimum integral error [degC * s] for the PI control calculation
    pub e_integral_min: f64,

    /// Maximum integral error [degC * s] for the PI control calculation
    pub e_integral_max: f64,

    /// The available power when there is no temperature error
    pub sustainable_power: Watts,

    /// The proportional gain [W / degC] for the PI control calculation
    pub proportional_gain: f64,

    /// The integral gain [W / (degC * s)] for the PI control calculation
    pub integral_gain: f64,
}

/// State information that is used for calculations across controller iterations
struct ThermalState {
    /// Provides filtered temperature values according to the configured filter constant.
    temperature_filter: TemperatureFilter,

    /// The time of the previous controller iteration
    prev_timestamp: Cell<Nanoseconds>,

    /// The largest observed time between controller iterations (may be used to detect hangs)
    max_time_delta: Cell<Seconds>,

    /// The integral error [degC * s] that is accumulated across controller iterations
    error_integral: Cell<f64>,

    /// A cached value in the range [0 - MAX_THERMAL_LOAD] which is defined as
    /// ((temperature - range_start) / (range_end - range_start) * MAX_THERMAL_LOAD).
    thermal_load: Cell<ThermalLoad>,

    /// Current throttling state.
    throttling_state: Cell<ThrottlingState>,

    /// After we exit throttling, if `throttle_end_delay` is nonzero then this value will
    /// indicate the time that we may officially consider a throttle event complete.
    throttle_end_deadline: Cell<Option<Nanoseconds>>,
}

#[derive(Copy, Clone, Debug, PartialEq)]
enum ThrottlingState {
    /// Selected when thermal load is above zero.
    ThrottlingActive,

    /// Selected when thermal load is zero and the throttling cooldown timer is not active.
    ThrottlingInactive,

    /// Selected when throttling has ended (thermal load is zero) but the throttling cooldown timer
    /// is active.
    CooldownActive,
}

impl ThermalPolicy {
    /// Creates a Future driven by a timer firing at the interval specified by
    /// ThermalControllerParams.sample_interval. At each fire, `iterate_thermal_control` is called
    /// and any resulting errors are logged.
    fn periodic_thermal_loop<'a>(self: Rc<Self>) -> LocalBoxFuture<'a, ()> {
        let mut periodic_timer = fasync::Interval::new(
            self.config.policy_params.controller_params.sample_interval.into(),
        );

        async move {
            while let Some(()) = periodic_timer.next().await {
                fuchsia_trace::instant!(
                    "power_manager",
                    "ThermalPolicy::periodic_timer_fired",
                    fuchsia_trace::Scope::Thread
                );
                let result = self.iterate_thermal_control().await;
                log_if_err!(result, "Error while running thermal control iteration");
                fuchsia_trace::instant!(
                    "power_manager",
                    "ThermalPolicy::iterate_thermal_control_result",
                    fuchsia_trace::Scope::Thread,
                    "result" => format!("{:?}", result).as_str()
                );
            }
        }
        .boxed_local()
    }

    /// This is the main body of the closed loop thermal control logic. The function is called
    /// periodically by the timer started in `start_periodic_thermal_loop`. For each iteration, the
    /// following steps will be taken:
    ///     1. Read the current temperature from the temperature driver specified in ThermalConfig
    ///     2. Filter the raw temperature value using a low-pass filter
    ///     3. Use the new filtered temperature to calculate the proportional error and integral
    ///        error of temperature relative to the configured target temperature
    ///     4. Use the proportional error and integral error to derive thermal load and available
    ///        power values
    ///     5. Update the relevant nodes with the new thermal load and available power information
    async fn iterate_thermal_control(&self) -> Result<(), Error> {
        fuchsia_trace::duration!("power_manager", "ThermalPolicy::iterate_thermal_control");

        let timestamp = get_current_timestamp();
        let time_delta = self.get_time_delta(timestamp);

        let temperature = self.state.temperature_filter.get_temperature(timestamp).await?;
        let (error_proportional, error_integral) =
            self.get_temperature_error(temperature.filtered, time_delta);
        let thermal_load = Self::calculate_thermal_load(
            error_integral,
            self.config.policy_params.controller_params.e_integral_min,
            self.config.policy_params.controller_params.e_integral_max,
        );
        let throttling_state = self.update_throttling_state(timestamp, thermal_load).await;

        self.log_thermal_iteration_metrics(
            timestamp,
            time_delta,
            temperature.raw,
            temperature.filtered,
            error_integral,
            thermal_load,
            throttling_state,
        );

        // TODO(fxbug.dev/32618): Having both gain values of 0 indicates an intention to disable the
        // thermal policy. Bail after logging the above metrics. We only need this for Sherlock to
        // publish the metrics before the real thermal policy is brought up. Once we have Sherlock
        // thermal policy ready, this should be removed.
        if (
            self.config.policy_params.controller_params.proportional_gain,
            self.config.policy_params.controller_params.integral_gain,
        ) == (0.0, 0.0)
        {
            return Ok(());
        }

        // If the new temperature is above the critical threshold then shut down the system
        let result = self.check_critical_temperature(timestamp, temperature.raw).await;
        log_if_err!(result, "Error checking critical temperature");
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::check_critical_temperature_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        // Update the ThermalLimiter node with the latest thermal load
        let result = self.process_thermal_load(timestamp, thermal_load).await;
        log_if_err!(result, "Error updating thermal load");
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::process_thermal_load_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        // Update the power allocation according to the new temperature error readings
        let result = self.update_power_allocation(error_proportional, error_integral).await;
        log_if_err!(result, "Error running thermal feedback controller");
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::update_power_allocation_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        Ok(())
    }

    /// Gets the time delta from the previous call to this function using the provided timestamp.
    /// Logs the largest delta into Inspect.
    fn get_time_delta(&self, timestamp: Nanoseconds) -> Seconds {
        let time_delta = (timestamp - self.state.prev_timestamp.get()).into();
        if time_delta > self.state.max_time_delta.get().into() {
            self.state.max_time_delta.set(time_delta);
            self.inspect.max_time_delta.set(time_delta.0);
        }
        self.state.prev_timestamp.set(timestamp);
        time_delta
    }

    /// Calculates proportional error and integral error of temperature using the provided input
    /// temperature and time delta. Stores the new integral error and logs it to Inspect.
    fn get_temperature_error(&self, temperature: Celsius, time_delta: Seconds) -> (f64, f64) {
        let controller_params = &self.config.policy_params.controller_params;
        let error_proportional = controller_params.target_temperature.0 - temperature.0;
        let error_integral = num_traits::clamp(
            self.state.error_integral.get() + error_proportional * time_delta.0,
            controller_params.e_integral_min,
            controller_params.e_integral_max,
        );
        self.state.error_integral.set(error_integral);
        self.inspect.error_integral.set(error_integral);
        (error_proportional, error_integral)
    }

    /// Logs various state data that is updated on each iteration of the thermal policy.
    fn log_thermal_iteration_metrics(
        &self,
        timestamp: Nanoseconds,
        time_delta: Seconds,
        raw_temperature: Celsius,
        filtered_temperature: Celsius,
        temperature_error_integral: f64,
        thermal_load: ThermalLoad,
        throttling_state: ThrottlingState,
    ) {
        self.thermal_metrics.log_raw_temperature(raw_temperature);
        self.inspect.timestamp.set(timestamp.0);
        self.inspect.time_delta.set(time_delta.0);
        self.inspect.log_raw_cpu_temperature(raw_temperature);
        self.inspect.temperature_filtered.set(filtered_temperature.0);
        self.inspect.thermal_load.set(thermal_load.0.into());
        self.inspect.throttling_state.set(format!("{:?}", throttling_state).as_str());
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::thermal_control_iteration_data",
            fuchsia_trace::Scope::Thread,
            "timestamp" => timestamp.0
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy raw_temperature",
            0,
            "raw_temperature" => raw_temperature.0
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy filtered_temperature",
            0,
            "filtered_temperature" => filtered_temperature.0
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy error_integral", 0,
            "error_integral" => temperature_error_integral
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy thermal_load",
            0,
            "thermal_load" => thermal_load.0
        );
    }

    /// Compares the supplied temperature with the thermal config thermal shutdown temperature. If
    /// we've reached or exceeded the shutdown temperature, message the system power handler node
    /// to initiate a system shutdown.
    async fn check_critical_temperature(
        &self,
        timestamp: Nanoseconds,
        temperature: Celsius,
    ) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::check_critical_temperature",
            "temperature" => temperature.0
        );

        // Temperature has exceeded the thermal shutdown temperature
        if temperature.0 >= self.config.policy_params.thermal_shutdown_temperature.0 {
            fuchsia_trace::instant!(
                "power_manager",
                "ThermalPolicy::thermal_shutdown_reached",
                fuchsia_trace::Scope::Thread,
                "temperature" => temperature.0,
                "shutdown_temperature" => self.config.policy_params.thermal_shutdown_temperature.0
            );

            self.thermal_metrics.log_throttle_end_shutdown(timestamp);
            self.inspect.throttle_history().mark_throttling_inactive(timestamp);

            self.send_message(
                &self.config.sys_pwr_handler,
                &Message::SystemShutdown(ShutdownRequest::Reboot(RebootReason::HighTemperature)),
            )
            .await
            .map_err(|e| format_err!("Failed to shut down the system: {}", e))?;
        }

        Ok(())
    }

    /// Process a new thermal load value. If there is a change from the cached thermal_load, then
    /// the new value is sent out to the ThermalLimiter node.
    async fn process_thermal_load(
        &self,
        timestamp: Nanoseconds,
        new_load: ThermalLoad,
    ) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::process_thermal_load",
            "timestamp" => timestamp.0,
            "new_load" => new_load.0
        );

        let old_load = self.state.thermal_load.get();
        self.state.thermal_load.set(new_load);
        self.inspect.throttle_history().record_thermal_load(new_load);

        if new_load != old_load {
            fuchsia_trace::instant!(
                "power_manager",
                "ThermalPolicy::thermal_load_changed",
                fuchsia_trace::Scope::Thread,
                "old_load" => old_load.0,
                "new_load" => new_load.0
            );

            self.send_message(
                &self.config.thermal_limiter_node,
                &Message::UpdateThermalLoad(new_load),
            )
            .await?;
        }

        Ok(())
    }

    /// Updates the throttling state by considering the current throttling state, new thermal load,
    /// and timestamp. When the throttling state is updated, there may be an associated Cobalt event
    /// or crash report dispatched.
    async fn update_throttling_state(
        &self,
        timestamp: Nanoseconds,
        new_load: ThermalLoad,
    ) -> ThrottlingState {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::update_throttling_state",
            "new_load" => new_load.0,
            "timestamp" => timestamp.0
        );

        let old_state = self.state.throttling_state.get();
        let new_state = match old_state {
            ThrottlingState::ThrottlingActive => {
                // If throttling was previously active but the thermal load is now zero, throttling
                // is over. Mark cooldown active if there is a cooldown timer configured.
                if (new_load == ThermalLoad(0))
                    && (self.config.policy_params.throttle_end_delay > Seconds(0.0))
                {
                    ThrottlingState::CooldownActive
                // If no cooldown delay is configured, we can mark throttling inactive immediately
                } else if new_load == ThermalLoad(0) {
                    ThrottlingState::ThrottlingInactive
                } else {
                    old_state
                }
            }
            ThrottlingState::ThrottlingInactive => {
                // If throttling was previously inactive but the thermal load is now nonzero,
                // throttling is now active.
                if new_load > ThermalLoad(0) {
                    ThrottlingState::ThrottlingActive
                } else {
                    old_state
                }
            }
            ThrottlingState::CooldownActive => {
                // If the cooldown timer is active but the thermal load is nonzero again, we can
                // mark throttling active
                if new_load > ThermalLoad(0) {
                    ThrottlingState::ThrottlingActive
                // If the cooldown timer is active and has now expired, we can mark throttling
                // inactive
                } else if timestamp >= self.state.throttle_end_deadline.get().unwrap() {
                    ThrottlingState::ThrottlingInactive
                } else {
                    old_state
                }
            }
        };

        // Handle the new throttling state
        match (old_state, new_state) {
            // Begin a new throttling event
            (ThrottlingState::ThrottlingInactive, ThrottlingState::ThrottlingActive) => {
                info!("Begin thermal mitigation");
                self.thermal_metrics.log_throttle_start(timestamp);
                self.inspect.throttle_history().mark_throttling_active(timestamp);
            }

            // Cancel the cooldown timer and resume the existing throttling event
            (ThrottlingState::CooldownActive, ThrottlingState::ThrottlingActive) => {
                info!("Begin thermal mitigation");
                self.state.throttle_end_deadline.set(None);
            }

            // Begin the cooldown timer
            (ThrottlingState::ThrottlingActive, ThrottlingState::CooldownActive) => {
                info!("End thermal mitigation");
                self.state
                    .throttle_end_deadline
                    .set(Some(timestamp + self.config.policy_params.throttle_end_delay.into()));
            }

            // End the current throttling event and file a crash report
            (ThrottlingState::ThrottlingActive, ThrottlingState::ThrottlingInactive)
            | (ThrottlingState::CooldownActive, ThrottlingState::ThrottlingInactive) => {
                if let ThrottlingState::ThrottlingActive = old_state {
                    info!("End thermal mitigation");
                }
                self.state.throttle_end_deadline.set(None);
                self.thermal_metrics.log_throttle_end_mitigated(timestamp);
                self.inspect.throttle_history().mark_throttling_inactive(timestamp);
                self.file_thermal_crash_report().await;
            }
            _ => {}
        }

        self.state.throttling_state.set(new_state);
        new_state
    }

    /// Calculates the thermal load which is a value in the range [0 - MAX_THERMAL_LOAD] defined as
    /// ((error_integral - range_start) / (range_end - range_start) * MAX_THERMAL_LOAD), where the
    /// range is defined by the maximum and minimum integral error according to the controller
    /// parameters.
    fn calculate_thermal_load(
        error_integral: f64,
        error_integral_min: f64,
        error_integral_max: f64,
    ) -> ThermalLoad {
        debug_assert!(
            error_integral >= error_integral_min,
            "error_integral ({}) less than error_integral_min ({})",
            error_integral,
            error_integral_min
        );
        debug_assert!(
            error_integral <= error_integral_max,
            "error_integral ({}) greater than error_integral_max ({})",
            error_integral,
            error_integral_max
        );

        if error_integral < error_integral_min {
            error!(
                "error_integral {} less than error_integral_min {}",
                error_integral, error_integral_min
            );
            thermal_limiter::MAX_THERMAL_LOAD
        } else if error_integral > error_integral_max {
            error!(
                "error_integral {} greater than error_integral_max {}",
                error_integral, error_integral_max
            );
            ThermalLoad(0)
        } else {
            ThermalLoad(
                ((error_integral - error_integral_max) / (error_integral_min - error_integral_max)
                    * thermal_limiter::MAX_THERMAL_LOAD.0 as f64) as u32,
            )
        }
    }

    /// Updates the power allocation according to the provided proportional error and integral error
    /// of temperature.
    async fn update_power_allocation(
        &self,
        error_proportional: f64,
        error_integral: f64,
    ) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::update_power_allocation",
            "error_proportional" => error_proportional,
            "error_integral" => error_integral
        );
        let available_power = self.calculate_available_power(error_proportional, error_integral);
        self.inspect.throttle_history().record_available_power(available_power);
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy available_power",
            0,
            "available_power" => available_power.0
        );

        self.distribute_power(available_power).await
    }

    /// A PI control algorithm that uses temperature as the measured process variable, and
    /// available power as the control variable.
    fn calculate_available_power(&self, error_proportional: f64, error_integral: f64) -> Watts {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::calculate_available_power",
            "error_proportional" => error_proportional,
            "error_integral" => error_integral
        );

        let controller_params = &self.config.policy_params.controller_params;
        let p_term = error_proportional * controller_params.proportional_gain;
        let i_term = error_integral * controller_params.integral_gain;
        let power_available =
            f64::max(0.0, controller_params.sustainable_power.0 + p_term + i_term);

        Watts(power_available)
    }

    /// This function is responsible for distributing the available power (as determined by the
    /// prior PI calculation) to the various power actors that are included in this closed loop
    /// system. Initially, CPU is the only power actor. In later versions of the thermal policy,
    /// there may be more power actors with associated "weights" for distributing power amongst
    /// them.
    async fn distribute_power(&self, mut total_available_power: Watts) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::distribute_power",
            "total_available_power" => total_available_power.0
        );

        // The power distribution currently works by allocating the total available power to the
        // first CPU control node in `cpu_control_nodes`. The node replies to the
        // SetMaxPowerConsumption message with the amount of power it was able to utilize. This
        // utilized amount is subtracted from the total available power, then the remaining power is
        // allocated to the remaining CPU control nodes in the same way.

        // TODO(fxbug.dev/48205): We may want to revisit this distribution algorithm to avoid potentially
        // starving some CPU control nodes. We'll want to have some discussions and learn more about
        // intended big.LITTLE scheduling and operation to better inform our decisions here. We may
        // find that we'll need to first query the nodes to learn how much power they intend to use
        // before making allocation decisions.
        for (i, node) in self.config.cpu_control_nodes.iter().enumerate() {
            if let MessageReturn::SetMaxPowerConsumption(power_used) = self
                .send_message(&node, &Message::SetMaxPowerConsumption(total_available_power))
                .await?
            {
                self.inspect
                    .throttle_history
                    .borrow_mut()
                    .record_cpu_power_consumption(i, power_used);
                total_available_power = total_available_power - power_used;
            }
        }

        Ok(())
    }

    /// File a crash report with the signature "fuchsia-thermal-throttle".
    async fn file_thermal_crash_report(&self) {
        log_if_err!(
            self.send_message(
                &self.config.crash_report_handler,
                &Message::FileCrashReport("fuchsia-thermal-throttle".to_string()),
            )
            .await,
            "Failed to file crash report"
        );
    }
}

#[async_trait(?Send)]
impl Node for ThermalPolicy {
    fn name(&self) -> String {
        "ThermalPolicy".to_string()
    }

    async fn handle_message(&self, msg: &Message) -> Result<MessageReturn, PowerManagerError> {
        match msg {
            _ => Err(PowerManagerError::Unsupported),
        }
    }
}

struct InspectData {
    // Nodes
    root_node: inspect::Node,

    // Properties
    timestamp: inspect::IntProperty,
    time_delta: inspect::DoubleProperty,
    temperature_raw: inspect::DoubleProperty,
    temperature_filtered: inspect::DoubleProperty,
    error_integral: inspect::DoubleProperty,
    thermal_load: inspect::UintProperty,
    throttling_state: inspect::StringProperty,
    max_time_delta: inspect::DoubleProperty,
    throttle_history: RefCell<InspectThrottleHistory>,
    historical_max_cpu_temperature: RefCell<HistoricalMaxCpuTemperature>,
}

impl InspectData {
    /// Rolling number of throttle events to store in `throttle_history`.
    const NUM_THROTTLE_EVENTS: usize = 10;

    fn new(parent: &inspect::Node, name: String, config: &ThermalConfig) -> Self {
        // Create a local root node and properties
        let root_node = parent.create_child(name);
        let state_node = root_node.create_child("state");
        let stats_node = root_node.create_child("stats");
        let timestamp = state_node.create_int("timestamp (ns)", 0);
        let time_delta = state_node.create_double("time_delta (s)", 0.0);
        let temperature_raw = state_node.create_double("temperature_raw (C)", 0.0);
        let temperature_filtered = state_node.create_double("temperature_filtered (C)", 0.0);
        let error_integral = state_node.create_double("error_integral", 0.0);
        let thermal_load = state_node.create_uint("thermal_load", 0);
        let throttling_state = state_node.create_string(
            "throttling_state",
            format!("{:?}", ThrottlingState::ThrottlingInactive),
        );
        let max_time_delta = stats_node.create_double("max_time_delta (s)", 0.0);
        let throttle_history = RefCell::new(InspectThrottleHistory::new(
            root_node.create_child("throttle_history"),
            Self::NUM_THROTTLE_EVENTS,
        ));

        let platform_metrics_node = parent.create_child("platform_metrics");

        // Every 60 seconds record the max observed CPU temperature. Configured based on the thermal
        // controller sample interval.
        let historical_max_cpu_temperature = RefCell::new(HistoricalMaxCpuTemperature::new(
            &platform_metrics_node,
            (60.0 / config.policy_params.controller_params.sample_interval.0) as usize,
        ));

        // Pass ownership of the new nodes to the root node, otherwise they'll be dropped
        root_node.record(state_node);
        root_node.record(stats_node);
        parent.record(platform_metrics_node);

        let inspect_data = InspectData {
            root_node,
            timestamp,
            time_delta,
            max_time_delta,
            temperature_raw,
            temperature_filtered,
            error_integral,
            thermal_load,
            throttling_state,
            throttle_history,
            historical_max_cpu_temperature,
        };
        inspect_data.set_thermal_config(config);
        inspect_data
    }

    fn set_thermal_config(&self, config: &ThermalConfig) {
        let policy_params_node = self.root_node.create_child("policy_params");
        let ctrl_params_node = policy_params_node.create_child("controller_params");
        let params = &config.policy_params.controller_params;
        ctrl_params_node.record_double("sample_interval (s)", params.sample_interval.0);
        ctrl_params_node.record_double("filter_time_constant (s)", params.filter_time_constant.0);
        ctrl_params_node.record_double("target_temperature (C)", params.target_temperature.0);
        ctrl_params_node.record_double("e_integral_min", params.e_integral_min);
        ctrl_params_node.record_double("e_integral_max", params.e_integral_max);
        ctrl_params_node.record_double("sustainable_power (W)", params.sustainable_power.0);
        ctrl_params_node.record_double("proportional_gain", params.proportional_gain);
        ctrl_params_node.record_double("integral_gain", params.integral_gain);
        policy_params_node.record(ctrl_params_node);

        self.root_node.record(policy_params_node);
    }

    fn log_raw_cpu_temperature(&self, temperature: Celsius) {
        self.temperature_raw.set(temperature.0);
        self.historical_max_cpu_temperature.borrow_mut().log_raw_cpu_temperature(temperature);
    }

    /// A convenient wrapper to mutably borrow `throttle_history`.
    fn throttle_history(&self) -> RefMut<'_, InspectThrottleHistory> {
        self.throttle_history.borrow_mut()
    }
}

/// Tracks the max CPU temperature observed over the last 60 seconds and writes the most recent two
/// values to Inspect.
struct HistoricalMaxCpuTemperature {
    /// Stores the two max temperature values.
    inspect_node: inspect::Node,
    entries: VecDeque<inspect::IntProperty>,

    /// Number of samples before recording the max temperature to Inspect.
    max_sample_count: usize,

    /// Current number of samples observed. Resets back to zero after reaching `max_sample_count`.
    sample_count: usize,

    /// Current observed max temperature.
    max_temperature: Celsius,
}

impl HistoricalMaxCpuTemperature {
    /// Number of temperature values to record in the Inspect BoundedListNode.
    const RECORD_COUNT: usize = 2;

    const INVALID_TEMPERATURE: Celsius = Celsius(f64::NEG_INFINITY);

    fn new(platform_metrics_root: &inspect::Node, max_sample_count: usize) -> Self {
        Self {
            inspect_node: platform_metrics_root.create_child("historical_max_cpu_temperature_c"),
            entries: VecDeque::with_capacity(Self::RECORD_COUNT),
            max_sample_count,
            sample_count: 0,
            max_temperature: Self::INVALID_TEMPERATURE,
        }
    }

    /// Logs a raw CPU temperature reading and updates the max temperature observed. After
    /// `max_sample_count` times, records the max temperature to Inspect and resets the sample count
    /// and max values.
    fn log_raw_cpu_temperature(&mut self, temperature: Celsius) {
        if temperature > self.max_temperature {
            self.max_temperature = temperature
        }

        self.sample_count += 1;
        if self.sample_count == self.max_sample_count {
            self.record_temperature_entry(self.max_temperature);
            self.sample_count = 0;
            self.max_temperature = Self::INVALID_TEMPERATURE;
        }
    }

    /// Records the temperature to Inspect while removing stale records according to `RECORD_COUNT`.
    /// The current timestamp in seconds after system boot is used as the property key, and
    /// temperature is recorded in Celsius as an integer.
    fn record_temperature_entry(&mut self, temperature: Celsius) {
        while self.entries.len() >= Self::RECORD_COUNT {
            self.entries.pop_front();
        }

        let time = Seconds::from(get_current_timestamp()).0 as i64;
        let temperature = temperature.0 as i64;
        self.entries.push_back(self.inspect_node.create_int(time.to_string(), temperature));
    }
}

#[cfg(test)]
mod historical_max_cpu_temperature_tests {
    use super::*;
    use inspect::assert_inspect_tree;

    /// Tests that after each max temperature recording, the max temperature is reset for the next
    /// round. The test would fail if HistoricalMaxCpuTemperature was not resetting the previous max
    /// temperature at the end of each N samples.
    #[test]
    fn test_reset_max_temperature_after_sample_count() {
        let executor = fasync::Executor::new_with_fake_time().unwrap();
        let inspector = inspect::Inspector::new();
        let mut max_temperatures = HistoricalMaxCpuTemperature::new(inspector.root(), 10);

        // Log 10 samples to dispatch the first max temperature reading
        executor.set_fake_time(Seconds(0.0).into());
        for _ in 0..10 {
            max_temperatures.log_raw_cpu_temperature(Celsius(50.0));
        }

        // Log 10 more samples to disaptch the second max temperature reading (with a lower max
        // temperature)
        executor.set_fake_time(Seconds(1.0).into());
        for _ in 0..10 {
            max_temperatures.log_raw_cpu_temperature(Celsius(40.0));
        }

        assert_inspect_tree!(
            inspector,
            root: {
                historical_max_cpu_temperature_c: {
                    "0": 50i64,
                    "1": 40i64
                }
            }
        );
    }

    /// Tests that the max CPU temperature isn't logged until after the specified number of
    /// temperature samples are observed.
    #[test]
    fn test_dispatch_reading_after_n_samples() {
        let executor = fasync::Executor::new_with_fake_time().unwrap();
        let inspector = inspect::Inspector::new();
        let mut max_temperatures = HistoricalMaxCpuTemperature::new(inspector.root(), 10);

        executor.set_fake_time(Seconds(0.0).into());

        // Tree is initially empty
        assert_inspect_tree!(
            inspector,
            root: {
                historical_max_cpu_temperature_c: {}
            }
        );

        // Observe n-1 temperature samples
        for _ in 0..9 {
            max_temperatures.log_raw_cpu_temperature(Celsius(50.0));
        }

        // Tree is still empty
        assert_inspect_tree!(
            inspector,
            root: {
                historical_max_cpu_temperature_c: {}
            }
        );

        // After one more temperature sample, the max temperature should be logged
        max_temperatures.log_raw_cpu_temperature(Celsius(50.0));
        assert_inspect_tree!(
            inspector,
            root: {
                historical_max_cpu_temperature_c: {
                    "0": 50i64
                }
            }
        );
    }

    /// Tests that there are never more than the two most recent max temperature recordings logged
    /// into Inspect.
    #[test]
    fn test_max_record_count() {
        let executor = fasync::Executor::new_with_fake_time().unwrap();
        let inspector = inspect::Inspector::new();
        let mut max_temperatures = HistoricalMaxCpuTemperature::new(inspector.root(), 2);

        executor.set_fake_time(Seconds(0.0).into());
        for _ in 0..2 {
            max_temperatures.log_raw_cpu_temperature(Celsius(50.0));
        }

        executor.set_fake_time(Seconds(1.0).into());
        for _ in 0..2 {
            max_temperatures.log_raw_cpu_temperature(Celsius(50.0));
        }

        executor.set_fake_time(Seconds(2.0).into());
        for _ in 0..2 {
            max_temperatures.log_raw_cpu_temperature(Celsius(50.0));
        }

        assert_inspect_tree!(
            inspector,
            root: {
                historical_max_cpu_temperature_c: {
                    "1": 50i64,
                    "2": 50i64
                }
            }
        );
    }

    /// Tests that the actual max value is recorded after varying temperature values were logged.
    #[test]
    fn test_max_temperature_selection() {
        let executor = fasync::Executor::new_with_fake_time().unwrap();
        let inspector = inspect::Inspector::new();
        let mut max_temperatures = HistoricalMaxCpuTemperature::new(inspector.root(), 3);

        executor.set_fake_time(Seconds(0.0).into());
        max_temperatures.log_raw_cpu_temperature(Celsius(10.0));
        max_temperatures.log_raw_cpu_temperature(Celsius(30.0));
        max_temperatures.log_raw_cpu_temperature(Celsius(20.0));

        assert_inspect_tree!(
            inspector,
            root: {
                historical_max_cpu_temperature_c: {
                    "0": 30i64
                }
            }
        );
    }
}

/// Captures and retains data from previous throttling events in a rolling buffer.
struct InspectThrottleHistory {
    /// The Inspect node that will be used as the parent for throttle event child nodes.
    root_node: inspect::Node,

    /// A running count of the number of throttle events ever captured in `throttle_history_list`.
    /// The count is always increasing, even when older throttle events are removed from the list.
    entry_count: usize,

    /// The maximum number of throttling events to keep in `throttle_history_list`.
    capacity: usize,

    /// State to track if throttling is currently active (used to ignore readings when throttling
    /// isn't active).
    throttling_active: bool,

    /// List to store the throttle entries.
    throttle_history_list: VecDeque<InspectThrottleHistoryEntry>,
}

impl InspectThrottleHistory {
    fn new(root_node: inspect::Node, capacity: usize) -> Self {
        Self {
            entry_count: 0,
            capacity,
            throttling_active: false,
            throttle_history_list: VecDeque::with_capacity(capacity),
            root_node,
        }
    }

    /// Mark the start of throttling.
    fn mark_throttling_active(&mut self, timestamp: Nanoseconds) {
        // Must have ended previous throttling
        debug_assert_eq!(self.throttling_active, false);
        if self.throttling_active {
            return;
        }

        // Begin a new throttling entry
        self.new_entry();

        self.throttling_active = true;
        self.throttle_history_list.back().unwrap().throttle_start_time.set(timestamp.0);
    }

    /// Mark the end of throttling.
    fn mark_throttling_inactive(&mut self, timestamp: Nanoseconds) {
        if self.throttling_active {
            self.throttle_history_list.back().unwrap().throttle_end_time.set(timestamp.0);
            self.throttling_active = false
        }
    }

    /// Begin a new throttling entry. Removes the oldest entry once we've reached
    /// InspectData::NUM_THROTTLE_EVENTS number of entries.
    fn new_entry(&mut self) {
        if self.throttle_history_list.len() >= self.capacity {
            self.throttle_history_list.pop_front();
        }

        let node = self.root_node.create_child(&self.entry_count.to_string());
        let entry = InspectThrottleHistoryEntry::new(node);
        self.throttle_history_list.push_back(entry);
        self.entry_count += 1;
    }

    /// Record the current thermal load. No-op unless throttling has been set active.
    fn record_thermal_load(&self, thermal_load: ThermalLoad) {
        if self.throttling_active {
            self.throttle_history_list
                .back()
                .unwrap()
                .thermal_load_hist
                .insert(thermal_load.0.into());
        }
    }

    /// Record the current available power. No-op unless throttling has been set active.
    fn record_available_power(&self, available_power: Watts) {
        if self.throttling_active {
            self.throttle_history_list
                .back()
                .unwrap()
                .available_power_hist
                .insert(available_power.0);
        }
    }

    /// Record the current CPU power consumption for a given CPU index. No-op unless throttling has
    /// been set active.
    fn record_cpu_power_consumption(&mut self, cpu_index: usize, power_used: Watts) {
        if self.throttling_active {
            self.throttle_history_list
                .back_mut()
                .unwrap()
                .get_cpu_power_usage_property(cpu_index)
                .insert(power_used.0);
        }
    }
}

/// Stores data for a single throttle event.
struct InspectThrottleHistoryEntry {
    _node: inspect::Node,
    throttle_start_time: inspect::IntProperty,
    throttle_end_time: inspect::IntProperty,
    thermal_load_hist: inspect::UintLinearHistogramProperty,
    available_power_hist: inspect::DoubleLinearHistogramProperty,
    cpu_power_usage_node: inspect::Node,
    cpu_power_usage: Vec<inspect::DoubleLinearHistogramProperty>,
}

impl InspectThrottleHistoryEntry {
    /// Creates a new InspectThrottleHistoryEntry which creates new properties under `node`.
    fn new(node: inspect::Node) -> Self {
        Self {
            throttle_start_time: node.create_int("throttle_start_time", 0),
            throttle_end_time: node.create_int("throttle_end_time", 0),
            thermal_load_hist: node.create_uint_linear_histogram(
                "thermal_load_hist",
                LinearHistogramParams { floor: 0, step_size: 1, buckets: 100 },
            ),
            available_power_hist: node.create_double_linear_histogram(
                "available_power_hist",
                LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            ),
            cpu_power_usage_node: node.create_child("cpu_power_usage"),
            cpu_power_usage: Vec::new(),
            _node: node,
        }
    }

    /// Gets the property to record CPU power usage for the given CPU index. These properties are
    /// created dynamically because CPU domain count is not a fixed number.
    fn get_cpu_power_usage_property(
        &mut self,
        index: usize,
    ) -> &inspect::DoubleLinearHistogramProperty {
        if self.cpu_power_usage.get(index).is_none() {
            self.cpu_power_usage.push(self.cpu_power_usage_node.create_double_linear_histogram(
                index.to_string(),
                LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            ))
        }
        &self.cpu_power_usage[index]
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::cobalt_metrics::mock_cobalt_metrics::MockCobaltMetrics;
    use crate::test::mock_node::{create_dummy_node, MessageMatcher, MockNodeMaker};
    use crate::{msg_eq, msg_ok_return};
    use inspect::testing::{assert_inspect_tree, HistogramAssertion};

    pub fn get_sample_interval(thermal_policy: &ThermalPolicy) -> Seconds {
        thermal_policy.config.policy_params.controller_params.sample_interval
    }

    fn default_policy_params() -> ThermalPolicyParams {
        ThermalPolicyParams {
            controller_params: ThermalControllerParams {
                sample_interval: Seconds(1.0),
                filter_time_constant: Seconds(10.0),
                target_temperature: Celsius(85.0),
                e_integral_min: -20.0,
                e_integral_max: 0.0,
                sustainable_power: Watts(1.1),
                proportional_gain: 0.0,
                integral_gain: 0.2,
            },
            thermal_shutdown_temperature: Celsius(95.0),
            throttle_end_delay: Seconds(0.0),
        }
    }

    /// Tests the calculate_thermal_load function for correctness.
    #[test]
    fn test_calculate_thermal_load() {
        // These tests using invalid error integral values will panic on debug builds and the
        // `test_calculate_thermal_load_error_integral_*` tests will verify that. For now (non-debug
        // build), just test that the invalid values are clamped to a valid ThermalLoad value.
        if cfg!(not(debug_assertions)) {
            // An invalid error_integral greater than e_integral_max should clamp at ThermalLoad(0)
            assert_eq!(ThermalPolicy::calculate_thermal_load(5.0, -20.0, 0.0), ThermalLoad(0));

            // An invalid error_integral less than e_integral_min should clamp at ThermalLoad(100)
            assert_eq!(ThermalPolicy::calculate_thermal_load(-25.0, -20.0, 0.0), ThermalLoad(100));
        }

        // Test some error_integral values ranging from e_integral_max to e_integral_min
        assert_eq!(ThermalPolicy::calculate_thermal_load(0.0, -20.0, 0.0), ThermalLoad(0));
        assert_eq!(ThermalPolicy::calculate_thermal_load(-10.0, -20.0, 0.0), ThermalLoad(50));
        assert_eq!(ThermalPolicy::calculate_thermal_load(-20.0, -20.0, 0.0), ThermalLoad(100));
    }

    /// Tests that an invalid low error integral value will cause a panic on debug builds.
    #[test]
    #[should_panic = "error_integral (-25) less than error_integral_min (-20)"]
    #[cfg(debug_assertions)]
    fn test_calculate_thermal_load_error_integral_low_panic() {
        assert_eq!(ThermalPolicy::calculate_thermal_load(-25.0, -20.0, 0.0), ThermalLoad(100));
    }

    /// Tests that an invalid high error integral value will cause a panic on debug builds.
    #[test]
    #[should_panic = "error_integral (5) greater than error_integral_max (0)"]
    #[cfg(debug_assertions)]
    fn test_calculate_thermal_load_error_integral_high_panic() {
        assert_eq!(ThermalPolicy::calculate_thermal_load(5.0, -20.0, 0.0), ThermalLoad(0));
    }

    /// Tests that the `get_time_delta` function correctly calculates time delta while updating the
    /// `max_time_delta` state variable.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_time_delta() {
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };

        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(thermal_config).build(&node_futures).unwrap();

        assert_eq!(node.get_time_delta(Seconds(1.5).into()), Seconds(1.5));
        assert_eq!(node.get_time_delta(Seconds(2.0).into()), Seconds(0.5));
        assert_eq!(node.state.max_time_delta.get(), Seconds(1.5));
    }

    /// Tests that the `get_temperature_error` function correctly calculates proportional error and
    /// integral error.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_temperature_error() {
        let mut policy_params = default_policy_params();
        policy_params.controller_params.target_temperature = Celsius(80.0);
        policy_params.controller_params.e_integral_min = -20.0;
        policy_params.controller_params.e_integral_max = 0.0;

        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };
        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(thermal_config).build(&node_futures).unwrap();

        assert_eq!(node.get_temperature_error(Celsius(40.0), Seconds(1.0)), (40.0, 0.0));
        assert_eq!(node.get_temperature_error(Celsius(90.0), Seconds(1.0)), (-10.0, -10.0));
        assert_eq!(node.get_temperature_error(Celsius(90.0), Seconds(1.0)), (-10.0, -20.0));
    }

    /// Tests that the ThermalPolicy will correctly divide total available power amongst multiple
    /// CPU control nodes.
    #[fasync::run_singlethreaded(test)]
    async fn test_multiple_cpu_actors() {
        let mut mock_maker = MockNodeMaker::new();

        // Set up the two CpuControlHandler mock nodes. The message reply to SetMaxPowerConsumption
        // indicates how much power the mock node was able to utilize, and ultimately drives the
        // test logic.
        let cpu_node_1 = mock_maker.make(
            "CpuCtrlNode1",
            vec![
                // On the first iteration, this node will consume all available power (1W)
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(1.0))),
                ),
                // On the second iteration, this node will consume half of the available power
                // (0.5W)
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.5))),
                ),
                // On the third iteration, this node will consume none of the available power
                // (0.0W)
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.0))),
                ),
            ],
        );
        let cpu_node_2 = mock_maker.make(
            "CpuCtrlNode2",
            vec![
                // On the first iteration, the first node consumes all available power (1W), so
                // expect to receive a power allocation of 0W
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(0.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.0))),
                ),
                // On the second iteration, the first node consumes half of the available power
                // (1W), so expect to receive a power allocation of 0.5W
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(0.5))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.5))),
                ),
                // On the third iteration, the first node consumes none of the available power
                // (1W), so expect to receive a power allocation of 1W
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(1.0))),
                ),
            ],
        );

        let thermal_config = ThermalConfig {
            temperature_node: mock_maker.make("TemperatureNode", vec![]),
            cpu_control_nodes: vec![cpu_node_1, cpu_node_2],
            sys_pwr_handler: mock_maker.make("SysPwrNode", vec![]),
            thermal_limiter_node: mock_maker.make("ThermalLimiterNode", vec![]),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(thermal_config).build(&node_futures).unwrap();

        // Distribute 1W of total power across the two CPU nodes. The real test logic happens inside
        // the mock node, where we verify that the expected power amounts are granted to both CPU
        // nodes via the SetMaxPowerConsumption message. Repeat for the number of messages that the
        // mock nodes expect to receive (three).
        node.distribute_power(Watts(1.0)).await.unwrap();
        node.distribute_power(Watts(1.0)).await.unwrap();
        node.distribute_power(Watts(1.0)).await.unwrap();
    }

    /// Tests for the presence and correctness of dynamically-added inspect data
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_data() {
        let mut mock_maker = MockNodeMaker::new();

        let policy_params = default_policy_params();
        let thermal_config = ThermalConfig {
            temperature_node: mock_maker.make("TemperatureNode", vec![]),
            cpu_control_nodes: vec![mock_maker.make("CpuCtrlNode", vec![])],
            sys_pwr_handler: mock_maker.make("SysPwrNode", vec![]),
            thermal_limiter_node: mock_maker.make("ThermalLimiterNode", vec![]),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let inspector = inspect::Inspector::new();
        let node_futures = FuturesUnordered::new();
        let _node = ThermalPolicyBuilder::new(thermal_config)
            .with_inspect_root(inspector.root())
            .build(&node_futures)
            .unwrap();

        assert_inspect_tree!(
            inspector,
            root: {
                ThermalPolicy: {
                    state: contains {},
                    stats: contains {},
                    throttle_history: contains {},
                    policy_params: {
                        controller_params: {
                            "sample_interval (s)":
                                policy_params.controller_params.sample_interval.0,
                            "filter_time_constant (s)":
                                policy_params.controller_params.filter_time_constant.0,
                            "target_temperature (C)":
                                policy_params.controller_params.target_temperature.0,
                            "e_integral_min": policy_params.controller_params.e_integral_min,
                            "e_integral_max": policy_params.controller_params.e_integral_max,
                            "sustainable_power (W)":
                                policy_params.controller_params.sustainable_power.0,
                            "proportional_gain": policy_params.controller_params.proportional_gain,
                            "integral_gain": policy_params.controller_params.integral_gain,
                        }
                    }
                },
                platform_metrics: {
                    historical_max_cpu_temperature_c: {}
                }
            }
        );
    }

    /// Tests that throttle data is collected and stored properly in Inspect.
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_throttle_history() {
        let mut mock_maker = MockNodeMaker::new();

        // Set relevant policy parameters so we have deterministic power and thermal load
        // calculations
        let mut policy_params = default_policy_params();
        policy_params.controller_params.sustainable_power = Watts(1.0);
        policy_params.controller_params.proportional_gain = 0.0;
        policy_params.controller_params.integral_gain = 0.0;

        // Calculate the values that we expect to be reported by the thermal policy based on the
        // parameters chosen above
        let thermal_load = ThermalLoad(50);
        let throttle_available_power = policy_params.controller_params.sustainable_power;
        let throttle_available_power_cpu1 = throttle_available_power;
        let throttle_cpu1_power_used = throttle_available_power_cpu1 - Watts(0.3); // arbitrary
        let throttle_available_power_cpu2 =
            throttle_available_power_cpu1 - throttle_cpu1_power_used;
        let throttle_cpu2_power_used = throttle_available_power_cpu2;
        let throttle_start_time = Nanoseconds(0);
        let throttle_end_time = Nanoseconds(1000);

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![
                mock_maker.make(
                    "Cpu1Node",
                    vec![(
                        msg_eq!(SetMaxPowerConsumption(throttle_available_power_cpu1)),
                        msg_ok_return!(SetMaxPowerConsumption(throttle_cpu1_power_used)),
                    )],
                ),
                mock_maker.make(
                    "Cpu2Node",
                    vec![(
                        msg_eq!(SetMaxPowerConsumption(throttle_available_power_cpu2)),
                        msg_ok_return!(SetMaxPowerConsumption(throttle_cpu2_power_used)),
                    )],
                ),
            ],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };
        let inspector = inspect::Inspector::new();
        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_inspect_root(inspector.root())
            .build(&node_futures)
            .unwrap();

        // Causes Inspect to receive throttle_start_time and one reading into thermal_load_hist
        let _ = node.update_throttling_state(throttle_start_time, thermal_load).await;
        let _ = node.process_thermal_load(throttle_start_time, thermal_load).await;

        // Causes Inspect to receive one reading into available_power_hist and one reading into both
        // entries of cpu_power_usage
        let _ = node.update_power_allocation(0.0, 0.0).await;

        // Causes Inspect to receive throttle_end_time
        let _ = node.update_throttling_state(throttle_end_time, ThermalLoad(0)).await;
        let _ = node.process_thermal_load(throttle_end_time, ThermalLoad(0)).await;

        let mut expected_thermal_load_hist = HistogramAssertion::linear(LinearHistogramParams {
            floor: 0u64,
            step_size: 1,
            buckets: 100,
        });
        expected_thermal_load_hist.insert_values(vec![thermal_load.0.into()]);

        let mut expected_available_power_hist = HistogramAssertion::linear(LinearHistogramParams {
            floor: 0.0,
            step_size: 0.1,
            buckets: 100,
        });
        expected_available_power_hist.insert_values(vec![throttle_available_power.0]);

        let mut expected_cpu_1_power_usage_hist =
            HistogramAssertion::linear(LinearHistogramParams {
                floor: 0.0,
                step_size: 0.1,
                buckets: 100,
            });
        expected_cpu_1_power_usage_hist.insert_values(vec![throttle_cpu1_power_used.0]);

        let mut expected_cpu_2_power_usage_hist =
            HistogramAssertion::linear(LinearHistogramParams {
                floor: 0.0,
                step_size: 0.1,
                buckets: 100,
            });
        expected_cpu_2_power_usage_hist.insert_values(vec![throttle_cpu2_power_used.0]);

        assert_inspect_tree!(
            inspector,
            root: contains {
                ThermalPolicy: contains {
                    throttle_history: {
                        "0": {
                            throttle_start_time: throttle_start_time.0,
                            throttle_end_time: throttle_end_time.0,
                            thermal_load_hist: expected_thermal_load_hist,
                            available_power_hist: expected_available_power_hist,
                            cpu_power_usage: {
                                "0": expected_cpu_1_power_usage_hist,
                                "1": expected_cpu_2_power_usage_hist
                            }
                        },
                    }
                }
            }
        )
    }

    /// Verifies that InspectThrottleHistory correctly removes old entries without increasing the
    /// size of the underlying vector.
    #[test]
    fn test_inspect_throttle_history_length() {
        // Create a InspectThrottleHistory with capacity for only one throttling entry
        let mut throttle_history = InspectThrottleHistory::new(
            inspect::Inspector::new().root().create_child("test_node"),
            1,
        );

        // Add a throttling entry
        throttle_history.mark_throttling_active(Nanoseconds(0));
        throttle_history.mark_throttling_inactive(Nanoseconds(0));

        // Verify one entry and unchanged capacity
        assert_eq!(throttle_history.throttle_history_list.len(), 1);
        assert_eq!(throttle_history.throttle_history_list.capacity(), 1);
        assert_eq!(throttle_history.entry_count, 1);

        // Add one more throttling entry
        throttle_history.mark_throttling_active(Nanoseconds(0));
        throttle_history.mark_throttling_inactive(Nanoseconds(0));

        // Verify still one entry and unchanged capacity
        assert_eq!(throttle_history.throttle_history_list.len(), 1);
        assert_eq!(throttle_history.throttle_history_list.capacity(), 1);
        assert_eq!(throttle_history.entry_count, 2);
    }

    /// Tests that the platform_metrics Inspect node is present and correctly populated. The test
    /// works by iteration the thermal policy 200 times, then verifying that (200 % 60 = 20) max CPU
    /// temperature entries were made to the platform_metrics Inspect node.
    #[test]
    fn test_inspect_platform_metrics() {
        let mut executor = fasync::Executor::new_with_fake_time().unwrap();
        executor.set_fake_time(Seconds(0.0).into());

        let mut mock_maker = MockNodeMaker::new();

        let thermal_config = ThermalConfig {
            temperature_node: mock_maker.make(
                "TemperatureNode",
                (0..60)
                    .map(|_| {
                        (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(50.0))))
                    })
                    .collect(),
            ),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let inspector = inspect::Inspector::new();
        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_inspect_root(inspector.root())
            .build(&node_futures)
            .unwrap();

        for _ in 0..60 {
            assert!(executor
                .run_until_stalled(&mut node.iterate_thermal_control().boxed_local())
                .is_ready());
        }

        assert_inspect_tree!(
            inspector,
            root: contains {
                platform_metrics: {
                    historical_max_cpu_temperature_c: {
                        "0": 50i64
                    }
                }
            }
        );
    }

    /// Tests that well-formed configuration JSON does not panic the `new_from_json` function.
    #[fasync::run_singlethreaded(test)]
    async fn test_new_from_json() {
        let json_data = json::json!({
            "type": "ThermalPolicy",
            "name": "thermal_policy",
            "config": {
                "thermal_shutdown_temperature": 95.0,
                "throttle_end_delay": 0.0,
                "controller_params": {
                    "sample_interval": 1.0,
                    "filter_time_constant": 5.0,
                    "target_temperature": 80.0,
                    "e_integral_min": -20.0,
                    "e_integral_max": 0.0,
                    "sustainable_power": 0.876,
                    "proportional_gain": 0.0,
                    "integral_gain": 0.08
                }
            },
            "dependencies": {
                "cpu_control_nodes": [
                    "cpu_control"
                ],
                "system_power_handler_node": "sys_power",
                "temperature_handler_node": "temperature",
                "thermal_limiter_node": "limiter",
                "crash_report_handler_node": "crash_report"
              },
        });

        let mut nodes: HashMap<String, Rc<dyn Node>> = HashMap::new();
        nodes.insert("temperature".to_string(), create_dummy_node());
        nodes.insert("cpu_control".to_string(), create_dummy_node());
        nodes.insert("sys_power".to_string(), create_dummy_node());
        nodes.insert("limiter".to_string(), create_dummy_node());
        nodes.insert("crash_report".to_string(), create_dummy_node());
        let _ = ThermalPolicyBuilder::new_from_json(json_data, &nodes);
    }

    /// Tests that the ThermalPolicy correctly updates the Cobalt metrics instance as its thermal
    /// state cycles between the various states.
    #[test]
    fn test_cobalt_metrics() {
        let mut mock_maker = MockNodeMaker::new();

        // Set custom thermal policy parameters to have easier control over throttling state changes
        let mut policy_params = default_policy_params();
        policy_params.throttle_end_delay = Seconds(10.0);
        policy_params.controller_params.target_temperature = Celsius(50.0);
        policy_params.thermal_shutdown_temperature = Celsius(80.0);

        // The executor's fake time must be set before node creation to ensure the periodic timer's
        // deadline is properly initialized.
        let executor = fasync::Executor::new_with_fake_time().unwrap();
        executor.set_fake_time(Seconds(0.0).into());

        let mock_metrics = MockCobaltMetrics::new();
        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(ThermalConfig {
            temperature_node: mock_maker.make(
                "Temperature",
                vec![
                    // These temperature readings combined with the TemperatureFilter reset in
                    // between policy iterations below causes us to cycle easily and
                    // deterministically between throttling states

                    // Begin thermal throttling
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(55.0)))),
                    // Activate cooldown timer
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    // Back into thermal throttling
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(55.0)))),
                    // Activate and run down cooldown timer
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    // End thermal throttling
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(45.0)))),
                    // Thermal shutdown
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(90.0)))),
                ],
            ),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: mock_maker.make(
                "CrashReport",
                vec![(
                    // The test ends with a thermal shutdown so ensure the mock node expects it
                    msg_eq!(FileCrashReport("fuchsia-thermal-throttle".to_string())),
                    msg_ok_return!(FileCrashReport),
                )],
            ),
            policy_params,
        })
        .with_thermal_metrics(Box::new(mock_metrics.clone()))
        .build(&node_futures)
        .unwrap();

        // Wrap the executor, node, and futures in a simple struct that drives time steps. This
        // enables the control flow:
        // 1. Call TimeStepper::schedule_wakeup to wake the periodic timer, exposing the time of the
        //    wakeup, which may be needed for metric expectations.
        // 2. Set metric expectations.
        // 3. Iterate the ThermalPolicy.
        // 4. Verify metric expectations.
        struct TimeStepper<'a> {
            executor: fasync::Executor,
            node: Rc<ThermalPolicy>,
            node_futures: FuturesUnordered<LocalBoxFuture<'a, ()>>,
        }

        impl<'a> TimeStepper<'a> {
            fn schedule_wakeup(&mut self) -> Nanoseconds {
                let wakeup_time = self.executor.wake_next_timer().unwrap();
                self.executor.set_fake_time(wakeup_time);
                Nanoseconds::from(wakeup_time)
            }

            fn iterate_policy(&mut self) {
                self.node.state.temperature_filter.reset();

                assert_eq!(
                    futures::task::Poll::Pending,
                    self.executor.run_until_stalled(&mut self.node_futures.next())
                );
            }
        }

        let mut stepper = TimeStepper { executor, node, node_futures };

        // Begin thermal throttling
        let next_time = stepper.schedule_wakeup();
        mock_metrics.expect_log_throttle_start(next_time);
        mock_metrics.expect_log_raw_temperature(Celsius(55.0));
        stepper.iterate_policy();
        mock_metrics.verify("Didn't receive expected calls for 'Begin thermal throttling 1'");

        // Active cooldown timer
        stepper.schedule_wakeup();
        mock_metrics.expect_log_raw_temperature(Celsius(45.0));
        stepper.iterate_policy();
        mock_metrics.verify("Didn't receive expected calls for 'Active cooldown timer 1'");

        // Back into thermal throttling
        stepper.schedule_wakeup();
        mock_metrics.expect_log_raw_temperature(Celsius(55.0));
        stepper.iterate_policy();
        mock_metrics.verify("Didn't receive expected calls for 'Begin thermal throttling 2'");

        // Begin the active cooldown timer on iteration 0, and run for 9 more seconds
        for _ in 0..10 {
            stepper.schedule_wakeup();
            mock_metrics.expect_log_raw_temperature(Celsius(45.0));
            stepper.iterate_policy();
            mock_metrics.verify("Didn't receive expected calls for 'Active cooldown timer 2'");
        }

        // Ten seconds after cooldown starts, thermal throttling ends
        let next_time = stepper.schedule_wakeup();
        mock_metrics.expect_log_throttle_end_mitigated(next_time);
        mock_metrics.expect_log_raw_temperature(Celsius(45.0));
        stepper.iterate_policy();
        mock_metrics.verify("Didn't receive expected calls for 'End thermal throttling'");

        // Cause the thermal policy to enter thermal shutdown
        let next_time = stepper.schedule_wakeup();
        mock_metrics.expect_log_throttle_start(next_time);
        mock_metrics.expect_log_raw_temperature(Celsius(90.0));
        mock_metrics.expect_log_throttle_end_shutdown(next_time);
        stepper.iterate_policy();
        mock_metrics.verify("Didn't receive expected calls for 'Thermal shutdown'");
    }

    /// Tests that when thermal throttling exits, the ThermalPolicy triggers a crash report on the
    /// CrashReportHandler node.
    #[fasync::run_singlethreaded(test)]
    async fn test_throttle_crash_report() {
        let mut mock_maker = MockNodeMaker::new();

        // Define the test parameters
        let mut policy_params = default_policy_params();
        policy_params.throttle_end_delay = Seconds(0.0);

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: mock_maker.make(
                "CrashReportMock",
                vec![(
                    msg_eq!(FileCrashReport("fuchsia-thermal-throttle".to_string())),
                    msg_ok_return!(FileCrashReport),
                )],
            ),
            policy_params,
        };
        let node_futures = FuturesUnordered::new();
        let node = ThermalPolicyBuilder::new(thermal_config).build(&node_futures).unwrap();

        // Enter and then exit thermal throttling. The mock crash_report_handler node will assert if
        // it does not receive a FileCrashReport message.
        let _ = node.update_throttling_state(Nanoseconds(0), ThermalLoad(50)).await;
        let _ = node.update_throttling_state(Nanoseconds(0), ThermalLoad(0)).await;
    }
}
