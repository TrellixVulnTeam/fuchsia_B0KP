// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The audio policy handler intercepts and modifies requests going into and out of the audio
//! setting in order to apply audio policies that have been added through the
//! fuchsia.settings.policy.Audio FIDL interface.
//!
//! # Role of the audio policy handler
//!
//! From the audio policy handler perspective, there are two views of the audio state, external and
//! internal. The external state is what clients of the settings service receive when they request
//! the audio state. The internal state is what the underlying audio setting thinks the audio state
//! is. In the absence of any policies, these states are always the same, but when policies are
//! applied, the two may be different. The policy handler's job is to intercept communications and
//! convert between internal and external state as needed so that both sides have a self-consistent
//! view of the audio state.
//!
//! # Handling SettingRequests
//!
//! There are two main classes of SettingRequests to the audio setting itself, gets and sets. When a
//! set request that seeks to modify the audio state is intercepted, the policy handler modifies the
//! request by applying the audio policy transforms to the request. This includes actions like
//! scaling and clamping the volume levels. The set request is then propagated to be handled by the
//! setting handler.
//!
//! When a get request is intercepted, the handler asks the underlying audio setting for the current
//! value, then performs the reverse calculations of the policy transforms to recover the external
//! audio state. The policy handler then directly responds to the client with the calculated audio
//! state.
//!
//! # Handling SettingEvents
//!
//! Setting handlers send a [`Changed`] event when their state changes so that the settings service
//! notify any active listeners. When the policy handler intercepts one of these events, it performs
//! the same reverse calculation as is done for a get request and passes along the changed event.
//!
//! # Handling policy changes
//!
//! When policies are added and removed, the policy handler may need to update the internal and
//! external audio states due to the newly added policies. For both additions and removals of
//! policies, the policy handler will request the latest audio state from the underlying setting and
//! calculate the current external audio state. Once it knows the current internal and external
//! states, it updates and persists the policy state, then calculates the new internal and external
//! states.
//!
//! If the internal audio state needs to be updated, the policy handler simply sends its own set
//! request to the underlying setting. This will trigger updates to external listeners, which the
//! setting handler sends as a Changed event that is intercepted and dealt with as described above.
//!
//! However, in some cases, the internal state may not change but the external state changes, such
//! as removing a max volume limit, which is transparent to the user. For example, if the max volume
//! is 80% and the external volume is 100% and the max volume policy is removed, the external volume
//! has to be updated to match the internal volume of 80%. In these cases, the policy handler sends
//! a Changed event in place of the setting handler to trigger updates to external listeners.
//!
//! [`Changed`]: handler::base::Event::Changed

use crate::audio::default_audio_info;
use crate::audio::policy::{
    self as audio_policy, PolicyId, PropertyTarget, Request as PolicyRequest, Response, State,
    StateBuilder, Transform, TransformFlags,
};
use crate::audio::types::AudioInfo;
use crate::audio::utils::round_volume_level;
use crate::base::{SettingInfo, SettingType};
use crate::handler::base::{Request as SettingRequest, Response as SettingResponse};
use crate::handler::device_storage::{DeviceStorageAccess, DeviceStorageCompatible};
use crate::internal::core::Payload;
use crate::policy::base as policy_base;
use crate::policy::base::response::Error as PolicyError;
use crate::policy::base::PolicyInfo;
use crate::policy::policy_handler::{
    ClientProxy, Create, EventTransform, PolicyHandler, RequestTransform, ResponseTransform,
};
use crate::switchboard::base::SettingEvent;
use anyhow::{format_err, Error};
use async_trait::async_trait;
use fuchsia_syslog::fx_log_err;

/// Used as the argument field in a ControllerError::InvalidArgument to signal the FIDL handler to
/// signal that the policy ID was invalid.
pub const ARG_POLICY_ID: &'static str = "policy_id";

/// `AudioPolicyHandler` controls the persistence and enforcement of audio policies set by
/// fuchsia.settings.policy.VolumePolicyController.
pub struct AudioPolicyHandler {
    /// Policy state containing all of the transforms.
    state: State,

    /// Offers access to common functionality like messaging and storage.
    client_proxy: ClientProxy<State>,
}

impl DeviceStorageAccess for AudioPolicyHandler {
    const STORAGE_KEYS: &'static [&'static str] = &[State::KEY];
}

/// Maximum allowed volume for an audio stream.
const MAX_VOLUME: f32 = 1.0;

/// Minimum allowed volume for an audio stream.
const MIN_VOLUME: f32 = 0.0;

struct VolumeLimits {
    min_volume: f32,
    max_volume: f32,
}

#[async_trait]
impl Create<State> for AudioPolicyHandler {
    async fn create(client_proxy: ClientProxy<State>) -> Result<Self, Error> {
        Ok(Self {
            state: AudioPolicyHandler::restore_policy_state(client_proxy.read().await),
            client_proxy,
        })
    }
}

#[async_trait]
impl PolicyHandler for AudioPolicyHandler {
    async fn handle_policy_request(
        &mut self,
        request: policy_base::Request,
    ) -> policy_base::response::Response {
        match request {
            policy_base::Request::Get => Ok(policy_base::response::Payload::PolicyInfo(
                PolicyInfo::Audio(self.state.clone()),
            )),
            policy_base::Request::Audio(audio_request) => match audio_request {
                PolicyRequest::AddPolicy(target, transform) => {
                    self.add_policy_transform(target, transform).await
                }
                PolicyRequest::RemovePolicy(policy_id) => {
                    self.remove_policy_transform(policy_id).await
                }
            },
        }
    }

    async fn handle_setting_request(
        &mut self,
        request: SettingRequest,
    ) -> Option<RequestTransform> {
        match request {
            SettingRequest::SetVolume(mut streams) => {
                // When anyone attempts to set the volume level, scale it according to the policy
                // limits and pass it along to the setting proxy.
                for stream in streams.iter_mut() {
                    stream.user_volume_level = self
                        .calculate_internal_volume(stream.stream_type, stream.user_volume_level);
                    stream.user_volume_muted &= self.calculate_can_mute(stream.stream_type);
                }

                Some(RequestTransform::Request(SettingRequest::SetVolume(streams)))
            }
            SettingRequest::Get => {
                // When the audio settings are read, scale the internal values to their external
                // values and return this directly to the caller.
                // TODO(fxbug.dev/67678): use policy proxy mechanism to subscribe to reply.
                let audio_info = match self.fetch_audio_info().await {
                    Ok(audio_info) => audio_info,
                    // Failed to fetch audio info, don't attempt to serve the request.
                    // TODO(fxbug.dev/67667): surface these errors higher in the policy design and
                    // handle them.
                    Err(_) => {
                        fx_log_err!("Failed to fetch audio info");
                        return None;
                    }
                };
                Some(RequestTransform::Result(Ok(Some(SettingInfo::Audio(
                    self.transform_internal_audio_info(audio_info),
                )))))
            }
            _ => None,
        }
    }

    async fn handle_setting_event(&mut self, event: SettingEvent) -> Option<EventTransform> {
        match event {
            SettingEvent::Changed(SettingInfo::Audio(audio_info)) => {
                // The setting changed in response to a Set. Note that this is event is not sent if
                // there are no active listeners.
                Some(EventTransform::Event(SettingEvent::Changed(SettingInfo::Audio(
                    self.transform_internal_audio_info(audio_info),
                ))))
            }
            _ => None,
        }
    }

    async fn handle_setting_response(
        &mut self,
        response: SettingResponse,
    ) -> Option<ResponseTransform> {
        match response {
            Ok(Some(SettingInfo::Audio(audio_info))) => {
                // The setting changed in response to a Set. Note that this is
                // is not sent if there are no listeners.
                Some(ResponseTransform::Response(Ok(Some(SettingInfo::Audio(
                    self.transform_internal_audio_info(audio_info),
                )))))
            }
            _ => None,
        }
    }
}

impl AudioPolicyHandler {
    /// Restores the policy state based on the configured audio streams and the previously persisted
    /// state.
    fn restore_policy_state(persisted_state: State) -> State {
        // Read the audio default info to see what policy targets are valid and create properties.
        let audio_info = default_audio_info();
        let mut state_builder = StateBuilder::new();
        for stream in audio_info.streams.iter() {
            // TODO(fxbug.dev/60925): read configuration to see what transform flags to enable for a
            // given stream.
            state_builder = state_builder.add_property(stream.stream_type, TransformFlags::all());
        }

        let mut state = state_builder.build();

        // Restore active policies from the persisted properties.
        for (target, persisted_property) in persisted_state.properties.into_iter() {
            state.properties.entry(target).and_modify(|property| {
                property.active_policies = persisted_property.active_policies
            });
        }

        state
    }

    /// Requests the current audio state from the audio setting proxy.
    async fn fetch_audio_info(&self) -> Result<AudioInfo, Error> {
        self.client_proxy.send_setting_request(SettingRequest::Get).next_payload().await.and_then(
            |(payload, _)| {
                if let Payload::Event(SettingEvent::Response(
                    _,
                    Ok(Some(SettingInfo::Audio(audio_info))),
                )) = payload
                {
                    Ok(audio_info)
                } else {
                    Err(format_err!("did not receive setting value"))
                }
            },
        )
    }

    /// Determines the max and min volume levels for the given property based on the active
    /// transforms.
    ///
    /// Returns a struct containing the min and max volume limits.
    fn determine_volume_limits(&self, target: PropertyTarget) -> VolumeLimits {
        let mut max_volume: f32 = MAX_VOLUME;
        let mut min_volume: f32 = MIN_VOLUME;
        match self.state.properties.get(&target) {
            Some(property) => {
                for policy in property.active_policies.iter() {
                    match policy.transform {
                        // Only the lowest max volume applies.
                        audio_policy::Transform::Max(max) => max_volume = max_volume.min(max),
                        // Only the highest min volume applies
                        audio_policy::Transform::Min(min) => min_volume = min_volume.max(min),
                    }
                }
            }
            // If the property doesn't have a state, there are no limits, so return the default min
            // and max.
            _ => {}
        }
        VolumeLimits { min_volume, max_volume }
    }

    /// Reverses the policy limits on internal volume for the given audio stream to their external
    /// levels.
    fn calculate_external_volume(&self, target: PropertyTarget, internal_volume: f32) -> f32 {
        let VolumeLimits { max_volume, .. } = self.determine_volume_limits(target);

        round_volume_level(internal_volume / max_volume)
    }

    /// Scales an external volume from client input for the given audio stream to the limits set by
    /// policies.
    fn calculate_internal_volume(&self, target: PropertyTarget, external_volume: f32) -> f32 {
        let VolumeLimits { max_volume, min_volume } = self.determine_volume_limits(target);

        // We don't need to round this value as the audio setting internals will round it anyways.
        min_volume.max(external_volume * max_volume)
    }

    /// Determines whether or not the stream can be muted based on the policy state.
    fn calculate_can_mute(&self, target: PropertyTarget) -> bool {
        // Stream can only be muted if there's no min volume limit.
        self.determine_volume_limits(target).min_volume == MIN_VOLUME
    }

    /// Clamps the volume of the given audio stream based on the limits set by policies.
    ///
    /// This function should only be used on internal volume levels since the policy limits only
    /// apply on the internal volume; external volume levels are purely calculated based on the
    /// internal levels which are already clamped.
    fn clamp_internal_volume(&self, target: PropertyTarget, internal_volume: f32) -> f32 {
        let VolumeLimits { max_volume, min_volume } = self.determine_volume_limits(target);

        // We don't need to round this value as the audio setting internals will round it anyways.
        internal_volume.max(min_volume).min(max_volume)
    }

    /// Adds a transform to the given target.
    // TODO(fxbug.dev/60966): perform validations and return errors for invalid inputs, such as
    // a max being lower than a min.
    async fn add_policy_transform(
        &mut self,
        target: PropertyTarget,
        transform: Transform,
    ) -> policy_base::response::Response {
        // Request the latest audio info.
        let audio_info =
            self.fetch_audio_info().await.map_err(|_| policy_base::response::Error::Unexpected)?;

        // Calculate the current external volume level.
        let stream = audio_info
            .streams
            .iter()
            .find(|stream| stream.stream_type == target)
            .ok_or_else(|| policy_base::response::Error::Unexpected)?;
        let external_volume = self.calculate_external_volume(target, stream.user_volume_level);

        // Add the transform the policy state.
        let policy_id = self
            .state
            .properties
            .get_mut(&target)
            // TODO(fxbug.dev/60925): once policy targets are configurable, test this error case.
            .ok_or(PolicyError::InvalidArgument(
                self.client_proxy.policy_type(),
                "target".into(),
                format!("{:?}", target).into(),
            ))?
            .add_transform(transform);

        // Persist the policy state.
        self.client_proxy.write(self.state.clone(), false).await?;

        // Put the transform into effect, updating internal/external volume levels as needed.
        self.apply_policy_transforms(target, audio_info, external_volume).await?;

        Ok(policy_base::response::Payload::Audio(Response::Policy(policy_id)))
    }

    /// Removes a transform with the given ID.
    async fn remove_policy_transform(
        &mut self,
        policy_id: PolicyId,
    ) -> policy_base::response::Response {
        // Find the target this policy ID is on.
        let target =
            self.state.find_policy_target(policy_id).ok_or(PolicyError::InvalidArgument(
                self.client_proxy.policy_type(),
                ARG_POLICY_ID.into(),
                format!("{:?}", policy_id).into(),
            ))?;

        // Found a policy.
        let audio_info =
            self.fetch_audio_info().await.map_err(|_| policy_base::response::Error::Unexpected)?;

        // Calculate the current external volume level.
        let stream = audio_info
            .streams
            .iter()
            .find(|stream| stream.stream_type == target)
            .ok_or_else(|| policy_base::response::Error::Unexpected)?;
        let external_volume = self.calculate_external_volume(target, stream.user_volume_level);

        // Attempt to remove the policy.
        self.state.remove_policy(policy_id).ok_or(PolicyError::InvalidArgument(
            self.client_proxy.policy_type(),
            ARG_POLICY_ID.into(),
            format!("{:?}", policy_id).into(),
        ))?;

        // Persist the policy state.
        self.client_proxy.write(self.state.clone(), false).await?;

        // Put the transform into effect, updating internal/external volume levels as needed.
        self.apply_policy_transforms(target, audio_info, external_volume).await?;

        Ok(policy_base::response::Payload::Audio(Response::Policy(policy_id)))
    }

    /// Calculates and applies updates to the internal and external volume levels of the given audio
    /// stream.
    ///
    /// This method needs to know the external volume level before changes were made in order to
    /// determine if a changed event should be sent to listeners of the base setting.
    // TODO(fxbug.dev/67784): consider keeping copy of external audio info so previous external
    // volume doesn't need to be calculated.
    async fn apply_policy_transforms(
        &mut self,
        target: PropertyTarget,
        audio_info: AudioInfo,
        previous_external_volume: f32,
    ) -> Result<(), policy_base::response::Error> {
        let original = audio_info
            .streams
            .iter()
            .find(|stream| stream.stream_type == target)
            .ok_or_else(|| policy_base::response::Error::Unexpected)?
            .clone();

        // Make a copy to apply policy transforms on.
        let mut transformed = original.clone();

        transformed.user_volume_level =
            self.clamp_internal_volume(target, original.user_volume_level);
        transformed.user_volume_muted =
            original.user_volume_muted & self.calculate_can_mute(target);

        let new_external_volume =
            self.calculate_external_volume(target, transformed.user_volume_level);

        // Set internal/external volume state as needed.
        if transformed != original {
            // When the internal volume level should change, send a Set request to the audio
            // controller. If this would cause the external volume levels to change as well, that
            // will be handled automatically as a Changed event in handle_setting_event.

            self.client_proxy.send_setting_request(SettingRequest::SetVolume(vec![transformed]));
        } else if new_external_volume != previous_external_volume {
            // In some cases, such as when a max volume limit is removed, the internal volume may
            // not change but the external volume should still be updated. We send a Rebroadcast
            // request to the setting proxy, triggering an update for external clients.
            self.client_proxy.request_rebroadcast(SettingType::Audio);
        }

        Ok(())
    }

    /// Transforms an audio info from the audio setting based on the active policy transforms into
    /// the result that we surface externally.
    fn transform_internal_audio_info(&mut self, mut external_audio_info: AudioInfo) -> AudioInfo {
        for stream in external_audio_info.streams.iter_mut() {
            stream.user_volume_level =
                self.calculate_external_volume(stream.stream_type, stream.user_volume_level);
        }
        external_audio_info
    }
}
