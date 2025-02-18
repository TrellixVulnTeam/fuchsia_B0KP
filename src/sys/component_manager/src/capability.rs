// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{component::WeakComponentInstance, error::ModelError},
    async_trait::async_trait,
    cm_rust::*,
    from_enum::FromEnum,
    fuchsia_zircon as zx,
    moniker::AbsoluteMoniker,
    std::{fmt, path::PathBuf},
    thiserror::Error,
};

#[derive(Debug, Error)]
pub enum Error {
    #[error("Invalid framework capability.")]
    InvalidFrameworkCapability {},
    #[error("Invalid builtin capability.")]
    InvalidBuiltinCapability {},
}

/// The list of declarations for capabilities from component manager's namespace.
pub type NamespaceCapabilities = Vec<cm_rust::CapabilityDecl>;

/// Describes the source of a capability, as determined by `find_capability_source`
#[derive(Clone, Debug)]
pub enum CapabilitySource {
    /// This capability originates from the component instance for the given Realm.
    /// point.
    Component { capability: ComponentCapability, component: WeakComponentInstance },
    /// This capability originates from "framework". It's implemented by component manager and is
    /// scoped to the realm of the source.
    Framework { capability: InternalCapability, scope_moniker: AbsoluteMoniker },
    /// This capability originates from the parent of the root component, and is
    /// built in to component manager.
    Builtin { capability: InternalCapability },
    /// This capability originates from the parent of the root component, and is
    /// offered from component manager's namespace.
    Namespace { capability: ComponentCapability },
    /// This capability is provided by the framework based on some other capability.
    Capability { source_capability: ComponentCapability, component: WeakComponentInstance },
}

impl CapabilitySource {
    /// Returns whether the given CapabilitySource can be available in a component's namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        match self {
            CapabilitySource::Component { capability, .. } => capability.can_be_in_namespace(),
            CapabilitySource::Framework { capability, .. } => capability.can_be_in_namespace(),
            CapabilitySource::Builtin { capability } => capability.can_be_in_namespace(),
            CapabilitySource::Namespace { capability } => capability.can_be_in_namespace(),
            CapabilitySource::Capability { .. } => true,
        }
    }

    pub fn source_name(&self) -> Option<&CapabilityName> {
        match self {
            CapabilitySource::Component { capability, .. } => capability.source_name(),
            CapabilitySource::Framework { capability, .. } => Some(capability.source_name()),
            CapabilitySource::Builtin { capability } => Some(capability.source_name()),
            CapabilitySource::Namespace { capability } => capability.source_name(),
            CapabilitySource::Capability { .. } => None,
        }
    }

    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            CapabilitySource::Component { capability, .. } => capability.type_name(),
            CapabilitySource::Framework { capability, .. } => capability.type_name(),
            CapabilitySource::Builtin { capability } => capability.type_name(),
            CapabilitySource::Namespace { capability } => capability.type_name(),
            CapabilitySource::Capability { source_capability, .. } => source_capability.type_name(),
        }
    }
}

impl fmt::Display for CapabilitySource {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                CapabilitySource::Component { capability, component } => {
                    format!("{} '{}'", capability, component.moniker)
                }
                CapabilitySource::Framework { capability, .. } => capability.to_string(),
                CapabilitySource::Builtin { capability } => capability.to_string(),
                CapabilitySource::Namespace { capability } => capability.to_string(),
                CapabilitySource::Capability { source_capability, .. } =>
                    format!("{}", source_capability),
            }
        )
    }
}

/// Describes a capability provided by the component manager which could be a framework capability
/// scoped to a realm, a built-in global capability, or a capability from component manager's own
/// namespace.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InternalCapability {
    Service(CapabilityName),
    Protocol(CapabilityName),
    Directory(CapabilityName),
    Runner(CapabilityName),
    Event(CapabilityName),
    Resolver(CapabilityName),
}

impl InternalCapability {
    /// Returns whether the given InternalCapability can be available in a component's namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        matches!(
            self,
            InternalCapability::Service(_)
                | InternalCapability::Protocol(_)
                | InternalCapability::Directory(_)
        )
    }

    /// Returns a name for the capability type.
    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            InternalCapability::Service(_) => CapabilityTypeName::Service,
            InternalCapability::Protocol(_) => CapabilityTypeName::Protocol,
            InternalCapability::Directory(_) => CapabilityTypeName::Directory,
            InternalCapability::Runner(_) => CapabilityTypeName::Runner,
            InternalCapability::Event(_) => CapabilityTypeName::Event,
            InternalCapability::Resolver(_) => CapabilityTypeName::Resolver,
        }
    }

    pub fn source_name(&self) -> &CapabilityName {
        match self {
            InternalCapability::Service(name) => &name,
            InternalCapability::Protocol(name) => &name,
            InternalCapability::Directory(name) => &name,
            InternalCapability::Runner(name) => &name,
            InternalCapability::Event(name) => &name,
            InternalCapability::Resolver(name) => &name,
        }
    }

    /// Returns true if this is a protocol with name that matches `name`.
    pub fn matches_protocol(&self, name: &CapabilityName) -> bool {
        match self {
            Self::Protocol(source_name) => source_name == name,
            _ => false,
        }
    }
}

impl fmt::Display for InternalCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} '{}' from component manager", self.type_name(), self.source_name())
    }
}

/// The server-side of a capability implements this trait.
/// Multiple `CapabilityProvider` objects can compose with one another for a single
/// capability request. For example, a `CapabitilityProvider` can be interposed
/// between the primary `CapabilityProvider and the client for the purpose of
/// logging and testing. A `CapabilityProvider` is typically provided by a
/// corresponding `Hook` in response to the `CapabilityRouted` event.
/// A capability provider is used exactly once as a result of exactly one route.
#[async_trait]
pub trait CapabilityProvider: Send + Sync {
    // Called to bind a server end of a zx::Channel to the provided capability.
    // If the capability is a directory, then |flags|, |open_mode| and |relative_path|
    // will be propagated along to open the appropriate directory.
    async fn open(
        self: Box<Self>,
        flags: u32,
        open_mode: u32,
        relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), ModelError>;
}

/// A capability being routed from a component.
#[derive(FromEnum, Clone, Debug, PartialEq, Eq)]
pub enum ComponentCapability {
    Use(UseDecl),
    /// Models a capability used from the environment.
    Environment(EnvironmentCapability),
    Expose(ExposeDecl),
    Offer(OfferDecl),
    Protocol(ProtocolDecl),
    Directory(DirectoryDecl),
    Storage(StorageDecl),
    Runner(RunnerDecl),
    Resolver(ResolverDecl),
}

impl ComponentCapability {
    /// Returns whether the given ComponentCapability can be available in a component's namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        match self {
            ComponentCapability::Use(use_) => {
                matches!(use_, UseDecl::Protocol(_) | UseDecl::Directory(_) | UseDecl::Service(_))
            }
            ComponentCapability::Expose(expose) => {
                matches!(
                    expose,
                    ExposeDecl::Protocol(_) | ExposeDecl::Directory(_) | ExposeDecl::Service(_)
                )
            }
            ComponentCapability::Offer(offer) => matches!(
                offer,
                OfferDecl::Protocol(_) | OfferDecl::Directory(_) | OfferDecl::Service(_)
            ),
            ComponentCapability::Protocol(_) | ComponentCapability::Directory(_) => true,
            _ => false,
        }
    }

    /// Returns a name for the capability type.
    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Protocol(_) => CapabilityTypeName::Protocol,
                UseDecl::Directory(_) => CapabilityTypeName::Directory,
                UseDecl::Service(_) => CapabilityTypeName::Service,
                UseDecl::Storage(_) => CapabilityTypeName::Storage,
                UseDecl::Event(_) => CapabilityTypeName::Event,
                UseDecl::EventStream(_) => CapabilityTypeName::EventStream,
            },
            ComponentCapability::Environment(env) => match env {
                EnvironmentCapability::Runner { .. } => CapabilityTypeName::Runner,
                EnvironmentCapability::Resolver { .. } => CapabilityTypeName::Resolver,
                EnvironmentCapability::Debug { .. } => CapabilityTypeName::Protocol,
            },
            ComponentCapability::Expose(expose) => match expose {
                ExposeDecl::Protocol(_) => CapabilityTypeName::Protocol,
                ExposeDecl::Directory(_) => CapabilityTypeName::Directory,
                ExposeDecl::Service(_) => CapabilityTypeName::Service,
                ExposeDecl::Runner(_) => CapabilityTypeName::Runner,
                ExposeDecl::Resolver(_) => CapabilityTypeName::Resolver,
            },
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Protocol(_) => CapabilityTypeName::Protocol,
                OfferDecl::Directory(_) => CapabilityTypeName::Directory,
                OfferDecl::Service(_) => CapabilityTypeName::Service,
                OfferDecl::Storage(_) => CapabilityTypeName::Storage,
                OfferDecl::Runner(_) => CapabilityTypeName::Runner,
                OfferDecl::Resolver(_) => CapabilityTypeName::Resolver,
                OfferDecl::Event(_) => CapabilityTypeName::Event,
            },
            ComponentCapability::Protocol(_) => CapabilityTypeName::Protocol,
            ComponentCapability::Directory(_) => CapabilityTypeName::Directory,
            ComponentCapability::Storage(_) => CapabilityTypeName::Storage,
            ComponentCapability::Runner(_) => CapabilityTypeName::Runner,
            ComponentCapability::Resolver(_) => CapabilityTypeName::Resolver,
        }
    }

    /// Return the source path of the capability, if one exists.
    pub fn source_path(&self) -> Option<&CapabilityPath> {
        match self {
            ComponentCapability::Storage(_) => None,
            ComponentCapability::Protocol(protocol) => Some(&protocol.source_path),
            ComponentCapability::Directory(directory) => Some(&directory.source_path),
            ComponentCapability::Runner(runner) => Some(&runner.source_path),
            ComponentCapability::Resolver(resolver) => Some(&resolver.source_path),
            _ => None,
        }
    }

    /// Return the name of the capability, if this is a capability declaration.
    pub fn source_name(&self) -> Option<&CapabilityName> {
        match self {
            ComponentCapability::Storage(storage) => Some(&storage.name),
            ComponentCapability::Protocol(protocol) => Some(&protocol.name),
            ComponentCapability::Directory(directory) => Some(&directory.name),
            ComponentCapability::Runner(runner) => Some(&runner.name),
            ComponentCapability::Resolver(resolver) => Some(&resolver.name),
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Protocol(UseProtocolDecl { source_name, .. }) => Some(source_name),
                UseDecl::Directory(UseDirectoryDecl { source_name, .. }) => Some(source_name),
                UseDecl::Event(UseEventDecl { source_name, .. }) => Some(source_name),
                UseDecl::Storage(UseStorageDecl { source_name, .. }) => Some(source_name),
                _ => None,
            },
            ComponentCapability::Environment(env_cap) => match env_cap {
                EnvironmentCapability::Runner { source_name, .. } => Some(source_name),
                EnvironmentCapability::Resolver { source_name, .. } => Some(source_name),
                EnvironmentCapability::Debug { source_name, .. } => Some(source_name),
            },
            ComponentCapability::Expose(expose) => match expose {
                ExposeDecl::Protocol(ExposeProtocolDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Directory(ExposeDirectoryDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Runner(ExposeRunnerDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Resolver(ExposeResolverDecl { source_name, .. }) => Some(source_name),
                _ => None,
            },
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Protocol(OfferProtocolDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Directory(OfferDirectoryDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Runner(OfferRunnerDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Event(OfferEventDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Storage(OfferStorageDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Resolver(OfferResolverDecl { source_name, .. }) => Some(source_name),
                _ => None,
            },
        }
    }

    pub fn source_capability_name(&self) -> Option<&CapabilityName> {
        match self {
            ComponentCapability::Offer(OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Capability(name),
                ..
            })) => Some(name),
            ComponentCapability::Expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                source: ExposeSource::Capability(name),
                ..
            })) => Some(name),
            ComponentCapability::Use(UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Capability(name),
                ..
            })) => Some(name),
            _ => None,
        }
    }

    /// Returns the source path or name of the capability as a string, useful for debugging.
    pub fn source_id(&self) -> String {
        self.source_name()
            .map(|p| format!("{}", p))
            .or_else(|| self.source_path().map(|p| format!("{}", p)))
            .unwrap_or_default()
    }
}

impl fmt::Display for ComponentCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} '{}' from component", self.type_name(), self.source_id())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum EnvironmentCapability {
    Runner { source_name: CapabilityName, source: RegistrationSource },
    Resolver { source_name: CapabilityName, source: RegistrationSource },
    Debug { source_name: CapabilityName, source: RegistrationSource },
}

impl EnvironmentCapability {
    pub fn registration_source(&self) -> &RegistrationSource {
        match self {
            Self::Runner { source, .. }
            | Self::Resolver { source, .. }
            | Self::Debug { source, .. } => &source,
        }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, cm_rust::StorageDirectorySource};

    #[test]
    fn capability_type_name() {
        let storage_capability = ComponentCapability::Storage(StorageDecl {
            name: "foo".into(),
            source: StorageDirectorySource::Parent,
            backing_dir: "bar".into(),
            subdir: None,
        });
        assert_eq!(storage_capability.type_name(), CapabilityTypeName::Storage);

        let event_capability = ComponentCapability::Use(UseDecl::Event(UseEventDecl {
            source: cm_rust::UseSource::Parent,
            source_name: "started".into(),
            target_name: "started-x".into(),
            filter: None,
            mode: EventMode::Async,
        }));
        assert_eq!(event_capability.type_name(), CapabilityTypeName::Event);
    }
}
