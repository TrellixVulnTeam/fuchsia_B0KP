// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::agent::restore_agent,
    crate::audio::types::{
        AudioInfo, AudioInputInfo, AudioSettingSource, AudioStream, AudioStreamType,
    },
    crate::audio::{create_default_modified_counters, default_audio_info},
    crate::base::SettingType,
    crate::handler::device_storage::testing::*,
    crate::handler::device_storage::DeviceStorage,
    crate::input::common::MediaButtonsEventBuilder,
    crate::tests::fakes::audio_core_service::{self, AudioCoreService},
    crate::tests::fakes::input_device_registry_service::InputDeviceRegistryService,
    crate::tests::fakes::service_registry::ServiceRegistry,
    crate::tests::fakes::sound_player_service::SoundPlayerService,
    crate::tests::test_failure_utils::create_test_env_with_failures,
    crate::AgentType,
    crate::EnvironmentBuilder,
    fidl::Error::ClientChannelClosed,
    fidl_fuchsia_media::AudioRenderUsage,
    fidl_fuchsia_settings::*,
    fuchsia_component::server::NestedEnvironment,
    fuchsia_zircon::Status,
    futures::lock::Mutex,
    matches::assert_matches,
    std::sync::Arc,
};

const ENV_NAME: &str = "settings_service_audio_test_environment";
const CONTEXT_ID: u64 = 0;

const CHANGED_VOLUME_LEVEL: f32 = 0.7;
const CHANGED_VOLUME_LEVEL_2: f32 = 0.95;
const CHANGED_VOLUME_MUTED: bool = true;

const CHANGED_MEDIA_STREAM: AudioStream = AudioStream {
    stream_type: AudioStreamType::Media,
    source: AudioSettingSource::User,
    user_volume_level: CHANGED_VOLUME_LEVEL,
    user_volume_muted: CHANGED_VOLUME_MUTED,
};

const CHANGED_MEDIA_STREAM_2: AudioStream = AudioStream {
    stream_type: AudioStreamType::Media,
    source: AudioSettingSource::User,
    user_volume_level: CHANGED_VOLUME_LEVEL_2,
    user_volume_muted: CHANGED_VOLUME_MUTED,
};

const CHANGED_MEDIA_STREAM_SETTINGS: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(CHANGED_VOLUME_LEVEL),
        muted: Some(CHANGED_VOLUME_MUTED),
        ..Volume::EMPTY
    }),
    ..AudioStreamSettings::EMPTY
};

const CHANGED_MEDIA_STREAM_SETTINGS_2: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(CHANGED_VOLUME_LEVEL_2),
        muted: Some(CHANGED_VOLUME_MUTED),
        ..Volume::EMPTY
    }),
    ..AudioStreamSettings::EMPTY
};

/// Creates an environment that will fail on a get request.
async fn create_audio_test_env_with_failures(
    storage_factory: Arc<Mutex<InMemoryStorageFactory>>,
) -> AudioProxy {
    create_test_env_with_failures(storage_factory, ENV_NAME, SettingType::Audio)
        .await
        .connect_to_service::<AudioMarker>()
        .unwrap()
}

// Used to store fake services for mocking dependencies and checking input/outputs.
// To add a new fake to these tests, add here, in create_services, and then use
// in your test.
struct FakeServices {
    audio_core: Arc<Mutex<AudioCoreService>>,
    input_device_registry: Arc<Mutex<InputDeviceRegistryService>>,
}

fn get_default_stream(stream_type: AudioStreamType) -> AudioStream {
    *default_audio_info()
        .streams
        .iter()
        .find(|x| x.stream_type == stream_type)
        .expect("contains stream")
}

async fn set_volume(proxy: &AudioProxy, streams: Vec<AudioStreamSettings>) {
    let mut audio_settings = AudioSettings::EMPTY;
    audio_settings.streams = Some(streams);
    proxy.set(audio_settings).await.expect("set completed").expect("set successful");
}

// Verifies that a stream equal to |stream| is inside of |settings|.
fn verify_audio_stream(settings: AudioSettings, stream: AudioStreamSettings) {
    settings
        .streams
        .expect("audio settings contain streams")
        .into_iter()
        .find(|x| *x == stream)
        .expect("contains stream");
}

// Gets the store from |factory| and populate it with default values.
async fn create_storage(factory: Arc<Mutex<InMemoryStorageFactory>>) -> DeviceStorage {
    let store = factory
        .lock()
        .await
        .get_device_storage::<AudioInfo>(StorageAccessContext::Test, CONTEXT_ID);
    let audio_info = default_audio_info();
    store.write(&audio_info, false).await.unwrap();
    store
}

// Verify that |streams| contain |stream|.
fn verify_contains_stream(streams: &[AudioStream; 5], stream: &AudioStream) {
    streams.into_iter().find(|x| *x == stream).expect("contains changed media stream");
}

// Returns a registry and audio related services it is populated with
async fn create_services() -> (Arc<Mutex<ServiceRegistry>>, FakeServices) {
    let service_registry = ServiceRegistry::create();
    let audio_core_service_handle = audio_core_service::Builder::new().build();
    service_registry.lock().await.register_service(audio_core_service_handle.clone());

    let input_device_registry_service_handle =
        Arc::new(Mutex::new(InputDeviceRegistryService::new()));
    service_registry.lock().await.register_service(input_device_registry_service_handle.clone());

    let sound_player_service_handle = Arc::new(Mutex::new(SoundPlayerService::new()));
    service_registry.lock().await.register_service(sound_player_service_handle.clone());

    (
        service_registry,
        FakeServices {
            audio_core: audio_core_service_handle,
            input_device_registry: input_device_registry_service_handle,
        },
    )
}

async fn create_environment(
    service_registry: Arc<Mutex<ServiceRegistry>>,
) -> (NestedEnvironment, DeviceStorage) {
    let storage_factory = InMemoryStorageFactory::create();
    let store = create_storage(storage_factory.clone()).await;

    let env = EnvironmentBuilder::new(storage_factory)
        .service(ServiceRegistry::serve(service_registry))
        .settings(&[SettingType::Audio])
        .agents(&[AgentType::MediaButtons.into()])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    (env, store)
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_audio() {
    let (service_registry, fake_services) = create_services().await;
    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS]).await;
    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    // Check to make sure value wrote out to store correctly.
    let stored_streams = store.get::<AudioInfo>().await.streams;
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM);
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_consecutive_volume_changes() {
    let (service_registry, fake_services) = create_services().await;
    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS]).await;
    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_2]).await;
    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS_2);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL_2, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    // Check to make sure value wrote out to store correctly.
    let stored_streams = store.get::<AudioInfo>().await.streams;
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM_2);
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_multiple_changes_on_stream() {
    let (service_registry, _) = create_services().await;
    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS]).await;
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_2]).await;
    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS_2);

    // Check to make sure value wrote out to store correctly.
    let stored_streams = store.get::<AudioInfo>().await.streams;
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM_2);
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_volume_overwritten() {
    let (service_registry, fake_services) = create_services().await;
    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS]).await;
    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    // Check to make sure value wrote out to store correctly.
    let stored_streams = store.get::<AudioInfo>().await.streams;
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM);

    const CHANGED_BACKGROUND_STREAM_SETTINGS: AudioStreamSettings = AudioStreamSettings {
        stream: Some(fidl_fuchsia_media::AudioRenderUsage::Background),
        source: Some(AudioStreamSettingSource::User),
        user_volume: Some(Volume { level: Some(0.3), muted: Some(true), ..Volume::EMPTY }),
        ..AudioStreamSettings::EMPTY
    };

    set_volume(&audio_proxy, vec![CHANGED_BACKGROUND_STREAM_SETTINGS]).await;
    let settings = audio_proxy.watch().await.expect("watch completed");

    // Changing the background volume should not affect media volume.
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);
    verify_audio_stream(settings.clone(), CHANGED_BACKGROUND_STREAM_SETTINGS);
}

// Tests that the volume level gets rounded to two decimal places.
#[fuchsia_async::run_until_stalled(test)]
async fn test_volume_rounding() {
    let (service_registry, fake_services) = create_services().await;

    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(
        &audio_proxy,
        vec![AudioStreamSettings {
            stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
            source: Some(AudioStreamSettingSource::User),
            user_volume: Some(Volume {
                level: Some(0.7015),
                muted: Some(CHANGED_VOLUME_MUTED),
                ..Volume::EMPTY
            }),
            ..AudioStreamSettings::EMPTY
        }],
    )
    .await;

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    // Check to make sure value wrote out to store correctly.
    let stored_streams = store.get::<AudioInfo>().await.streams;
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM);
}

// Test to ensure mic input change events are received.
#[fuchsia_async::run_until_stalled(test)]
async fn test_audio_input() {
    let (service_registry, fake_services) = create_services().await;

    let (env, _) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let buttons_event = MediaButtonsEventBuilder::new().set_volume(1).set_mic_mute(true).build();

    fake_services.input_device_registry.lock().await.send_media_button_event(buttons_event.clone());

    let updated_settings = audio_proxy.watch().await.expect("watch completed");

    let input = updated_settings.input.expect("Should have input settings");
    let mic_mute = input.muted.expect("Should have mic mute value");
    assert!(mic_mute);
}

/// Test that the audio settings are restored correctly.
#[fuchsia_async::run_until_stalled(test)]
async fn test_volume_restore() {
    let (service_registry, fake_services) = create_services().await;
    let storage_factory = InMemoryStorageFactory::create();
    let expected_info = (0.9, false);
    {
        let store = storage_factory
            .lock()
            .await
            .get_device_storage::<AudioInfo>(StorageAccessContext::Test, CONTEXT_ID);
        let mut stored_info = default_audio_info();
        for stream in stored_info.streams.iter_mut() {
            if stream.stream_type == AudioStreamType::Media {
                stream.user_volume_level = expected_info.0;
                stream.user_volume_muted = expected_info.1;
            }
        }
        assert!(store.write(&stored_info, false).await.is_ok());
    }

    assert!(EnvironmentBuilder::new(storage_factory)
        .service(Box::new(ServiceRegistry::serve(service_registry)))
        .agents(&[restore_agent::blueprint::create()])
        .settings(&[SettingType::Audio])
        .spawn_nested(ENV_NAME)
        .await
        .is_ok());

    let stored_info =
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap();
    assert_eq!(stored_info, expected_info);
}

// Test to ensure mic input change events are received.
// TODO(fxbug.dev/56537): Remove with switchover to input interface.
#[fuchsia_async::run_until_stalled(test)]
async fn test_bringup_without_input_registry() {
    let service_registry = ServiceRegistry::create();
    let audio_core_service_handle = audio_core_service::Builder::new().build();
    service_registry.lock().await.register_service(audio_core_service_handle.clone());

    let (env, _) = create_environment(service_registry).await;

    // At this point we should not crash.
    assert!(env.connect_to_service::<AudioMarker>().is_ok());
}

// Ensure that we won't crash if audio core fails.
#[fuchsia_async::run_until_stalled(test)]
async fn test_bringup_without_audio_core() {
    let service_registry = ServiceRegistry::create();
    let input_registry_service_handle = Arc::new(Mutex::new(InputDeviceRegistryService::new()));
    service_registry.lock().await.register_service(input_registry_service_handle.clone());

    let (env, _) = create_environment(service_registry).await;

    // At this point we should not crash.
    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );
}

#[test]
fn test_audio_info_copy() {
    let audio_info = default_audio_info();
    let copy_audio_info = audio_info.clone();
    assert_eq!(audio_info, copy_audio_info);
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_persisted_values_applied_at_start() {
    let (service_registry, fake_services) = create_services().await;
    let storage_factory = InMemoryStorageFactory::create();
    let store = create_storage(storage_factory.clone()).await;

    let test_audio_info = AudioInfo {
        streams: [
            AudioStream {
                stream_type: AudioStreamType::Background,
                source: AudioSettingSource::User,
                user_volume_level: 0.5,
                user_volume_muted: true,
            },
            AudioStream {
                stream_type: AudioStreamType::Media,
                source: AudioSettingSource::User,
                user_volume_level: 0.6,
                user_volume_muted: true,
            },
            AudioStream {
                stream_type: AudioStreamType::Interruption,
                source: AudioSettingSource::System,
                user_volume_level: 0.3,
                user_volume_muted: false,
            },
            AudioStream {
                stream_type: AudioStreamType::SystemAgent,
                source: AudioSettingSource::User,
                user_volume_level: 0.7,
                user_volume_muted: true,
            },
            AudioStream {
                stream_type: AudioStreamType::Communication,
                source: AudioSettingSource::User,
                user_volume_level: 0.8,
                user_volume_muted: false,
            },
        ],
        input: AudioInputInfo { mic_mute: true },
        modified_counters: Some(create_default_modified_counters()),
    };

    // Write values in the store.
    store.write(&test_audio_info, false).await.expect("write audio info in store");

    let env = EnvironmentBuilder::new(storage_factory)
        .service(ServiceRegistry::serve(service_registry))
        .agents(&[restore_agent::blueprint::create()])
        .settings(&[SettingType::Audio])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed");

    // Check to make sure mic mute value is loaded properly.
    let mut audio_input = AudioInput::EMPTY;
    audio_input.muted = Some(test_audio_info.input.mic_mute);

    assert_eq!(settings.input, Some(audio_input));
    // Check that the stored values were returned from watch() and applied to the audio core
    // service.
    for stream in test_audio_info.streams.iter() {
        verify_audio_stream(settings.clone(), AudioStreamSettings::from(*stream));
        assert_eq!(
            (stream.user_volume_level, stream.user_volume_muted),
            fake_services
                .audio_core
                .lock()
                .await
                .get_level_and_mute(AudioRenderUsage::from(stream.stream_type))
                .unwrap()
        );
    }
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_channel_failure_watch() {
    let audio_proxy = create_audio_test_env_with_failures(InMemoryStorageFactory::create()).await;
    let result = audio_proxy.watch().await;
    assert_matches!(result, Err(ClientChannelClosed { status: Status::UNAVAILABLE, .. }));
}
