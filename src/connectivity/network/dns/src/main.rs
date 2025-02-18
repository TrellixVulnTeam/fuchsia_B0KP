// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    async_trait::async_trait,
    dns::{
        async_resolver::{Resolver, Spawner},
        policy::{ServerConfigSink, ServerConfigSinkError, ServerList},
    },
    fidl_fuchsia_net::{self as fnet, NameLookupRequest, NameLookupRequestStream},
    fidl_fuchsia_net_ext as net_ext,
    fidl_fuchsia_net_name::{LookupAdminRequest, LookupAdminRequestStream},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_inspect, fuchsia_zircon as zx,
    futures::{
        channel::mpsc,
        lock::Mutex,
        task::{Context, Poll},
        FutureExt as _, Sink, SinkExt as _, StreamExt as _, TryFutureExt as _, TryStreamExt as _,
    },
    log::{debug, error, info, warn},
    net_declare::fidl_ip_v6,
    parking_lot::RwLock,
    std::collections::VecDeque,
    std::convert::TryFrom,
    std::net::IpAddr,
    std::pin::Pin,
    std::rc::Rc,
    std::sync::Arc,
    trust_dns_proto::rr::{domain::IntoName, TryParseIp},
    trust_dns_resolver::{
        config::{
            LookupIpStrategy, NameServerConfig, NameServerConfigGroup, Protocol, ResolverConfig,
            ResolverOpts,
        },
        error::{ResolveError, ResolveErrorKind},
        lookup, lookup_ip,
    },
};

struct SharedResolver<T>(RwLock<Rc<T>>);

impl<T> SharedResolver<T> {
    fn new(resolver: T) -> Self {
        SharedResolver(RwLock::new(Rc::new(resolver)))
    }

    fn read(&self) -> Rc<T> {
        self.0.read().clone()
    }

    fn write(&self, other: Rc<T>) {
        *self.0.write() = other;
    }
}

const STAT_WINDOW_DURATION: fuchsia_zircon::Duration = fuchsia_zircon::Duration::from_seconds(60);
const STAT_WINDOW_COUNT: usize = 30;

/// Stats about queries during the last `STAT_WINDOW_COUNT` windows of
/// `STAT_WINDOW_DURATION` time.
///
/// For example, if `STAT_WINDOW_DURATION` == 1 minute, and
/// `STAT_WINDOW_COUNT` == 30, `past_queries` contains information about, at
/// most, 30 one-minute windows of completed queries.
///
/// NB: there is no guarantee that these windows are directly consecutive; only
/// that each window begins at least `STAT_WINDOW_DURATION` after the previous
/// window's start time.
struct QueryStats {
    inner: Mutex<VecDeque<QueryWindow>>,
}

impl QueryStats {
    fn new() -> Self {
        Self { inner: Mutex::new(VecDeque::new()) }
    }

    async fn finish_query(&self, start_time: fasync::Time, error: Option<&ResolveErrorKind>) {
        let now = fasync::Time::now();
        let past_queries = &mut *self.inner.lock().await;

        let current_window = if let Some(window) = past_queries.back_mut() {
            if now - window.start >= STAT_WINDOW_DURATION {
                past_queries.push_back(QueryWindow::new(now));
                if past_queries.len() > STAT_WINDOW_COUNT {
                    // Remove the oldest window of query stats.
                    let _: QueryWindow = past_queries
                        .pop_front()
                        .expect("there should be at least one element in `past_queries`");
                }
                // This is safe because we've just pushed an element to `past_queries`.
                past_queries.back_mut().unwrap()
            } else {
                window
            }
        } else {
            past_queries.push_back(QueryWindow::new(now));
            // This is safe because we've just pushed an element to `past_queries`.
            past_queries.back_mut().unwrap()
        };

        let elapsed_time = now - start_time;
        if let Some(e) = error {
            current_window.fail(elapsed_time, e)
        } else {
            current_window.succeed(elapsed_time)
        }
    }
}

/// Stats about queries that failed due to an internal trust-dns error.
/// These counters map to variants of
/// [`trust_dns_resolver::error::ResolveErrorKind`].
#[derive(Default, Debug, PartialEq)]
struct FailureStats {
    message: u64,
    no_records_found: u64,
    io: u64,
    proto: u64,
    timeout: u64,
}

impl FailureStats {
    fn increment(&mut self, kind: &ResolveErrorKind) {
        let FailureStats { message, no_records_found, io, proto, timeout } = self;
        match kind {
            ResolveErrorKind::Message(error) => {
                let _: &str = error;
                *message += 1
            }
            ResolveErrorKind::Msg(error) => {
                let _: &String = error;
                *message += 1
            }
            ResolveErrorKind::NoRecordsFound { query: _, valid_until: _ } => *no_records_found += 1,
            ResolveErrorKind::Io(error) => {
                let _: &std::io::Error = error;
                *io += 1
            }
            ResolveErrorKind::Proto(error) => {
                let _: &trust_dns_proto::error::ProtoError = error;
                *proto += 1
            }
            ResolveErrorKind::Timeout => *timeout += 1,
        }
    }
}

struct QueryWindow {
    start: fasync::Time,
    success_count: u64,
    failure_count: u64,
    success_elapsed_time: fuchsia_zircon::Duration,
    failure_elapsed_time: fuchsia_zircon::Duration,
    failure_stats: FailureStats,
}

impl QueryWindow {
    fn new(start: fasync::Time) -> Self {
        Self {
            start,
            success_count: 0,
            failure_count: 0,
            success_elapsed_time: fuchsia_zircon::Duration::from_nanos(0),
            failure_elapsed_time: fuchsia_zircon::Duration::from_nanos(0),
            failure_stats: FailureStats::default(),
        }
    }

    fn succeed(&mut self, elapsed_time: fuchsia_zircon::Duration) {
        let QueryWindow {
            success_count,
            success_elapsed_time,
            start: _,
            failure_count: _,
            failure_elapsed_time: _,
            failure_stats: _,
        } = self;
        *success_count += 1;
        *success_elapsed_time += elapsed_time;
    }

    fn fail(&mut self, elapsed_time: fuchsia_zircon::Duration, error: &ResolveErrorKind) {
        let QueryWindow {
            failure_count,
            failure_elapsed_time,
            failure_stats,
            start: _,
            success_count: _,
            success_elapsed_time: _,
        } = self;
        *failure_count += 1;
        *failure_elapsed_time += elapsed_time;
        failure_stats.increment(error)
    }
}

/// `SharedResolverConfigSink` acts as a `Sink` that takes resolver
/// configurations in the form of [`ServerList`]s and applies them to a
/// [`SharedResolver`].
struct SharedResolverConfigSink<'a, T> {
    shared_resolver: &'a SharedResolver<T>,
    staged_change: Option<ServerList>,
    flush_state: Option<futures::future::LocalBoxFuture<'a, ()>>,
}

impl<'a, T: ResolverLookup> SharedResolverConfigSink<'a, T> {
    fn new(shared_resolver: &'a SharedResolver<T>) -> Self {
        Self { shared_resolver, staged_change: None, flush_state: None }
    }

    fn update_resolver(&self, servers: ServerList) -> impl 'a + futures::Future<Output = ()> {
        let mut resolver_opts = ResolverOpts::default();
        resolver_opts.ip_strategy = LookupIpStrategy::Ipv4AndIpv6;

        // We're going to add each server twice, once with protocol UDP and
        // then with protocol TCP.
        let mut name_servers = NameServerConfigGroup::with_capacity(servers.len() * 2);

        name_servers.extend(servers.into_iter().flat_map(|server| {
            let net_ext::SocketAddress(socket_addr) = server.into();
            // Every server config gets UDP and TCP versions with
            // preference for UDP.
            std::iter::once(NameServerConfig {
                socket_addr,
                protocol: Protocol::Udp,
                tls_dns_name: None,
            })
            .chain(std::iter::once(NameServerConfig {
                socket_addr,
                protocol: Protocol::Tcp,
                tls_dns_name: None,
            }))
        }));

        let resolver_ref = self.shared_resolver;

        T::new(ResolverConfig::from_parts(None, Vec::new(), name_servers), resolver_opts)
            .map(move |r| resolver_ref.write(Rc::new(r)))
    }

    fn poll(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), never::Never>> {
        // Finish the last update first.
        if let Some(fut) = self.flush_state.as_mut() {
            match fut.poll_unpin(cx) {
                Poll::Ready(()) => {
                    self.flush_state = None;
                }
                Poll::Pending => return Poll::Pending,
            }
        }

        // Update to the latest state if any.
        if let Some(pending) = self.staged_change.take() {
            self.flush_state = Some(self.update_resolver(pending).boxed_local());
            return self.poll(cx);
        }
        Poll::Ready(Ok(()))
    }
}

impl<'a, T: ResolverLookup> Sink<ServerList> for SharedResolverConfigSink<'a, T> {
    type Error = never::Never;

    fn poll_ready(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Result<(), Self::Error>> {
        // We always allow overwriting the staged_change, configuration will
        // always only be applied on flush. This allows a series of
        // configuration events to be received but consolidation only happens
        // on flush.
        Poll::Ready(Ok(()))
    }

    fn start_send(self: Pin<&mut Self>, item: ServerList) -> Result<(), Self::Error> {
        self.get_mut().staged_change = Some(item);
        Ok(())
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), Self::Error>> {
        self.get_mut().poll(cx)
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), Self::Error>> {
        Self::poll_flush(self, cx)
    }
}

enum IncomingRequest {
    // NameLookup service.
    NameLookup(NameLookupRequestStream),
    // LookupAdmin Service.
    LookupAdmin(LookupAdminRequestStream),
}

#[async_trait]
trait ResolverLookup {
    async fn new(config: ResolverConfig, options: ResolverOpts) -> Self;

    async fn lookup_ip<N: IntoName + TryParseIp + Send>(
        &self,
        host: N,
    ) -> Result<lookup_ip::LookupIp, ResolveError>;

    async fn ipv4_lookup<N: IntoName + Send>(
        &self,
        host: N,
    ) -> Result<lookup::Ipv4Lookup, ResolveError>;

    async fn ipv6_lookup<N: IntoName + Send>(
        &self,
        host: N,
    ) -> Result<lookup::Ipv6Lookup, ResolveError>;

    async fn reverse_lookup(&self, addr: IpAddr) -> Result<lookup::ReverseLookup, ResolveError>;
}

#[async_trait]
impl ResolverLookup for Resolver {
    async fn new(config: ResolverConfig, options: ResolverOpts) -> Self {
        Resolver::new(config, options, Spawner).await.expect("failed to create resolver")
    }

    async fn lookup_ip<N: IntoName + TryParseIp + Send>(
        &self,
        host: N,
    ) -> Result<lookup_ip::LookupIp, ResolveError> {
        self.lookup_ip(host).await
    }

    async fn ipv4_lookup<N: IntoName + Send>(
        &self,
        host: N,
    ) -> Result<lookup::Ipv4Lookup, ResolveError> {
        self.ipv4_lookup(host).await
    }

    async fn ipv6_lookup<N: IntoName + Send>(
        &self,
        host: N,
    ) -> Result<lookup::Ipv6Lookup, ResolveError> {
        self.ipv6_lookup(host).await
    }

    async fn reverse_lookup(&self, addr: IpAddr) -> Result<lookup::ReverseLookup, ResolveError> {
        self.reverse_lookup(addr).await
    }
}

/// Helper function to handle a [`ResolverError`] and convert it into a
/// [`fnet::LookupError`].
///
/// `source` is used for debugging information.
fn handle_err(source: &'static str, err: ResolveError) -> fnet::LookupError {
    use trust_dns_proto::error::ProtoErrorKind;

    let (lookup_err, ioerr) = match err.kind() {
        // The following mapping is based on the analysis of `ResolveError` enumerations.
        // For cases that are not obvious such as `ResolveErrorKind::Msg` and
        // `ResolveErrorKind::Message`, I (chunyingw) did code searches to have more insights.
        // `ResolveErrorKind::Msg`: An error with arbitrary message, it could be ex. "lock was
        // poisoned, this is non-recoverable" and ""DNS Error".
        // `ResolveErrorKind::Message`: An error with arbitrary message, it is mostly returned when
        // there is no name in the input vector to look up with "can not lookup for no names".
        // This is a best-effort mapping.
        ResolveErrorKind::NoRecordsFound { query: _, valid_until: _ } => {
            (fnet::LookupError::NotFound, None)
        }
        ResolveErrorKind::Proto(err) => match err.kind() {
            ProtoErrorKind::DomainNameTooLong(_) | ProtoErrorKind::EdnsNameNotRoot(_) => {
                (fnet::LookupError::InvalidArgs, None)
            }
            ProtoErrorKind::Canceled(_) | ProtoErrorKind::Timeout => {
                (fnet::LookupError::Transient, None)
            }
            ProtoErrorKind::Io(inner) => (fnet::LookupError::Transient, Some(inner)),
            ProtoErrorKind::CharacterDataTooLong { max: _, len: _ }
            | ProtoErrorKind::LabelOverlapsWithOther { label: _, other: _ }
            | ProtoErrorKind::DnsKeyProtocolNot3(_)
            | ProtoErrorKind::IncorrectRDataLengthRead { read: _, len: _ }
            | ProtoErrorKind::LabelBytesTooLong(_)
            | ProtoErrorKind::PointerNotPriorToLabel { idx: _, ptr: _ }
            | ProtoErrorKind::MaxBufferSizeExceeded(_)
            | ProtoErrorKind::Message(_)
            | ProtoErrorKind::Msg(_)
            | ProtoErrorKind::NoError
            | ProtoErrorKind::NotAllRecordsWritten { count: _ }
            | ProtoErrorKind::RrsigsNotPresent { name: _, record_type: _ }
            | ProtoErrorKind::UnknownAlgorithmTypeValue(_)
            | ProtoErrorKind::UnknownDnsClassStr(_)
            | ProtoErrorKind::UnknownDnsClassValue(_)
            | ProtoErrorKind::UnknownRecordTypeStr(_)
            | ProtoErrorKind::UnknownRecordTypeValue(_)
            | ProtoErrorKind::UnrecognizedLabelCode(_)
            | ProtoErrorKind::UnrecognizedNsec3Flags(_)
            | ProtoErrorKind::Poisoned
            | ProtoErrorKind::Ring(_)
            | ProtoErrorKind::SSL(_)
            | ProtoErrorKind::Timer
            | ProtoErrorKind::UrlParsing(_)
            | ProtoErrorKind::Utf8(_) => (fnet::LookupError::InternalError, None),
        },
        ResolveErrorKind::Io(inner) => (fnet::LookupError::Transient, Some(inner)),
        ResolveErrorKind::Timeout => (fnet::LookupError::Transient, None),
        ResolveErrorKind::Msg(_) | ResolveErrorKind::Message(_) => {
            (fnet::LookupError::InternalError, None)
        }
    };

    if let Some(ioerr) = ioerr {
        match ioerr.raw_os_error() {
            Some(libc::EHOSTUNREACH) => debug!("{} error: {}; (IO error {:?})", source, err, ioerr),
            // TODO(fxbug.dev/55621): We should log at WARN below, but trust-dns is
            // erasing raw_os_error for us. Logging to debug for now to reduce
            // log spam.
            _ => debug!("{} error: {}; (IO error {:?})", source, err, ioerr),
        }
    } else {
        warn!("{} error: {}", source, err)
    }

    lookup_err
}

struct LookupMode {
    ipv4_lookup: bool,
    ipv6_lookup: bool,
}

async fn lookup_ip_inner<T: ResolverLookup>(
    caller: &'static str,
    resolver: &SharedResolver<T>,
    stats: Arc<QueryStats>,
    hostname: String,
    LookupMode { ipv4_lookup, ipv6_lookup }: LookupMode,
) -> Result<Vec<fnet::IpAddress>, fnet::LookupError> {
    let start_time = fasync::Time::now();
    let resolver = resolver.read();
    let result: Result<Vec<_>, _> = match (ipv4_lookup, ipv6_lookup) {
        (true, false) => resolver.ipv4_lookup(hostname).await.map(|addrs| {
            addrs.into_iter().map(|addr| net_ext::IpAddress(IpAddr::V4(addr)).into()).collect()
        }),
        (false, true) => resolver.ipv6_lookup(hostname).await.map(|addrs| {
            addrs.into_iter().map(|addr| net_ext::IpAddress(IpAddr::V6(addr)).into()).collect()
        }),
        (true, true) => resolver
            .lookup_ip(hostname)
            .await
            .map(|addrs| addrs.into_iter().map(|addr| net_ext::IpAddress(addr).into()).collect()),
        (false, false) => {
            return Err(fnet::LookupError::InvalidArgs);
        }
    };
    let () = stats.finish_query(start_time, result.as_ref().err().map(|e| e.kind())).await;
    result.map_err(|e| handle_err(caller, e)).and_then(|addrs| {
        if addrs.is_empty() {
            Err(fnet::LookupError::NotFound)
        } else {
            Ok(addrs)
        }
    })
}

async fn handle_lookup_ip<T: ResolverLookup>(
    resolver: &SharedResolver<T>,
    stats: Arc<QueryStats>,
    hostname: String,
    options: fnet::LookupIpOptions,
) -> Result<fnet::IpAddressInfo, fnet::LookupError> {
    let mode = LookupMode {
        ipv4_lookup: options.contains(fnet::LookupIpOptions::V4Addrs),
        ipv6_lookup: options.contains(fnet::LookupIpOptions::V6Addrs),
    };
    let response = lookup_ip_inner("LookupIp", resolver, stats, hostname, mode).await?;
    let mut result =
        fnet::IpAddressInfo { ipv4_addrs: vec![], ipv6_addrs: vec![], canonical_name: None };
    for address in response.into_iter() {
        match address {
            fnet::IpAddress::Ipv4(ipv4) => {
                result.ipv4_addrs.push(ipv4);
            }
            fnet::IpAddress::Ipv6(ipv6) => {
                result.ipv6_addrs.push(ipv6);
            }
        }
    }
    Ok(result)
}

async fn handle_lookup_ip2<T: ResolverLookup>(
    resolver: &SharedResolver<T>,
    stats: Arc<QueryStats>,
    routes: &fidl_fuchsia_net_routes::StateProxy,
    hostname: String,
    options: fnet::LookupIpOptions2,
) -> Result<fnet::LookupResult, fnet::LookupError> {
    let fnet::LookupIpOptions2 { ipv4_lookup, ipv6_lookup, sort_addresses, .. } = options;
    let mode = LookupMode {
        ipv4_lookup: ipv4_lookup.unwrap_or(false),
        ipv6_lookup: ipv6_lookup.unwrap_or(false),
    };
    let addrs = lookup_ip_inner("LookupIp2", resolver, stats, hostname, mode).await?;
    let addrs = if sort_addresses.unwrap_or(false) {
        sort_preferred_addresses(addrs, routes).await?
    } else {
        addrs
    };
    Ok(fnet::LookupResult { addresses: Some(addrs), ..fnet::LookupResult::EMPTY })
}

async fn sort_preferred_addresses(
    mut addrs: Vec<fnet::IpAddress>,
    routes: &fidl_fuchsia_net_routes::StateProxy,
) -> Result<Vec<fnet::IpAddress>, fnet::LookupError> {
    let mut addrs_info = futures::future::try_join_all(
        addrs
            // Drain addresses from addrs, but keep it alive so we don't need to
            // reallocate.
            .drain(..)
            .map(|mut addr| async move {
                let source_addr = match routes.resolve(&mut addr).await? {
                    Ok(fidl_fuchsia_net_routes::Resolved::Direct(
                        fidl_fuchsia_net_routes::Destination { source_address, .. },
                    ))
                    | Ok(fidl_fuchsia_net_routes::Resolved::Gateway(
                        fidl_fuchsia_net_routes::Destination { source_address, .. },
                    )) => source_address,
                    // If resolving routes returns an error treat it as an
                    // unreachable address.
                    Err(e) => {
                        debug!(
                            "fuchsia.net.routes/State.resolve({}) failed {}",
                            fidl_fuchsia_net_ext::IpAddress::from(addr),
                            fuchsia_zircon::Status::from_raw(e)
                        );
                        None
                    }
                };
                Ok((addr, DasCmpInfo::from_addrs(&addr, source_addr.as_ref())))
            }),
    )
    .await
    .map_err(|e: fidl::Error| {
        warn!("fuchsia.net.routes/State.resolve FIDL error {:?}", e);
        fnet::LookupError::InternalError
    })?;
    let () = addrs_info.sort_by(|(_laddr, left), (_raddr, right)| left.cmp(right));
    // Reinsert the addresses in order from addr_info.
    let () = addrs.extend(addrs_info.into_iter().map(|(addr, _)| addr));
    Ok(addrs)
}

#[derive(Debug)]
struct Policy {
    prefix: net_types::ip::Subnet<net_types::ip::Ipv6Addr>,
    precedence: usize,
    label: usize,
}

macro_rules! decl_policy {
    ($ip:tt/$prefix:expr => $precedence:expr, $label:expr) => {
        Policy {
            // Unsafe allows us to declare constant subnets.
            // We make sure no invalid subnets are created in
            // test_valid_policy_table.
            prefix: unsafe {
                net_types::ip::Subnet::new_unchecked(
                    net_types::ip::Ipv6Addr::new(fidl_ip_v6!($ip).addr),
                    $prefix,
                )
            },
            precedence: $precedence,
            label: $label,
        }
    };
}

/// Policy table is defined in RFC 6724, section 2.1
///
/// A more human-readable version:
///
///  Prefix        Precedence Label
///  ::1/128               50     0
///  ::/0                  40     1
///  ::ffff:0:0/96         35     4
///  2002::/16             30     2
///  2001::/32              5     5
///  fc00::/7               3    13
///  ::/96                  1     3
///  fec0::/10              1    11
///  3ffe::/16              1    12
///
/// We willingly left out ::/96, fec0::/10, 3ffe::/16 since those prefix
/// assignments are deprecated.
///
/// The table is sorted by prefix length so longest-prefix match can be easily
/// achieved.
const POLICY_TABLE: [Policy; 6] = [
    decl_policy!("::1"/128 => 50, 0),
    decl_policy!("::ffff:0:0"/96 => 35, 4),
    decl_policy!("2001::"/32 => 5, 5),
    decl_policy!("2002::"/16 => 30, 2),
    decl_policy!("fc00::"/7 => 3, 13),
    decl_policy!("::"/0 => 40, 1),
];

fn policy_lookup(addr: &net_types::ip::Ipv6Addr) -> &'static Policy {
    POLICY_TABLE
        .iter()
        .find(|policy| policy.prefix.contains(addr))
        .expect("policy table MUST contain the all addresses subnet")
}

/// Destination Address selection information.
///
/// `DasCmpInfo` provides an implementation of a subset of Destination Address
/// Selection according to the sorting rules defined in [RFC 6724 Section 6].
///
/// TODO(fxbug.dev/65219): Implement missing rules 3, 4, and 7.
/// Rules 3, 4, and 7 are omitted for compatibility with the equivalent
/// implementation in Fuchsia's libc.
///
/// `DasCmpInfo` provides an [`std::cmp::Ord`] implementation that will return
/// preferred addresses as "lesser" values.
///
/// [RFC 6724 Section 6]: https://tools.ietf.org/html/rfc6724#section-6
#[derive(Debug)]
struct DasCmpInfo {
    usable: bool,
    matching_scope: bool,
    matching_label: bool,
    precedence: usize,
    scope: net_types::ip::Ipv6Scope,
    common_prefix_len: u8,
}

impl DasCmpInfo {
    /// Helper function to convert a FIDL IP address into
    /// [`net_types::ip::Ipv6Addr`], using a mapped IPv4 when that's the case.
    fn convert_addr(fidl: &fnet::IpAddress) -> net_types::ip::Ipv6Addr {
        match fidl {
            fnet::IpAddress::Ipv4(fnet::Ipv4Address { addr }) => {
                net_types::ip::Ipv6Addr::from(net_types::ip::Ipv4Addr::new(*addr))
            }
            fnet::IpAddress::Ipv6(fnet::Ipv6Address { addr }) => {
                net_types::ip::Ipv6Addr::new(*addr)
            }
        }
    }

    fn from_addrs(dst_addr: &fnet::IpAddress, src_addr: Option<&fnet::IpAddress>) -> Self {
        use net_types::ScopeableAddress;

        let dst_addr = Self::convert_addr(dst_addr);
        let Policy { prefix: _, precedence, label: dst_label } = policy_lookup(&dst_addr);
        let (usable, matching_scope, matching_label, common_prefix_len) = match src_addr {
            Some(src_addr) => {
                let src_addr = Self::convert_addr(src_addr);
                let Policy { prefix: _, precedence: _, label: src_label } =
                    policy_lookup(&src_addr);
                (
                    true,
                    dst_addr.scope() == src_addr.scope(),
                    dst_label == src_label,
                    dst_addr.common_prefix_length(&src_addr),
                )
            }
            None => (false, false, false, 0),
        };
        DasCmpInfo {
            usable,
            matching_scope,
            matching_label,
            precedence: *precedence,
            scope: dst_addr.scope(),
            common_prefix_len,
        }
    }
}

impl std::cmp::Ord for DasCmpInfo {
    // TODO(fxbug.dev/65219): Implement missing rules 3, 4, and 7.
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        use std::cmp::Ordering;
        let DasCmpInfo {
            usable: self_usable,
            matching_scope: self_matching_scope,
            matching_label: self_matching_label,
            precedence: self_precedence,
            scope: self_scope,
            common_prefix_len: self_common_prefix_len,
        } = self;
        let DasCmpInfo {
            usable: other_usable,
            matching_scope: other_matching_scope,
            matching_label: other_matching_label,
            precedence: other_precedence,
            scope: other_scope,
            common_prefix_len: other_common_prefix_len,
        } = other;

        fn prefer_true(left: bool, right: bool) -> Ordering {
            match (left, right) {
                (true, false) => Ordering::Less,
                (false, true) => Ordering::Greater,
                (false, false) | (true, true) => Ordering::Equal,
            }
        }

        // Rule 1: Avoid unusable destinations.
        prefer_true(*self_usable, *other_usable)
            .then(
                // Rule 2: Prefer matching scope.
                prefer_true(*self_matching_scope, *other_matching_scope),
            )
            .then(
                // Rule 5: Prefer matching label.
                prefer_true(*self_matching_label, *other_matching_label),
            )
            .then(
                // Rule 6: Prefer higher precedence.
                self_precedence.cmp(other_precedence).reverse(),
            )
            .then(
                // Rule 8: Prefer smaller scope.
                self_scope.cmp(other_scope),
            )
            .then(
                // Rule 9: Use longest matching prefix.
                self_common_prefix_len.cmp(other_common_prefix_len).reverse(),
            )
        // Rule 10: Otherwise, leave the order unchanged.
    }
}

impl std::cmp::PartialOrd for DasCmpInfo {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl std::cmp::PartialEq for DasCmpInfo {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == std::cmp::Ordering::Equal
    }
}

impl std::cmp::Eq for DasCmpInfo {}

async fn handle_lookup_hostname<T: ResolverLookup>(
    resolver: &SharedResolver<T>,
    addr: fnet::IpAddress,
) -> Result<String, fnet::LookupError> {
    let net_ext::IpAddress(addr) = addr.into();
    let resolver = resolver.read();

    match resolver.reverse_lookup(addr).await {
        // TODO(chuningw): Revisit LookupHostname() method of namelookup.fidl.
        Ok(response) => {
            response.iter().next().ok_or(fnet::LookupError::NotFound).map(|h| h.to_string())
        }
        Err(error) => Err(handle_err("LookupHostname", error)),
    }
}

/// IP lookup variants from [`fidl_fuchsia_net::NameLookupRequest`].
enum IpLookupRequest {
    LookupIp {
        hostname: String,
        options: fnet::LookupIpOptions,
        responder: fnet::NameLookupLookupIpResponder,
    },
    LookupIp2 {
        hostname: String,
        options: fnet::LookupIpOptions2,
        responder: fnet::NameLookupLookupIp2Responder,
    },
}

async fn run_name_lookup<T: ResolverLookup>(
    resolver: &SharedResolver<T>,
    stream: NameLookupRequestStream,
    sender: mpsc::Sender<IpLookupRequest>,
) -> Result<(), fidl::Error> {
    let result = stream
        .try_for_each_concurrent(None, |request| async {
            match request {
                NameLookupRequest::LookupIp { hostname, options, responder } => {
                    let () = sender
                        .clone()
                        .send(IpLookupRequest::LookupIp { hostname, options, responder })
                        .await
                        .expect("receiver should not be closed");
                    Ok(())
                }
                NameLookupRequest::LookupIp2 { hostname, options, responder } => {
                    let () = sender
                        .clone()
                        .send(IpLookupRequest::LookupIp2 { hostname, options, responder })
                        .await
                        .expect("receiver should not be closed");
                    Ok(())
                }
                NameLookupRequest::LookupHostname { addr, responder } => {
                    responder.send(&mut handle_lookup_hostname(&resolver, addr).await)
                }
            }
        })
        .await;
    // Some clients will drop the channel when timing out
    // requests. Mute those errors to prevent log spamming.
    if let Err(fidl::Error::ServerResponseWrite(zx::Status::PEER_CLOSED)) = result {
        Ok(())
    } else {
        result
    }
}

const MAX_PARALLEL_REQUESTS: usize = 256;

fn create_ip_lookup_fut<T: ResolverLookup>(
    resolver: &SharedResolver<T>,
    stats: Arc<QueryStats>,
    routes: fidl_fuchsia_net_routes::StateProxy,
    recv: mpsc::Receiver<IpLookupRequest>,
) -> impl futures::Future<Output = ()> + '_ {
    recv.for_each_concurrent(MAX_PARALLEL_REQUESTS, move |request| {
        let stats = stats.clone();
        let routes = routes.clone();
        async move {
            #[derive(Debug)]
            enum IpLookupResult {
                LookupIp(Result<fnet::IpAddressInfo, fnet::LookupError>),
                LookupIp2(Result<fnet::LookupResult, fnet::LookupError>),
            }
            let (lookup_result, send_result) = match request {
                IpLookupRequest::LookupIp { hostname, options, responder } => {
                    let mut lookup_result =
                        handle_lookup_ip(resolver, stats.clone(), hostname, options).await;
                    let send_result = responder.send(&mut lookup_result);
                    (IpLookupResult::LookupIp(lookup_result), send_result)
                }
                IpLookupRequest::LookupIp2 { hostname, options, responder } => {
                    let mut lookup_result =
                        handle_lookup_ip2(resolver, stats.clone(), &routes, hostname, options)
                            .await;
                    let send_result = responder.send(&mut lookup_result);
                    (IpLookupResult::LookupIp2(lookup_result), send_result)
                }
            };
            send_result.unwrap_or_else(|e| match e {
                // Some clients will drop the channel when timing out
                // requests. Mute those errors to prevent log spamming.
                fidl::Error::ServerResponseWrite(zx::Status::PEER_CLOSED) => {}
                e => warn!(
                    "failed to send IP lookup result {:?} due to FIDL error: {}",
                    lookup_result, e
                ),
            })
        }
    })
}

/// The error variants returned by [`run_lookup_admin`].
#[derive(Debug, thiserror::Error)]
enum LookupAdminError {
    #[error("FIDL error: {}.", _0)]
    Fidl(#[from] fidl::Error),
    #[error("Sink send error: {}.", _0)]
    Sink(#[from] futures::channel::mpsc::SendError),
}

/// Serves `stream` and forwards received configurations to `sink`.
async fn run_lookup_admin(
    sink: mpsc::Sender<dns::policy::ServerList>,
    policy_state: Arc<dns::policy::ServerConfigState>,
    stream: LookupAdminRequestStream,
) -> Result<(), LookupAdminError> {
    stream
        .try_filter_map(|req| async {
            match req {
                LookupAdminRequest::SetDnsServers { servers, responder } => {
                    let (mut response, ret) = if servers.iter().any(|s| {
                        // Addresses must not be an unspecified or multicast address.
                        let net_ext::SocketAddress(sockaddr) = From::from(*s);
                        let ip = sockaddr.ip();
                        ip.is_multicast() || ip.is_unspecified()
                    }) {
                        (Err(zx::Status::INVALID_ARGS.into_raw()), None)
                    } else {
                        (Ok(()), Some(servers))
                    };
                    let () = responder.send(&mut response)?;
                    Ok(ret)
                }
                LookupAdminRequest::GetDnsServers { responder } => {
                    let () = responder.send(&mut policy_state.servers().iter_mut())?;
                    Ok(None)
                }
            }
        })
        .map_err(LookupAdminError::Fidl)
        .forward(sink.sink_map_err(LookupAdminError::Sink))
        .await
}

/// Creates a tuple of [`mpsc::Sender`] and [`Future`] handled by
/// [`dns::policy::ServerConfiguration`].
///
/// The returned `sender` is used to publish configuration changes to a `Sink`
/// composed of [`dns::policy::ServerConfigSink`] and
/// [`SharedResolverConfigSink`], which will set the consolidated configuration
/// on `resolver`.
///
/// Configuration changes are only when the returned `Future` is polled.
fn create_policy_fut<T: ResolverLookup>(
    resolver: &SharedResolver<T>,
    config_state: Arc<dns::policy::ServerConfigState>,
) -> (
    mpsc::Sender<dns::policy::ServerList>,
    impl futures::Future<Output = Result<(), anyhow::Error>> + '_,
) {
    // Create configuration channel pair. A small buffer in the channel allows
    // for multiple configurations coming in rapidly to be flushed together.
    let (servers_config_sink, servers_config_source) = mpsc::channel(10);
    let policy = ServerConfigSink::new_with_state(
        SharedResolverConfigSink::new(&resolver)
            .sink_map_err(never::Never::into_any::<anyhow::Error>),
        config_state,
    );
    let policy_fut = servers_config_source.map(Ok).forward(policy).map_err(|e| match e {
        ServerConfigSinkError::InvalidArg => anyhow::anyhow!("Sink error {:?}", e),
        ServerConfigSinkError::SinkError(e) => e.context("Sink error"),
    });
    (servers_config_sink, policy_fut)
}

/// Adds a [`dns::policy::ServerConfigState`] inspection child node to
/// `parent`.
fn add_config_state_inspect(
    parent: &fuchsia_inspect::Node,
    config_state: Arc<dns::policy::ServerConfigState>,
) -> fuchsia_inspect::LazyNode {
    parent.create_lazy_child("servers", move || {
        let config_state = config_state.clone();
        async move {
            let srv = fuchsia_inspect::Inspector::new();
            let server_list = config_state.servers();
            for (i, server) in server_list.into_iter().enumerate() {
                let child = srv.root().create_child(format!("{}", i));
                let net_ext::SocketAddress(addr) = server.into();
                let () = child.record_string("address", format!("{}", addr));
                let () = srv.root().record(child);
            }
            Ok(srv)
        }
        .boxed()
    })
}

/// Adds a [`QueryStats`] inspection child node to `parent`.
fn add_query_stats_inspect(
    parent: &fuchsia_inspect::Node,
    stats: Arc<QueryStats>,
) -> fuchsia_inspect::LazyNode {
    parent.create_lazy_child("query_stats", move || {
        let stats = stats.clone();
        async move {
            let past_queries = &*stats.inner.lock().await;
            let node = fuchsia_inspect::Inspector::new();
            for (
                i,
                QueryWindow {
                    start,
                    success_count,
                    failure_count,
                    success_elapsed_time,
                    failure_elapsed_time,
                    failure_stats,
                },
            ) in past_queries.iter().enumerate()
            {
                let child = node.root().create_child(format!("window {}", i + 1));

                match u64::try_from(start.into_nanos()) {
                    Ok(nanos) => {
                        let () = child.record_uint("start_time_nanos", nanos);
                    },
                    Err(e) => warn!(
                        "error computing `start_time_nanos`: {:?}.into_nanos() from i64 -> u64 failed: {}",
                        start, e
                    ),
                }
                let () = child.record_uint("successful_queries", *success_count);
                let () = child.record_uint("failed_queries", *failure_count);
                let record_average = |name: &str, total: fuchsia_zircon::Duration, count: u64| {
                    // Don't record an average if there are no stats.
                    if count == 0 {
                        return;
                    }
                    match u64::try_from(total.into_micros()) {
                        Ok(micros) => child.record_uint(name, micros / count),
                        Err(e) => warn!(
                            "error computing `{}`: {:?}.into_micros() from i64 -> u64 failed: {}",
                            name, success_elapsed_time, e
                        ),
                    }
                };
                let () = record_average(
                    "average_success_duration_micros",
                    *success_elapsed_time,
                    *success_count,
                );
                let () = record_average(
                    "average_failure_duration_micros",
                    *failure_elapsed_time,
                    *failure_count,
                );
                let FailureStats { message, no_records_found, io, proto, timeout } = failure_stats;
                let errors = child.create_child("errors");
                let () = errors.record_uint("Message", *message);
                let () = errors.record_uint("NoRecordsFound", *no_records_found);
                let () = errors.record_uint("Io", *io);
                let () = errors.record_uint("Proto", *proto);
                let () = errors.record_uint("Timeout", *timeout);
                let () = child.record(errors);

                let () = node.root().record(child);
            }
            Ok(node)
        }
        .boxed()
    })
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // NB: We manually set tags to syslog so logs from trust-dns crates also get
    // the same tags as opposed to only the crate path.
    let () = fuchsia_syslog::init_with_tags(&["dns"]).context("cannot init logger")?;
    info!("starting");

    let mut resolver_opts = ResolverOpts::default();
    // Resolver will query for A and AAAA in parallel for lookup_ip.
    resolver_opts.ip_strategy = LookupIpStrategy::Ipv4AndIpv6;
    let resolver = SharedResolver::new(
        Resolver::new(ResolverConfig::default(), resolver_opts, Spawner)
            .await
            .expect("failed to create resolver"),
    );

    let config_state = Arc::new(dns::policy::ServerConfigState::new());
    let (servers_config_sink, policy_fut) = create_policy_fut(&resolver, config_state.clone());

    let stats = Arc::new(QueryStats::new());

    let mut fs = ServiceFs::new_local();

    let inspector = fuchsia_inspect::component::inspector();
    let _state_inspect_node = add_config_state_inspect(inspector.root(), config_state.clone());
    let _query_stats_inspect_node = add_query_stats_inspect(inspector.root(), stats.clone());
    let () = inspector.serve(&mut fs)?;

    let routes =
        fuchsia_component::client::connect_to_service::<fidl_fuchsia_net_routes::StateMarker>()
            .context("failed to connect to fuchsia.net.routes/State")?;

    fs.dir("svc")
        .add_fidl_service(IncomingRequest::NameLookup)
        .add_fidl_service(IncomingRequest::LookupAdmin);
    fs.take_and_serve_directory_handle()?;

    // Create a channel with buffer size `MAX_PARALLEL_REQUESTS`, which allows
    // request processing to always be fully saturated.
    let (sender, recv) = mpsc::channel(MAX_PARALLEL_REQUESTS);
    let serve_fut = fs
        .for_each_concurrent(None, |incoming_service| async {
            match incoming_service {
                IncomingRequest::LookupAdmin(stream) => {
                    run_lookup_admin(servers_config_sink.clone(), config_state.clone(), stream)
                        .await
                        .unwrap_or_else(|e| match e {
                            LookupAdminError::Fidl(e) => {
                                warn!("run_lookup_admin finished with FIDL error: {}", e)
                            }
                            LookupAdminError::Sink(e) => {
                                error!("run_lookup_admin finished with Sink error: {}", e)
                            }
                        })
                }
                IncomingRequest::NameLookup(stream) => {
                    run_name_lookup(&resolver, stream, sender.clone())
                        .await
                        .unwrap_or_else(|e| warn!("run_name_lookup finished with error: {}", e))
                }
            }
        })
        .map(Ok);
    let ip_lookup_fut = create_ip_lookup_fut(&resolver, stats.clone(), routes, recv).map(Ok);

    let ((), (), ()) = futures::future::try_join3(policy_fut, serve_fut, ip_lookup_fut).await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::{
        net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
        str::FromStr,
        sync::Arc,
    };

    use fidl_fuchsia_net_name as fname;

    use dns::test_util::*;
    use dns::DEFAULT_PORT;
    use fidl_fuchsia_net_ext::IntoExt as _;
    use fuchsia_inspect::{assert_inspect_tree, testing::NonZeroUintProperty, tree_assertion};
    use matches::assert_matches;
    use net_declare::{fidl_ip, fidl_ip_v4, fidl_ip_v6, std_ip, std_ip_v4, std_ip_v6};
    use net_types::ip::Ip as _;
    use pin_utils::pin_mut;
    use trust_dns_proto::{
        op::Query,
        rr::{Name, RData, Record},
    };
    use trust_dns_resolver::{
        lookup::Ipv4Lookup, lookup::Ipv6Lookup, lookup::Lookup, lookup::ReverseLookup,
        lookup_ip::LookupIp,
    };

    use super::*;

    const IPV4_LOOPBACK: fnet::Ipv4Address = fidl_ip_v4!("127.0.0.1");
    const IPV6_LOOPBACK: fnet::Ipv6Address = fidl_ip_v6!("::1");
    const LOCAL_HOST: &str = "localhost.";

    // IPv4 address returned by mock lookup.
    const IPV4_HOST: Ipv4Addr = std_ip_v4!("240.0.0.2");
    // IPv6 address returned by mock lookup.
    const IPV6_HOST: Ipv6Addr = std_ip_v6!("abcd::2");

    // host which has IPv4 address only.
    const REMOTE_IPV4_HOST: &str = "www.foo.com";
    // host which has IPv6 address only.
    const REMOTE_IPV6_HOST: &str = "www.bar.com";
    // host used in reverse_lookup when multiple hostnames are returned.
    const REMOTE_IPV6_HOST_EXTRA: &str = "www.bar2.com";
    // host which has IPv4 and IPv6 address if reset name servers.
    const REMOTE_IPV4_IPV6_HOST: &str = "www.foobar.com";

    async fn setup_namelookup_service() -> (fnet::NameLookupProxy, impl futures::Future<Output = ()>)
    {
        let (name_lookup_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fnet::NameLookupMarker>()
                .expect("failed to create NamelookupProxy");

        let mut resolver_opts = ResolverOpts::default();
        resolver_opts.ip_strategy = LookupIpStrategy::Ipv4AndIpv6;

        let resolver = SharedResolver::new(
            Resolver::new(ResolverConfig::default(), resolver_opts, Spawner)
                .await
                .expect("failed to create resolver"),
        );
        let stats = Arc::new(QueryStats::new());
        let (routes_proxy, routes_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_net_routes::StateMarker>()
                .expect("failed to create routes.StateProxy");
        let routes_fut =
            routes_stream.try_for_each(|req| -> futures::future::Ready<Result<(), fidl::Error>> {
                panic!("Should not call routes/State. Received request {:?}", req)
            });
        let (sender, recv) = mpsc::channel(MAX_PARALLEL_REQUESTS);

        (name_lookup_proxy, async move {
            futures::future::try_join3(
                run_name_lookup(&resolver, stream, sender),
                routes_fut,
                create_ip_lookup_fut(&resolver, stats.clone(), routes_proxy, recv).map(Ok),
            )
            .map(|r| match r {
                Ok(((), (), ())) => (),
                Err(e) => panic!("namelookup service error {:?}", e),
            })
            .await
        })
    }

    async fn check_lookup_ip(
        proxy: &fnet::NameLookupProxy,
        host: &str,
        option: fnet::LookupIpOptions,
        expected: Result<fnet::IpAddressInfo, fnet::LookupError>,
    ) {
        let res = proxy.lookup_ip(host, option).await.expect("failed to lookup ip");
        assert_eq!(res, expected);
    }

    async fn check_lookup_hostname(
        proxy: &fnet::NameLookupProxy,
        mut addr: fnet::IpAddress,
        expected: Result<String, fnet::LookupError>,
    ) {
        let res = proxy.lookup_hostname(&mut addr).await.expect("failed to lookup hostname");
        assert_eq!(res, expected);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookupip_invalid_option() {
        let (proxy, fut) = setup_namelookup_service().await;

        let ((), ()) = futures::future::join(fut, async move {
            // IP Lookup localhost with invalid option.
            let res = proxy
                .lookup_ip(LOCAL_HOST, fnet::LookupIpOptions::CnameLookup)
                .await
                .expect("failed to LookupIp");
            assert_eq!(res, Err(fnet::LookupError::InvalidArgs));
        })
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookupip_localhost() {
        let (proxy, fut) = setup_namelookup_service().await;
        let ((), ()) = futures::future::join(fut, async move {
            // IP Lookup IPv4 and IPv6 for localhost.
            check_lookup_ip(
                &proxy,
                LOCAL_HOST,
                fnet::LookupIpOptions::V4Addrs | fnet::LookupIpOptions::V6Addrs,
                Ok(fnet::IpAddressInfo {
                    ipv4_addrs: vec![IPV4_LOOPBACK],
                    ipv6_addrs: vec![IPV6_LOOPBACK],
                    canonical_name: None,
                }),
            )
            .await;

            // IP Lookup IPv4 only for localhost.
            check_lookup_ip(
                &proxy,
                LOCAL_HOST,
                fnet::LookupIpOptions::V4Addrs,
                Ok(fnet::IpAddressInfo {
                    ipv4_addrs: vec![IPV4_LOOPBACK],
                    ipv6_addrs: vec![],
                    canonical_name: None,
                }),
            )
            .await;

            // IP Lookup IPv6 only for localhost.
            check_lookup_ip(
                &proxy,
                LOCAL_HOST,
                fnet::LookupIpOptions::V6Addrs,
                Ok(fnet::IpAddressInfo {
                    ipv4_addrs: vec![],
                    ipv6_addrs: vec![IPV6_LOOPBACK],
                    canonical_name: None,
                }),
            )
            .await;
        })
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookuphostname_localhost() {
        let (proxy, fut) = setup_namelookup_service().await;
        let ((), ()) = futures::future::join(fut, async move {
            check_lookup_hostname(&proxy, IPV4_LOOPBACK.into_ext(), Ok(String::from(LOCAL_HOST)))
                .await
        })
        .await;
    }

    struct MockResolver {
        config: ResolverConfig,
    }

    impl MockResolver {
        fn ip_lookup<N: IntoName + Send>(&self, host: N) -> Lookup {
            let rdatas = match host.into_name().unwrap().to_utf8().as_str() {
                REMOTE_IPV4_HOST => vec![RData::A(IPV4_HOST)],
                REMOTE_IPV6_HOST => vec![RData::AAAA(IPV6_HOST)],
                REMOTE_IPV4_IPV6_HOST => vec![RData::A(IPV4_HOST), RData::AAAA(IPV6_HOST)],
                _ => vec![],
            };

            let records: Vec<Record> = rdatas
                .into_iter()
                .map(|rdata| {
                    Record::from_rdata(
                        Name::new(),
                        // The following ttl value is taken arbitrarily and does not matter in the
                        // test.
                        60,
                        rdata,
                    )
                })
                .collect();

            Lookup::new_with_max_ttl(Query::default(), Arc::new(records))
        }
    }

    #[async_trait]
    impl ResolverLookup for MockResolver {
        async fn new(config: ResolverConfig, _options: ResolverOpts) -> Self {
            MockResolver { config }
        }

        async fn lookup_ip<N: IntoName + TryParseIp + Send>(
            &self,
            host: N,
        ) -> Result<lookup_ip::LookupIp, ResolveError> {
            Ok(LookupIp::from(self.ip_lookup(host)))
        }

        async fn ipv4_lookup<N: IntoName + Send>(
            &self,
            host: N,
        ) -> Result<lookup::Ipv4Lookup, ResolveError> {
            Ok(Ipv4Lookup::from(self.ip_lookup(host)))
        }

        async fn ipv6_lookup<N: IntoName + Send>(
            &self,
            host: N,
        ) -> Result<lookup::Ipv6Lookup, ResolveError> {
            Ok(Ipv6Lookup::from(self.ip_lookup(host)))
        }

        async fn reverse_lookup(
            &self,
            addr: IpAddr,
        ) -> Result<lookup::ReverseLookup, ResolveError> {
            let lookup = if addr == IPV4_HOST {
                Lookup::from_rdata(
                    Query::default(),
                    RData::PTR(Name::from_str(REMOTE_IPV4_HOST).unwrap()),
                )
            } else if addr == IPV6_HOST {
                Lookup::new_with_max_ttl(
                    Query::default(),
                    Arc::new(vec![
                        Record::from_rdata(
                            Name::new(),
                            60, // The value is taken arbitrarily and does not matter
                            // in the test.
                            RData::PTR(Name::from_str(REMOTE_IPV6_HOST).unwrap()),
                        ),
                        Record::from_rdata(
                            Name::new(),
                            60, // The value is taken arbitrarily and does not matter
                            // in the test.
                            RData::PTR(Name::from_str(REMOTE_IPV6_HOST_EXTRA).unwrap()),
                        ),
                    ]),
                )
            } else {
                Lookup::new_with_max_ttl(Query::default(), Arc::new(vec![]))
            };
            Ok(ReverseLookup::from(lookup))
        }
    }

    struct TestEnvironment {
        shared_resolver: SharedResolver<MockResolver>,
        config_state: Arc<dns::policy::ServerConfigState>,
        stats: Arc<QueryStats>,
    }

    impl TestEnvironment {
        fn new() -> Self {
            Self {
                shared_resolver: SharedResolver::new(MockResolver {
                    config: ResolverConfig::from_parts(
                        None,
                        vec![],
                        // Set name_servers as empty, so it's guaranteed to be different from IPV4_NAMESERVER
                        // and IPV6_NAMESERVER.
                        NameServerConfigGroup::with_capacity(0),
                    ),
                }),
                config_state: Arc::new(dns::policy::ServerConfigState::new()),
                stats: Arc::new(QueryStats::new()),
            }
        }

        async fn run_lookup<F, Fut>(&self, f: F)
        where
            Fut: futures::Future<Output = ()>,
            F: FnOnce(fnet::NameLookupProxy) -> Fut,
        {
            self.run_lookup_with_routes_handler(f, |req| {
                panic!("Should not call routes/State. Received request {:?}", req)
            })
            .await
        }

        async fn run_lookup_with_routes_handler<F, Fut, R>(&self, f: F, handle_routes: R)
        where
            Fut: futures::Future<Output = ()>,
            F: FnOnce(fnet::NameLookupProxy) -> Fut,
            R: Fn(fidl_fuchsia_net_routes::StateRequest),
        {
            let (name_lookup_proxy, name_lookup_stream) =
                fidl::endpoints::create_proxy_and_stream::<fnet::NameLookupMarker>()
                    .expect("failed to create NameLookupProxy");

            let (routes_proxy, routes_stream) =
                fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_net_routes::StateMarker>()
                    .expect("failed to create routes.StateProxy");

            let (sender, recv) = mpsc::channel(MAX_PARALLEL_REQUESTS);
            let ((), (), (), ()) = futures::future::try_join4(
                run_name_lookup(&self.shared_resolver, name_lookup_stream, sender),
                f(name_lookup_proxy).map(Ok),
                routes_stream.try_for_each(|req| futures::future::ok(handle_routes(req))),
                create_ip_lookup_fut(&self.shared_resolver, self.stats.clone(), routes_proxy, recv)
                    .map(Ok),
            )
            .await
            .expect("Error running lookup future");
        }

        async fn run_admin<F, Fut>(&self, f: F)
        where
            Fut: futures::Future<Output = ()>,
            F: FnOnce(fname::LookupAdminProxy) -> Fut,
        {
            let (lookup_admin_proxy, lookup_admin_stream) =
                fidl::endpoints::create_proxy_and_stream::<fname::LookupAdminMarker>()
                    .expect("failed to create AdminResolverProxy");

            let (sink, policy_fut) =
                create_policy_fut(&self.shared_resolver, self.config_state.clone());

            let ((), (), ()) = futures::future::try_join3(
                run_lookup_admin(sink, self.config_state.clone(), lookup_admin_stream)
                    .map_err(anyhow::Error::from),
                policy_fut,
                f(lookup_admin_proxy).map(Ok),
            )
            .await
            .expect("Error running admin future");
        }

        async fn run_config_sink<F, Fut>(&self, f: F)
        where
            Fut: futures::Future<Output = ()>,
            F: FnOnce(mpsc::Sender<dns::policy::ServerList>) -> Fut,
        {
            let (sink, policy_fut) =
                create_policy_fut(&self.shared_resolver, self.config_state.clone());

            let ((), ()) = futures::future::try_join(policy_fut, f(sink).map(Ok))
                .await
                .expect("Error running admin future");
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookupip_remotehost_ipv4() {
        TestEnvironment::new()
            .run_lookup(|proxy| async move {
                // IP Lookup IPv4 and IPv6 for REMOTE_IPV4_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV4_HOST,
                    fnet::LookupIpOptions::V4Addrs | fnet::LookupIpOptions::V6Addrs,
                    Ok(fnet::IpAddressInfo {
                        ipv4_addrs: vec![fnet::Ipv4Address { addr: IPV4_HOST.octets() }],
                        ipv6_addrs: vec![],
                        canonical_name: None,
                    }),
                )
                .await;

                // IP Lookup IPv4 for REMOTE_IPV4_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV4_HOST,
                    fnet::LookupIpOptions::V4Addrs,
                    Ok(fnet::IpAddressInfo {
                        ipv4_addrs: vec![fnet::Ipv4Address { addr: IPV4_HOST.octets() }],
                        ipv6_addrs: vec![],
                        canonical_name: None,
                    }),
                )
                .await;

                // IP Lookup IPv6 for REMOTE_IPV4_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV4_HOST,
                    fnet::LookupIpOptions::V6Addrs,
                    Err(fnet::LookupError::NotFound),
                )
                .await;
            })
            .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookupip_remotehost_ipv6() {
        TestEnvironment::new()
            .run_lookup(|proxy| async move {
                // IP Lookup IPv4 and IPv6 for REMOTE_IPV6_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV6_HOST,
                    fnet::LookupIpOptions::V4Addrs | fnet::LookupIpOptions::V6Addrs,
                    Ok(fnet::IpAddressInfo {
                        ipv4_addrs: vec![],
                        ipv6_addrs: vec![fnet::Ipv6Address { addr: IPV6_HOST.octets() }],
                        canonical_name: None,
                    }),
                )
                .await;

                // IP Lookup IPv4 for REMOTE_IPV6_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV6_HOST,
                    fnet::LookupIpOptions::V4Addrs,
                    Err(fnet::LookupError::NotFound),
                )
                .await;

                // IP Lookup IPv6 for REMOTE_IPV4_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV6_HOST,
                    fnet::LookupIpOptions::V6Addrs,
                    Ok(fnet::IpAddressInfo {
                        ipv4_addrs: vec![],
                        ipv6_addrs: vec![fnet::Ipv6Address { addr: IPV6_HOST.octets() }],
                        canonical_name: None,
                    }),
                )
                .await;
            })
            .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookup_hostname() {
        TestEnvironment::new()
            .run_lookup(|proxy| async move {
                check_lookup_hostname(
                    &proxy,
                    fnet::IpAddress::Ipv4(fnet::Ipv4Address { addr: IPV4_HOST.octets() }),
                    Ok(String::from(REMOTE_IPV4_HOST)),
                )
                .await;
            })
            .await;
    }

    // Multiple hostnames returned from trust-dns* APIs, and only the first one will be returned
    // by the FIDL.
    #[fasync::run_singlethreaded(test)]
    async fn test_lookup_hostname_multi() {
        TestEnvironment::new()
            .run_lookup(|proxy| async move {
                check_lookup_hostname(
                    &proxy,
                    fnet::IpAddress::Ipv6(fnet::Ipv6Address { addr: IPV6_HOST.octets() }),
                    Ok(String::from(REMOTE_IPV6_HOST)),
                )
                .await;
            })
            .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_set_server_names() {
        let env = TestEnvironment::new();

        let to_server_configs = |socket_addr: SocketAddr| -> [NameServerConfig; 2] {
            [
                NameServerConfig { socket_addr, protocol: Protocol::Udp, tls_dns_name: None },
                NameServerConfig { socket_addr, protocol: Protocol::Tcp, tls_dns_name: None },
            ]
        };

        // Assert that mock config has no servers originally.
        assert_eq!(env.shared_resolver.read().config.name_servers().to_vec(), vec![]);

        // Set servers.
        env.run_admin(|proxy| async move {
            let () = proxy
                .set_dns_servers(&mut vec![DHCP_SERVER, NDP_SERVER, DHCPV6_SERVER].iter_mut())
                .await
                .expect("Failed to call SetDnsServers")
                .expect("SetDnsServers error");
        })
        .await;
        assert_eq!(
            env.shared_resolver.read().config.name_servers().to_vec(),
            vec![DHCP_SERVER, NDP_SERVER, DHCPV6_SERVER]
                .into_iter()
                .map(|s| net_ext::SocketAddress::from(s).0)
                .flat_map(|x| to_server_configs(x).to_vec().into_iter())
                .collect::<Vec<_>>()
        );

        // Clear servers.
        env.run_admin(|proxy| async move {
            let () = proxy
                .set_dns_servers(&mut vec![].into_iter())
                .await
                .expect("Failed to call SetDnsServers")
                .expect("SetDnsServers error");
        })
        .await;
        assert_eq!(env.shared_resolver.read().config.name_servers().to_vec(), Vec::new());
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_set_server_names_error() {
        let env = TestEnvironment::new();
        // Assert that mock config has no servers originally.
        assert_eq!(env.shared_resolver.read().config.name_servers().to_vec(), vec![]);

        env.run_admin(|proxy| async move {
            // Attempt to set bad addresses.

            // Multicast not allowed.
            let status = proxy
                .set_dns_servers(
                    &mut vec![fnet::SocketAddress::Ipv4(fnet::Ipv4SocketAddress {
                        address: fnet::Ipv4Address { addr: [224, 0, 0, 1] },
                        port: DEFAULT_PORT,
                    })]
                    .iter_mut(),
                )
                .await
                .expect("Failed to call SetDnsServers")
                .expect_err("SetDnsServers should fail for multicast address");
            assert_eq!(zx::Status::from_raw(status), zx::Status::INVALID_ARGS);

            // Unspecified not allowed.
            let status = proxy
                .set_dns_servers(
                    &mut vec![fnet::SocketAddress::Ipv6(fnet::Ipv6SocketAddress {
                        address: fnet::Ipv6Address { addr: [0; 16] },
                        port: DEFAULT_PORT,
                        zone_index: 0,
                    })]
                    .iter_mut(),
                )
                .await
                .expect("Failed to call SetDnsServers")
                .expect_err("SetDnsServers should fail for unspecified address");
            assert_eq!(zx::Status::from_raw(status), zx::Status::INVALID_ARGS);
        })
        .await;

        // Assert that config didn't change.
        assert_eq!(env.shared_resolver.read().config.name_servers().to_vec(), vec![]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_servers() {
        let env = TestEnvironment::new();
        env.run_config_sink(|mut sink| async move {
            let () = sink
                .send(vec![NDP_SERVER, DHCP_SERVER, DHCPV6_SERVER, STATIC_SERVER])
                .await
                .unwrap();
        })
        .await;

        env.run_admin(|proxy| async move {
            let servers = proxy.get_dns_servers().await.expect("Failed to get DNS servers");
            assert_eq!(servers, vec![NDP_SERVER, DHCP_SERVER, DHCPV6_SERVER, STATIC_SERVER])
        })
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_config_inspect() {
        let env = TestEnvironment::new();
        let inspector = fuchsia_inspect::Inspector::new();
        let _config_state_node =
            add_config_state_inspect(inspector.root(), env.config_state.clone());
        assert_inspect_tree!(inspector, root:{
            servers: {}
        });
        env.run_config_sink(|mut sink| async move {
            let () = sink
                .send(vec![NDP_SERVER, DHCP_SERVER, DHCPV6_SERVER, STATIC_SERVER])
                .await
                .unwrap();
        })
        .await;
        assert_inspect_tree!(inspector, root:{
            servers: {
                "0": {
                    address: "[2001:4860:4860::4444%2]:53",
                },
                "1": {
                    address: "8.8.4.4:53",
                },
                "2": {
                    address: "[2002:4860:4860::4444%3]:53",
                },
                "3": {
                    address: "8.8.8.8:53",
                },
            }
        });
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_query_stats_updated() {
        let env = TestEnvironment::new();
        let inspector = fuchsia_inspect::Inspector::new();
        let _query_stats_inspect_node =
            add_query_stats_inspect(inspector.root(), env.stats.clone());
        assert_inspect_tree!(inspector, root:{
            query_stats: {}
        });

        let () = env
            .run_lookup(|proxy| async move {
                // IP Lookup IPv4 for REMOTE_IPV4_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV4_HOST,
                    fnet::LookupIpOptions::V4Addrs,
                    Ok(fnet::IpAddressInfo {
                        ipv4_addrs: vec![fnet::Ipv4Address { addr: IPV4_HOST.octets() }],
                        ipv6_addrs: vec![],
                        canonical_name: None,
                    }),
                )
                .await;
            })
            .await;
        let () = env
            .run_lookup(|proxy| async move {
                // IP Lookup IPv6 for REMOTE_IPV4_HOST.
                check_lookup_ip(
                    &proxy,
                    REMOTE_IPV4_HOST,
                    fnet::LookupIpOptions::V6Addrs,
                    Err(fnet::LookupError::NotFound),
                )
                .await;
            })
            .await;
        assert_inspect_tree!(inspector, root:{
            query_stats: {
                "window 1": {
                    start_time_nanos: NonZeroUintProperty,
                    successful_queries: 2u64,
                    failed_queries: 0u64,
                    average_success_duration_micros: NonZeroUintProperty,
                    errors: {
                        Message: 0u64,
                        NoRecordsFound: 0u64,
                        Io: 0u64,
                        Proto: 0u64,
                        Timeout: 0u64,
                    },
                },
            }
        });
    }

    fn run_fake_lookup(
        exec: &mut fasync::Executor,
        stats: Arc<QueryStats>,
        error: Option<ResolveErrorKind>,
        delay: fuchsia_zircon::Duration,
    ) {
        let start_time = fasync::Time::now();
        let () = exec.set_fake_time(fasync::Time::after(delay));
        let update_stats = (|| async {
            if let Some(error) = error {
                let () = stats.finish_query(start_time, Some(&error)).await;
            } else {
                let () = stats.finish_query(start_time, None).await;
            }
        })();
        pin_mut!(update_stats);
        assert!(exec.run_until_stalled(&mut update_stats).is_ready());
    }

    #[test]
    fn test_query_stats_inspect_average() {
        let mut exec = fasync::Executor::new_with_fake_time().unwrap();
        const START_NANOS: i64 = 1_234_567;
        let () = exec.set_fake_time(fasync::Time::from_nanos(START_NANOS));

        let env = TestEnvironment::new();
        let inspector = fuchsia_inspect::Inspector::new();
        let _query_stats_inspect_node =
            add_query_stats_inspect(inspector.root(), env.stats.clone());
        const SUCCESSFUL_QUERY_COUNT: u64 = 10;
        const SUCCESSFUL_QUERY_DURATION: fuchsia_zircon::Duration =
            fuchsia_zircon::Duration::from_seconds(30);
        for _ in 0..SUCCESSFUL_QUERY_COUNT / 2 {
            let () = run_fake_lookup(
                &mut exec,
                env.stats.clone(),
                None,
                fuchsia_zircon::Duration::from_nanos(0),
            );
            let () = run_fake_lookup(&mut exec, env.stats.clone(), None, SUCCESSFUL_QUERY_DURATION);
            let () = exec.set_fake_time(fasync::Time::after(
                STAT_WINDOW_DURATION - SUCCESSFUL_QUERY_DURATION,
            ));
        }
        let mut expected = tree_assertion!(query_stats: {});
        for i in 0..SUCCESSFUL_QUERY_COUNT / 2 {
            let name = &format!("window {}", i + 1);
            let child = tree_assertion!(var name: {
                start_time_nanos: u64::try_from(
                    START_NANOS + STAT_WINDOW_DURATION.into_nanos() * i64::try_from(i).unwrap()
                ).unwrap(),
                successful_queries: 2u64,
                failed_queries: 0u64,
                average_success_duration_micros: u64::try_from(
                    SUCCESSFUL_QUERY_DURATION.into_micros()
                ).unwrap() / 2,
                errors: {
                    Message: 0u64,
                    NoRecordsFound: 0u64,
                    Io: 0u64,
                    Proto: 0u64,
                    Timeout: 0u64,
                },
            });
            expected.add_child_assertion(child);
        }
        assert_inspect_tree!(inspector, root: {
            expected,
        });
    }

    #[test]
    fn test_query_stats_inspect_error_counters() {
        let mut exec = fasync::Executor::new_with_fake_time().unwrap();
        const START_NANOS: i64 = 1_234_567;
        let () = exec.set_fake_time(fasync::Time::from_nanos(START_NANOS));

        let env = TestEnvironment::new();
        let inspector = fuchsia_inspect::Inspector::new();
        let _query_stats_inspect_node =
            add_query_stats_inspect(inspector.root(), env.stats.clone());
        const FAILED_QUERY_COUNT: u64 = 10;
        const FAILED_QUERY_DURATION: fuchsia_zircon::Duration =
            fuchsia_zircon::Duration::from_millis(500);
        for _ in 0..FAILED_QUERY_COUNT {
            let () = run_fake_lookup(
                &mut exec,
                env.stats.clone(),
                Some(ResolveErrorKind::Timeout),
                FAILED_QUERY_DURATION,
            );
        }
        assert_inspect_tree!(inspector, root:{
            query_stats: {
                "window 1": {
                    start_time_nanos: u64::try_from(
                        START_NANOS + FAILED_QUERY_DURATION.into_nanos()
                    ).unwrap(),
                    successful_queries: 0u64,
                    failed_queries: FAILED_QUERY_COUNT,
                    average_failure_duration_micros: u64::try_from(
                        FAILED_QUERY_DURATION.into_micros()
                    ).unwrap(),
                    errors: {
                        Message: 0u64,
                        NoRecordsFound: 0u64,
                        Io: 0u64,
                        Proto: 0u64,
                        Timeout: FAILED_QUERY_COUNT,
                    },
                },
            }
        });
    }

    #[test]
    fn test_query_stats_inspect_oldest_stats_erased() {
        let mut exec = fasync::Executor::new_with_fake_time().unwrap();
        const START_NANOS: i64 = 1_234_567;
        let () = exec.set_fake_time(fasync::Time::from_nanos(START_NANOS));

        let env = TestEnvironment::new();
        let inspector = fuchsia_inspect::Inspector::new();
        let _query_stats_inspect_node =
            add_query_stats_inspect(inspector.root(), env.stats.clone());
        const DELAY: fuchsia_zircon::Duration = fuchsia_zircon::Duration::from_millis(100);
        for _ in 0..STAT_WINDOW_COUNT {
            let () = run_fake_lookup(
                &mut exec,
                env.stats.clone(),
                Some(ResolveErrorKind::Timeout),
                DELAY,
            );
            let () = exec.set_fake_time(fasync::Time::after(STAT_WINDOW_DURATION - DELAY));
        }
        for _ in 0..STAT_WINDOW_COUNT {
            let () = run_fake_lookup(&mut exec, env.stats.clone(), None, DELAY);
            let () = exec.set_fake_time(fasync::Time::after(STAT_WINDOW_DURATION - DELAY));
        }
        // All the failed queries should be erased from the stats as they are
        // now out of date.
        let mut expected = tree_assertion!(query_stats: {});
        let start_offset = START_NANOS
            + DELAY.into_nanos()
            + STAT_WINDOW_DURATION.into_nanos() * i64::try_from(STAT_WINDOW_COUNT).unwrap();
        for i in 0..STAT_WINDOW_COUNT {
            let name = &format!("window {}", i + 1);
            let child = tree_assertion!(var name: {
                start_time_nanos: u64::try_from(
                    start_offset + STAT_WINDOW_DURATION.into_nanos() * i64::try_from(i).unwrap()
                ).unwrap(),
                successful_queries: 1u64,
                failed_queries: 0u64,
                average_success_duration_micros: u64::try_from(DELAY.into_micros()).unwrap(),
                errors: {
                    Message: 0u64,
                    NoRecordsFound: 0u64,
                    Io: 0u64,
                    Proto: 0u64,
                    Timeout: 0u64,
                },
            });
            expected.add_child_assertion(child);
        }
        assert_inspect_tree!(inspector, root: {
            expected,
        });
    }

    struct BlockingResolver {}

    #[async_trait]
    impl ResolverLookup for BlockingResolver {
        async fn new(_config: ResolverConfig, _options: ResolverOpts) -> Self {
            BlockingResolver {}
        }

        async fn lookup_ip<N: IntoName + TryParseIp + Send>(
            &self,
            _host: N,
        ) -> Result<lookup_ip::LookupIp, ResolveError> {
            futures::future::pending().await
        }

        async fn ipv4_lookup<N: IntoName + Send>(
            &self,
            _host: N,
        ) -> Result<lookup::Ipv4Lookup, ResolveError> {
            futures::future::pending().await
        }

        async fn ipv6_lookup<N: IntoName + Send>(
            &self,
            _host: N,
        ) -> Result<lookup::Ipv6Lookup, ResolveError> {
            futures::future::pending().await
        }

        async fn reverse_lookup(
            &self,
            _addr: IpAddr,
        ) -> Result<lookup::ReverseLookup, ResolveError> {
            panic!("BlockingResolver does not handle reverse lookup")
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_parallel_query_limit() {
        // Collect requests by setting up a FIDL proxy and stream for the
        // NameLookup protocol, because there isn't a good way to directly
        // construct fake requests to be used for testing.
        let requests = {
            let (name_lookup_proxy, name_lookup_stream) =
                fidl::endpoints::create_proxy_and_stream::<fnet::NameLookupMarker>()
                    .expect("failed to create net.NameLookupProxy");
            const NUM_REQUESTS: usize = MAX_PARALLEL_REQUESTS * 2 + 2;
            for _ in 0..NUM_REQUESTS {
                // Don't await on this future because we are using these
                // requests to collect FIDL responders in order to send test
                // requests later, and will not respond to these requests.
                let _: fidl::client::QueryResponseFut<fnet::NameLookupLookupIpResult> =
                    name_lookup_proxy.lookup_ip(
                        LOCAL_HOST,
                        fnet::LookupIpOptions::V4Addrs | fnet::LookupIpOptions::V6Addrs,
                    );
            }
            // Terminate the stream so its items can be collected below.
            drop(name_lookup_proxy);
            let requests = name_lookup_stream
                .map(|request| match request.expect("channel error") {
                    NameLookupRequest::LookupIp { hostname, options, responder } => {
                        IpLookupRequest::LookupIp { hostname, options, responder }
                    }
                    req => panic!("Expected NameLookupRequest::LookupIp request, found {:?}", req),
                })
                .collect::<Vec<_>>()
                .await;
            assert_eq!(requests.len(), NUM_REQUESTS);
            requests
        };

        let (mut sender, recv) = mpsc::channel(MAX_PARALLEL_REQUESTS);

        // The channel's capacity is equal to buffer + num-senders. Thus the
        // channel has a capacity of `MAX_PARALLEL_REQUESTS` + 1, and the
        // `for_each_concurrent` future has a limit of `MAX_PARALLEL_REQUESTS`,
        // so the sender should be able to queue `MAX_PARALLEL_REQUESTS` * 2 + 1
        // requests before `send` fails.
        const BEFORE_LAST_INDEX: usize = MAX_PARALLEL_REQUESTS * 2;
        const LAST_INDEX: usize = MAX_PARALLEL_REQUESTS * 2 + 1;
        let send_fut = async {
            for (i, req) in requests.into_iter().enumerate() {
                match i {
                    BEFORE_LAST_INDEX => assert_matches!(sender.try_send(req), Ok(())),
                    LAST_INDEX => assert_matches!(sender.try_send(req), Err(e) if e.is_full()),
                    _ => assert_matches!(sender.send(req).await, Ok(())),
                }
            }
        }
        .fuse();
        let recv_fut = {
            let resolver = SharedResolver::new(
                BlockingResolver::new(ResolverConfig::default(), ResolverOpts::default()).await,
            );
            let stats = Arc::new(QueryStats::new());
            let (routes_proxy, _routes_stream) =
                fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_net_routes::StateMarker>()
                    .expect("failed to create routes.StateProxy");
            async move { create_ip_lookup_fut(&resolver, stats.clone(), routes_proxy, recv).await }
                .fuse()
        };
        pin_mut!(send_fut, recv_fut);
        futures::select! {
            () = send_fut => {},
            () = recv_fut => panic!("recv_fut should never complete"),
        };
    }

    #[test]
    fn test_failure_stats() {
        use anyhow::anyhow;
        use trust_dns_proto::{error::ProtoError, op::Query};

        let mut stats = FailureStats::default();
        for (error_kind, expected) in &[
            (ResolveErrorKind::Message("foo"), FailureStats { message: 1, ..Default::default() }),
            (
                ResolveErrorKind::Msg("foo".to_string()),
                FailureStats { message: 2, ..Default::default() },
            ),
            (
                ResolveErrorKind::NoRecordsFound { query: Query::default(), valid_until: None },
                FailureStats { message: 2, no_records_found: 1, ..Default::default() },
            ),
            (
                ResolveErrorKind::Io(std::io::Error::new(
                    std::io::ErrorKind::NotFound,
                    anyhow!("foo"),
                )),
                FailureStats { message: 2, no_records_found: 1, io: 1, ..Default::default() },
            ),
            (
                ResolveErrorKind::Proto(ProtoError::from("foo")),
                FailureStats {
                    message: 2,
                    no_records_found: 1,
                    io: 1,
                    proto: 1,
                    ..Default::default()
                },
            ),
            (
                ResolveErrorKind::Timeout,
                FailureStats { message: 2, no_records_found: 1, io: 1, proto: 1, timeout: 1 },
            ),
        ][..]
        {
            let () = stats.increment(error_kind);
            assert_eq!(&stats, expected, "invalid stats after incrementing with {:?}", error_kind);
        }
    }

    fn test_das_helper(
        l_addr: fnet::IpAddress,
        l_src: Option<fnet::IpAddress>,
        r_addr: fnet::IpAddress,
        r_src: Option<fnet::IpAddress>,
        want: std::cmp::Ordering,
    ) {
        let left = DasCmpInfo::from_addrs(&l_addr, l_src.as_ref());
        let right = DasCmpInfo::from_addrs(&r_addr, r_src.as_ref());
        assert_eq!(
            left.cmp(&right),
            want,
            "want = {:?}\n left = {:?}({:?}) DAS={:?}\n right = {:?}({:?}) DAS={:?}",
            want,
            l_addr,
            l_src,
            left,
            r_addr,
            r_src,
            right
        );
    }

    macro_rules! add_das_test {
        ($name:ident, preferred: $pref_dst:expr => $pref_src:expr, other: $other_dst:expr => $other_src:expr) => {
            #[test]
            fn $name() {
                test_das_helper(
                    $pref_dst,
                    $pref_src,
                    $other_dst,
                    $other_src,
                    std::cmp::Ordering::Less,
                )
            }
        };
    }

    add_das_test!(
        prefer_reachable,
        preferred: fidl_ip!("198.51.100.121") => Some(fidl_ip!("198.51.100.117")),
        other: fidl_ip!("2001:db8:1::1") => Option::<fnet::IpAddress>::None
    );

    // These test cases are taken from RFC 6724, section 10.2.

    add_das_test!(
        prefer_matching_scope,
        preferred: fidl_ip!("198.51.100.121") => Some(fidl_ip!("198.51.100.117")),
        other: fidl_ip!("2001:db8:1::1") => Some(fidl_ip!("fe80::1"))
    );

    add_das_test!(
        prefer_matching_label,
        preferred: fidl_ip!("2002:c633:6401::1") => Some(fidl_ip!("2002:c633:6401::2")),
        other:  fidl_ip!("2001:db8:1::1") => Some(fidl_ip!("2002:c633:6401::2"))
    );

    add_das_test!(
        prefer_higher_precedence_1,
        preferred: fidl_ip!("2001:db8:1::1") => Some(fidl_ip!("2001:db8:1::2")),
        other: fidl_ip!("10.1.2.3") => Some(fidl_ip!("10.1.2.4"))
    );

    add_das_test!(
        prefer_higher_precedence_2,
        preferred: fidl_ip!("2001:db8:1::1") => Some(fidl_ip!("2001:db8:1::2")),
        other: fidl_ip!("2002:c633:6401::1") => Some(fidl_ip!("2002:c633:6401::2"))
    );

    add_das_test!(
        prefer_smaller_scope,
        preferred: fidl_ip!("fe80::1") => Some(fidl_ip!("fe80::2")),
        other: fidl_ip!("2001:db8:1::1") => Some(fidl_ip!("2001:db8:1::2"))
    );

    add_das_test!(
        prefer_longest_matching_prefix,
        preferred: fidl_ip!("2001:db8:1::1") => Some(fidl_ip!("2001:db8:1::2")),
        other: fidl_ip!("2001:db8:3ffe::1") => Some(fidl_ip!("2001:db8:3f44::2"))
    );

    #[test]
    fn test_das_equals() {
        for (dst, src) in [
            (fidl_ip!("192.168.0.1"), fidl_ip!("192.168.0.2")),
            (fidl_ip!("2001:db8::1"), fidl_ip!("2001:db8::2")),
        ]
        .iter()
        {
            let () = test_das_helper(*dst, None, *dst, None, std::cmp::Ordering::Equal);
            let () = test_das_helper(*dst, Some(*src), *dst, Some(*src), std::cmp::Ordering::Equal);
        }
    }

    #[test]
    fn test_valid_policy_table() {
        // Last element in policy table MUST be ::/0.
        assert_eq!(
            POLICY_TABLE.iter().last().expect("empty policy table").prefix,
            net_types::ip::Subnet::new(net_types::ip::Ipv6::UNSPECIFIED_ADDRESS, 0)
                .expect("invalid subnet")
        );
        // Policy table must be sorted by prefix length.
        let () = POLICY_TABLE.windows(2).for_each(|w| {
            let Policy { prefix: cur, precedence: _, label: _ } = w[0];
            let Policy { prefix: nxt, precedence: _, label: _ } = w[1];
            assert!(
                cur.prefix() >= nxt.prefix(),
                "bad ordering of prefixes, {} must come after {}",
                cur,
                nxt
            )
        });
        // Assert that POLICY_TABLE declaration does not use any invalid
        // subnets.
        for policy in POLICY_TABLE.iter() {
            assert!(policy.prefix.prefix() <= 128, "Invalid subnet in policy {:?}", policy);
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_sort_preferred_addresses() {
        const TEST_IPS: [(fnet::IpAddress, Option<fnet::IpAddress>); 5] = [
            (fidl_ip!("127.0.0.1"), Some(fidl_ip!("127.0.0.1"))),
            (fidl_ip!("::1"), Some(fidl_ip!("::1"))),
            (fidl_ip!("192.168.50.22"), None),
            (fidl_ip!("2001::2"), None),
            (fidl_ip!("2001:db8:1::1"), Some(fidl_ip!("2001:db8:1::2"))),
        ];
        // Declared using std types so we get cleaner output when we assert
        // expectations.
        const SORTED: [IpAddr; 5] = [
            std_ip!("::1"),
            std_ip!("2001:db8:1::1"),
            std_ip!("127.0.0.1"),
            std_ip!("192.168.50.22"),
            std_ip!("2001::2"),
        ];
        let (routes_proxy, routes_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_net_routes::StateMarker>()
                .expect("failed to create routes.StateProxy");
        let routes_fut = routes_stream.map(|r| r.context("stream FIDL error")).try_for_each(
            |fidl_fuchsia_net_routes::StateRequest::Resolve { destination, responder }| {
                let mut result = TEST_IPS
                    .iter()
                    .enumerate()
                    .find_map(|(i, (dst, src))| {
                        if *dst == destination && src.is_some() {
                            let inner = fidl_fuchsia_net_routes::Destination {
                                address: Some(*dst),
                                source_address: *src,
                                ..fidl_fuchsia_net_routes::Destination::EMPTY
                            };
                            // Send both Direct and Gateway resolved routes to show we
                            // don't care about that part.
                            if i % 2 == 0 {
                                Some(fidl_fuchsia_net_routes::Resolved::Direct(inner))
                            } else {
                                Some(fidl_fuchsia_net_routes::Resolved::Gateway(inner))
                            }
                        } else {
                            None
                        }
                    })
                    .ok_or(fuchsia_zircon::Status::ADDRESS_UNREACHABLE.into_raw());
                futures::future::ready(
                    responder.send(&mut result).context("failed to send Resolve response"),
                )
            },
        );

        let ((), ()) = futures::future::try_join(routes_fut, async move {
            let addrs = TEST_IPS.iter().map(|(dst, _src)| *dst).collect();
            let addrs = sort_preferred_addresses(addrs, &routes_proxy)
                .await
                .expect("failed to sort addresses");
            let addrs = addrs
                .into_iter()
                .map(|a| {
                    let fidl_fuchsia_net_ext::IpAddress(a) = a.into();
                    a
                })
                .collect::<Vec<_>>();
            assert_eq!(&addrs[..], &SORTED[..]);
            Ok(())
        })
        .await
        .expect("error running futures");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_lookupip2() {
        fn map_ip<T: Into<IpAddr>>(addr: T) -> fnet::IpAddress {
            fidl_fuchsia_net_ext::IpAddress(addr.into()).into()
        }
        // Routes handler will say that only IPV6_HOST is reachable.
        let routes_handler =
            |fidl_fuchsia_net_routes::StateRequest::Resolve { destination, responder }| {
                let mut response = if destination == map_ip(IPV6_HOST) {
                    Ok(fidl_fuchsia_net_routes::Resolved::Direct(
                        fidl_fuchsia_net_routes::Destination {
                            address: Some(destination),
                            source_address: Some(destination),
                            ..fidl_fuchsia_net_routes::Destination::EMPTY
                        },
                    ))
                } else {
                    Err(fuchsia_zircon::Status::ADDRESS_UNREACHABLE.into_raw())
                };
                let () =
                    responder.send(&mut response).expect("failed to send Resolve FIDL response");
            };
        TestEnvironment::new()
            .run_lookup_with_routes_handler(
                |proxy| async move {
                    let proxy = &proxy;
                    let lookup_ip = |hostname, options| async move {
                        proxy.lookup_ip2(hostname, options).await.expect("FIDL error")
                    };

                    // All arguments unset.
                    assert_eq!(
                        lookup_ip(
                            REMOTE_IPV4_HOST,
                            fidl_fuchsia_net::LookupIpOptions2 {
                                ..fidl_fuchsia_net::LookupIpOptions2::EMPTY
                            }
                        )
                        .await,
                        Err(fidl_fuchsia_net::LookupError::InvalidArgs)
                    );
                    // No IP addresses to look.
                    assert_eq!(
                        lookup_ip(
                            REMOTE_IPV4_HOST,
                            fidl_fuchsia_net::LookupIpOptions2 {
                                ipv4_lookup: Some(false),
                                ipv6_lookup: Some(false),
                                ..fidl_fuchsia_net::LookupIpOptions2::EMPTY
                            }
                        )
                        .await,
                        Err(fidl_fuchsia_net::LookupError::InvalidArgs)
                    );
                    // No results for an IPv4 only host.
                    assert_eq!(
                        lookup_ip(
                            REMOTE_IPV4_HOST,
                            fidl_fuchsia_net::LookupIpOptions2 {
                                ipv4_lookup: Some(false),
                                ipv6_lookup: Some(true),
                                ..fidl_fuchsia_net::LookupIpOptions2::EMPTY
                            }
                        )
                        .await,
                        Err(fidl_fuchsia_net::LookupError::NotFound)
                    );
                    // Successfully resolve IPv4.
                    assert_eq!(
                        lookup_ip(
                            REMOTE_IPV4_HOST,
                            fidl_fuchsia_net::LookupIpOptions2 {
                                ipv4_lookup: Some(true),
                                ipv6_lookup: Some(true),
                                ..fidl_fuchsia_net::LookupIpOptions2::EMPTY
                            }
                        )
                        .await,
                        Ok(fidl_fuchsia_net::LookupResult {
                            addresses: Some(vec![map_ip(IPV4_HOST)]),
                            ..fidl_fuchsia_net::LookupResult::EMPTY
                        })
                    );
                    // Successfully resolve IPv4 + IPv6 (no sorting).
                    assert_eq!(
                        lookup_ip(
                            REMOTE_IPV4_IPV6_HOST,
                            fidl_fuchsia_net::LookupIpOptions2 {
                                ipv4_lookup: Some(true),
                                ipv6_lookup: Some(true),
                                ..fidl_fuchsia_net::LookupIpOptions2::EMPTY
                            }
                        )
                        .await,
                        Ok(fidl_fuchsia_net::LookupResult {
                            addresses: Some(vec![map_ip(IPV4_HOST), map_ip(IPV6_HOST)]),
                            ..fidl_fuchsia_net::LookupResult::EMPTY
                        })
                    );
                    // Successfully resolve IPv4 + IPv6 (with sorting).
                    assert_eq!(
                        lookup_ip(
                            REMOTE_IPV4_IPV6_HOST,
                            fidl_fuchsia_net::LookupIpOptions2 {
                                ipv4_lookup: Some(true),
                                ipv6_lookup: Some(true),
                                sort_addresses: Some(true),
                                ..fidl_fuchsia_net::LookupIpOptions2::EMPTY
                            }
                        )
                        .await,
                        Ok(fidl_fuchsia_net::LookupResult {
                            addresses: Some(vec![map_ip(IPV6_HOST), map_ip(IPV4_HOST)]),
                            ..fidl_fuchsia_net::LookupResult::EMPTY
                        })
                    );
                },
                routes_handler,
            )
            .await
    }
}
