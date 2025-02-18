// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// This module tests the property that pkg_resolver does not enter a bad
/// state (successfully handles retries) when the TUF server errors while
/// servicing fuchsia.pkg.PackageResolver.Resolve FIDL requests.
use {
    fuchsia_async as fasync,
    fuchsia_merkle::MerkleTree,
    fuchsia_pkg_testing::{
        serve::{handler, UriPathHandler},
        Package, PackageBuilder, RepositoryBuilder,
    },
    fuchsia_zircon::Status,
    lib::{extra_blob_contents, make_pkg_with_extra_blobs, TestEnvBuilder, EMPTY_REPO_PATH},
    matches::assert_matches,
    std::{net::Ipv4Addr, sync::Arc, time::Duration},
};

async fn verify_resolve_fails_then_succeeds<H: UriPathHandler>(
    pkg: Package,
    handler: H,
    failure_status: Status,
) {
    let env = TestEnvBuilder::new().build().await;

    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );
    let pkg_url = format!("fuchsia-pkg://test/{}", pkg.name());

    let should_fail = handler::AtomicToggle::new(true);
    let served_repository = repo
        .server()
        .uri_path_override_handler(handler::Toggleable::new(&should_fail, handler))
        .start()
        .unwrap();
    env.register_repo(&served_repository).await;

    // First resolve fails with the expected error.
    assert_matches!(env.resolve_package(&pkg_url).await, Err(status) if status == failure_status);

    // Disabling the custom URI path handler allows the subsequent resolves to succeed.
    should_fail.unset();
    let package_dir = env.resolve_package(&pkg_url).await.expect("package to resolve");
    pkg.verify_contents(&package_dir).await.expect("correct package contents");

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_far_404() {
    let pkg = make_pkg_with_extra_blobs("second_resolve_succeeds_when_far_404", 1).await;
    let path_to_override = format!("/blobs/{}", pkg.meta_far_merkle_root());

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::StaticResponseCode::not_found()),
        Status::UNAVAILABLE,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_blob_404() {
    let pkg = make_pkg_with_extra_blobs("second_resolve_succeeds_when_blob_404", 1).await;
    let path_to_override = format!(
        "/blobs/{}",
        MerkleTree::from_reader(
            extra_blob_contents("second_resolve_succeeds_when_blob_404", 0).as_slice()
        )
        .expect("merkle slice")
        .root()
    );

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::StaticResponseCode::not_found()),
        Status::UNAVAILABLE,
    )
    .await
}

// If the body of an https response is not large enough, hyper will download the body
// along with the header in the initial fuchsia_hyper::HttpsClient.request(). This means
// that even if the body is implemented with a stream that fails before the transfer is
// complete, the failure will occur during the initial request and before the batch loop
// that writes to pkgfs/blobfs. Value was found experimentally.
const FILE_SIZE_LARGE_ENOUGH_TO_TRIGGER_HYPER_BATCHING: usize = 1_000_000;

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_far_errors_mid_download() {
    let pkg = PackageBuilder::new("large_meta_far")
        .add_resource_at(
            "meta/large_file",
            vec![0; FILE_SIZE_LARGE_ENOUGH_TO_TRIGGER_HYPER_BATCHING].as_slice(),
        )
        .build()
        .await
        .unwrap();
    let path_to_override = format!("/blobs/{}", pkg.meta_far_merkle_root());

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::OneByteShortThenError),
        Status::UNAVAILABLE,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_blob_errors_mid_download() {
    let blob = vec![0; FILE_SIZE_LARGE_ENOUGH_TO_TRIGGER_HYPER_BATCHING];
    let pkg = PackageBuilder::new("large_blob")
        .add_resource_at("blobbity/blob", blob.as_slice())
        .build()
        .await
        .unwrap();
    let path_to_override = format!(
        "/blobs/{}",
        MerkleTree::from_reader(blob.as_slice()).expect("merkle slice").root()
    );

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::OneByteShortThenError),
        Status::UNAVAILABLE,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_disconnect_before_far_complete() {
    let pkg = PackageBuilder::new("large_meta_far")
        .add_resource_at(
            "meta/large_file",
            vec![0; FILE_SIZE_LARGE_ENOUGH_TO_TRIGGER_HYPER_BATCHING].as_slice(),
        )
        .build()
        .await
        .unwrap();
    let path_to_override = format!("/blobs/{}", pkg.meta_far_merkle_root());

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::OneByteShortThenDisconnect),
        Status::UNAVAILABLE,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_disconnect_before_blob_complete() {
    let blob = vec![0; FILE_SIZE_LARGE_ENOUGH_TO_TRIGGER_HYPER_BATCHING];
    let pkg = PackageBuilder::new("large_blob")
        .add_resource_at("blobbity/blob", blob.as_slice())
        .build()
        .await
        .unwrap();
    let path_to_override = format!(
        "/blobs/{}",
        MerkleTree::from_reader(blob.as_slice()).expect("merkle slice").root()
    );

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::OneByteShortThenDisconnect),
        Status::UNAVAILABLE,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_far_corrupted() {
    let pkg = make_pkg_with_extra_blobs("second_resolve_succeeds_when_far_corrupted", 1).await;
    let path_to_override = format!("/blobs/{}", pkg.meta_far_merkle_root());

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::OneByteFlipped),
        Status::IO,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_blob_corrupted() {
    let pkg = make_pkg_with_extra_blobs("second_resolve_succeeds_when_blob_corrupted", 1).await;
    let blob = extra_blob_contents("second_resolve_succeeds_when_blob_corrupted", 0);
    let path_to_override = format!(
        "/blobs/{}",
        MerkleTree::from_reader(blob.as_slice()).expect("merkle slice").root()
    );

    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new(path_to_override, handler::OneByteFlipped),
        Status::IO,
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn second_resolve_succeeds_when_tuf_metadata_update_fails() {
    // pkg-resolver uses tuf::client::Client::with_trusted_root_keys to create its TUF client.
    // That method will only retrieve the specified version of the root metadata (1 for these
    // tests), with the rest of the metadata being retrieved during the first update. This means
    // that hanging all attempts for 2.snapshot.json metadata will allow tuf client creation to
    // succeed but still fail tuf client update.
    // We want to specifically verify recovery from update failure because if creation fails,
    // pkg-resolver will not make a Repository object, so the next resolve attempt would try again
    // from scratch, but if update fails, pkg-resolver will keep its Repository object which
    // contains a rust-tuf client in a possibly invalid state, and we want to verify that
    // pkg-resolver calls update on the client again and that this update recovers the client.
    let pkg = PackageBuilder::new("no-blobs").build().await.unwrap();
    verify_resolve_fails_then_succeeds(
        pkg,
        handler::ForPath::new("/2.snapshot.json", handler::OneByteShortThenDisconnect),
        Status::INTERNAL,
    )
    .await
}

// The hyper clients used by the pkg-resolver to download blobs and TUF metadata sometimes end up
// waiting on operations on their TCP connections that will never return (e.g. because of an
// upstream network partition). To detect this, the pkg-resolver wraps the hyper client response
// futures with timeout futures. To recover from this, the pkg-resolver drops the hyper client
// response futures when the timeouts are hit. This recovery plan requires that dropping the hyper
// response future causes hyper to close the underlying TCP connection and create a new one the
// next time hyper is asked to perform a network operation. This assumption holds for http1, but
// not for http2.
//
// This test verifies the "dropping a hyper response future prevents the underlying connection
// from being reused" requirement. It does so by verifying that if a resolve fails due to a blob
// download timeout and the resolve is retried, the retry will cause pkg-resolver to make an
// additional TCP connection to the blob mirror.
//
// This test uses https because the test exists to catch changes to the Fuchsia hyper client
// that would cause pkg-resolver to use http2 before the Fuchsia hyper client is able to recover
// from bad TCP connections when using http2. The pkg-resolver does not explicitly enable http2
// on its hyper clients, so the way this change would sneak in is if the hyper client is changed
// to use ALPN to prefer http2. The blob server used in this test has ALPN configured to prefer
// http2.
#[fasync::run_singlethreaded(test)]
async fn blob_timeout_causes_new_tcp_connection() {
    let pkg = PackageBuilder::new("test").build().await.unwrap();
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );

    let env = TestEnvBuilder::new().blob_network_body_timeout(Duration::from_secs(0)).build().await;

    let server = repo
        .server()
        .uri_path_override_handler(handler::ForPathPrefix::new(
            "/blobs/",
            handler::Once::new(handler::HangBody),
        ))
        .use_https(true)
        .bind_to_addr(Ipv4Addr::LOCALHOST)
        .start()
        .expect("Starting server succeeds");

    env.register_repo(&server).await;

    let result = env.resolve_package("fuchsia-pkg://test/test").await;
    assert_eq!(result.unwrap_err(), Status::UNAVAILABLE);
    assert_eq!(server.connection_attempts(), 2);

    // The resolve may fail because of the zero second timeout on the blob body future,
    // but that happens after the header is downloaded and therefore after the new TCP
    // connection is established.
    let _ = env.resolve_package("fuchsia-pkg://test/test").await;
    assert_eq!(server.connection_attempts(), 3);

    env.stop().await;
}
