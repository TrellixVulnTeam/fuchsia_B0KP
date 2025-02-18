// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::handler::base::{Request, Response as SettingResponse};
use crate::handler::device_storage::testing::InMemoryStorageFactory;
use crate::handler::device_storage::DeviceStorageFactory;
use crate::handler::setting_handler::persist::Storage;
use crate::internal::core;
use crate::message::base::MessengerType;
use crate::policy::base::response::{Payload, Response};
use crate::policy::base::{PolicyInfo, PolicyType, Request as PolicyRequest, UnknownInfo};
use crate::policy::policy_handler::{
    ClientProxy, Create, EventTransform, PolicyHandler, RequestTransform, ResponseTransform,
};
use crate::privacy::types::PrivacyInfo;
use crate::service;
use crate::switchboard::base::SettingEvent;
use anyhow::Error;
use async_trait::async_trait;
use futures::future::BoxFuture;
use std::sync::Arc;

const CONTEXT_ID: u64 = 0;

pub type HandlePolicyRequestCallback<S> =
    Box<dyn Fn(PolicyRequest, ClientProxy<S>) -> BoxFuture<'static, Response> + Send + Sync>;

pub struct FakePolicyHandler<S: Storage + 'static> {
    client_proxy: ClientProxy<S>,
    handle_policy_request_callback: Option<HandlePolicyRequestCallback<S>>,
}

impl<S: Storage> FakePolicyHandler<S> {
    fn set_handle_policy_request_callback(
        &mut self,
        handle_policy_request_callback: HandlePolicyRequestCallback<S>,
    ) {
        self.handle_policy_request_callback = Some(handle_policy_request_callback);
    }
}

#[async_trait]
impl<S: Storage> Create<S> for FakePolicyHandler<S> {
    async fn create(client_proxy: ClientProxy<S>) -> Result<Self, Error> {
        Ok(Self { client_proxy, handle_policy_request_callback: None })
    }
}

#[async_trait]
impl<S: Storage> PolicyHandler for FakePolicyHandler<S> {
    async fn handle_policy_request(&mut self, request: PolicyRequest) -> Response {
        self.handle_policy_request_callback.as_ref().unwrap()(request, self.client_proxy.clone())
            .await
    }

    async fn handle_setting_request(&mut self, _request: Request) -> Option<RequestTransform> {
        None
    }

    async fn handle_setting_event(&mut self, _event: SettingEvent) -> Option<EventTransform> {
        None
    }

    async fn handle_setting_response(
        &mut self,
        _response: SettingResponse,
    ) -> Option<ResponseTransform> {
        None
    }
}

/// Verifies that policy handlers are able to write to storage through their client proxy.
#[fuchsia_async::run_until_stalled(test)]
async fn test_write() {
    let expected_value = PrivacyInfo { user_data_sharing_consent: Some(true) };
    let core_messenger_factory = core::message::create_hub();
    let (core_messenger, _) = core_messenger_factory.create(MessengerType::Unbound).await.unwrap();

    let messenger_factory = service::message::create_hub();
    let (messenger, _) = messenger_factory.create(MessengerType::Unbound).await.unwrap();

    let (_, setting_proxy_receptor) =
        core_messenger_factory.create(MessengerType::Unbound).await.unwrap();
    let storage_factory = InMemoryStorageFactory::create();
    let store = Arc::new(storage_factory.lock().await.get_store::<PrivacyInfo>(CONTEXT_ID));
    let client_proxy = ClientProxy::new(
        messenger,
        core_messenger,
        setting_proxy_receptor.get_signature(),
        store.clone(),
        PolicyType::Unknown,
    );

    // Create a handler that writes a value through the client proxy when handle_policy_request is
    // called.
    let mut handler =
        FakePolicyHandler::create(client_proxy.clone()).await.expect("failed to create handler");
    handler.set_handle_policy_request_callback(Box::new(move |_, client_proxy| {
        Box::pin(async move {
            client_proxy.write(expected_value.clone(), false).await.expect("write failed");
            Ok(Payload::PolicyInfo(PolicyInfo::Unknown(UnknownInfo(true))))
        })
    }));

    // Call handle_policy_request.
    handler.handle_policy_request(PolicyRequest::Get).await.expect("handle failed");

    // Verify the value was written to the store through the client proxy.
    assert_eq!(store.get::<PrivacyInfo>().await, expected_value);

    // Verify that the written value can be read again through the client proxy.
    assert_eq!(client_proxy.read().await, expected_value);
}
