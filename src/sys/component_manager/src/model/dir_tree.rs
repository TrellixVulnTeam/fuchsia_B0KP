// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        addable_directory::AddableDirectory, component::WeakComponentInstance, error::ModelError,
    },
    cm_rust::{CapabilityPath, ComponentDecl, ExposeDecl, UseDecl},
    directory_broker::{DirectoryBroker, RoutingFn},
    moniker::AbsoluteMoniker,
    std::collections::HashMap,
    vfs::directory::immutable::simple as pfs,
};

/// Represents the directory hierarchy of the exposed directory, not including the nodes for the
/// capabilities themselves.
pub(super) struct DirTree {
    directory_nodes: HashMap<String, Box<DirTree>>,
    broker_nodes: HashMap<String, RoutingFn>,
}

impl DirTree {
    /// Builds a directory hierarchy from a component's `uses` declarations.
    /// `routing_factory` is a closure that generates the routing function that will be called
    /// when a leaf node is opened.
    pub fn build_from_uses(
        routing_factory: impl Fn(WeakComponentInstance, UseDecl) -> RoutingFn,
        component: WeakComponentInstance,
        decl: ComponentDecl,
    ) -> Self {
        let mut tree = DirTree { directory_nodes: HashMap::new(), broker_nodes: HashMap::new() };
        for use_ in decl.uses {
            tree.add_use_capability(&routing_factory, component.clone(), &use_);
        }
        tree
    }

    /// Builds a directory hierarchy from a component's `exposes` declarations.
    /// `routing_factory` is a closure that generates the routing function that will be called
    /// when a leaf node is opened.
    pub fn build_from_exposes(
        routing_factory: impl Fn(WeakComponentInstance, ExposeDecl) -> RoutingFn,
        component: WeakComponentInstance,
        decl: ComponentDecl,
    ) -> Self {
        let mut tree = DirTree { directory_nodes: HashMap::new(), broker_nodes: HashMap::new() };
        for expose in decl.exposes {
            tree.add_expose_capability(&routing_factory, component.clone(), &expose);
        }
        tree
    }

    /// Installs the directory tree into `root_dir`.
    pub fn install<'entries>(
        self,
        abs_moniker: &AbsoluteMoniker,
        root_dir: &mut impl AddableDirectory,
    ) -> Result<(), ModelError> {
        for (name, subtree) in self.directory_nodes {
            let mut subdir = pfs::simple();
            subtree.install(abs_moniker, &mut subdir)?;
            root_dir.add_node(&name, subdir, abs_moniker)?;
        }
        for (name, route_fn) in self.broker_nodes {
            let node = DirectoryBroker::new(route_fn);
            root_dir.add_node(&name, node, abs_moniker)?;
        }
        Ok(())
    }

    fn add_use_capability(
        &mut self,
        routing_factory: &impl Fn(WeakComponentInstance, UseDecl) -> RoutingFn,
        component: WeakComponentInstance,
        use_: &UseDecl,
    ) {
        // Event and EventStream capabilities are used by the framework itself and not given to
        // components directly.
        match use_ {
            cm_rust::UseDecl::Event(_) | cm_rust::UseDecl::EventStream(_) => return,
            _ => {}
        }

        let path = match use_.path() {
            Some(path) => path,
            None => return,
        };
        let tree = self.to_directory_node(path);
        let routing_fn = routing_factory(component, use_.clone());
        tree.broker_nodes.insert(path.basename.to_string(), routing_fn);
    }

    fn add_expose_capability(
        &mut self,
        routing_factory: &impl Fn(WeakComponentInstance, ExposeDecl) -> RoutingFn,
        component: WeakComponentInstance,
        expose: &ExposeDecl,
    ) {
        let path = match expose {
            cm_rust::ExposeDecl::Service(d) => {
                format!("/{}", d.target_name).parse().expect("couldn't parse name as path")
            }
            cm_rust::ExposeDecl::Protocol(d) => {
                format!("/{}", d.target_name).parse().expect("couldn't parse name as path")
            }
            cm_rust::ExposeDecl::Directory(d) => {
                format!("/{}", d.target_name).parse().expect("couldn't parse name as path")
            }
            cm_rust::ExposeDecl::Runner(_) | cm_rust::ExposeDecl::Resolver(_) => {
                // Runners and resolvers do not add directory entries.
                return;
            }
        };
        let tree = self.to_directory_node(&path);
        let routing_fn = routing_factory(component, expose.clone());
        tree.broker_nodes.insert(path.basename.to_string(), routing_fn);
    }

    fn to_directory_node(&mut self, path: &CapabilityPath) -> &mut DirTree {
        let components = path.dirname.split("/");
        let mut tree = self;
        for component in components {
            if !component.is_empty() {
                tree = tree.directory_nodes.entry(component.to_string()).or_insert(Box::new(
                    DirTree { directory_nodes: HashMap::new(), broker_nodes: HashMap::new() },
                ));
            }
        }
        tree
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::{
            component::ComponentInstance,
            environment::Environment,
            testing::{mocks, test_helpers, test_helpers::*},
        },
        cm_rust::{
            CapabilityName, CapabilityPath, ExposeDecl, ExposeDirectoryDecl, ExposeProtocolDecl,
            ExposeRunnerDecl, ExposeSource, ExposeTarget, UseDecl, UseDirectoryDecl,
            UseProtocolDecl, UseSource, UseStorageDecl,
        },
        fidl::endpoints::{ClientEnd, ServerEnd},
        fidl_fuchsia_io::MODE_TYPE_DIRECTORY,
        fidl_fuchsia_io::{DirectoryMarker, NodeMarker, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE},
        fidl_fuchsia_io2 as fio2, fuchsia_zircon as zx,
        std::{
            convert::{TryFrom, TryInto},
            sync::Weak,
        },
        vfs::{directory::entry::DirectoryEntry, execution_scope::ExecutionScope, path},
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn build_from_uses() {
        // Call `build_from_uses` with a routing factory that routes to a mock directory or service,
        // and a `ComponentDecl` with `use` declarations.
        let routing_factory = mocks::proxy_use_routing_factory();
        let decl = ComponentDecl {
            uses: vec![
                UseDecl::Directory(UseDirectoryDecl {
                    source: UseSource::Parent,
                    source_name: "baz-dir".into(),
                    target_path: CapabilityPath::try_from("/in/data/hippo").unwrap(),
                    rights: fio2::Operations::Connect,
                    subdir: None,
                }),
                UseDecl::Protocol(UseProtocolDecl {
                    source: UseSource::Parent,
                    source_name: "baz-svc".into(),
                    target_path: CapabilityPath::try_from("/in/svc/hippo").unwrap(),
                }),
                UseDecl::Storage(UseStorageDecl {
                    source_name: "data".into(),
                    target_path: "/in/data/persistent".try_into().unwrap(),
                }),
                UseDecl::Storage(UseStorageDecl {
                    source_name: "cache".into(),
                    target_path: "/in/data/cache".try_into().unwrap(),
                }),
            ],
            ..default_component_decl()
        };
        let root = ComponentInstance::new_root(
            Environment::empty(),
            Weak::new(),
            Weak::new(),
            "test://root".to_string(),
        );
        let tree = DirTree::build_from_uses(routing_factory, root.as_weak(), decl.clone());

        // Convert the tree to a directory.
        let mut in_dir = pfs::simple();
        tree.install(&root.abs_moniker, &mut in_dir).expect("Unable to build pseudodirectory");
        let (in_dir_client, in_dir_server) = zx::Channel::create().unwrap();
        in_dir.open(
            ExecutionScope::new(),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            path::Path::empty(),
            ServerEnd::<NodeMarker>::new(in_dir_server.into()),
        );
        let in_dir_proxy = ClientEnd::<DirectoryMarker>::new(in_dir_client)
            .into_proxy()
            .expect("failed to create directory proxy");
        assert_eq!(
            vec!["in/data/cache", "in/data/hippo", "in/data/persistent", "in/svc/hippo"],
            test_helpers::list_directory_recursive(&in_dir_proxy).await
        );

        // Expect that calls on the directory nodes reach the mock directory/service.
        assert_eq!("friend", test_helpers::read_file(&in_dir_proxy, "in/data/hippo/hello").await);
        assert_eq!(
            "friend",
            test_helpers::read_file(&in_dir_proxy, "in/data/persistent/hello").await
        );
        assert_eq!("friend", test_helpers::read_file(&in_dir_proxy, "in/data/cache/hello").await);
        assert_eq!(
            "hippos".to_string(),
            test_helpers::call_echo(&in_dir_proxy, "in/svc/hippo").await
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn build_from_exposes() {
        // Call `build_from_exposes` with a routing factory that routes to a mock directory or
        // service, and a `ComponentDecl` with `expose` declarations.
        let routing_factory = mocks::proxy_expose_routing_factory();
        let decl = ComponentDecl {
            exposes: vec![
                ExposeDecl::Directory(ExposeDirectoryDecl {
                    source: ExposeSource::Self_,
                    source_name: "baz-dir".into(),
                    target_name: "hippo-dir".into(),
                    target: ExposeTarget::Parent,
                    rights: Some(fio2::Operations::Connect),
                    subdir: None,
                }),
                ExposeDecl::Directory(ExposeDirectoryDecl {
                    source: ExposeSource::Self_,
                    source_name: "foo-dir".into(),
                    target_name: "bar-dir".into(),
                    target: ExposeTarget::Parent,
                    rights: Some(fio2::Operations::Connect),
                    subdir: None,
                }),
                ExposeDecl::Protocol(ExposeProtocolDecl {
                    source: ExposeSource::Self_,
                    source_name: "baz-svc".into(),
                    target_name: "hippo-svc".into(),
                    target: ExposeTarget::Parent,
                }),
                ExposeDecl::Runner(ExposeRunnerDecl {
                    source: ExposeSource::Self_,
                    source_name: CapabilityName::from("elf"),
                    target: ExposeTarget::Parent,
                    target_name: CapabilityName::from("elf"),
                }),
            ],
            ..default_component_decl()
        };
        let root = ComponentInstance::new_root(
            Environment::empty(),
            Weak::new(),
            Weak::new(),
            "test://root".to_string(),
        );
        let tree = DirTree::build_from_exposes(routing_factory, root.as_weak(), decl.clone());

        // Convert the tree to a directory.
        let mut expose_dir = pfs::simple();
        tree.install(&root.abs_moniker, &mut expose_dir).expect("Unable to build pseudodirectory");
        let (expose_dir_client, expose_dir_server) = zx::Channel::create().unwrap();
        expose_dir.open(
            ExecutionScope::new(),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            path::Path::empty(),
            ServerEnd::<NodeMarker>::new(expose_dir_server.into()),
        );
        let expose_dir_proxy = ClientEnd::<DirectoryMarker>::new(expose_dir_client)
            .into_proxy()
            .expect("failed to create directory proxy");
        assert_eq!(
            vec!["bar-dir", "hippo-dir", "hippo-svc"],
            test_helpers::list_directory_recursive(&expose_dir_proxy).await
        );

        // Expect that calls on the directory nodes reach the mock directory/service.
        assert_eq!("friend", test_helpers::read_file(&expose_dir_proxy, "bar-dir/hello").await);
        assert_eq!("friend", test_helpers::read_file(&expose_dir_proxy, "hippo-dir/hello").await);
        assert_eq!(
            "hippos".to_string(),
            test_helpers::call_echo(&expose_dir_proxy, "hippo-svc").await
        );
    }
}
