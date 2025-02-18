// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::agent::base::{Context, Descriptor, Invocation, Lifespan};
use crate::agent::camera_watcher::CameraWatcherAgent;
use crate::internal::event::{self, Event};
use crate::internal::{agent, switchboard};
use crate::message::base::{Audience, MessageEvent, MessengerType};
use crate::service;
use crate::service_context::ServiceContext;
use crate::tests::fakes::camera3_service::Camera3Service;
use crate::tests::fakes::service_registry::ServiceRegistry;
use futures::lock::Mutex;
use futures::StreamExt;
use std::collections::HashSet;
use std::sync::Arc;

struct FakeServices {
    camera3_service: Arc<Mutex<Camera3Service>>,
}

// Returns a registry and input related services with which it is populated.
async fn create_services() -> (Arc<Mutex<ServiceRegistry>>, FakeServices) {
    let service_registry = ServiceRegistry::create();

    let camera3_service_handle = Arc::new(Mutex::new(Camera3Service::new()));
    service_registry.lock().await.register_service(camera3_service_handle.clone());

    (service_registry, FakeServices { camera3_service: camera3_service_handle })
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_camera_agent_proxy() {
    let agent_hub = agent::message::create_hub();
    let event_hub = event::message::create_hub();

    // Create the agent receptor for use by the agent.
    let agent_receptor =
        agent_hub.create(MessengerType::Unbound).await.expect("Unable to create agent messenger").1;
    let signature = agent_receptor.get_signature();

    // Create the messenger where we will send the invocations.
    let (agent_messenger, _) =
        agent_hub.create(MessengerType::Unbound).await.expect("Unable to create agent messenger");

    // Create the receptor which will receive the broadcast events.
    let (_, mut event_receptor) =
        event_hub.create(MessengerType::Unbound).await.expect("Unable to create agent messenger");

    // Create the agent context and agent.
    let context = Context::new(
        agent_receptor,
        Descriptor::new("test_camera_watcher_agent"),
        service::message::create_hub(),
        switchboard::message::create_hub(),
        event_hub,
        HashSet::new(),
        None,
    )
    .await;
    // Setup the fake services.
    let (service_registry, fake_services) = create_services().await;

    fake_services.camera3_service.lock().await.set_camera_sw_muted(true);
    CameraWatcherAgent::create(context).await;

    let service_context =
        ServiceContext::create(Some(ServiceRegistry::serve(service_registry)), None);

    // Create and send the invocation with faked services.
    let invocation = Invocation { lifespan: Lifespan::Service, service_context };
    let mut reply_receptor = agent_messenger
        .message(agent::Payload::Invocation(invocation), Audience::Messenger(signature))
        .send();
    let mut completion_result = None;
    while let Some(event) = reply_receptor.next().await {
        if let MessageEvent::Message(agent::Payload::Complete(result), _) = event {
            completion_result = Some(result);
            break;
        }
    }

    // Validate that the setup is complete.
    assert!(
        matches!(completion_result, Some(Ok(()))),
        "Did not receive a completion event from the invocation message"
    );

    // Track the events to make sure they came in.
    let mut camera_state = false;
    while let Ok((payload, _)) = event_receptor.next_payload().await {
        if let event::Payload::Event(Event::CameraUpdate(event)) = payload {
            match event {
                event::camera_watcher::Event::OnSWMuteState(muted) => {
                    camera_state = muted;
                    break;
                }
            }
        }
    }

    // Validate that we received all expected events.
    assert!(camera_state);
}
