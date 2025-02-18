// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]

pub use env_info::is_analytics_disabled_by_env;
pub use ga_event::*;
pub use notice::*;
use std::any::Any;
use std::collections::BTreeMap;
pub use user_status::*;
use {
    anyhow::Error,
    fuchsia_hyper::new_https_client,
    futures::{
        stream::Stream,
        task::{Context, Poll},
    },
    hyper::{Body, Method, Request, StatusCode},
    std::pin::Pin,
};

mod env_info;
mod ga_event;
pub mod notice;
mod user_status;

#[cfg(test)]
const GA_URL: &str = "https://www.google-analytics.com/debug/collect";
#[cfg(not(test))]
const GA_URL: &str = "https://www.google-analytics.com/collect";

pub async fn add_launch_event(
    app_name: &str,
    app_version: Option<&str>,
    args: Option<&str>,
) -> anyhow::Result<()> {
    add_custom_event(app_name, app_version, None, args, args, BTreeMap::new()).await
}

pub async fn add_custom_event(
    app_name: &str,
    app_version: Option<&str>,
    category: Option<&str>,
    action: Option<&str>,
    label: Option<&str>,
    custom_dimensions: BTreeMap<&str, String>,
) -> anyhow::Result<()> {
    if is_analytics_disabled_by_env() || !is_opted_in() {
        return Ok(());
    }

    let body = make_body_with_hash(
        &app_name,
        app_version,
        category,
        action,
        label,
        custom_dimensions,
        &(uuid as UuidBuilder),
    );
    let client = new_https_client();
    let req = Request::builder().method(Method::POST).uri(GA_URL).body(Body::from(body))?;
    let mut res = client.request(req).await;
    match res {
        Ok(res) => log::info!("Analytics response: {}", res.status()),
        Err(e) => log::debug!("Error posting analytics: {}", e),
    }
    Ok(())
}

//
// not sure if we will use this
// fx exception in subcommand
// "t=event" \
// "ec=fx_exception" \
// "ea=${subcommand}" \
// "el=${args}" \
// "cd1=${exit_status}" \
// )
pub async fn add_crash_event(err: &str) -> anyhow::Result<()> {
    Ok(())
}

// fx subcommand timing event
// hit_type="timing"
//  "t=timing" \
//     "utc=fx" \
//     "utv=${subcommand}" \
//     "utt=${timing}" \
//     "utl=${args}" \
//     )
pub async fn add_timing_event() -> anyhow::Result<()> {
    Ok(())
}

//#[cfg(test)]
//mod test
