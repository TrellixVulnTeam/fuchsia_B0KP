// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        accessor::PerformanceConfig,
        constants,
        container::{ReadSnapshot, SnapshotData},
        diagnostics::ConnectionStats,
        inspect::container::UnpopulatedInspectDataContainer,
    },
    anyhow::Error,
    collector::Moniker,
    diagnostics_data::{self as schema, Data, Inspect},
    diagnostics_hierarchy::{DiagnosticsHierarchy, InspectHierarchyMatcher},
    fidl_fuchsia_diagnostics::{self, Selector},
    fuchsia_inspect::reader::PartialNodeHierarchy,
    fuchsia_zircon as zx,
    futures::prelude::*,
    selectors,
    std::{
        convert::{TryFrom, TryInto},
        sync::Arc,
    },
    tracing::error,
};

pub mod collector;
pub mod container;

use container::PopulatedInspectDataContainer;

/// Packet containing a node hierarchy and all the metadata needed to
/// populate a diagnostics schema for that node hierarchy.
pub struct NodeHierarchyData {
    // Name of the file that created this snapshot.
    filename: String,
    // Timestamp at which this snapshot resolved or failed.
    timestamp: zx::Time,
    // Errors encountered when processing this snapshot.
    errors: Vec<schema::Error>,
    // Optional DiagnosticsHierarchy of the inspect hierarchy, in case reading fails
    // and we have errors to share with client.
    hierarchy: Option<DiagnosticsHierarchy>,
}

impl Into<NodeHierarchyData> for SnapshotData {
    fn into(self: SnapshotData) -> NodeHierarchyData {
        match self.snapshot {
            Some(snapshot) => match convert_snapshot_to_node_hierarchy(snapshot) {
                Ok(node_hierarchy) => NodeHierarchyData {
                    filename: self.filename,
                    timestamp: self.timestamp,
                    errors: self.errors,
                    hierarchy: Some(node_hierarchy),
                },
                Err(e) => NodeHierarchyData {
                    filename: self.filename,
                    timestamp: self.timestamp,
                    errors: vec![schema::Error { message: format!("{:?}", e) }],
                    hierarchy: None,
                },
            },
            None => NodeHierarchyData {
                filename: self.filename,
                timestamp: self.timestamp,
                errors: self.errors,
                hierarchy: None,
            },
        }
    }
}

/// ReaderServer holds the state and data needed to serve Inspect data
/// reading requests for a single client.
///
/// configured_selectors: are the selectors provided by the client which define
///                       what inspect data is returned by read requests. A none type
///                       implies that all available data should be returned.
///
/// inspect_repo: the DataRepo which holds the access-points for all relevant
///               inspect data.
pub struct ReaderServer {
    selectors: Option<Vec<Arc<Selector>>>,
}

fn convert_snapshot_to_node_hierarchy(
    snapshot: ReadSnapshot,
) -> Result<DiagnosticsHierarchy, Error> {
    match snapshot {
        ReadSnapshot::Single(snapshot) => Ok(PartialNodeHierarchy::try_from(snapshot)?.into()),
        ReadSnapshot::Tree(snapshot_tree) => Ok(snapshot_tree.try_into()?),
        ReadSnapshot::Finished(hierarchy) => Ok(hierarchy),
    }
}

pub struct BatchResultItem {
    /// Relative moniker of the component associated with this result.
    pub moniker: Moniker,
    /// The url with which the component associated with this result was launched.
    pub component_url: String,
    /// The resulting Node hierarchy plus some metadata.
    pub hierarchy_data: NodeHierarchyData,
}

impl ReaderServer {
    /// Create a stream of filtered inspect data, ready to serve.
    pub fn stream(
        unpopulated_diagnostics_sources: Vec<UnpopulatedInspectDataContainer>,
        performance_configuration: PerformanceConfig,
        selectors: Option<Vec<Arc<Selector>>>,
        stats: Arc<ConnectionStats>,
    ) -> impl Stream<Item = Data<Inspect>> + Send + 'static {
        let server = Self { selectors };

        let batch_timeout = performance_configuration.batch_timeout_sec;

        futures::stream::iter(unpopulated_diagnostics_sources.into_iter())
            // make a stream of futures of populated Vec's
            .map(move |unpopulated| {
                let global_stats = stats.global_stats().clone();

                // this returns a future, which means the closure capture must be 'static
                async move {
                    let start_time = zx::Time::get_monotonic();
                    let global_stats_2 = global_stats.clone();
                    let result = unpopulated
                        .populate(batch_timeout, move || global_stats.add_timeout())
                        .await;
                    global_stats_2.record_component_duration(
                        &result.identity.relative_moniker.join("/"),
                        zx::Time::get_monotonic() - start_time,
                    );
                    result
                }
            })
            // buffer a small number in memory in case later components time out
            .buffer_unordered(constants::MAXIMUM_SIMULTANEOUS_SNAPSHOTS_PER_READER)
            // filter each component's inspect
            .map(move |populated| server.filter_snapshot(populated))
            // turn each of the vecs of filtered snapshots into their own streams
            .map(futures::stream::iter)
            // and merge them all into a single stream
            .flatten()
    }

    fn filter_single_components_snapshots(
        snapshots: Vec<SnapshotData>,
        static_matcher: Option<InspectHierarchyMatcher>,
        client_matcher: Option<InspectHierarchyMatcher>,
    ) -> Vec<NodeHierarchyData> {
        let statically_filtered_hierarchies: Vec<NodeHierarchyData> = match static_matcher {
            Some(static_matcher) => snapshots
                .into_iter()
                .map(|snapshot_data| {
                    let node_hierarchy_data: NodeHierarchyData = snapshot_data.into();

                    match node_hierarchy_data.hierarchy {
                        Some(node_hierarchy) => {
                            match diagnostics_hierarchy::filter_hierarchy(
                                node_hierarchy,
                                &static_matcher,
                            ) {
                                Ok(Some(filtered_hierarchy)) => NodeHierarchyData {
                                    filename: node_hierarchy_data.filename,
                                    timestamp: node_hierarchy_data.timestamp,
                                    errors: node_hierarchy_data.errors,
                                    hierarchy: Some(filtered_hierarchy),
                                },
                                Ok(None) => NodeHierarchyData {
                                    filename: node_hierarchy_data.filename,
                                    timestamp: node_hierarchy_data.timestamp,
                                    errors: vec![schema::Error {
                                        message: concat!(
                                            "Inspect hierarchy was fully filtered",
                                            " by static selectors. No data remaining."
                                        )
                                        .to_string(),
                                    }],
                                    hierarchy: None,
                                },
                                Err(e) => {
                                    error!(?e, "Failed to filter a node hierarchy");
                                    NodeHierarchyData {
                                        filename: node_hierarchy_data.filename,
                                        timestamp: node_hierarchy_data.timestamp,
                                        errors: vec![schema::Error { message: format!("{:?}", e) }],
                                        hierarchy: None,
                                    }
                                }
                            }
                        }
                        None => NodeHierarchyData {
                            filename: node_hierarchy_data.filename,
                            timestamp: node_hierarchy_data.timestamp,
                            errors: node_hierarchy_data.errors,
                            hierarchy: None,
                        },
                    }
                })
                .collect(),

            // The only way we have a None value for the PopulatedDataContainer is
            // if there were no provided static selectors, which is only valid in
            // the AllAccess pipeline. For all other pipelines, if no static selectors
            // matched, the data wouldn't have ended up in the repository to begin
            // with.
            None => snapshots.into_iter().map(|snapshot_data| snapshot_data.into()).collect(),
        };

        match client_matcher {
            // If matcher is present, and there was an InspectHierarchyMatcher,
            // then this means the client provided their own selectors, and a subset of
            // them matched this component. So we need to filter each of the snapshots from
            // this component with the dynamically provided components.
            Some(dynamic_matcher) => statically_filtered_hierarchies
                .into_iter()
                .map(|node_hierarchy_data| match node_hierarchy_data.hierarchy {
                    Some(node_hierarchy) => {
                        match diagnostics_hierarchy::filter_hierarchy(
                            node_hierarchy,
                            &dynamic_matcher,
                        ) {
                            Ok(Some(filtered_hierarchy)) => NodeHierarchyData {
                                filename: node_hierarchy_data.filename,
                                timestamp: node_hierarchy_data.timestamp,
                                errors: node_hierarchy_data.errors,
                                hierarchy: Some(filtered_hierarchy),
                            },
                            Ok(None) => NodeHierarchyData {
                                filename: node_hierarchy_data.filename,
                                timestamp: node_hierarchy_data.timestamp,
                                errors: vec![schema::Error {
                                    message: concat!(
                                        "Inspect hierarchy was fully filtered",
                                        " by client provided selectors. No data remaining."
                                    )
                                    .to_string(),
                                }],
                                hierarchy: None,
                            },
                            Err(e) => NodeHierarchyData {
                                filename: node_hierarchy_data.filename,
                                timestamp: node_hierarchy_data.timestamp,
                                errors: vec![schema::Error { message: format!("{:?}", e) }],
                                hierarchy: None,
                            },
                        }
                    }
                    None => NodeHierarchyData {
                        filename: node_hierarchy_data.filename,
                        timestamp: node_hierarchy_data.timestamp,
                        errors: node_hierarchy_data.errors,
                        hierarchy: None,
                    },
                })
                .collect(),
            None => statically_filtered_hierarchies,
        }
    }

    /// Takes a PopulatedInspectDataContainer and converts all non-error
    /// results into in-memory node hierarchies. The hierarchies are filtered
    /// such that the only diagnostics properties they contain are those
    /// configured by the static and client-provided selectors.
    ///
    // TODO(fxbug.dev/4601): Error entries should still be included, but with a custom hierarchy
    //             that makes it clear to clients that snapshotting failed.
    fn filter_snapshot(
        &self,
        pumped_inspect_data: PopulatedInspectDataContainer,
    ) -> Vec<Data<Inspect>> {
        // Since a single PopulatedInspectDataContainer shares a moniker for all pieces of data it
        // contains, we can store the result of component selector filtering to avoid reapplying
        // the selectors.
        let mut client_selectors: Option<InspectHierarchyMatcher> = None;

        // We iterate the vector of pumped inspect data packets, consuming each inspect vmo
        // and filtering it using the provided selector regular expressions. Each filtered
        // inspect hierarchy is then added to an accumulator as a HierarchyData to be converted
        // into a JSON string and returned.
        let sanitized_moniker = pumped_inspect_data
            .identity
            .relative_moniker
            .iter()
            .map(|s| selectors::sanitize_string_for_selectors(s))
            .collect::<Vec<String>>()
            .join("/");

        if let Some(configured_selectors) = &self.selectors {
            client_selectors = {
                let matching_selectors = selectors::match_component_moniker_against_selectors(
                    &pumped_inspect_data.identity.relative_moniker,
                    configured_selectors,
                )
                .unwrap_or_else(|err| {
                    error!(
                        moniker = ?pumped_inspect_data.identity.relative_moniker, ?err,
                        "Failed to evaluate client selectors",
                    );
                    Vec::new()
                });

                if matching_selectors.is_empty() {
                    None
                } else {
                    match (&matching_selectors).try_into() {
                        Ok(hierarchy_matcher) => Some(hierarchy_matcher),
                        Err(e) => {
                            error!(?e, "Failed to create hierarchy matcher");
                            None
                        }
                    }
                }
            };

            // If there were configured matchers and none of them matched
            // this component, then we should return early since there is no data to
            // extract.
            if client_selectors.is_none() {
                return vec![];
            }
        }

        let identity = pumped_inspect_data.identity.clone();

        ReaderServer::filter_single_components_snapshots(
            pumped_inspect_data.snapshots,
            pumped_inspect_data.inspect_matcher,
            client_selectors,
        )
        .into_iter()
        .map(|hierarchy_data| {
            Data::for_inspect(
                sanitized_moniker.clone(),
                hierarchy_data.hierarchy,
                hierarchy_data.timestamp.into_nanos(),
                identity.url.clone(),
                hierarchy_data.filename,
                hierarchy_data.errors,
            )
        })
        .collect()
    }
}

#[cfg(test)]
mod tests {
    use {
        super::collector::InspectDataCollector,
        super::*,
        crate::{
            accessor::BatchIterator,
            container::ComponentIdentity,
            diagnostics,
            events::types::{ComponentIdentifier, InspectData},
            pipeline::Pipeline,
            repository::DataRepo,
        },
        anyhow::format_err,
        diagnostics_hierarchy::trie::TrieIterableNode,
        diagnostics_hierarchy::DiagnosticsHierarchy,
        fdio,
        fidl::endpoints::{create_proxy_and_stream, DiscoverableService},
        fidl_fuchsia_diagnostics::{BatchIteratorMarker, BatchIteratorProxy, StreamMode},
        fidl_fuchsia_inspect::TreeMarker,
        fidl_fuchsia_io::DirectoryMarker,
        fuchsia_async::{self as fasync, Task},
        fuchsia_component::server::ServiceFs,
        fuchsia_inspect::{assert_inspect_tree, reader, testing::AnyProperty, Inspector},
        fuchsia_zircon as zx,
        fuchsia_zircon::Peered,
        futures::future::join_all,
        futures::{FutureExt, StreamExt},
        parking_lot::RwLock,
        serde_json::json,
        std::path::PathBuf,
    };

    const TEST_URL: &'static str = "fuchsia-pkg://test";
    const BATCH_RETRIEVAL_TIMEOUT_SECONDS: i64 = 300;

    fn get_vmo(text: &[u8]) -> zx::Vmo {
        let vmo = zx::Vmo::create(4096).unwrap();
        vmo.write(text, 0).unwrap();
        vmo
    }

    #[fasync::run_singlethreaded(test)]
    async fn inspect_data_collector() {
        let path = PathBuf::from("/test-bindings");
        // Make a ServiceFs containing two files.
        // One is an inspect file, and one is not.
        let mut fs = ServiceFs::new();
        let vmo = get_vmo(b"test1");
        let vmo2 = get_vmo(b"test2");
        let vmo3 = get_vmo(b"test3");
        let vmo4 = get_vmo(b"test4");
        fs.dir("diagnostics").add_vmo_file_at("root.inspect", vmo, 0, 4096);
        fs.dir("diagnostics").add_vmo_file_at("root_not_inspect", vmo2, 0, 4096);
        fs.dir("diagnostics").dir("a").add_vmo_file_at("root.inspect", vmo3, 0, 4096);
        fs.dir("diagnostics").dir("b").add_vmo_file_at("root.inspect", vmo4, 0, 4096);
        // Create a connection to the ServiceFs.
        let (h0, h1) = zx::Channel::create().unwrap();
        fs.serve_connection(h1).unwrap();

        let ns = fdio::Namespace::installed().unwrap();
        ns.bind(path.join("out").to_str().unwrap(), h0).unwrap();

        fasync::Task::spawn(fs.collect()).detach();

        let (done0, done1) = zx::Channel::create().unwrap();

        let thread_path = path.join("out/diagnostics");

        // Run the actual test in a separate thread so that it does not block on FS operations.
        // Use signalling on a zx::Channel to indicate that the test is done.
        std::thread::spawn(move || {
            let path = thread_path;
            let done = done1;
            let mut executor = fasync::Executor::new().unwrap();

            executor.run_singlethreaded(async {
                let collector = InspectDataCollector::new();
                // Trigger collection on a clone of the inspect collector so
                // we can use collector to take the collected data.
                Box::new(collector.clone()).collect(path).await.unwrap();
                let collector: Box<InspectDataCollector> = Box::new(collector);

                let extra_data = collector.take_data().expect("collector missing data");
                assert_eq!(3, extra_data.len());

                let assert_extra_data = |path: &str, content: &[u8]| {
                    let extra = extra_data.get(path);
                    assert!(extra.is_some());

                    match extra.unwrap() {
                        InspectData::Vmo(vmo) => {
                            let mut buf = [0u8; 5];
                            vmo.read(&mut buf, 0).expect("reading vmo");
                            assert_eq!(content, &buf);
                        }
                        v => {
                            panic!("Expected Vmo, got {:?}", v);
                        }
                    }
                };

                assert_extra_data("root.inspect", b"test1");
                assert_extra_data("a/root.inspect", b"test3");
                assert_extra_data("b/root.inspect", b"test4");

                done.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).expect("signalling peer");
            });
        });

        fasync::OnSignals::new(&done0, zx::Signals::USER_0).await.unwrap();
        ns.unbind(path.join("out").to_str().unwrap()).unwrap();
    }

    #[fasync::run_singlethreaded(test)]
    async fn inspect_data_collector_tree() {
        let path = PathBuf::from("/test-bindings2");

        // Make a ServiceFs serving an inspect tree.
        let mut fs = ServiceFs::new();
        let inspector = Inspector::new();
        inspector.root().record_int("a", 1);
        inspector.root().record_lazy_child("lazy", || {
            async move {
                let inspector = Inspector::new();
                inspector.root().record_double("b", 3.14);
                Ok(inspector)
            }
            .boxed()
        });
        inspector.serve(&mut fs).expect("failed to serve inspector");

        // Create a connection to the ServiceFs.
        let (h0, h1) = zx::Channel::create().unwrap();
        fs.serve_connection(h1).unwrap();

        let ns = fdio::Namespace::installed().unwrap();
        ns.bind(path.join("out").to_str().unwrap(), h0).unwrap();

        fasync::Task::spawn(fs.collect()).detach();

        let (done0, done1) = zx::Channel::create().unwrap();
        let thread_path = path.join("out/diagnostics");

        // Run the actual test in a separate thread so that it does not block on FS operations.
        // Use signalling on a zx::Channel to indicate that the test is done.
        std::thread::spawn(move || {
            let path = thread_path;
            let done = done1;
            let mut executor = fasync::Executor::new().unwrap();

            executor.run_singlethreaded(async {
                let collector = InspectDataCollector::new();

                //// Trigger collection on a clone of the inspect collector so
                //// we can use collector to take the collected data.
                Box::new(collector.clone()).collect(path).await.unwrap();
                let collector: Box<InspectDataCollector> = Box::new(collector);

                let extra_data = collector.take_data().expect("collector missing data");
                assert_eq!(1, extra_data.len());

                let extra = extra_data.get(TreeMarker::SERVICE_NAME);
                assert!(extra.is_some());

                match extra.unwrap() {
                    InspectData::Tree(tree, vmo) => {
                        // Assert we can read the tree proxy and get the data we expected.
                        let hierarchy =
                            reader::read(tree).await.expect("failed to read hierarchy from tree");
                        assert_inspect_tree!(hierarchy, root: {
                            a: 1i64,
                            lazy: {
                                b: 3.14,
                            }
                        });
                        let partial_hierarchy: DiagnosticsHierarchy =
                            PartialNodeHierarchy::try_from(vmo.as_ref().unwrap())
                                .expect("failed to read hierarchy from vmo")
                                .into();
                        // Assert the vmo also points to that data (in this case since there's no
                        // lazy nodes).
                        assert_inspect_tree!(partial_hierarchy, root: {
                            a: 1i64,
                        });
                    }
                    v => {
                        panic!("Expected Tree, got {:?}", v);
                    }
                }

                done.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).expect("signalling peer");
            });
        });

        fasync::OnSignals::new(&done0, zx::Signals::USER_0).await.unwrap();
        ns.unbind(path.join("out").to_str().unwrap()).unwrap();
    }

    #[fasync::run_singlethreaded(test)]
    async fn reader_server_formatting() {
        let path = PathBuf::from("/test-bindings3");

        // Make a ServiceFs containing two files.
        // One is an inspect file, and one is not.
        let mut fs = ServiceFs::new();
        let vmo = zx::Vmo::create(4096).unwrap();
        let inspector = inspector_for_reader_test();

        let data = inspector.copy_vmo_data().unwrap();
        vmo.write(&data, 0).unwrap();
        fs.dir("diagnostics").add_vmo_file_at("test.inspect", vmo, 0, 4096);

        // Create a connection to the ServiceFs.
        let (h0, h1) = zx::Channel::create().unwrap();
        fs.serve_connection(h1).unwrap();

        let ns = fdio::Namespace::installed().unwrap();
        ns.bind(path.join("out").to_str().unwrap(), h0).unwrap();

        fasync::Task::spawn(fs.collect()).detach();
        let (done0, done1) = zx::Channel::create().unwrap();
        let thread_path = path.join("out");

        // Run the actual test in a separate thread so that it does not block on FS operations.
        // Use signalling on a zx::Channel to indicate that the test is done.
        std::thread::spawn(move || {
            let path = thread_path;
            let done = done1;
            let mut executor = fasync::Executor::new().unwrap();
            executor.run_singlethreaded(async {
                verify_reader(path).await;
                done.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).expect("signalling peer");
            });
        });

        fasync::OnSignals::new(&done0, zx::Signals::USER_0).await.unwrap();
        ns.unbind(path.join("out").to_str().unwrap()).unwrap();
    }

    #[fasync::run_singlethreaded(test)]
    async fn read_server_formatting_tree() {
        let path = PathBuf::from("/test-bindings4");

        // Make a ServiceFs containing two files.
        // One is an inspect file, and one is not.
        let mut fs = ServiceFs::new();
        let inspector = inspector_for_reader_test();
        inspector.serve(&mut fs).expect("failed to serve inspector");

        // Create a connection to the ServiceFs.
        let (h0, h1) = zx::Channel::create().unwrap();
        fs.serve_connection(h1).unwrap();

        let ns = fdio::Namespace::installed().unwrap();
        ns.bind(path.join("out").to_str().unwrap(), h0).unwrap();

        fasync::Task::spawn(fs.collect()).detach();
        let (done0, done1) = zx::Channel::create().unwrap();
        let thread_path = path.join("out");

        // Run the actual test in a separate thread so that it does not block on FS operations.
        // Use signalling on a zx::Channel to indicate that the test is done.
        std::thread::spawn(move || {
            let path = thread_path;
            let done = done1;
            let mut executor = fasync::Executor::new().unwrap();
            executor.run_singlethreaded(async {
                verify_reader(path).await;
                done.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).expect("signalling peer");
            });
        });
        fasync::OnSignals::new(&done0, zx::Signals::USER_0).await.unwrap();
        ns.unbind(path.join("out").to_str().unwrap()).unwrap();
    }

    #[fasync::run_singlethreaded(test)]
    async fn reader_server_reports_errors() {
        let path = PathBuf::from("/test-bindings-errors-01");

        // Make a ServiceFs containing something that looks like an inspect file but is not.
        let mut fs = ServiceFs::new();
        let vmo = zx::Vmo::create(4096).unwrap();
        fs.dir("diagnostics").add_vmo_file_at("test.inspect", vmo, 0, 4096);

        // Create a connection to the ServiceFs.
        let (h0, h1) = zx::Channel::create().unwrap();
        fs.serve_connection(h1).unwrap();

        let ns = fdio::Namespace::installed().unwrap();
        ns.bind(path.join("out").to_str().unwrap(), h0).unwrap();

        fasync::Task::spawn(fs.collect()).detach();
        let (done0, done1) = zx::Channel::create().unwrap();
        let thread_path = path.join("out");

        // Run the actual test in a separate thread so that it does not block on FS operations.
        // Use signalling on a zx::Channel to indicate that the test is done.
        std::thread::spawn(move || {
            let path = thread_path;
            let done = done1;
            let mut executor = fasync::Executor::new().unwrap();
            executor.run_singlethreaded(async {
                verify_reader_with_mode(path, VerifyMode::ExpectComponentFailure).await;
                done.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).expect("signalling peer");
            });
        });

        fasync::OnSignals::new(&done0, zx::Signals::USER_0).await.unwrap();
        ns.unbind(path.join("out").to_str().unwrap()).unwrap();
    }

    #[fasync::run_singlethreaded(test)]
    async fn inspect_repo_disallows_duplicated_dirs() {
        let inspect_repo = DataRepo::default();
        let mut inspect_repo = inspect_repo.write();
        let instance_id = "1234".to_string();

        let identity = ComponentIdentity::from_identifier_and_url(
            &ComponentIdentifier::Legacy { instance_id, moniker: vec!["a", "b", "foo.cmx"].into() },
            TEST_URL,
        );
        let (proxy, _) =
            fidl::endpoints::create_proxy::<DirectoryMarker>().expect("create directory proxy");

        inspect_repo
            .add_inspect_artifacts(identity.clone(), proxy, zx::Time::from_nanos(0))
            .expect("add to repo");

        let (proxy, _) =
            fidl::endpoints::create_proxy::<DirectoryMarker>().expect("create directory proxy");

        inspect_repo
            .add_inspect_artifacts(identity.clone(), proxy, zx::Time::from_nanos(0))
            .expect("add to repo");

        assert_eq!(
            inspect_repo.data_directories.get(&identity.unique_key).unwrap().get_values().len(),
            1
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn three_directories_two_batches() {
        stress_test_diagnostics_repository(vec![33, 33, 33], vec![64, 35]).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn max_batch_intact_two_batches_merged() {
        stress_test_diagnostics_repository(vec![64, 63, 1], vec![64, 64]).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn sixty_four_vmos_packed_into_one_batch() {
        stress_test_diagnostics_repository([1usize; 64].to_vec(), vec![64]).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn component_with_more_than_max_batch_size_is_split_in_two() {
        stress_test_diagnostics_repository(vec![65], vec![64, 1]).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn errorful_component_doesnt_halt_iteration() {
        stress_test_diagnostics_repository(vec![64, 65, 64, 64], vec![64, 64, 64, 64, 1]).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn merge_errorful_component_into_next_batch() {
        stress_test_diagnostics_repository(vec![63, 65], vec![64, 64]).await;
    }

    async fn stress_test_diagnostics_repository(
        directory_vmo_counts: Vec<usize>,
        expected_batch_results: Vec<usize>,
    ) {
        let path = PathBuf::from("/stress_test_root_directory");

        let dir_name_and_filecount: Vec<(String, usize)> = directory_vmo_counts
            .into_iter()
            .enumerate()
            .map(|(index, filecount)| (format!("diagnostics_{}", index), filecount))
            .collect();

        // Make a ServiceFs that will host inspect vmos under each
        // of the new diagnostics directories.
        let mut fs = ServiceFs::new();

        let inspector = inspector_for_reader_test();

        for (directory_name, filecount) in dir_name_and_filecount.clone() {
            for i in 0..filecount {
                let vmo = inspector
                    .duplicate_vmo()
                    .ok_or(format_err!("Failed to duplicate VMO"))
                    .unwrap();

                let size = vmo.get_size().unwrap();
                fs.dir(directory_name.clone()).add_vmo_file_at(
                    format!("root_{}.inspect", i),
                    vmo,
                    0, /* vmo offset */
                    size,
                );
            }
        }
        let (h0, h1) = zx::Channel::create().unwrap();
        fs.serve_connection(h1).unwrap();

        // We bind the root of the FS that hosts our 3 test dirs to
        // stress_test_root_dir. Now each directory can be found at
        // stress_test_root_dir/diagnostics_<i>
        let ns = fdio::Namespace::installed().unwrap();
        ns.bind(path.to_str().unwrap(), h0).unwrap();

        fasync::Task::spawn(fs.collect()).detach();

        let (done0, done1) = zx::Channel::create().unwrap();

        let cloned_path = path.clone();
        // Run the actual test in a separate thread so that it does not block on FS operations.
        // Use signalling on a zx::Channel to indicate that the test is done.
        std::thread::spawn(move || {
            let done = done1;
            let mut executor = fasync::Executor::new().unwrap();

            executor.run_singlethreaded(async {
                let id_and_directory_proxy =
                    join_all(dir_name_and_filecount.iter().map(|(dir, _)| {
                        let new_async_clone = cloned_path.clone();
                        async move {
                            let full_path = new_async_clone.join(dir);
                            let proxy = InspectDataCollector::find_directory_proxy(&full_path)
                                .await
                                .unwrap();
                            let unique_cid = ComponentIdentifier::Legacy {
                                instance_id: "1234".into(),
                                moniker: vec![format!("component_{}.cmx", dir)].into(),
                            };
                            (unique_cid, proxy)
                        }
                    }))
                    .await;

                let inspect_repo = DataRepo::default();
                let pipeline_wrapper =
                    Arc::new(RwLock::new(Pipeline::for_test(None, inspect_repo.clone())));

                for (cid, proxy) in id_and_directory_proxy {
                    let identity = ComponentIdentity::from_identifier_and_url(&cid, TEST_URL);
                    inspect_repo
                        .write()
                        .add_inspect_artifacts(identity.clone(), proxy, zx::Time::from_nanos(0))
                        .unwrap();

                    pipeline_wrapper
                        .write()
                        .add_inspect_artifacts(&identity.relative_moniker)
                        .unwrap();
                }

                let inspector = Inspector::new();
                let root = inspector.root();
                let test_archive_accessor_node = root.create_child("test_archive_accessor_node");

                let test_accessor_stats =
                    Arc::new(diagnostics::AccessorStats::new(test_archive_accessor_node));
                let test_batch_iterator_stats1 = Arc::new(
                    diagnostics::ConnectionStats::for_inspect(test_accessor_stats.clone()),
                );

                let _result_json = read_snapshot_verify_batch_count_and_batch_size(
                    pipeline_wrapper.clone(),
                    expected_batch_results,
                    test_batch_iterator_stats1,
                )
                .await;

                done.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).expect("signalling peer");
            });
        });

        fasync::OnSignals::new(&done0, zx::Signals::USER_0).await.unwrap();
        ns.unbind(path.to_str().unwrap()).unwrap();
    }

    fn inspector_for_reader_test() -> Inspector {
        let inspector = Inspector::new();
        let root = inspector.root();
        let child_1 = root.create_child("child_1");
        child_1.record_int("some-int", 2);
        let child_1_1 = child_1.create_child("child_1_1");
        child_1_1.record_int("some-int", 3);
        child_1_1.record_int("not-wanted-int", 4);
        root.record(child_1_1);
        root.record(child_1);
        let child_2 = root.create_child("child_2");
        child_2.record_int("some-int", 2);
        root.record(child_2);
        inspector
    }

    enum VerifyMode {
        ExpectSuccess,
        ExpectComponentFailure,
    }

    async fn verify_reader(path: PathBuf) {
        verify_reader_with_mode(path, VerifyMode::ExpectSuccess).await;
    }

    async fn verify_reader_with_mode(path: PathBuf, mode: VerifyMode) {
        let child_1_1_selector = selectors::parse_selector(r#"*:root/child_1/*:some-int"#).unwrap();
        let child_2_selector =
            selectors::parse_selector(r#"test_component.cmx:root/child_2:*"#).unwrap();
        let inspect_repo = DataRepo::default();
        let static_selectors_opt =
            Some(vec![Arc::new(child_1_1_selector), Arc::new(child_2_selector)]);

        let pipeline_wrapper =
            Arc::new(RwLock::new(Pipeline::for_test(static_selectors_opt, inspect_repo.clone())));

        let out_dir_proxy = InspectDataCollector::find_directory_proxy(&path).await.unwrap();

        // The absolute moniker here is made up since the selector is a glob
        // selector, so any path would match.
        let component_id = ComponentIdentifier::Legacy {
            instance_id: "1234".into(),
            moniker: vec!["test_component.cmx"].into(),
        };

        let inspector = Inspector::new();
        let root = inspector.root();
        let test_archive_accessor_node = root.create_child("test_archive_accessor_node");

        assert_inspect_tree!(inspector, root: {test_archive_accessor_node: {}});

        let test_accessor_stats =
            Arc::new(diagnostics::AccessorStats::new(test_archive_accessor_node));

        let test_batch_iterator_stats1 =
            Arc::new(diagnostics::ConnectionStats::for_inspect(test_accessor_stats.clone()));

        assert_inspect_tree!(inspector, root: {
            test_archive_accessor_node: {
                archive_accessor_connections_closed: 0u64,
                archive_accessor_connections_opened: 0u64,
                inspect_batch_iterator_connection0:{
                    inspect_batch_iterator_terminal_responses: 0u64,
                    inspect_batch_iterator_get_next_responses: 0u64,
                    inspect_batch_iterator_get_next_requests: 0u64,
                },
                inspect_batch_iterator_connections_closed: 0u64,
                inspect_batch_iterator_connections_opened: 0u64,
                inspect_batch_iterator_get_next_errors: 0u64,
                inspect_batch_iterator_get_next_requests: 0u64,
                inspect_batch_iterator_get_next_responses: 0u64,
                inspect_batch_iterator_get_next_result_count: 0u64,
                inspect_batch_iterator_get_next_result_errors: 0u64,
                inspect_component_timeouts_count: 0u64,
                inspect_reader_servers_constructed: 1u64,
                inspect_reader_servers_destroyed: 0u64,
                inspect_schema_truncation_count: 0u64,
                inspect_batch_iterator_get_next_time_usec: AnyProperty,
                inspect_max_snapshot_sizes_bytes: AnyProperty,
                inspect_snapshot_schema_truncation_percentage: AnyProperty,
                lifecycle_batch_iterator_connections_closed: 0u64,
                lifecycle_batch_iterator_connections_opened: 0u64,
                lifecycle_batch_iterator_get_next_errors: 0u64,
                lifecycle_batch_iterator_get_next_requests: 0u64,
                lifecycle_batch_iterator_get_next_responses: 0u64,
                lifecycle_batch_iterator_get_next_result_count: 0u64,
                lifecycle_batch_iterator_get_next_result_errors: 0u64,
                lifecycle_component_timeouts_count: 0u64,
                lifecycle_reader_servers_constructed: 0u64,
                lifecycle_reader_servers_destroyed: 0u64,
                lifecycle_batch_iterator_get_next_time_usec: AnyProperty,
                lifecycle_max_snapshot_sizes_bytes: AnyProperty,
                lifecycle_snapshot_schema_truncation_percentage: AnyProperty,
                lifecycle_schema_truncation_count: 0u64,
                logs_batch_iterator_connections_closed: 0u64,
                logs_batch_iterator_connections_opened: 0u64,
                logs_batch_iterator_get_next_errors: 0u64,
                logs_batch_iterator_get_next_requests: 0u64,
                logs_batch_iterator_get_next_responses: 0u64,
                logs_batch_iterator_get_next_result_count: 0u64,
                logs_batch_iterator_get_next_result_errors: 0u64,
                logs_component_timeouts_count: 0u64,
                logs_reader_servers_constructed: 0u64,
                logs_reader_servers_destroyed: 0u64,
                logs_batch_iterator_get_next_time_usec: AnyProperty,
                logs_max_snapshot_sizes_bytes: AnyProperty,
                logs_snapshot_schema_truncation_percentage: AnyProperty,
                logs_schema_truncation_count: 0u64,
                stream_diagnostics_requests: 0u64,
            }
        });

        let inspector_arc = Arc::new(inspector);

        let identity = ComponentIdentity::from_identifier_and_url(&component_id, TEST_URL);
        inspect_repo
            .write()
            .add_inspect_artifacts(identity.clone(), out_dir_proxy, zx::Time::from_nanos(0))
            .unwrap();

        pipeline_wrapper.write().add_inspect_artifacts(&identity.relative_moniker).unwrap();

        let expected_get_next_result_errors = match mode {
            VerifyMode::ExpectComponentFailure => 1u64,
            _ => 0u64,
        };

        {
            let result_json = read_snapshot(
                pipeline_wrapper.clone(),
                inspector_arc.clone(),
                test_batch_iterator_stats1,
            )
            .await;

            let result_array = result_json.as_array().expect("unit test json should be array.");
            assert_eq!(result_array.len(), 1, "Expect only one schema to be returned.");

            let result_map =
                result_array[0].as_object().expect("entries in the schema array are json objects.");

            let result_payload =
                result_map.get("payload").expect("diagnostics schema requires payload entry.");

            let expected_payload = match mode {
                VerifyMode::ExpectSuccess => json!({
                    "root": {
                        "child_1": {
                            "child_1_1": {
                                "some-int": 3
                            }
                        },
                        "child_2": {
                            "some-int": 2
                        }
                    }
                }),
                VerifyMode::ExpectComponentFailure => json!(null),
            };
            assert_eq!(*result_payload, expected_payload);

            // stream_diagnostics_requests is 0 since its tracked via archive_accessor server,
            // which isnt running in this unit test.
            assert_inspect_tree!(inspector_arc.clone(), root: {
                test_archive_accessor_node: {
                    archive_accessor_connections_closed: 0u64,
                    archive_accessor_connections_opened: 0u64,
                    inspect_batch_iterator_connections_closed: 1u64,
                    inspect_batch_iterator_connections_opened: 1u64,
                    inspect_batch_iterator_get_next_errors: 0u64,
                    inspect_batch_iterator_get_next_requests: 2u64,
                    inspect_batch_iterator_get_next_responses: 2u64,
                    inspect_batch_iterator_get_next_result_count: 1u64,
                    inspect_batch_iterator_get_next_result_errors: expected_get_next_result_errors,
                    inspect_component_timeouts_count: 0u64,
                    inspect_reader_servers_constructed: 1u64,
                    inspect_reader_servers_destroyed: 1u64,
                    inspect_batch_iterator_get_next_time_usec: AnyProperty,
                    inspect_max_snapshot_sizes_bytes: AnyProperty,
                    inspect_snapshot_schema_truncation_percentage: AnyProperty,
                    inspect_component_time_usec: AnyProperty,
                    inspect_schema_truncation_count:0u64,
                    inspect_longest_processing_times: contains {
                        "test_component.cmx": contains {
                            "@time": AnyProperty,
                            "duration_seconds": AnyProperty
                        }
                    },
                    lifecycle_batch_iterator_connections_closed: 0u64,
                    lifecycle_batch_iterator_connections_opened: 0u64,
                    lifecycle_batch_iterator_get_next_errors: 0u64,
                    lifecycle_batch_iterator_get_next_requests: 0u64,
                    lifecycle_batch_iterator_get_next_responses: 0u64,
                    lifecycle_batch_iterator_get_next_result_count: 0u64,
                    lifecycle_batch_iterator_get_next_result_errors: 0u64,
                    lifecycle_component_timeouts_count: 0u64,
                    lifecycle_reader_servers_constructed: 0u64,
                    lifecycle_reader_servers_destroyed: 0u64,
                    lifecycle_batch_iterator_get_next_time_usec: AnyProperty,
                    lifecycle_max_snapshot_sizes_bytes: AnyProperty,
                    lifecycle_snapshot_schema_truncation_percentage: AnyProperty,
                    lifecycle_schema_truncation_count:0u64,
                    logs_batch_iterator_connections_closed: 0u64,
                    logs_batch_iterator_connections_opened: 0u64,
                    logs_batch_iterator_get_next_errors: 0u64,
                    logs_batch_iterator_get_next_requests: 0u64,
                    logs_batch_iterator_get_next_responses: 0u64,
                    logs_batch_iterator_get_next_result_count: 0u64,
                    logs_batch_iterator_get_next_result_errors: 0u64,
                    logs_component_timeouts_count: 0u64,
                    logs_reader_servers_constructed: 0u64,
                    logs_reader_servers_destroyed: 0u64,
                    logs_batch_iterator_get_next_time_usec: AnyProperty,
                    logs_max_snapshot_sizes_bytes: AnyProperty,
                    logs_snapshot_schema_truncation_percentage: AnyProperty,
                    logs_schema_truncation_count:0u64,
                    stream_diagnostics_requests: 0u64,
                }
            });
        }

        let test_batch_iterator_stats2 =
            Arc::new(diagnostics::ConnectionStats::for_inspect(test_accessor_stats.clone()));

        inspect_repo.write().remove(&identity.unique_key);
        pipeline_wrapper.write().remove(&identity.relative_moniker);
        {
            let result_json = read_snapshot(
                pipeline_wrapper.clone(),
                inspector_arc.clone(),
                test_batch_iterator_stats2,
            )
            .await;

            let result_array = result_json.as_array().expect("unit test json should be array.");
            assert_eq!(result_array.len(), 0, "Expect no schemas to be returned.");

            assert_inspect_tree!(inspector_arc.clone(), root: {
                test_archive_accessor_node: {
                    archive_accessor_connections_closed: 0u64,
                    archive_accessor_connections_opened: 0u64,
                    inspect_batch_iterator_connections_closed: 2u64,
                    inspect_batch_iterator_connections_opened: 2u64,
                    inspect_batch_iterator_get_next_errors: 0u64,
                    inspect_batch_iterator_get_next_requests: 3u64,
                    inspect_batch_iterator_get_next_responses: 3u64,
                    inspect_batch_iterator_get_next_result_count: 1u64,
                    inspect_batch_iterator_get_next_result_errors: expected_get_next_result_errors,
                    inspect_component_timeouts_count: 0u64,
                    inspect_reader_servers_constructed: 2u64,
                    inspect_reader_servers_destroyed: 2u64,
                    inspect_batch_iterator_get_next_time_usec: AnyProperty,
                    inspect_max_snapshot_sizes_bytes: AnyProperty,
                    inspect_snapshot_schema_truncation_percentage: AnyProperty,
                    inspect_component_time_usec: AnyProperty,
                    inspect_schema_truncation_count: 0u64,
                    inspect_longest_processing_times: contains {
                        "test_component.cmx": contains {
                            "@time": AnyProperty,
                            "duration_seconds": AnyProperty,
                        }
                    },
                    lifecycle_batch_iterator_connections_closed: 0u64,
                    lifecycle_batch_iterator_connections_opened: 0u64,
                    lifecycle_batch_iterator_get_next_errors: 0u64,
                    lifecycle_batch_iterator_get_next_requests: 0u64,
                    lifecycle_batch_iterator_get_next_responses: 0u64,
                    lifecycle_batch_iterator_get_next_result_count: 0u64,
                    lifecycle_batch_iterator_get_next_result_errors: 0u64,
                    lifecycle_component_timeouts_count: 0u64,
                    lifecycle_reader_servers_constructed: 0u64,
                    lifecycle_reader_servers_destroyed: 0u64,
                    lifecycle_schema_truncation_count: 0u64,
                    lifecycle_batch_iterator_get_next_time_usec: AnyProperty,
                    lifecycle_max_snapshot_sizes_bytes: AnyProperty,
                    lifecycle_snapshot_schema_truncation_percentage: AnyProperty,
                    logs_batch_iterator_connections_closed: 0u64,
                    logs_batch_iterator_connections_opened: 0u64,
                    logs_batch_iterator_get_next_errors: 0u64,
                    logs_batch_iterator_get_next_requests: 0u64,
                    logs_batch_iterator_get_next_responses: 0u64,
                    logs_batch_iterator_get_next_result_count: 0u64,
                    logs_batch_iterator_get_next_result_errors: 0u64,
                    logs_component_timeouts_count: 0u64,
                    logs_reader_servers_constructed: 0u64,
                    logs_reader_servers_destroyed: 0u64,
                    logs_batch_iterator_get_next_time_usec: AnyProperty,
                    logs_max_snapshot_sizes_bytes: AnyProperty,
                    logs_snapshot_schema_truncation_percentage: AnyProperty,
                    logs_schema_truncation_count: 0u64,
                    stream_diagnostics_requests: 0u64,
                }
            });
        }
    }

    fn start_snapshot(
        inspect_pipeline: Arc<RwLock<Pipeline>>,
        stats: Arc<ConnectionStats>,
    ) -> (BatchIteratorProxy, Task<()>) {
        let test_performance_config = PerformanceConfig {
            batch_timeout_sec: BATCH_RETRIEVAL_TIMEOUT_SECONDS,
            aggregated_content_limit_bytes: None,
        };

        let reader_server = Box::pin(ReaderServer::stream(
            inspect_pipeline.read().fetch_inspect_data(&None),
            test_performance_config,
            None,
            stats.clone(),
        ));
        let (consumer, batch_iterator_requests) =
            create_proxy_and_stream::<BatchIteratorMarker>().unwrap();
        (
            consumer,
            Task::spawn(async {
                BatchIterator::new(
                    reader_server,
                    batch_iterator_requests,
                    StreamMode::Snapshot,
                    stats,
                    None,
                )
                .unwrap()
                .run()
                .await
                .unwrap()
            }),
        )
    }

    async fn read_snapshot(
        inspect_pipeline: Arc<RwLock<Pipeline>>,
        _test_inspector: Arc<Inspector>,
        stats: Arc<ConnectionStats>,
    ) -> serde_json::Value {
        let (consumer, server) = start_snapshot(inspect_pipeline, stats);

        let mut result_vec: Vec<String> = Vec::new();
        loop {
            let next_batch: Vec<fidl_fuchsia_diagnostics::FormattedContent> =
                consumer.get_next().await.unwrap().unwrap();

            if next_batch.is_empty() {
                break;
            }
            for formatted_content in next_batch {
                match formatted_content {
                    fidl_fuchsia_diagnostics::FormattedContent::Json(data) => {
                        let mut buf = vec![0; data.size as usize];
                        data.vmo.read(&mut buf, 0).expect("reading vmo");
                        let hierarchy_string = std::str::from_utf8(&buf).unwrap();
                        result_vec.push(hierarchy_string.to_string());
                    }
                    _ => panic!("test only produces json formatted data"),
                }
            }
        }

        // ensures connection is marked as closed, wait for stream to terminate
        drop(consumer);
        server.await;

        let result_string = format!("[{}]", result_vec.join(","));
        serde_json::from_str(&result_string)
            .expect(&format!("unit tests shouldn't be creating malformed json: {}", result_string))
    }

    async fn read_snapshot_verify_batch_count_and_batch_size(
        inspect_repo: Arc<RwLock<Pipeline>>,
        expected_batch_sizes: Vec<usize>,
        stats: Arc<ConnectionStats>,
    ) -> serde_json::Value {
        let (consumer, server) = start_snapshot(inspect_repo, stats);

        let mut result_vec: Vec<String> = Vec::new();
        let mut batch_counts = Vec::new();
        loop {
            let next_batch: Vec<fidl_fuchsia_diagnostics::FormattedContent> =
                consumer.get_next().await.unwrap().unwrap();

            if next_batch.is_empty() {
                assert_eq!(expected_batch_sizes, batch_counts);
                break;
            }

            batch_counts.push(next_batch.len());

            for formatted_content in next_batch {
                match formatted_content {
                    fidl_fuchsia_diagnostics::FormattedContent::Json(data) => {
                        let mut buf = vec![0; data.size as usize];
                        data.vmo.read(&mut buf, 0).expect("reading vmo");
                        let hierarchy_string = std::str::from_utf8(&buf).unwrap();
                        result_vec.push(hierarchy_string.to_string());
                    }
                    _ => panic!("test only produces json formatted data"),
                }
            }
        }

        // ensures connection is marked as closed, wait for stream to terminate
        drop(consumer);
        server.await;

        let result_string = format!("[{}]", result_vec.join(","));
        serde_json::from_str(&result_string)
            .expect(&format!("unit tests shouldn't be creating malformed json: {}", result_string))
    }
}
