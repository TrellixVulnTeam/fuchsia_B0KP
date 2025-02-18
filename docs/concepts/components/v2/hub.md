# Hub (Components v2)

<<../_v2_banner.md>>

## Definition

The hub provides access to detailed structural information about component
instances at runtime. The hub provides information such as:

-   Instance name
-   Instance state
-   Instance ID
-   Children
-   Component URL
-   Exposed capabilities

## Features

### Immutability

The hub’s structure is mostly read-only. It is not possible to create, rename,
delete, or otherwise modify directories and files that form the structure of
the hub itself. However, the [outgoing](/docs/concepts/system/abi/system.md)
directory of an instance may include mutable directories, files, and services
which can be accessed through the hub.

### Scoping

The root of a hub directory is always scoped to a particular [realm](realms.md).

The hub of a realm gives information about:

-   The realm itself
-   All child realms
-   All component instances

The hub of a realm cannot give information about the parent realm. This
structure makes it easy to constrain the parts of the hub particular clients can
access.

If a [realm](realms.md) requests access to its hub, it can also access the hub
of all of its descendant [realms](realms.md), by traversing down the `children/`
subdirectory.

## Requesting the hub

Like other capabilities, hub access is requested with a `use` declaration in the
[component manifest](component_manifests.md). This example shows the manifest
file of a component that accesses its own hub.

```
{
    "program": {
        "runner": "elf",
        "binary": "bin/program"
    },
    "use": [
        {
            "directory": "hub",
            "from": "framework",
        },
    ]
}
```

Since the hub is a [directory capability](capabilities/directory.md), with the
right routing it can be exposed upwards, offered downwards or even shared
between sibling components. See the [advanced routing](#advanced-routing)
section for more details.

Once the capability is added to the manifest, a component can access the hub
from `/hub`.

## Structure of the hub

### `/hub`

The directory structure of `/hub` looks like:

```
/hub
├── children
├── deleting
├── exec
├── component_type
├── id
└── url
```

A hub contains all the information pertaining to a component instance and its
realm.

Directories:

-   [`/hub/children`](#hub-children)
-   [`/hub/deleting`](#hub-deleting)
-   [`/hub/exec`](#hub-exec)

Files:

-   `/hub/component_type`: Contains the string `static` or `dynamic`. A
    component is static when its definition exists in the parent’s component
    manifest at compile time. A component is dynamic when the parent defines the
    component at runtime.
-   `/hub/id`: Contains a non-negative integer corresponding to the version of a
    particular instance. A new instance with the same name is a new version and
    will be given a new ID.
-   `/hub/url`: Contains the URL string corresponding to this component. The URL
    is defined by the parent either in its component manifest file or at
    runtime.

### `/hub/children` {#hub-children}

The directory structure of `/hub/children` looks like:

```
/hub/children
├── foo
|   ├── children
|   ├── deleting
|   ├── exec
|   ├── component_type
|   ├── id
|   └── url
├── bar
|   ├── children
|   ├── deleting
|   ├── exec
|   ├── component_type
|   ├── id
|   └── url
└── baz:qux
    ├── children
    ├── deleting
    ├── component_type
    ├── id
    └── url
```

The `/hub/children` directory contains the hubs of all created child instances,
whether they be static, dynamic, running or stopped.

Note: A child does not have access to the hub of its parent unless its parent
offers the hub capability to it. For more information, see
[advanced routing](#advanced-routing).

The hub gives a name to each child’s hub directory based on this format:

-   For static instances, the format is `<instance name>`
-   For dynamic instances, the format is `<collection name>:<instance name>`

### `/hub/deleting` {#hub-deleting}

The directory structure of `/hub/deleting` looks like:

```
/hub/deleting
├── baz:qux:1
|   ├── children
|   ├── deleting
|   ├── component_type
|   ├── id
|   └── url
└── baz:qux:2
    ├── children
    ├── deleting
    ├── exec
    ├── component_type
    ├── id
    └── url
```

Since deletion is not an atomic process, the deleting directory contains
information about children that are in the process of being deleted. When a
child’s deletion is complete, it is removed from this directory and ceases to
exist in its parent’s hub. The full deletion process is explained later in this
document.

The hub gives a name to each child’s hub directory based on this format:

-   For static instances, the format is `<instance name>:<instance id>`
-   For dynamic instances, the format is `<collection name>:<instance
    name>:<instance id>`

Unlike the `/hub/children` directory, the instance ID is a part of the format
for the `/hub/deleting` directory because multiple versions of the same instance
may be in the process of deletion and the instance ID is used to distinguish
between them.

### `/hub/exec` {#hub-exec}

The directory structure of `/hub/exec` looks like:

```
/hub/exec
├── exposed
├── in
├── out
├── runtime
├── used
└── resolved_url
```

The `/hub/exec` directory is only visible when the instance is started since it
contains information about the current execution of the instance.

Directories:

-   [`/hub/exec/exposed`](#hub-exec-exposed)
-   [`/hub/exec/in`](#hub-exec-in)
-   [`/hub/exec/out`](#hub-exec-out)
-   [`/hub/exec/runtime`](#hub-exec-runtime)
-   [`/hub/exec/used`](#hub-exec-used)

Files:

-   `/hub/exec/resolved_url`: The component's resolved URL in text format.

### `/hub/exec/exposed` {#hub-exec-exposed}

The instance's exposed services as listed in its manifest file. A component can
connect directly to these services from the hub by opening the provided path.

### `/hub/exec/in` {#hub-exec-in}

The instance's incoming namespace, as supplied by the component manager. This
contains a listing of services and directories accessible to the given component
instance. A component can open the provided path to connect directly to these
services from the Hub.

### `/hub/exec/out/` {#hub-exec-out}

The instance's outgoing namespace, served by the instance itself. A component
can connect directly to these services from the hub by opening the provided
path.

### `/hub/exec/runtime` {#hub-exec-runtime}

The directory structure of `/hub/exec/runtime` looks like:

```
/hub/exec/runtime
└── elf
    ├── args
    |   ├── 0
    |   └── 1
    ├── process-id
    └── job-id
```

If a component has an [ELF runtime](elf_runner.md), then it will have a
`/hub/exec/runtime/elf` directory.

Directories:

-   `args`: command-line arguments, presented as a series of files from `0`
    onward.

Files:

-   `process-id`: the instance's process id in text format.
-   `job-id`: the instance's job id in text format.

### `/hub/exec/used` {#hub-exec-used}

Lists services used at least once by the instance. A service is considered
"used" when it is requested by the instance and has been routed successfully by
the component manager.

## Advanced Routing {#advanced-routing}

### Offering parent hub

In this example, the parent component instance passes its view of the hub to
`hub_client`, which then maps it as `/parent_hub` in its namespace. `hub_client`
can inspect information about its parent and siblings through `/parent_hub`.

In the parent component manifest:

```
{
    "offer": [
        // Route the root hub to hub_client.
        {
          "directory": "/hub",
          "from": "framework",
          "to": "#hub_client",
        },
        // Route the ELF runner capability to hub_client.
        {
          "runner": "elf",
          "from": "parent",
          "to": "#hub_client",
        },
    ],
    "children": [
        {
            "name": "hub_client",
            "url": "fuchsia-pkg://fuchsia.com/hub_test#meta/hub_client.cm",
            "startup": "eager",
        },
    ],
```

In `hub_client.cml`:

```
{
    "program": {
        "runner": "elf",
        "binary": "bin/hub_client",
    },
    "use": [
        {
          "directory": "/hub",
          "from": "parent",
          "as": "/parent_hub"
        }
    ]
}
```

### Exposing a sibling hub

In this example, `hub_client_sibling` exposes its view of the hub to its parent.
The realm, in turn, offers that view of the hub as `/sibling_hub` to
`hub_client`. `hub_client` maps that view of the hub to its incoming namespace.

In `hub_client_sibling.cml`:

```
{
    "program": {
        "runner": "elf",
        "binary": "bin/hub_client_sibling",
    },
    "expose": [
        {
            "directory": "/hub",
            "from": "framework",
        },
    ],
}
```

In the parent component manifest file:

```
{
    // Route hub_client_sibling's view of the hub to hub_client.
    "offer": [
        {
            "directory": "/hub",
            "from": "#hub_client_sibling",
            "to": "#hub_client",
            "as": "/sibling_hub",
        }
    ],
    "children": [
        {
            "name": "hub_client_sibling",
            "url": "fuchsia-pkg://fuchsia.com/hub_test#meta/hub_client_sibling.cm",
            "startup": "eager",
        },
        {
            "name": "hub_client",
            "url": "fuchsia-pkg://fuchsia.com/hub_test#meta/hub_client.cm",
            "startup": "eager",
        },
    ],
}
```

In hub_client.cml:

```
{
    "program": {
        "runner": "elf",
        "binary": "bin/hub_client",
    },
    "use": [
        {
            "directory": "/sibling_hub", "from": "parent",
        }
    ]
}
```

## Lifecycle of an instance

<!--
TODO: Avoid using `"startup": "eager"` in component manifests.
-->

To illustrate how the hub changes in response to lifecycle events, consider an
example involving 3 components named `A`, `B` and `C`. The manifest of each
component is given below:

-   `A.cmx`

    ```
    {
        "program": {
            "binary": "bin/A"
        },
        "use": [
            {
                "directory": "/hub",
                "from": "framework",
            },
        ]
        "collections": [
            {
                "name": "coll",
                "durability": "transient",
            },
        ]
    }
    ```

-   `B.cmx`

    ```
    {
        "program": {
            "binary": "bin/B"
        },
        "children": [
            {
                "name": "baz",
                "url": "fuchsia-pkg://fuchsia.com/example#meta/C.cm"
                "startup": "eager",
            }
        ]
    }
    ```

-   `C.cmx`

    ```
    // C.cmx
    {
        "program": {
            "binary": "bin/C"
        }
    }
    ```

Also consider 3 instances, one for each component:

-   `A` -> `foo`
-   `B` -> `bar`
-   `C` -> `baz`

`foo` can perform various actions that modify the hub:

-   [View the hub](#view-hub)
-   [Connect to the Realm framework service](#connect-to-realm)
-   [Create a dynamic child](#create-dynamic-child)
-   [Start a dynamic child](#start-dynamic-child)
-   [Destroy a dynamic child](#destroy-dynamic-child)

### View the hub {#view-hub}

`foo` can see its own hub by listing the contents of `/hub`. In Rust, this looks
like:

```
let entries = std::fs::read_dir("/hub")?;
for entry in entries {
    println!("{}", entry?.path());
}
```

Without making any changes to the component hierarchy, the hub will look like
the following:

```
/hub
├── children
├── deleting
├── exec
|    ├── exposed
|    ├── in
|    ├── out
|    ├── runtime
|    └── resolved_url
├── component_type => "static"
├── id => "0"
└── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
```

### Connect to the Realm framework service {#connect-to-realm}

A component must connect to the
[Realm framework service](realms.md#realm-framework-service) to manipulate its
dynamic children. In Rust, this looks like:

```
let realm = connect_to_service::<fsys::RealmMarker>()?;
```

### Create a dynamic child {#create-dynamic-child}

`foo` can define an instance of `B` called `bar` under the
[collection](realms.md#collections) `coll`. In Rust, this looks like:

```
// Define the child and the collection
let mut collection_ref = fsys::CollectionRef { name: String::from("coll") };
let child_decl = fsys::ChildDecl {
    name: Some(String::from("bar")),
    url: Some(String::from("fuchsia-pkg://fuchsia.com/example#meta/B.cm")),
    startup: Some(fsys::StartupMode::Lazy),
};

// Create the child
realm.create_child(&mut collection_ref, child_decl).await??;
```

After executing this code, the hub changes to the following:

```
/hub
├── children
|    └── coll:bar
|         ├── children
|         ├── deleting
|         ├── component_type => "dynamic"
|         ├── id => "1"
|         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
├── deleting
├── exec
|    ├── exposed
|    ├── in
|    ├── out
|    ├── runtime
|    └── resolved_url
├── component_type => "static"
├── id => "0"
└── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
```

Note: The instance `baz` (which is a child of `bar`) does not appear in the hub
yet.

Note: The instance `bar` has not been started, so it does not have an `exec`
directory.

### Start the dynamic child {#start-dynamic-child}

`foo` can now bind to the instance `bar` and start it. In Rust, this looks like
the following:

```
// Create a reference to the dynamic child
let mut child_ref = fsys::ChildRef {
    name: "bar".to_string(),
    collection: Some("coll".to_string()),
};

// Create a proxy for the exposed directory
let (dir_proxy, server_end) = create_proxy::<DirectoryMarker>()?;

// Bind to the child
realm.bind_child(&mut child_ref, server_end).await??;
```

After executing this code, the hub changes to the following:

```
/hub
├── children
|    └── coll:bar
|         ├── children
|         |    └── baz
|         |         ├── children
|         |         ├── deleting
|         |         ├── exec
|         |         |    ├── exposed
|         |         |    ├── in
|         |         |    ├── out
|         |         |    ├── runtime
|         |         |    ├── used
|         |         |    └── resolved_url
|         |         ├── component_type => "static"
|         |         ├── id => "0"
|         |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/C.cm"
|         ├── deleting
|         ├── exec
|         |    ├── exposed
|         |    ├── in
|         |    ├── out
|         |    ├── runtime
|         |    ├── used
|         |    └── resolved_url
|         ├── component_type => "dynamic"
|         ├── id => "1"
|         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
├── deleting
├── exec
|    ├── exposed
|    ├── in
|    ├── out
|    ├── runtime
|    ├── used
|    └── resolved_url
├── component_type => "static"
├── id => "0"
└── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
```

Note: The instance `baz` is automatically created and started as a static child
of `bar`.

Note: The instances `bar` and `baz` now have `exec` directories since they both
have been started.

### Destroy the dynamic child {#destroy-dynamic-child}

`foo` can destroy the instance `bar`. In Rust, this looks like the following:

```
// Create a reference to the dynamic child
let mut child_ref = fsys::ChildRef {
    name: "bar".to_string(),
    collection: Some("coll".to_string()),
};

// Destroy the child
realm.destroy_child(&mut child_ref).await??;
```

The above code begins the deletion process for `bar`. This process has several
stages, most of which occur asynchronously. As a result, the hub’s structure
changes several times.

1.  `bar` is marked for deletion. The hub changes to the following:

    ```
    /hub
    ├── children
    ├── deleting
    |    └── coll:bar:1
    |         ├── children
    |         |    └── baz
    |         |         ├── children
    |         |         ├── deleting
    |         |         ├── exec
    |         |         |    ├── exposed
    |         |         |    ├── in
    |         |         |    ├── out
    |         |         |    ├── runtime
    |         |         |    ├── used
    |         |         |    └── resolved_url
    |         |         ├── component_type => "static"
    |         |         ├── id => "0"
    |         |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/C.cm"
    |         ├── deleting
    |         ├── exec
    |         |    ├── exposed
    |         |    ├── in
    |         |    ├── out
    |         |    ├── runtime
    |         |    ├── used
    |         |    └── resolved_url
    |         ├── component_type => "dynamic"
    |         ├── id => "1"
    |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
    ├── exec
    |    ├── exposed
    |    ├── in
    |    ├── out
    |    ├── runtime
    |    ├── used
    |    └── resolved_url
    ├── component_type => "static"
    ├── id => "0"
    └── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
    ```

1.  `baz` is stopped. The hub changes to the following:

    ```
    /hub
    ├── children
    ├── deleting
    |    └── coll:bar:1
    |         ├── children
    |         |    └── baz
    |         |         ├── children
    |         |         ├── deleting
    |         |         ├── component_type => "static"
    |         |         ├── id => "0"
    |         |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/C.cm"
    |         ├── deleting
    |         ├── exec
    |         |    ├── exposed
    |         |    ├── in
    |         |    ├── out
    |         |    ├── runtime
    |         |    ├── used
    |         |    └── resolved_url
    |         ├── component_type => "dynamic"
    |         ├── id => "1"
    |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
    ├── exec
    |    ├── exposed
    |    ├── in
    |    ├── out
    |    ├── runtime
    |    ├── used
    |    └── resolved_url
    ├── component_type => "static"
    ├── id => "0"
    └── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
    ```

1.  `bar` is stopped. The hub changes to the following:

    ```
    /hub
    ├── children
    ├── deleting
    |    └── coll:bar
    |         ├── children
    |         |    └── baz
    |         |         ├── children
    |         |         ├── deleting
    |         |         ├── component_type => "static"
    |         |         ├── id => "0"
    |         |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/C.cm"
    |         ├── deleting
    |         ├── component_type => "dynamic"
    |         ├── id => "1"
    |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
    ├── exec
    |    ├── exposed
    |    ├── in
    |    ├── out
    |    ├── runtime
    |    ├── used
    |    └── resolved_url
    ├── component_type => "static"
    ├── id => "0"
    └── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
    ```

1.  `baz` is marked for deletion. The hub changes to the following:

    ```
    /hub
    ├── children
    ├── deleting
    |    └── coll:bar:1
    |         ├── children
    |         ├── deleting
    |         |    └── baz:0
    |         |         ├── children
    |         |         ├── deleting
    |         |         ├── component_type => "static"
    |         |         ├── id => "0"
    |         |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/C.cm"
    |         ├── component_type => "dynamic"
    |         ├── id => "1"
    |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
    ├── exec
    |    ├── exposed
    |    ├── in
    |    ├── out
    |    ├── runtime
    |    ├── used
    |    └── resolved_url
    ├── component_type => "static"
    ├── id => "0"
    └── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
    ```

1.  `baz` is deleted. The hub changes to the following:

    ```
    /hub
    ├── children
    ├── deleting
    |    └── coll:bar:1
    |         ├── children
    |         ├── deleting
    |         ├── component_type => "dynamic"
    |         ├── id => "1"
    |         └── url => "fuchsia-pkg://fuchsia.com/example#meta/B.cm"
    ├── exec
    |    ├── exposed
    |    ├── in
    |    ├── out
    |    ├── runtime
    |    ├── used
    |    └── resolved_url
    ├── component_type => "static"
    ├── id => "0"
    └── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
    ```

1.  `bar` is deleted. The hub changes to the following:

    ```
    /hub
    ├── children
    ├── deleting
    ├── exec
    |    ├── exposed
    |    ├── in
    |    ├── out
    |    ├── runtime
    |    ├── used
    |    └── resolved_url
    ├── component_type => "static"
    ├── id => "0"
    └── url => "fuchsia-pkg://fuchsia.com/example#meta/A.cm"
    ```
