// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_diagnostics::{ComponentSelector, Selector, StringSelector, TreeSelector},
    lazy_static::lazy_static,
    proc_macro2::{Punct, Span, TokenStream},
    quote::quote,
    std::collections::HashMap,
    syn::{
        parse::{Parse, ParseStream},
        punctuated::Punctuated,
        spanned::Spanned,
        token::Comma,
        Error, FnArg, Ident, ItemFn, ItemStruct, Lit, LitStr, Pat, PatType, PathSegment, Token,
        Type::Path,
        TypePath,
    },
};

const EXPECTED_SIGNATURE: &str = "ffx_plugin expects at least the command created in the args.rs \
                                  file and will accept FIDL proxies if mapped in the ffx_plugin \
                                  annotation.";

const UNRECOGNIZED_PARAMETER: &str = "If this is a proxy, make sure the parameter's type matches \
the mapping passed into the ffx_plugin attribute.";

const REMOTE_CONTROL_PROXY: &str = "RemoteControlProxy";
const REMOTE_CONTROL_INDEX: usize = 0;

lazy_static! {
    static ref KNOWN_PROXIES: Vec<(&'static str, &'static str, &'static str)> = vec![
        // If this order changes, change the REMOTE_CONTROL_INDEX const.
        ("RemoteControlProxy", "remote_proxy", "remote_factory"),
        ("DaemonProxy", "daemon_proxy", "daemon_factory"),
        ("FastbootProxy", "fastboot_proxy", "fastboot_factory"),
    ];
}

pub fn ffx_command(input: ItemStruct) -> TokenStream {
    let cmd = input.ident.clone();
    quote! {
        #input
        pub type FfxPluginCommand = #cmd;
    }
}

fn qualified_name(path: &syn::Path) -> String {
    path.segments
        .pairs()
        .map(|pair| {
            if let Some(_) = pair.punct() {
                format!("{}::", pair.value().ident.to_string())
            } else {
                // last ident won't have a punctuation
                pair.value().ident.to_string()
            }
        })
        .fold(String::new(), |accum, elem| format!("{}{}", accum, elem))
}

fn generate_fake_test_proxy_method(
    proxy_name: Ident,
    qualified_proxy_type: &syn::Path,
) -> TokenStream {
    let method_name = Ident::new(&format!("setup_fake_{}", proxy_name), Span::call_site());
    // Oneshot method is needed only for the 'component run' unit tests that leaks memory
    // everywhere unless shut down from the server side.
    let oneshot_method_name =
        Ident::new(&format!("setup_oneshot_fake_{}", proxy_name), Span::call_site());
    quote! {
        #[cfg(test)]
        fn #method_name<R:'static>(handle_request: R) -> #qualified_proxy_type
            where R: Fn(fidl::endpoints::Request<<#qualified_proxy_type as fidl::endpoints::Proxy>::Service>) + std::marker::Send
        {
            use futures::TryStreamExt;
            let (proxy, mut stream) =
                fidl::endpoints::create_proxy_and_stream::<<#qualified_proxy_type as fidl::endpoints::Proxy>::Service>().unwrap();
            fuchsia_async::Task::spawn(async move {
                while let Ok(Some(req)) = stream.try_next().await {
                    handle_request(req);
                }
            })
            .detach();
            proxy
        }

        #[cfg(test)]
        fn #oneshot_method_name<R:'static>(handle_request: R) -> #qualified_proxy_type
            where R: Fn(fidl::endpoints::Request<<#qualified_proxy_type as fidl::endpoints::Proxy>::Service>) + std::marker::Send
        {
            use futures::TryStreamExt;
            let (proxy, mut stream) =
                fidl::endpoints::create_proxy_and_stream::<<#qualified_proxy_type as fidl::endpoints::Proxy>::Service>().unwrap();
            fuchsia_async::Task::spawn(async move {
                if let Ok(Some(req)) = stream.try_next().await {
                    handle_request(req);
                }
            })
            .detach();
            proxy
        }
    }
}

struct GeneratedCodeParts {
    preamble: TokenStream,
    args: Punctuated<Ident, Token!(,)>,
    futures: Punctuated<Ident, Token!(,)>,
    proxies_to_generate: Vec<TokenStream>,
    test_fake_methods_to_generate: Vec<TokenStream>,
    cmd: FnArg,
}

fn parse_arguments(
    args: Punctuated<FnArg, Comma>,
    proxies: &ProxyMap,
) -> Result<GeneratedCodeParts, Error> {
    let mut preamble = quote! {};
    let mut inner_args: Punctuated<Ident, Token!(,)> = Punctuated::new();
    let mut futures: Punctuated<Ident, Token!(,)> = Punctuated::new();
    let mut proxies_to_generate = Vec::new();
    let mut test_fake_methods_to_generate = Vec::<TokenStream>::new();
    let mut cmd: Option<FnArg> = None;
    let mut remote_used = false;
    for arg in &args {
        match arg.clone() {
            FnArg::Receiver(_) => {
                return Err(Error::new(
                    arg.span(),
                    "ffx plugin method signature cannot contain self",
                ))
            }
            FnArg::Typed(PatType { ty, pat, .. }) => match ty.as_ref() {
                Path(TypePath { path, .. }) => match path.segments.last() {
                    Some(simple_proxy_type) => {
                        if let Some(GeneratedKnownProxyParts {
                            name,
                            factory,
                            testing,
                            is_remote,
                        }) = generate_known_proxy(simple_proxy_type, &pat, path)
                        {
                            preamble = quote! {
                                #preamble
                                let #name = #factory().await?;
                            };
                            inner_args.push(name);
                            test_fake_methods_to_generate.push(testing);
                            if is_remote {
                                remote_used = true;
                            }
                        } else if let Some(GeneratedMappedProxyParts {
                            name,
                            fut,
                            implementation,
                            testing,
                        }) = generate_mapped_proxy(proxies, &pat, path)
                        {
                            inner_args.push(name);
                            futures.push(fut);
                            proxies_to_generate.push(implementation);
                            test_fake_methods_to_generate.push(testing);
                        } else if let Some(command) = parse_argh_command(&pat) {
                            // This SHOULD be the argh command - and there should only be one.
                            if let Some(_) = cmd {
                                return Err(Error::new(
                                    arg.span(),
                                    format!(
                                        "ffx_plugin could not recognize the parameters: {} \n{}",
                                        command.clone(),
                                        UNRECOGNIZED_PARAMETER
                                    ),
                                ));
                            } else {
                                cmd = Some(arg.clone());
                                inner_args.push(command.clone());
                            }
                        } else {
                            if let Pat::Ident(pat_ident) = pat.as_ref() {
                                return Err(Error::new(
                                    arg.span(),
                                    format!(
                                        "ffx_plugin could not recognize the parameter: {}\n{}",
                                        pat_ident.ident.clone(),
                                        UNRECOGNIZED_PARAMETER
                                    ),
                                ));
                            } else {
                                return Err(Error::new(arg.span(), EXPECTED_SIGNATURE));
                            }
                        }
                    }
                    _ => return Err(Error::new(arg.span(), EXPECTED_SIGNATURE)),
                },
                _ => return Err(Error::new(arg.span(), EXPECTED_SIGNATURE)),
            },
        }
    }

    // Generated proxies need the remote_proxy. If the remote_proxy is not in the argument list and
    // there are proxies to generate, we still need to add it to the preamble.
    if proxies_to_generate.len() > 0 && !remote_used {
        let (_, name, factory) = KNOWN_PROXIES[REMOTE_CONTROL_INDEX];
        let name_ident = Ident::new(name, Span::call_site());
        let factory_ident = Ident::new(factory, Span::call_site());
        preamble = quote! {
            #preamble
            let #name_ident = #factory_ident().await?;
        };
    }

    if let Some(cmd) = cmd {
        Ok(GeneratedCodeParts {
            preamble,
            args: inner_args,
            futures,
            proxies_to_generate,
            test_fake_methods_to_generate,
            cmd,
        })
    } else {
        Err(Error::new(args.span(), EXPECTED_SIGNATURE))
    }
}

fn parse_argh_command(pattern_type: &Box<Pat>) -> Option<Ident> {
    if let Pat::Ident(pat_ident) = pattern_type.as_ref() {
        Some(pat_ident.ident.clone())
    } else {
        None
    }
}

struct GeneratedKnownProxyParts {
    name: Ident,
    factory: Ident,
    testing: TokenStream,
    is_remote: bool,
}

fn generate_known_proxy(
    simple_proxy_type: &PathSegment,
    pattern_type: &Box<Pat>,
    path: &syn::Path,
) -> Option<GeneratedKnownProxyParts> {
    for known_proxy in KNOWN_PROXIES.iter() {
        if simple_proxy_type.ident == Ident::new(known_proxy.0, Span::call_site()) {
            let testing = if let Pat::Ident(pat_ident) = pattern_type.as_ref() {
                generate_fake_test_proxy_method(pat_ident.ident.clone(), path)
            } else {
                quote! {}
            };
            return Some(GeneratedKnownProxyParts {
                name: Ident::new(known_proxy.1, Span::call_site()),
                factory: Ident::new(known_proxy.2, Span::call_site()),
                testing,
                is_remote: known_proxy.0 == REMOTE_CONTROL_PROXY,
            });
        }
    }
    None
}

struct GeneratedMappedProxyParts {
    name: Ident,
    fut: Ident,
    implementation: TokenStream,
    testing: TokenStream,
}

fn generate_mapped_proxy(
    proxies: &ProxyMap,
    pattern_type: &Box<Pat>,
    path: &syn::Path,
) -> Option<GeneratedMappedProxyParts> {
    let qualified_proxy_name = qualified_name(path);
    if let Some(mapping) = proxies.map.get(&qualified_proxy_name) {
        let mapping_lit = LitStr::new(mapping, Span::call_site());
        if let Pat::Ident(pat_ident) = pattern_type.as_ref() {
            let output = pat_ident.ident.clone();
            let output_fut = Ident::new(&format!("{}_fut", output), Span::call_site());
            let server_end = Ident::new(&format!("{}_server_end", output), Span::call_site());
            let selector = Ident::new(&format!("{}_selector", output), Span::call_site());
            let implementation = generate_proxy_from_selector(
                path,
                mapping,
                mapping_lit,
                &output,
                &output_fut,
                server_end,
                selector,
            );
            let testing = generate_fake_test_proxy_method(pat_ident.ident.clone(), path);
            return Some(GeneratedMappedProxyParts {
                name: output,
                fut: output_fut,
                implementation,
                testing,
            });
        }
    }
    None
}

fn generate_proxy_from_selector(
    path: &syn::Path,
    mapping: &String,
    mapping_lit: LitStr,
    output: &Ident,
    output_fut: &Ident,
    server_end: Ident,
    selector: Ident,
) -> TokenStream {
    quote! {
        let (#output, #server_end) =
            fidl::endpoints::create_proxy::<<#path as fidl::endpoints::Proxy>::Service>()?;
        let #selector =
            selectors::parse_selector(#mapping_lit)?;
        let #output_fut;
        {
            // This needs to be a block in order for this `use` to avoid conflicting with a plugins own `use` for this trait.
            use futures::TryFutureExt;
            #output_fut = remote_proxy
            .connect(#selector, #server_end.into_channel())
            .map_ok_or_else(|e| Result::<(), anyhow::Error>::Err(anyhow::anyhow!(e)), |fidl_result| {
                fidl_result
                .map(|_| ())
                .map_err(|e| {
                    match e {
                        fidl_fuchsia_developer_remotecontrol::ConnectError::NoMatchingServices => {
                            ffx_core::ffx_error!(format!(
"The plugin service selector '{}' did not match any services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '<selector>'` to explore the component topology of your target device and fix this selector.",
                            #mapping)).into()
                        }
                        fidl_fuchsia_developer_remotecontrol::ConnectError::MultipleMatchingServices => {
                            ffx_core::ffx_error!(
                                format!(
"Plugin service selectors must match exactly one service, but '{}' matched multiple services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '{}'` to see which services matched the provided selector.",
                                    #mapping, #mapping)).into()
                        }
                        _ => {
                            anyhow::anyhow!(
                                format!("This service dependency exists but connecting to it failed with error {:?}. Selector: {}.", e, #mapping)
                            )
                        }
                    }
                })
            });
        }
    }
}

pub fn ffx_plugin(input: ItemFn, proxies: ProxyMap) -> Result<TokenStream, Error> {
    let method = input.sig.ident.clone();
    let asyncness = if let Some(_) = input.sig.asyncness {
        quote! {.await}
    } else {
        quote! {}
    };

    let GeneratedCodeParts {
        mut preamble,
        args,
        futures,
        proxies_to_generate,
        test_fake_methods_to_generate,
        cmd,
    } = parse_arguments(input.sig.inputs.clone(), &proxies)?;

    let mut outer_args: Punctuated<_, Token!(,)> = Punctuated::new();
    outer_args.push(quote! {daemon_factory: D});
    outer_args.push(quote! {remote_factory: R});
    outer_args.push(quote! {fastboot_factory: F});
    outer_args.push(quote! {is_experiment: E});
    outer_args.push(quote! {#cmd});

    preamble = quote! {
        #preamble
        #(#proxies_to_generate)*
    };

    let implementation = if proxies_to_generate.len() > 0 {
        quote! {
            #preamble
            match futures::try_join!(#futures) {
                Ok(_) => {
                    #method(#args)#asyncness
                },
                Err(e) => {
                    log::error!("There was an error getting proxies from the Remote Control Service: {}", e);
                    anyhow::bail!(e)
                }
            }
        }
    } else {
        quote! {
            #preamble
            #method(#args)#asyncness
        }
    };

    let gated_impl = if let Some(key) = proxies.experiment_key {
        quote! {
            if is_experiment(#key).await {
                #implementation
            } else {
                println!("This is an experimental subcommand.  To enable this subcommand run 'ffx config set {} true'", #key);
                Ok(())
            }
        }
    } else {
        implementation
    };

    Ok(quote! {
        #input
        pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
            #outer_args
        ) -> anyhow::Result<()>
            where
            D: FnOnce() -> DFut,
            DFut: std::future::Future<
                Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
            >,
            R: FnOnce() -> RFut,
            RFut: std::future::Future<
                Output = anyhow::Result<
                    fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                >,
            >,
            E: FnOnce(&'static str) -> EFut,
            EFut: std::future::Future<Output = bool>,
            F: FnOnce() -> FFut,
            FFut: std::future::Future<
                Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
            >,
        {
            #gated_impl
        }

        #(#test_fake_methods_to_generate)*
    })
}

fn cmp_string_selector(selector: &StringSelector, expected: &str) -> bool {
    match selector {
        StringSelector::ExactMatch(s) => s == expected,
        StringSelector::StringPattern(s) => s == expected,
        _ => false,
    }
}

fn is_v1_moniker(span: Span, selector: ComponentSelector) -> Result<bool, Error> {
    let segments = selector
        .moniker_segments
        .as_ref()
        .ok_or(Error::new(span, format!("Got an invalid component selector. {:?}", selector)))?;
    return Ok(segments.len() == 2
        && cmp_string_selector(segments.get(0).unwrap(), "core")
        && cmp_string_selector(segments.get(1).unwrap(), "appmgr"));
}

fn is_expose_dir(span: Span, selector: TreeSelector) -> Result<bool, Error> {
    match selector {
        TreeSelector::SubtreeSelector(ref subtree) => {
            if subtree.node_path.is_empty() {
                return Err(Error::new(
                    span,
                    format!("Got an invalid tree selector. {:?}", selector),
                ));
            }
            Ok(cmp_string_selector(subtree.node_path.get(0).unwrap(), "expose"))
        }
        TreeSelector::PropertySelector(ref selector) => {
            if selector.node_path.is_empty() {
                return Err(Error::new(
                    span,
                    format!("Got an invalid tree selector. {:?}", selector),
                ));
            }
            Ok(cmp_string_selector(selector.node_path.get(0).unwrap(), "expose"))
        }
        _ => Err(Error::new(span, "Compiled with an unexpected TreeSelector variant.")),
    }
}

fn has_wildcard(span: Span, selector: &StringSelector) -> Result<bool, Error> {
    match selector {
        StringSelector::ExactMatch(_) => Ok(false),
        StringSelector::StringPattern(s) => Ok(s.contains("*")),
        _ => Err(Error::new(span, "Compiled with an unexpected StringSelector variant.")),
    }
}

fn any_wildcards(span: Span, selectors: &Vec<StringSelector>) -> Result<bool, Error> {
    for selector in selectors.iter() {
        if has_wildcard(span, selector)? {
            return Ok(true);
        }
    }
    return Ok(false);
}

fn has_wildcards(span: Span, selector: &Selector) -> Result<bool, Error> {
    let moniker = selector.component_selector.as_ref().unwrap();
    if any_wildcards(span, moniker.moniker_segments.as_ref().unwrap())? {
        return Ok(true);
    }

    let tree_selector = selector.tree_selector.as_ref().unwrap();
    match tree_selector {
        TreeSelector::SubtreeSelector(subtree) => Ok(any_wildcards(span, &subtree.node_path)?),
        TreeSelector::PropertySelector(selector) => {
            if any_wildcards(span, &selector.node_path)? {
                Ok(true)
            } else {
                has_wildcard(span, &selector.target_properties)
            }
        }
        _ => Err(Error::new(span, "Compiled with an unexpected TreeSelector variant.")),
    }
}

#[derive(Debug)]
pub struct ProxyMap {
    experiment_key: Option<String>,
    map: HashMap<String, String>,
}

impl Default for ProxyMap {
    fn default() -> Self {
        Self { experiment_key: None, map: HashMap::new() }
    }
}

impl Parse for ProxyMap {
    fn parse(input: ParseStream<'_>) -> Result<Self, Error> {
        let mut experiment_key = None;
        let mut map = HashMap::new();
        while !input.is_empty() {
            if input.peek(Ident) {
                // Dump the next parse since we got it via the peek
                if let Path(TypePath { path, .. }) = input.parse()? {
                    let _: Punct = input.parse()?;
                    if let Lit::Str(selection) = input.parse()? {
                        if input.peek(Token!(,)) {
                            // Parse the trailing comma
                            let _: Punct = input.parse()?;
                        }
                        let parsed_selector = selectors::parse_selector(&selection.value())
                            .map_err(|e| {
                                Error::new(
                                    selection.span(),
                                    format!("Invalid component selector string: {}", e),
                                )
                            })?;

                        if has_wildcards(selection.span(), &parsed_selector)? {
                            return Err(Error::new(selection.span(), format!("Component selectors in plugin definitions cannot use wildcards ('*').")));
                        }
                        let moniker = parsed_selector.component_selector.unwrap();
                        let subdir = parsed_selector.tree_selector.unwrap();
                        if !is_v1_moniker(selection.span(), moniker)?
                            && !is_expose_dir(selection.span(), subdir)?
                        {
                            return Err(Error::new(selection.span(), format!("Selectors for V2 components in plugin definitions must use `expose`, not `out`. See fxbug.dev/60910.")));
                        }
                        map.insert(qualified_name(&path), selection.value());
                    }
                }
            } else if input.peek(Lit) {
                if let Lit::Str(found_key) = input.parse()? {
                    // This must be the experiment key.
                    if let Some(key) = experiment_key {
                        // experiment_key was already found
                        return Err(Error::new(
                            found_key.span(),
                            format!(
                                "Experiment key set twice.  First found: {}, Second found: {}",
                                key,
                                found_key.value()
                            ),
                        ));
                    } else {
                        experiment_key = Some(format!("{}", found_key.value()));
                        if input.peek(Token!(,)) {
                            // Parse the trailing comma
                            let _: Punct = input.parse()?;
                        }
                    }
                }
            } else {
                return Err(Error::new(Span::call_site(), "Invalid plugin inputs"));
            }
        }
        Ok(Self { map, experiment_key })
    }
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use {
        super::*,
        std::default::Default,
        syn::{
            parse::{Parse, ParseStream},
            parse2, parse_quote, ItemType,
        },
    };

    const V1_SELECTOR: &str = "core/appmgr";

    struct WrappedCommand {
        original: ItemStruct,
        plugin: ItemType,
    }

    impl Parse for WrappedCommand {
        fn parse(input: ParseStream<'_>) -> Result<Self, Error> {
            Ok(WrappedCommand { original: input.parse()?, plugin: input.parse()? })
        }
    }

    struct WrappedFunction {
        original: ItemFn,
        plugin: ItemFn,
        fake_tests: Vec<ItemFn>,
    }

    impl Parse for WrappedFunction {
        fn parse(input: ParseStream<'_>) -> Result<Self, Error> {
            let original = input.parse()?;
            let plugin = input.parse()?;
            let mut fake_tests = Vec::new();
            while !input.is_empty() {
                fake_tests.push(input.parse()?);
            }
            Ok(WrappedFunction { original, plugin, fake_tests })
        }
    }

    #[test]
    fn test_ffx_command() -> Result<(), Error> {
        let item: ItemStruct = parse_quote! {pub struct EchoCommand {}};
        let plugin: ItemType = parse_quote! {pub type FfxPluginCommand = EchoCommand;};
        let result: WrappedCommand = parse2(ffx_command(item.clone()))?;
        assert_eq!(item, result.original);
        assert_eq!(plugin, result.plugin);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_just_a_command() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(_cmd: EchoCommand) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                echo(_cmd).await
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(0, result.fake_tests.len());
        Ok(())
    }

    #[test]
    fn test_non_async_ffx_plugin_with_just_a_command() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub fn echo(_cmd: EchoCommand) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                echo(_cmd)
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(0, result.fake_tests.len());
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_a_daemon_proxy_and_command() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                daemon: DaemonProxy,
                _cmd: EchoCommand) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let daemon_proxy = daemon_factory().await?;
                echo(daemon_proxy, _cmd).await
            }
        };
        let fake_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_daemon<R:'static>(handle_request: R) -> DaemonProxy
                where R: Fn(fidl::endpoints::Request<<DaemonProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<DaemonProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let oneshot_fake_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_oneshot_fake_daemon<R:'static>(handle_request: R) -> DaemonProxy
                where R: Fn(fidl::endpoints::Request<<DaemonProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<DaemonProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    if let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(2, result.fake_tests.len());
        assert_eq!(fake_test, result.fake_tests[0]);
        assert_eq!(oneshot_fake_test, result.fake_tests[1]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_a_fastboot_proxy_and_command() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                fastboot: FastbootProxy,
                _cmd: EchoCommand) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let fastboot_proxy = fastboot_factory().await?;
                echo(fastboot_proxy, _cmd).await
            }
        };
        let fake_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_fastboot<R:'static>(handle_request: R) -> FastbootProxy
                where R: Fn(fidl::endpoints::Request<<FastbootProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<FastbootProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let oneshot_fake_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_oneshot_fake_fastboot<R:'static>(handle_request: R) -> FastbootProxy
                where R: Fn(fidl::endpoints::Request<<FastbootProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<FastbootProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    if let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(2, result.fake_tests.len());
        assert_eq!(fake_test, result.fake_tests[0]);
        assert_eq!(oneshot_fake_test, result.fake_tests[1]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_a_remote_proxy_and_command() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                remote: RemoteControlProxy,
                _cmd: EchoCommand) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let remote_proxy = remote_factory().await?;
                echo(remote_proxy, _cmd).await
            }
        };
        let fake_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_remote<R:'static>(handle_request: R) -> RemoteControlProxy
                where R: Fn(fidl::endpoints::Request<<RemoteControlProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<RemoteControlProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin, "{:?}", result.plugin);
        assert_eq!(2, result.fake_tests.len());
        assert_eq!(fake_test, result.fake_tests[0]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_a_remote_proxy_and_daemon_proxy_and_command() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                daemon: DaemonProxy,
                remote: RemoteControlProxy,
                _cmd: EchoCommand) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let daemon_proxy = daemon_factory().await?;
                let remote_proxy = remote_factory().await?;
                echo(daemon_proxy, remote_proxy, _cmd).await
            }
        };
        let fake_daemon_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_daemon<R:'static>(handle_request: R) -> DaemonProxy
                where R: Fn(fidl::endpoints::Request<<DaemonProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<DaemonProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let fake_remote_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_remote<R:'static>(handle_request: R) -> RemoteControlProxy
                where R: Fn(fidl::endpoints::Request<<RemoteControlProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<RemoteControlProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(4, result.fake_tests.len());
        assert_eq!(fake_daemon_test, result.fake_tests[0]);
        assert_eq!(fake_remote_test, result.fake_tests[2]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_a_remote_proxy_and_daemon_proxy_and_command_out_of_order(
    ) -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                remote: RemoteControlProxy,
                _cmd: EchoCommand,
                daemon: DaemonProxy) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                _cmd: EchoCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let remote_proxy = remote_factory().await?;
                let daemon_proxy = daemon_factory().await?;
                echo(remote_proxy, _cmd, daemon_proxy).await
            }
        };
        let fake_daemon_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_daemon<R:'static>(handle_request: R) -> DaemonProxy
                where R: Fn(fidl::endpoints::Request<<DaemonProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<DaemonProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let fake_remote_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_remote<R:'static>(handle_request: R) -> RemoteControlProxy
                where R: Fn(fidl::endpoints::Request<<RemoteControlProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<RemoteControlProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(4, result.fake_tests.len());
        assert_eq!(fake_remote_test, result.fake_tests[0]);
        assert_eq!(fake_daemon_test, result.fake_tests[2]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_proxy_map_and_command() -> Result<(), Error> {
        let mut map = HashMap::new();
        map.insert("TestProxy".to_string(), "test".to_string());
        let proxies = ProxyMap { map, ..Default::default() };
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                test: TestProxy,
                cmd: WhateverCommand,
                ) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                cmd: WhateverCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let remote_proxy = remote_factory().await?;
                let (test, test_server_end) =
                    fidl::endpoints::create_proxy::<<TestProxy as fidl::endpoints::Proxy>::Service>()?;
                let test_selector = selectors::parse_selector("test")?;
                let test_fut;
                {
                    // This needs to be a block in order for this `use` to avoid conflicting with a plugins own `use` for this trait.
                    use futures::TryFutureExt;
                    test_fut = remote_proxy
                    .connect(test_selector, test_server_end.into_channel())
                    .map_ok_or_else(|e| Result::<(), anyhow::Error>::Err(anyhow::anyhow!(e)), |fidl_result| {
                        fidl_result
                        .map(|_| ())
                        .map_err(|e| {
                            match e {
                                fidl_fuchsia_developer_remotecontrol::ConnectError::NoMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"The plugin service selector '{}' did not match any services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '<selector>'` to explore the component topology of your target device and fix this selector.",
                                            "test")).into()
                                }
                                fidl_fuchsia_developer_remotecontrol::ConnectError::MultipleMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"Plugin service selectors must match exactly one service, but '{}' matched multiple services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '{}'` to see which services matched the provided selector.",
                                            "test", "test")).into()
                                }
                                _ => {
                                    anyhow::anyhow!(
                                        format!("This service dependency exists but connecting to it failed with error {:?}. Selector: {}.", e, "test")
                                    )
                                }
                            }
                        })
                    });
                }

                match futures::try_join!(test_fut) {
                    Ok(_) => {
                        echo(test, cmd).await
                    },
                    Err(e) => {
                        log::error!("There was an error getting proxies from the Remote Control Service: {}", e);
                        anyhow::bail!(e)
                    }
                }
            }
        };
        let fake_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_test<R:'static>(handle_request: R) -> TestProxy
                where R: Fn(fidl::endpoints::Request<<TestProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<TestProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(2, result.fake_tests.len());
        assert_eq!(fake_test, result.fake_tests[0]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_multiple_proxy_map_and_command() -> Result<(), Error> {
        let mut map = HashMap::new();
        map.insert("TestProxy".to_string(), "test".to_string());
        map.insert("FooProxy".to_string(), "foo".to_string());
        let proxies = ProxyMap { map, ..Default::default() };
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                foo: FooProxy,
                cmd: WhateverCommand,
                test: TestProxy,
                ) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                cmd: WhateverCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                let remote_proxy = remote_factory().await?;
                let (foo, foo_server_end) =
                    fidl::endpoints::create_proxy::<<FooProxy as fidl::endpoints::Proxy>::Service>()?;
                let foo_selector = selectors::parse_selector("foo")?;
                let foo_fut;
                {
                    // This needs to be a block in order for this `use` to avoid conflicting with a plugins own `use` for this trait.
                    use futures::TryFutureExt;
                    foo_fut = remote_proxy
                    .connect(foo_selector, foo_server_end.into_channel())
                    .map_ok_or_else(|e| Result::<(), anyhow::Error>::Err(anyhow::anyhow!(e)), |fidl_result| {
                        fidl_result
                        .map(|_| ())
                        .map_err(|e| {
                            match e {
                                fidl_fuchsia_developer_remotecontrol::ConnectError::NoMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"The plugin service selector '{}' did not match any services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '<selector>'` to explore the component topology of your target device and fix this selector.",
                                            "foo")).into()
                                }
                                fidl_fuchsia_developer_remotecontrol::ConnectError::MultipleMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"Plugin service selectors must match exactly one service, but '{}' matched multiple services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '{}'` to see which services matched the provided selector.",
                                            "foo", "foo")).into()
                                }
                                _ => {
                                    anyhow::anyhow!(
                                        format!("This service dependency exists but connecting to it failed with error {:?}. Selector: {}.", e, "foo")
                                    )
                                }
                            }
                        })
                    });
                }
                let (test, test_server_end) =
                    fidl::endpoints::create_proxy::<<TestProxy as fidl::endpoints::Proxy>::Service>()?;
                let test_selector = selectors::parse_selector("test")?;
                let test_fut;
                {
                    // This needs to be a block in order for this `use` to avoid conflicting with a plugins own `use` for this trait.
                    use futures::TryFutureExt;
                    test_fut = remote_proxy
                    .connect(test_selector, test_server_end.into_channel())
                    .map_ok_or_else(|e| Result::<(), anyhow::Error>::Err(anyhow::anyhow!(e)), |fidl_result| {
                        fidl_result
                        .map(|_| ())
                        .map_err(|e| {
                            match e {
                                fidl_fuchsia_developer_remotecontrol::ConnectError::NoMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"The plugin service selector '{}' did not match any services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '<selector>'` to explore the component topology of your target device and fix this selector.",
                                            "test")).into()
                                }
                                fidl_fuchsia_developer_remotecontrol::ConnectError::MultipleMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"Plugin service selectors must match exactly one service, but '{}' matched multiple services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '{}'` to see which services matched the provided selector.",
                                            "test", "test")).into()
                                }
                                _ => {
                                    anyhow::anyhow!(
                                        format!("This service dependency exists but connecting to it failed with error {:?}. Selector: {}.", e, "test")
                                    )
                                }
                            }
                        })
                    });
                }
                match futures::try_join!(foo_fut, test_fut) {
                    Ok(_) => {
                        echo(foo, cmd, test).await
                    },
                    Err(e) => {
                        log::error!("There was an error getting proxies from the Remote Control Service: {}", e);
                        anyhow::bail!(e)
                    }
                }
            }
        };
        let fake_foo_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_foo<R:'static>(handle_request: R) -> FooProxy
                where R: Fn(fidl::endpoints::Request<<FooProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<FooProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let fake_test_test: ItemFn = parse_quote! {
            #[cfg(test)]
            fn setup_fake_test<R:'static>(handle_request: R) -> TestProxy
                where R: Fn(fidl::endpoints::Request<<TestProxy as fidl::endpoints::Proxy>::Service>) + std::marker::Send
            {
                use futures::TryStreamExt;
                let (proxy, mut stream) =
                    fidl::endpoints::create_proxy_and_stream::<<TestProxy as fidl::endpoints::Proxy>::Service>().unwrap();
                fuchsia_async::Task::spawn(async move {
                    while let Ok(Some(req)) = stream.try_next().await {
                        handle_request(req);
                    }
                })
                .detach();
                proxy
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        assert_eq!(4, result.fake_tests.len());
        assert_eq!(fake_foo_test, result.fake_tests[0]);
        assert_eq!(fake_test_test, result.fake_tests[2]);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_multiple_proxy_map_and_command_and_experiment_key() -> Result<(), Error>
    {
        let mut map = HashMap::new();
        map.insert("TestProxy".to_string(), "test".to_string());
        map.insert("FooProxy".to_string(), "foo".to_string());
        let experiment_key = Some("foo_key".to_string());
        let proxies = ProxyMap { map, experiment_key };
        let original: ItemFn = parse_quote! {
            pub async fn echo(
                foo: FooProxy,
                cmd: WhateverCommand,
                test: TestProxy,
                ) -> anyhow::Result<()> { Ok(()) }
        };
        let plugin: ItemFn = parse_quote! {
            pub async fn ffx_plugin_impl<D, R, DFut, RFut, E, EFut, F, FFut>(
                daemon_factory: D,
                remote_factory: R,
                fastboot_factory: F,
                is_experiment: E,
                cmd: WhateverCommand
            ) -> anyhow::Result<()>
                where
                D: FnOnce() -> DFut,
                DFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
                >,
                R: FnOnce() -> RFut,
                RFut: std::future::Future<
                    Output = anyhow::Result<
                        fidl_fuchsia_developer_remotecontrol::RemoteControlProxy
                    >,
                >,
                E: FnOnce(&'static str) -> EFut,
                EFut: std::future::Future<Output = bool>,
                F: FnOnce() -> FFut,
                FFut: std::future::Future<
                    Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
                >,
            {
                if is_experiment("foo_key").await {
                    let remote_proxy = remote_factory().await?;
                    let (foo, foo_server_end) =
                        fidl::endpoints::create_proxy::<<FooProxy as fidl::endpoints::Proxy>::Service>()?;
                    let foo_selector = selectors::parse_selector("foo")?;
                let foo_fut;
                {
                    // This needs to be a block in order for this `use` to avoid conflicting with a plugins own `use` for this trait.
                    use futures::TryFutureExt;
                    foo_fut = remote_proxy
                    .connect(foo_selector, foo_server_end.into_channel())
                    .map_ok_or_else(|e| Result::<(), anyhow::Error>::Err(anyhow::anyhow!(e)), |fidl_result| {
                        fidl_result
                        .map(|_| ())
                        .map_err(|e| {
                            match e {
                                fidl_fuchsia_developer_remotecontrol::ConnectError::NoMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"The plugin service selector '{}' did not match any services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '<selector>'` to explore the component topology of your target device and fix this selector.",
                                            "foo")).into()
                                }
                                fidl_fuchsia_developer_remotecontrol::ConnectError::MultipleMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"Plugin service selectors must match exactly one service, but '{}' matched multiple services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '{}'` to see which services matched the provided selector.",
                                            "foo", "foo")).into()
                                }
                                _ => {
                                    anyhow::anyhow!(
                                        format!("This service dependency exists but connecting to it failed with error {:?}. Selector: {}.", e, "foo")
                                    )
                                }
                            }
                        })
                    });
                }
                    let (test, test_server_end) =
                        fidl::endpoints::create_proxy::<<TestProxy as fidl::endpoints::Proxy>::Service>()?;
                    let test_selector = selectors::parse_selector("test")?;
                let test_fut;
                {
                    // This needs to be a block in order for this `use` to avoid conflicting with a plugins own `use` for this trait.
                    use futures::TryFutureExt;
                    test_fut = remote_proxy
                    .connect(test_selector, test_server_end.into_channel())
                    .map_ok_or_else(|e| Result::<(), anyhow::Error>::Err(anyhow::anyhow!(e)), |fidl_result| {
                        fidl_result
                        .map(|_| ())
                        .map_err(|e| {
                            match e {
                                fidl_fuchsia_developer_remotecontrol::ConnectError::NoMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"The plugin service selector '{}' did not match any services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '<selector>'` to explore the component topology of your target device and fix this selector.",
                                            "test")).into()
                                }
                                fidl_fuchsia_developer_remotecontrol::ConnectError::MultipleMatchingServices => {
                                    ffx_core::ffx_error!(
                                        format!(
"Plugin service selectors must match exactly one service, but '{}' matched multiple services on the target.
If you are not developing this plugin, then this is a bug. Please report it at http://fxbug.dev/new?template=ffx+User+Bug.

Plugin developers: you can use `ffx component select '{}'` to see which services matched the provided selector.",
                                            "test", "test")).into()
                                }
                                _ => {
                                    anyhow::anyhow!(
                                        format!("This service dependency exists but connecting to it failed with error {:?}. Selector: {}.", e, "test")
                                    )
                                }
                            }
                        })
                    });
                }
                    match futures::try_join!(foo_fut, test_fut) {
                        Ok(_) => {
                            echo(foo, cmd, test).await
                        },
                        Err(e) => {
                            log::error!("There was an error getting proxies from the Remote Control Service: {}", e);
                            anyhow::bail!(e)
                        }
                    }
                } else {
                    println!("This is an experimental subcommand.  To enable this subcommand run 'ffx config set {} true'", "foo_key");
                    Ok(())
                }
            }
        };
        let result: WrappedFunction = parse2(ffx_plugin(original.clone(), proxies)?)?;
        assert_eq!(original, result.original);
        assert_eq!(plugin, result.plugin);
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_no_parameters_should_err() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo() -> Result<(), Error> { Ok(()) }
        };
        if let Ok(_) = ffx_plugin(original.clone(), proxies) {
            assert!(false, "A method with no parameters should throw an error");
        }
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_self_receiver_should_err() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(self, cmd: EchoCommand) -> Result<(), Error> { Ok(()) }
        };
        if let Ok(_) = ffx_plugin(original.clone(), proxies) {
            assert!(false, "A method with a receiver should throw an error");
        }
        Ok(())
    }

    #[test]
    fn test_ffx_plugin_with_referenced_param_should_err() -> Result<(), Error> {
        let proxies = Default::default();
        let original: ItemFn = parse_quote! {
            pub async fn echo(proxy: &TestProxy, cmd: EchoCommand) -> Result<(), Error> { Ok(()) }
        };
        if let Ok(_) = ffx_plugin(original.clone(), proxies) {
            assert!(false, "A method with references should throw an error");
        }
        Ok(())
    }

    #[test]
    fn test_empty_proxy_map_should_not_err() {
        let _proxy_map: ProxyMap = parse_quote! {};
    }

    #[test]
    fn test_v2_proxy_map_succeeds() {
        let _proxy_map: ProxyMap = parse_quote! {test = "test:expose"};
    }

    #[test]
    fn test_v2_proxy_map_expose_with_service_succeeds() {
        let _proxy_map: ProxyMap = parse_quote! {test = "test:expose:anything"};
    }

    #[test]
    fn test_v1_proxy_map_using_out_succeeds() {
        let _proxy_map: ProxyMap = parse_quote! {test = "core/appmgr:out"};
    }

    #[test]
    fn test_v1_proxy_map_using_out_and_service_succeeds() {
        let _proxy_map: ProxyMap = parse_quote! {test = "core/appmgr:out:anything"};
    }

    fn proxy_map_test_value(test: String) -> (String, String, TokenStream) {
        let test_value = format!("{}:{}", V1_SELECTOR, test);
        let test_ident = Ident::new(&test, Span::call_site());
        let mapping_lit = LitStr::new(&test_value, Span::call_site());
        (test.to_string(), test_value.clone(), quote! {#test_ident = #mapping_lit})
    }

    fn test_populating_proxy_map_times(num_of_mappings: usize) {
        let mut proxy_mapping = quote! {};
        let mut key_values = Vec::<(String, String)>::with_capacity(num_of_mappings);
        for x in 0..num_of_mappings {
            let (test_key, test_value, test_proxy_mapping) =
                proxy_map_test_value(format!("test{}", x));
            key_values.push((test_key, test_value));
            proxy_mapping = quote! {
                #test_proxy_mapping,
                #proxy_mapping
            };
        }
        let proxy_map: ProxyMap = parse_quote! { #proxy_mapping };
        for (key, value) in key_values {
            assert_eq!(proxy_map.map.get(&key), Some(&value));
        }
    }

    #[test]
    fn test_populating_proxy_map() -> Result<(), Error> {
        test_populating_proxy_map_times(20);
        Ok(())
    }

    #[test]
    fn test_populating_proxy_map_without_trailing_comma() -> Result<(), Error> {
        let (test1_key, test1_value, test1_proxy_mapping) =
            proxy_map_test_value("test1".to_string());
        let (test2_key, test2_value, test2_proxy_mapping) =
            proxy_map_test_value("test2".to_string());
        let proxy_map: ProxyMap = parse_quote! {
            #test1_proxy_mapping,
            #test2_proxy_mapping
        };
        assert_eq!(proxy_map.map.get(&test1_key), Some(&test1_value));
        assert_eq!(proxy_map.map.get(&test2_key), Some(&test2_value));
        Ok(())
    }

    #[test]
    fn test_invalid_selection_should_err() {
        let result: Result<ProxyMap, Error> = parse2(quote! {
            test = "test"
        });
        assert!(result.is_err());
    }

    #[test]
    fn test_v2_proxy_map_using_out_fails() {
        let result: Result<ProxyMap, Error> = parse2(quote! {
            test = "test:out"
        });
        assert!(result.is_err());
    }

    #[test]
    fn test_v2_proxy_map_not_using_expose_and_service_fails() {
        let result: Result<ProxyMap, Error> = parse2(quote! {
            test = "test:anything:anything"
        });
        assert!(result.is_err());
    }

    #[test]
    fn test_invalid_mapping_should_err() {
        let result: Result<ProxyMap, Error> = parse2(quote! {
            test, "test"
        });
        assert!(result.is_err());
    }

    #[test]
    fn test_invalid_input_should_err() -> Result<(), Error> {
        let result: Result<ProxyMap, Error> = parse2(quote! {test});
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_experiment_key_is_none_when_empty() -> Result<(), Error> {
        let result: ProxyMap = parse2(quote! {})?;
        assert_eq!(result.experiment_key, None);
        Ok(())
    }

    #[test]
    fn test_experiment_key_literal() -> Result<(), Error> {
        let result: ProxyMap = parse2(quote! {"test"})?;
        assert_eq!(result.experiment_key, Some("test".to_string()));
        Ok(())
    }

    #[test]
    fn test_experiment_key_literal_with_trailing_comma() -> Result<(), Error> {
        let result: ProxyMap = parse2(quote! {"test",})?;
        assert_eq!(result.experiment_key, Some("test".to_string()));
        Ok(())
    }

    #[test]
    fn test_experiment_key_before_mapping() -> Result<(), Error> {
        let (test1_key, test1_value, test1_proxy_mapping) =
            proxy_map_test_value("test1".to_string());
        let (test2_key, test2_value, test2_proxy_mapping) =
            proxy_map_test_value("test2".to_string());
        let ex_key = "test_experimental_key".to_string();
        let proxy_map: ProxyMap = parse_quote! {
            #ex_key,
            #test1_proxy_mapping,
            #test2_proxy_mapping
        };
        assert_eq!(proxy_map.map.get(&test1_key), Some(&test1_value));
        assert_eq!(proxy_map.map.get(&test2_key), Some(&test2_value));
        assert_eq!(proxy_map.experiment_key, Some(ex_key));
        Ok(())
    }

    #[test]
    fn test_experiment_key_after_mapping() -> Result<(), Error> {
        let (test1_key, test1_value, test1_proxy_mapping) =
            proxy_map_test_value("test1".to_string());
        let (test2_key, test2_value, test2_proxy_mapping) =
            proxy_map_test_value("test2".to_string());
        let ex_key = "test_experimental_key".to_string();
        let proxy_map: ProxyMap = parse_quote! {
            #test1_proxy_mapping,
            #test2_proxy_mapping,
            #ex_key,
        };
        assert_eq!(proxy_map.map.get(&test1_key), Some(&test1_value));
        assert_eq!(proxy_map.map.get(&test2_key), Some(&test2_value));
        assert_eq!(proxy_map.experiment_key, Some(ex_key));
        Ok(())
    }

    #[test]
    fn test_experiment_key_in_the_middle_of_the_mapping() -> Result<(), Error> {
        let (test1_key, test1_value, test1_proxy_mapping) =
            proxy_map_test_value("test1".to_string());
        let (test2_key, test2_value, test2_proxy_mapping) =
            proxy_map_test_value("test2".to_string());
        let ex_key = "test_experimental_key".to_string();
        let proxy_map: ProxyMap = parse_quote! {
            #test1_proxy_mapping,
            #ex_key,
            #test2_proxy_mapping,
        };
        assert_eq!(proxy_map.map.get(&test1_key), Some(&test1_value));
        assert_eq!(proxy_map.map.get(&test2_key), Some(&test2_value));
        assert_eq!(proxy_map.experiment_key, Some(ex_key));
        Ok(())
    }

    #[test]
    fn test_multiple_experiment_keys_should_err() -> Result<(), Error> {
        let ex_key = "test_experimental_key".to_string();
        let ex_key_2 = "test_experimental_key_2".to_string();
        let result: Result<ProxyMap, Error> = parse2(quote! {
            #ex_key,
            #ex_key_2,
        });
        assert!(result.is_err());
        Ok(())
    }
}
