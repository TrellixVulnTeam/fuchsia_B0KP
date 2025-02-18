// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::audio::types::AudioStreamType;
#[cfg(test)]
use crate::audio::{create_default_audio_stream, StreamVolumeControl};
use crate::internal::event;
use crate::message::base::{MessageEvent, MessengerType};
use crate::service_context::ServiceContext;
use crate::tests::fakes::audio_core_service;
use crate::tests::fakes::service_registry::ServiceRegistry;
use futures::lock::Mutex;
use futures::StreamExt;
use std::sync::Arc;

// Returns a registry populated with the AudioCore service.
async fn create_service() -> Arc<Mutex<ServiceRegistry>> {
    let service_registry = ServiceRegistry::create();
    let audio_core_service_handle =
        audio_core_service::Builder::new().set_suppress_client_errors(true).build();
    service_registry.lock().await.register_service(audio_core_service_handle.clone());
    service_registry
}

// Tests that the volume event stream thread exits when the StreamVolumeControl is deleted.
#[fuchsia_async::run_until_stalled(test)]
async fn test_drop_thread() {
    let factory = event::message::create_hub();

    let (_, mut receptor) =
        factory.create(MessengerType::Unbound).await.expect("Should be able to retrieve receptor");

    let publisher = event::Publisher::create(&factory, MessengerType::Unbound).await;

    let service_context =
        ServiceContext::create(Some(ServiceRegistry::serve(create_service().await)), None);

    let audio_proxy = service_context
        .lock()
        .await
        .connect::<fidl_fuchsia_media::AudioCoreMarker>()
        .await
        .expect("service should be present");

    // Scoped to cause the object to be dropped.
    {
        StreamVolumeControl::create(
            &audio_proxy,
            create_default_audio_stream(AudioStreamType::Media),
            None,
            Some(publisher),
        )
        .await
        .ok();
    }

    let received_event =
        receptor.next().await.expect("First message should have been the closed event");

    match received_event {
        MessageEvent::Message(event::Payload::Event(broadcasted_event), _) => {
            assert_eq!(broadcasted_event, event::Event::Closed("volume_control_events"));
        }
        _ => {
            panic!("Should have received an event payload");
        }
    }
}

/// Ensures that the StreamVolumeControl properly fires the provided early exit
/// closure when the underlying AudioCoreService closes unexpectedly.
#[fuchsia_async::run_until_stalled(test)]
async fn test_detect_early_exit() {
    let service_registry = ServiceRegistry::create();
    let audio_core_service_handle =
        audio_core_service::Builder::new().set_suppress_client_errors(true).build();
    service_registry.lock().await.register_service(audio_core_service_handle.clone());

    let service_context_handle =
        ServiceContext::create(Some(ServiceRegistry::serve(service_registry)), None);

    let audio_proxy = service_context_handle
        .lock()
        .await
        .connect::<fidl_fuchsia_media::AudioCoreMarker>()
        .await
        .expect("proxy should be present");
    let (tx, mut rx) = futures::channel::mpsc::unbounded::<()>();

    // Create StreamVolumeControl, specifying firing an event as the early exit
    // action. Note that we must store the returned value or else the normal
    // drop behavior will clean up it before the AudioCoreService's exit can
    // be detected.
    let _stream_volume_control = StreamVolumeControl::create(
        &audio_proxy,
        create_default_audio_stream(AudioStreamType::Media),
        Some(Arc::new(move || {
            tx.unbounded_send(()).ok();
        })),
        None,
    )
    .await
    .expect("should successfully build");

    // Trigger AudioCoreService exit.
    audio_core_service_handle.lock().await.exit();

    // Check to make sure early exit event was received.
    assert!(matches!(rx.next().await, Some(..)));
}
