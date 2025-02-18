// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO: Edit this doc comment (it's copy+pasted from the Netstack3 core)

//! IP protocol types.
//!
//! We provide the following types:
//!
//! # IP Versions
//!
//! * [`IpVersion`]: An enum representing IPv4 or IPv6
//! * [`Ip`]: An IP version trait that can be used write code which is generic
//!   over both versions of the IP protocol
//!
//! # IP Addresses
//!
//! * [`Ipv4Addr`]: A concrete IPv4 address
//! * [`Ipv6Addr`]: A concrete IPv6 address
//! * [`IpAddr`]: An enum representing either a v4 or v6 address.
//! * [`IpAddress`]: An IP address trait that can be used to write code which is
//!   generic over both types of IP address
//!
//! # Subnets
//!
//! * [`Subnet`]: A v4 or v6 subnet, as specified by the type parameter.
//! * [`SubnetEither`]: An enum of either a v4 subnet or a v6 subnet.
//!
//! # Address + Subnet Pairs:
//!
//! * [`AddrSubnet`]: A v4 or v6 subnet + address pair, as specified by the type
//!   parameter.
//! * [`AddrSubnetEither`]: An enum of either a v4 or a v6 subnet + address
//!   pair.
//!
//! [`IpVersion`]: crate::ip::IpVersion
//! [`Ip`]: crate::ip::Ip
//! [`Ipv4Addr`]: crate::ip::Ipv4Addr
//! [`Ipv6Addr`]: crate::ip::Ipv6Addr
//! [`IpAddr`]: crate::ip::IpAddr
//! [`IpAddress`]: crate::ip::IpAddress
//! [`Subnet`]: crate::ip::Subnet
//! [`SubnetEither`]: crate::ip::SubnetEither
//! [`AddrSubnet`]: crate::ip::AddrSubnet
//! [`AddrSubnetEither`]: crate::ip::AddrSubnetEither

// TODO(joshlf): Add RFC references for various standards such as the global
// broadcast address or the Class E subnet.

use core::convert::TryFrom;
use core::fmt::{self, Debug, Display, Formatter};
use core::hash::Hash;
use core::ops::Deref;

#[cfg(std)]
use std::net;

use zerocopy::{AsBytes, FromBytes, Unaligned};

use crate::{
    sealed, LinkLocalAddr, LinkLocalAddress, LinkLocalMulticastAddr, LinkLocalUnicastAddr,
    MulticastAddr, MulticastAddress, Scope, ScopeableAddress, SpecifiedAddr, SpecifiedAddress,
    UnicastAddr, UnicastAddress, Witness,
};

// NOTE on passing by reference vs by value: Clippy advises us to pass IPv4
// addresses by value, and IPv6 addresses by reference. For concrete types, we
// do the right thing. For the IpAddress trait, we use references in order to
// optimize (albeit very slightly) for IPv6 performance.

/// An IP protocol version.
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub enum IpVersion {
    V4,
    V6,
}

/// A ZST that carries IP version information.
///
/// Typically used by types that need to receive external information of which
/// IP version the type is specialized for, but without any other associated data.
#[derive(Copy, Clone, PartialEq, Eq, Hash)]
pub struct IpVersionMarker<I> {
    _marker: core::marker::PhantomData<I>,
}

impl<I: Ip> Default for IpVersionMarker<I> {
    fn default() -> Self {
        Self { _marker: core::marker::PhantomData }
    }
}

impl<I: Ip> Debug for IpVersionMarker<I> {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        write!(f, "IpVersionMarker<{}>", I::NAME)
    }
}

/// An IP address.
///
/// By default, the contained address types are `Ipv4Addr` and `Ipv6Addr`.
/// However, any types can be provided. This is intended to support types like
/// `IpAddr<SpecifiedAddr<Ipv4Addr>, SpecifiedAddr<Ipv6Addr>>`. `From` is
/// implemented to support conversions in both directions between
/// `IpAddr<SpecifiedAddr<Ipv4Addr>, SpecifiedAddr<Ipv6Addr>>` and
/// `SpecifiedAddr<IpAddr>`, and similarly for other witness types.
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub enum IpAddr<V4 = Ipv4Addr, V6 = Ipv6Addr> {
    V4(V4),
    V6(V6),
}

impl<V4, V6> IpAddr<V4, V6> {
    /// Transposes a `IpAddr` of a witness type to a witness type of an
    /// `IpAddr`.
    ///
    /// For example, you could use `transpose` to convert an
    /// `IpAddr<SpecifiedAddr<Ipv4Addr>, SpecifiedAddr<Ipv6Addr>>` into a
    /// `SpecifiedAddr<IpAddr<Ipv4Addr, Ipv6Addr>>`.
    pub fn transpose<W: IpAddrWitness<V4 = V4, V6 = V6>>(self) -> W {
        match self {
            IpAddr::V4(addr) => W::from_v4(addr),
            IpAddr::V6(addr) => W::from_v6(addr),
        }
    }
}

impl<A: IpAddress> From<A> for IpAddr {
    #[inline]
    fn from(addr: A) -> IpAddr {
        addr.into_ip_addr()
    }
}

#[cfg(std)]
impl From<net::IpAddr> for IpAddr {
    #[inline]
    fn from(addr: net::IpAddr) -> IpAddr {
        match addr {
            net::IpAddr::V4(addr) => IpAddr::V4(addr.into()),
            net::IpAddr::V6(addr) => IpAddr::V6(addr.into()),
        }
    }
}

#[cfg(std)]
impl From<IpAddr> for net::IpAddr {
    fn from(addr: IpAddr) -> net::IpAddr {
        match addr {
            IpAddr::V4(addr) => net::IpAddr::V4(addr.into()),
            IpAddr::V6(addr) => net::IpAddr::V6(addr.into()),
        }
    }
}

impl IpVersion {
    /// The number for this IP protocol version.
    ///
    /// 4 for `V4` and 6 for `V6`.
    #[inline]
    pub fn version_number(self) -> u8 {
        match self {
            IpVersion::V4 => 4,
            IpVersion::V6 => 6,
        }
    }

    /// Is this IPv4?
    #[inline]
    pub fn is_v4(self) -> bool {
        self == IpVersion::V4
    }

    /// Is this IPv6?
    #[inline]
    pub fn is_v6(self) -> bool {
        self == IpVersion::V6
    }
}

/// A trait for IP protocol versions.
///
/// `Ip` encapsulates the details of a version of the IP protocol. It includes
/// the [`IpVersion`] enum (`VERSION`) and an [`IpAddress`] type (`Addr`). It is
/// implemented by [`Ipv4`] and [`Ipv6`]. This trait is sealed, and there are
/// guaranteed to be no other implementors besides these. Code - including
/// unsafe code - may rely on this assumption for its correctness and soundness.
///
/// Note that the implementors of this trait are not meant to be instantiated
/// (in fact, they can't be instantiated). They are only meant to exist at the
/// type level.
pub trait Ip:
    Sized
    + Clone
    + Copy
    + Debug
    + Default
    + Eq
    + Hash
    + Ord
    + PartialEq
    + PartialOrd
    + Send
    + Sync
    + sealed::Sealed
    + 'static
{
    /// The IP version.
    ///
    /// `V4` for IPv4 and `V6` for IPv6.
    const VERSION: IpVersion;

    /// The unspecified address.
    ///
    /// This is 0.0.0.0 for IPv4 and :: for IPv6.
    const UNSPECIFIED_ADDRESS: Self::Addr;

    /// The default loopback address.
    ///
    /// When sending packets to a loopback interface, this address is used as
    /// the source address. It is an address in the loopback subnet.
    const LOOPBACK_ADDRESS: SpecifiedAddr<Self::Addr>;

    /// The subnet of loopback addresses.
    ///
    /// Addresses in this subnet must not appear outside a host, and may only be
    /// used for loopback interfaces.
    const LOOPBACK_SUBNET: Subnet<Self::Addr>;

    /// The subnet of multicast addresses.
    const MULTICAST_SUBNET: Subnet<Self::Addr>;

    /// The subnet of link-local unicast addresses.
    ///
    /// Note that some multicast addresses are also link-local. In IPv4, these
    /// are contained in the [link-local multicast subnet]. In IPv6, the
    /// link-local multicast addresses are not organized into a single subnet;
    /// instead, whether a multicast IPv6 address is link-local is a function of
    /// its scope.
    ///
    /// [link-local multicast subnet]: Ipv4::LINK_LOCAL_MULTICAST_SUBNET
    const LINK_LOCAL_UNICAST_SUBNET: Subnet<Self::Addr>;

    /// "IPv4" or "IPv6".
    const NAME: &'static str;

    /// The minimum link MTU for this version.
    ///
    /// Every internet link supporting this IP version must have a maximum
    /// transmission unit (MTU) of at least this many bytes. This MTU applies to
    /// the size of an IP packet, and does not include any extra bytes used by
    /// encapsulating packets (Ethernet frames, GRE packets, etc).
    const MINIMUM_LINK_MTU: u16;

    /// The address type for this IP version.
    ///
    /// [`Ipv4Addr`] for IPv4 and [`Ipv6Addr`] for IPv6.
    type Addr: IpAddress<Version = Self>;
}

/// IPv4.
///
/// `Ipv4` implements `Ip` for IPv4.
///
/// Note that this type has no value constructor. It is used purely at the type
/// level. Attempting to construct it by calling `Default::default` will panic.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum Ipv4 {}

impl Default for Ipv4 {
    fn default() -> Ipv4 {
        panic!("Ipv4 default")
    }
}

impl sealed::Sealed for Ipv4 {}

impl Ip for Ipv4 {
    const VERSION: IpVersion = IpVersion::V4;
    const UNSPECIFIED_ADDRESS: Ipv4Addr = Ipv4Addr::new([0, 0, 0, 0]);
    // https://tools.ietf.org/html/rfc5735#section-3
    const LOOPBACK_ADDRESS: SpecifiedAddr<Ipv4Addr> =
        unsafe { SpecifiedAddr::new_unchecked(Ipv4Addr::new([127, 0, 0, 1])) };
    const LOOPBACK_SUBNET: Subnet<Ipv4Addr> =
        Subnet { network: Ipv4Addr::new([127, 0, 0, 0]), prefix: 8 };
    const MULTICAST_SUBNET: Subnet<Ipv4Addr> =
        Subnet { network: Ipv4Addr::new([224, 0, 0, 0]), prefix: 4 };
    /// The subnet of link-local unicast addresses, outlined in [RFC 3927
    /// Section 2.1].
    ///
    /// [RFC 3927 Section 2.1]: https://tools.ietf.org/html/rfc3927#section-2.1
    const LINK_LOCAL_UNICAST_SUBNET: Subnet<Ipv4Addr> =
        Subnet { network: Ipv4Addr::new([169, 254, 0, 0]), prefix: 16 };
    const NAME: &'static str = "IPv4";
    /// The IPv4 minimum link MTU.
    ///
    /// Per [RFC 791 Section 3.2], "[\e\]very internet module must be able to
    /// forward a datagram of 68 octets without further fragmentation."
    ///
    /// [RFC 791 Section 3.2]: https://tools.ietf.org/html/rfc791#section-3.2
    const MINIMUM_LINK_MTU: u16 = 68;
    type Addr = Ipv4Addr;
}

impl Ipv4 {
    /// The global broadcast address.
    ///
    /// This address is considered to be a broadcast address on all networks
    /// regardless of subnet address. This is distinct from the subnet-specific
    /// broadcast address (e.g., 192.168.255.255 on the subnet 192.168.0.0/16).
    pub const GLOBAL_BROADCAST_ADDRESS: SpecifiedAddr<Ipv4Addr> =
        unsafe { SpecifiedAddr::new_unchecked(Ipv4Addr::new([255, 255, 255, 255])) };

    /// The Class E subnet.
    ///
    /// The Class E subnet is meant for experimental purposes, and should not be
    /// used on the general internet. [RFC 1812 Section 5.3.7] suggests that
    /// routers SHOULD discard packets with a source address in the Class E
    /// subnet.
    ///
    /// [RFC 1812 Section 5.3.7]: https://tools.ietf.org/html/rfc1812#section-5.3.7
    pub const CLASS_E_SUBNET: Subnet<Ipv4Addr> =
        Subnet { network: Ipv4Addr::new([240, 0, 0, 0]), prefix: 4 };

    /// The subnet of link-local multicast addresses, outlined in [RFC 5771
    /// Section 4].
    ///
    /// [RFC 5771 Section 4]: https://tools.ietf.org/html/rfc5771#section-4
    pub const LINK_LOCAL_MULTICAST_SUBNET: Subnet<Ipv4Addr> =
        Subnet { network: Ipv4Addr::new([169, 254, 0, 0]), prefix: 16 };

    /// The multicast address subscribed to by all routers on the local network.
    pub const ALL_ROUTERS_MULTICAST_ADDRESS: MulticastAddr<Ipv4Addr> =
        unsafe { MulticastAddr::new_unchecked(Ipv4Addr::new([224, 0, 0, 2])) };
}

/// IPv6.
///
/// `Ipv6` implements `Ip` for IPv6.
///
/// Note that this type has no value constructor. It is used purely at the type
/// level. Attempting to construct it by calling `Default::default` will panic.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum Ipv6 {}

impl Default for Ipv6 {
    fn default() -> Ipv6 {
        panic!("Ipv6 default")
    }
}

impl sealed::Sealed for Ipv6 {}

impl Ip for Ipv6 {
    const VERSION: IpVersion = IpVersion::V6;
    const UNSPECIFIED_ADDRESS: Ipv6Addr = Ipv6Addr::new([0; 16]);
    const LOOPBACK_ADDRESS: SpecifiedAddr<Ipv6Addr> = unsafe {
        SpecifiedAddr::new_unchecked(Ipv6Addr::new([
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        ]))
    };
    const LOOPBACK_SUBNET: Subnet<Ipv6Addr> = Subnet {
        network: Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
        prefix: 128,
    };
    const MULTICAST_SUBNET: Subnet<Ipv6Addr> = Subnet {
        network: Ipv6Addr::new([0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        prefix: 8,
    };
    /// The subnet of link-local unicast addresses, defined in [RFC 4291 Section
    /// 2.4].
    ///
    /// Note that multicast addresses can also be link-local. However, there is no
    /// single subnet of link-local multicast addresses. For more details on
    /// link-local multicast addresses, see [RFC 4291 Section 2.7].
    ///
    /// [RFC 4291 Section 2.4]: https://tools.ietf.org/html/rfc4291#section-2.4
    /// [RFC 4291 Section 2.7]: https://tools.ietf.org/html/rfc4291#section-2.7
    const LINK_LOCAL_UNICAST_SUBNET: Subnet<Ipv6Addr> = Subnet {
        network: Ipv6Addr::new([0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        prefix: 10,
    };
    const NAME: &'static str = "IPv6";
    /// The IPv6 minimum link MTU.
    ///
    /// Per [RFC 8200 Section 5]:
    ///
    /// > IPv6 requires that every link in the Internet have an MTU of 1280
    /// > octets or greater. This is known as the IPv6 minimum link MTU. On any
    /// > link that cannot convey a 1280-octet packet in one piece, link-
    /// > specific fragmentation and reassembly must be provided at a layer
    /// > below IPv6.
    ///
    /// [RFC 8200 Section 5]: https://tools.ietf.org/html/rfc8200#section-5
    const MINIMUM_LINK_MTU: u16 = 1280;
    type Addr = Ipv6Addr;
}

impl Ipv6 {
    /// The default loopback address.
    ///
    /// When sending packets to a loopback interface, this address is used as
    /// the source address. It is an address in the loopback subnet.
    ///
    /// Unlike [`Ip::LOOPBACK_ADDRESS`], `LOOPBACK_IPV6_ADDRESS` is a
    /// [`UnicastAddr`].
    pub const LOOPBACK_IPV6_ADDRESS: UnicastAddr<Ipv6Addr> =
        unsafe { UnicastAddr::new_unchecked(Ipv6::LOOPBACK_ADDRESS.0) };

    /// The IPv6 All Nodes multicast address in link-local scope, as defined in
    /// [RFC 4291 Section 2.7.1].
    ///
    /// [RFC 4291 Section 2.7.1]: https://tools.ietf.org/html/rfc4291#section-2.7.1
    pub const ALL_NODES_LINK_LOCAL_MULTICAST_ADDRESS: MulticastAddr<Ipv6Addr> = unsafe {
        MulticastAddr::new_unchecked(Ipv6Addr::new([
            0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        ]))
    };

    /// The IPv6 All Routers multicast address in link-local scope, as defined
    /// in [RFC 4291 Section 2.7.1].
    ///
    /// [RFC 4291 Section 2.7.1]: https://tools.ietf.org/html/rfc4291#section-2.7.1
    pub const ALL_ROUTERS_LINK_LOCAL_MULTICAST_ADDRESS: MulticastAddr<Ipv6Addr> = unsafe {
        MulticastAddr::new_unchecked(Ipv6Addr::new([
            0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2,
        ]))
    };

    /// The (deprecated) subnet of site-local unicast addresses, defined in [RFC
    /// 3513 Section 2.5.6].
    ///
    /// The site-local unicast subnet was deprecated in [RFC 3879]:
    ///
    /// > The special behavior of this prefix MUST no longer be supported in new
    /// > implementations. The prefix MUST NOT be reassigned for other use
    /// > except by a future IETF standards action... However, router
    /// > implementations SHOULD be configured to prevent routing of this prefix
    /// > by default.
    ///
    /// [RFC 3513 Section 2.5.6]: https://tools.ietf.org/html/rfc3513#section-2.5.6
    /// [RFC 3879]: https://tools.ietf.org/html/rfc3879
    pub const SITE_LOCAL_UNICAST_SUBNET: Subnet<Ipv6Addr> = Subnet {
        network: Ipv6Addr::new([0xfe, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        prefix: 10,
    };

    /// The length, in bits, of the interface identifier portion of unicast IPv6
    /// addresses *except* for addresses which start with the binary value 000.
    ///
    /// According to [RFC 4291 Section 2.5.1], "\[f\]or all unicast addresses,
    /// except those that start with the binary value 000, Interface IDs are
    /// required to be 64 bits."
    ///
    /// Note that, per [RFC 4862 Section 5.5.3], "a future revision of the
    /// address architecture \[RFC4291\] and a future link-type-specific
    /// document, which will still be consistent with each other, could
    /// potentially allow for an interface identifier of length other than the
    /// value defined in the current documents.  Thus, an implementation should
    /// not assume a particular constant.  Rather, it should expect any lengths
    /// of interface identifiers." In other words, this constant may be used to
    /// generate addresses or subnet prefix lengths, but should *not* be used to
    /// validate addresses or subnet prefix lengths generated by other software
    /// or other machines, as it might be valid for other software or other
    /// machines to use an interface identifier length different from this one.
    ///
    /// [RFC 4291 Section 2.5.1]: https://tools.ietf.org/html/rfc4291#section-2.5.1
    /// [RFC 4862 Section 5.5.3]: https://tools.ietf.org/html/rfc4862#section-5.5.3
    pub const UNICAST_INTERFACE_IDENTIFIER_BITS: u8 = 64;
}

/// An IPv4 or IPv6 address.
///
/// `IpAddress` is implemented by [`Ipv4Addr`] and [`Ipv6Addr`]. It is sealed,
/// and there are guaranteed to be no other implementors besides these. Code -
/// including unsafe code - may rely on this assumption for its correctness and
/// soundness.
pub trait IpAddress:
    Sized
    + Eq
    + PartialEq
    + Hash
    + Copy
    + Display
    + Debug
    + Default
    + Sync
    + Send
    + LinkLocalAddress
    + ScopeableAddress
    + sealed::Sealed
    + 'static
{
    /// The number of bytes in an address of this type.
    ///
    /// 4 for IPv4 and 16 for IPv6.
    const BYTES: u8;

    /// The IP version type of this address.
    ///
    /// `Ipv4` for `Ipv4Addr` and `Ipv6` for `Ipv6Addr`.
    type Version: Ip<Addr = Self>;

    /// Gets the underlying bytes of the address.
    fn bytes(&self) -> &[u8];

    /// Masks off the top bits of the address.
    ///
    /// Returns a copy of `self` where all but the top `bits` bits are set to
    /// 0.
    ///
    /// # Panics
    ///
    /// `mask` panics if `bits` is out of range - if it is greater than 32 for
    /// IPv4 or greater than 128 for IPv6.
    fn mask(&self, bits: u8) -> Self;

    /// Converts a statically-typed IP address into a dynamically-typed one.
    fn into_ip_addr(self) -> IpAddr;

    /// Is this a loopback address?
    ///
    /// `is_loopback` returns `true` if this address is a member of the loopback
    /// subnet.
    #[inline]
    fn is_loopback(&self) -> bool {
        Self::Version::LOOPBACK_SUBNET.contains(self)
    }

    /// Is this a unicast address contained in the given subnet?
    ///
    /// `is_unicast_in_subnet` returns `true` if a given subnet contains this
    /// address and the address is none of:
    /// - a multicast address
    /// - the IPv4 global broadcast address
    /// - the IPv4 subnet-specific broadcast address for the given `subnet`
    /// - an IPv4 address whose host bits (those bits following the network
    ///   prefix) are all 0
    /// - the unspecified address
    /// - an IPv4 Class E address
    ///
    /// Note two exceptions to these rules: If `subnet` is an IPv4 /32, then the
    /// single unicast address in the subnet is also technically the subnet
    /// broadcast address. If `subnet` is an IPv4 /31, then both addresses in
    /// that subnet are broadcast addresses. In either case, the "no
    /// subnet-specific broadcast" and "no address with a host part of all
    /// zeroes" rules don't apply. Note further that this exception *doesn't*
    /// apply to the unspecified address, which is never considered a unicast
    /// address regardless of what subnet it's in.
    ///
    /// # RFC Deep Dive
    ///
    /// ## IPv4 addresses ending in zeroes
    ///
    /// In this section, we justify the rule that IPv4 addresses whose host bits
    /// are all 0 are not considered unicast addresses.
    ///
    /// In earlier standards, an IPv4 address whose bits were all 0 after the
    /// network prefix (e.g., 192.168.0.0 in the subnet 192.168.0.0/16) were a
    /// form of "network-prefix-directed" broadcast addresses. Similarly,
    /// 0.0.0.0 was considered a form of "limited broadcast address". These have
    /// since been deprecated (in the case of 0.0.0.0, it is now considered the
    /// "unspecified" address).
    ///
    /// As evidence that this deprecation is official, consider [RFC 1812
    /// Section 5.3.5]. In reference to these types of addresses, it states that
    /// "packets addressed to any of these addresses SHOULD be silently
    /// discarded [by routers]". This not only deprecates them as broadcast
    /// addresses, but also as unicast addresses (after all, unicast addresses
    /// are not particularly useful if packets destined to them are discarded by
    /// routers).
    ///
    /// ## IPv4 /31 and /32 exceptions
    ///
    /// In this section, we justify the exceptions that all addresses in IPv4
    /// /31 and /32 subnets are considered unicast.
    ///
    /// For /31 subnets, the case is easy. [RFC 3021 Section 2.1] states that
    /// both addresses in a /31 subnet "MUST be interpreted as host addresses."
    ///
    /// For /32, the case is a bit more vague. RFC 3021 makes no mention of /32
    /// subnets. However, the same reasoning applies - if an exception is not
    /// made, then there do not exist any host addresses in a /32 subnet. [RFC
    /// 4632 Section 3.1] also vaguely implies this interpretation by referring
    /// to addresses in /32 subnets as "host routes."
    ///
    /// [RFC 1812 Section 5.3.5]: https://tools.ietf.org/html/rfc1812#page-92
    /// [RFC 4632 Section 3.1]: https://tools.ietf.org/html/rfc4632#section-3.1
    fn is_unicast_in_subnet(&self, subnet: &Subnet<Self>) -> bool;

    /// Invokes one function on this address if it is an [`Ipv4Addr`] and
    /// another if it is an [`Ipv6Addr`].
    ///
    /// Watch out for the common pitfall that, if `v4` and `v6` are closures,
    /// even though only one will be invoked in practice, the Rust borrow
    /// checker will still not allow them to both borrow the same mutable state
    /// simultaneously. If this becomes a problem, consider separate calls to
    /// [`with_v4`] and [`with_v6`] instead.
    ///
    /// [`with_v4`]: crate::ip::IpAddress::with_v4
    /// [`with_v6`]: crate::ip::IpAddress::with_v6
    fn with<O, F4: FnOnce(Ipv4Addr) -> O, F6: FnOnce(Ipv6Addr) -> O>(self, v4: F4, v6: F6) -> O;

    /// Invokes a function on this address if it is an [`Ipv4Addr`] or return
    /// `default` if it is an [`Ipv6Addr`].
    fn with_v4<O, F: FnOnce(Ipv4Addr) -> O>(self, f: F, default: O) -> O {
        self.with(f, |_| default)
    }

    /// Invokes a function on this address if it is an [`Ipv6Addr`] or return
    /// `default` if it is an [`Ipv4Addr`].
    fn with_v6<O, F: FnOnce(Ipv6Addr) -> O>(self, f: F, default: O) -> O {
        self.with(|_| default, f)
    }

    // Functions used to implement internal types. These functions aren't
    // particularly useful to users, but allow us to implement certain
    // specialization-like behavior without actually relying on the unstable
    // `specialization` feature.

    #[doc(hidden)]
    fn subnet_into_either(subnet: Subnet<Self>) -> SubnetEither;
}

/// An witness of an [`IpAddress`].
///
/// `IpAddressWitnessExt` extends [`Witness<A>`] for `A: IpAddress`, adding
/// extra IP address-specific functionality.
///
/// [`Witness<A>`]: crate::Witness
pub trait IpAddressWitnessExt<A: IpAddress>: Witness<A> {
    /// This witness type instantiated on the concrete address type
    /// [`Ipv4Addr`].
    type V4: Witness<Ipv4Addr>;

    /// This witness type instantiated on the concrete address type
    /// [`Ipv6Addr`].
    type V6: Witness<Ipv6Addr>;

    /// Invokes one function on this address if it is a witness of [`Ipv4Addr`]
    /// and another if it is a witness of [`Ipv6Addr`].
    ///
    /// Watch out for the common pitfall that, if `v4` and `v6` are closures,
    /// even though only one will be invoked in practice, the Rust borrow
    /// checker will still not allow them to both borrow the same mutable state
    /// simultaneously. If this becomes a problem, consider separate calls to
    /// [`with_v4`] and [`with_v6`] instead.
    ///
    /// [`with_v4`]: crate::ip::IpAddressWitnessExt::with_v4
    /// [`with_v6`]: crate::ip::IpAddressWitnessExt::with_v6
    fn with<O, F4: FnOnce(Self::V4) -> O, F6: FnOnce(Self::V6) -> O>(self, v4: F4, v6: F6) -> O;

    /// Invokes a function on this address if it is a witness of [`Ipv4Addr`] or return
    /// `default` if it is a witness of [`Ipv6Addr`].
    fn with_v4<O, F: FnOnce(Self::V4) -> O>(self, f: F, default: O) -> O {
        self.with(f, |_| default)
    }

    /// Invokes a function on this address if it is a witness of [`Ipv6Addr`] or
    /// return `default` if it is a witness of [`Ipv4Addr`].
    fn with_v6<O, F: FnOnce(Self::V6) -> O>(self, f: F, default: O) -> O {
        self.with(|_| default, f)
    }
}

macro_rules! impl_ip_address_witness_ext {
    ($ty:ident) => {
        impl<A: IpAddress> IpAddressWitnessExt<A> for $ty<A> {
            type V4 = $ty<Ipv4Addr>;
            type V6 = $ty<Ipv6Addr>;

            fn with<O, F4: FnOnce(Self::V4) -> O, F6: FnOnce(Self::V6) -> O>(
                self,
                v4: F4,
                v6: F6,
            ) -> O {
                self.into_addr().with(
                    |addr| v4(unsafe { $ty::new_unchecked(addr) }),
                    |addr| v6(unsafe { $ty::new_unchecked(addr) }),
                )
            }
        }
    };
    ($ty:ident, $($tys:ident),*) => {
        impl_ip_address_witness_ext!($ty);
        impl_ip_address_witness_ext!($($tys),*);
    }
}

impl_ip_address_witness_ext!(SpecifiedAddr, MulticastAddr, LinkLocalAddr);

impl<A: IpAddress> SpecifiedAddress for A {
    /// Is this an address other than the unspecified address?
    ///
    /// `is_specified` returns true if `self` is not equal to [`A::Version::UNSPECIFIED_ADDRESS`].
    ///
    /// [`A::Version::UNSPECIFIED_ADDRESS`]: crate::ip::Ip::UNSPECIFIED_ADDRESS
    #[inline]
    fn is_specified(&self) -> bool {
        self != &A::Version::UNSPECIFIED_ADDRESS
    }
}

/// Map a method over an `IpAddr`, calling it after matching on the type of IP
/// address.
macro_rules! map_ip_addr {
    ($val:expr, $method:ident) => {
        match $val {
            IpAddr::V4(a) => a.$method(),
            IpAddr::V6(a) => a.$method(),
        }
    };
}

impl SpecifiedAddress for IpAddr {
    /// Is this an address other than the unspecified address?
    ///
    /// `is_specified` returns true if `self` is not equal to
    /// [`Ip::UNSPECIFIED_ADDRESS`] for the IP version of this address.
    #[inline]
    fn is_specified(&self) -> bool {
        map_ip_addr!(self, is_specified)
    }
}

impl<A: IpAddress> MulticastAddress for A {
    /// Is this address in the multicast subnet?
    ///
    /// `is_multicast` returns true if `self` is in
    /// [`A::Version::MULTICAST_SUBNET`].
    ///
    /// [`A::Version::MULTICAST_SUBNET`]: crate::ip::Ip::MULTICAST_SUBNET
    #[inline]
    fn is_multicast(&self) -> bool {
        <A as IpAddress>::Version::MULTICAST_SUBNET.contains(self)
    }
}

impl MulticastAddress for IpAddr {
    /// Is this an address in the multicast subnet?
    ///
    /// `is_multicast` returns true if `self` is in [`Ip::MULTICAST_SUBNET`] for
    /// the IP version of this address.
    #[inline]
    fn is_multicast(&self) -> bool {
        map_ip_addr!(self, is_multicast)
    }
}

impl LinkLocalAddress for Ipv4Addr {
    /// Is this address in the link-local subnet?
    ///
    /// `is_linklocal` returns true if `self` is in
    /// [`Ipv4::LINK_LOCAL_UNICAST_SUBNET`] or
    /// [`Ipv4::LINK_LOCAL_MULTICAST_SUBNET`].
    ///
    /// [`Ipv4::LINK_LOCAL_UNICAST_SUBNET`]: crate::ip::Ip::LINK_LOCAL_UNICAST_SUBNET
    /// [`Ipv4::LINK_LOCAL_MULTICAST_SUBNET`]: crate::ip::Ipv4::LINK_LOCAL_MULTICAST_SUBNET
    #[inline]
    fn is_linklocal(&self) -> bool {
        Ipv4::LINK_LOCAL_UNICAST_SUBNET.contains(self)
            || Ipv4::LINK_LOCAL_MULTICAST_SUBNET.contains(self)
    }
}

impl LinkLocalAddress for Ipv6Addr {
    /// Is this address in the link-local subnet?
    ///
    /// `is_linklocal` returns true if `self` is in
    /// [`Ipv6::LINK_LOCAL_UNICAST_SUBNET`], is a multicast address whose scope
    /// is link-local, or is the address [`Ipv6::LOOPBACK_ADDRESS`] (per [RFC
    /// 4291 Section 2.5.3], the loopback address is considered to have
    /// link-local scope).
    ///
    /// [`Ipv6::LINK_LOCAL_UNICAST_SUBNET`]: crate::ip::Ip::LINK_LOCAL_UNICAST_SUBNET
    /// [`Ipv6::LOOPBACK_ADDRESS`]: crate::ip::Ip::LOOPBACK_ADDRESS
    /// [RFC 4291 Section 2.5.3]: https://tools.ietf.org/html/rfc4291#section-2.5.3
    #[inline]
    fn is_linklocal(&self) -> bool {
        const LINK_LOCAL_SCOPE: u8 = 0x02;
        // TODO(joshlf): Stop doing this manually once we have a general-purpose
        // mechanism for extracting the scope from a multicast address.
        Ipv6::LINK_LOCAL_UNICAST_SUBNET.contains(self)
            || (self.is_multicast() && self.0[1] & 0x0F == LINK_LOCAL_SCOPE)
            || self == Ipv6::LOOPBACK_ADDRESS.deref()
    }
}

impl LinkLocalAddress for IpAddr {
    /// Is this address link-local?
    #[inline]
    fn is_linklocal(&self) -> bool {
        map_ip_addr!(self, is_linklocal)
    }
}

impl ScopeableAddress for Ipv4Addr {
    type Scope = ();

    /// The scope of this address.
    ///
    /// Although IPv4 defines a link local subnet, IPv4 addresses are always
    /// considered to be in the global scope.
    fn scope(&self) {}
}

/// The list of IPv6 scopes.
///
/// These scopes are defined by [RFC 4291 Section 2.7].
///
/// [RFC 4291 Section 2.7]: https://tools.ietf.org/html/rfc4291#section-2.7
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Ipv6Scope {
    /// The interface-local scope.
    InterfaceLocal,
    /// The link-local scope.
    LinkLocal,
    /// The admin-local scope.
    AdminLocal,
    /// The (deprecated) site-local scope.
    ///
    /// The site-local scope was deprecated in [RFC 3879]. While this scope
    /// is returned for both site-local unicast and site-local multicast
    /// addresses, RFC 3879 says the following about site-local unicast addresses
    /// in particular ("this prefix" refers to the [site-local unicast subnet]):
    ///
    /// > The special behavior of this prefix MUST no longer be supported in new
    /// > implementations. The prefix MUST NOT be reassigned for other use
    /// > except by a future IETF standards action... However, router
    /// > implementations SHOULD be configured to prevent routing of this prefix
    /// > by default.
    ///
    /// [RFC 3879]: https://tools.ietf.org/html/rfc3879
    /// [site-local unicast subnet]: Ipv6::SITE_LOCAL_UNICAST_SUBNET
    SiteLocal,
    /// The organization-local scope.
    OrganizationLocal,
    /// The global scope.
    Global,
    /// Scopes which are reserved for future use by [RFC 4291 Section 2.7].
    ///
    /// [RFC 4291 Section 2.7]: https://tools.ietf.org/html/rfc4291#section-2.7
    Reserved(Ipv6ReservedScope),
    /// Scopes which are available for local definition by administrators.
    Unassigned(Ipv6UnassignedScope),
}

/// The list of IPv6 scopes which are reserved for future use by [RFC 4291
/// Section 2.7].
///
/// [RFC 4291 Section 2.7]: https://tools.ietf.org/html/rfc4291#section-2.7
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Ipv6ReservedScope {
    /// The scope with numerical value 0.
    Scope0 = 0,
    /// The scope with numerical value 3.
    Scope3 = 3,
    /// The scope with numerical value 0xF.
    ScopeF = 0xF,
}

/// The list of IPv6 scopes which are available for local definition by
/// administrators.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Ipv6UnassignedScope {
    /// The scope with numerical value 6.
    Scope6 = 6,
    /// The scope with numerical value 7.
    Scope7 = 7,
    /// The scope with numerical value 9.
    Scope9 = 9,
    /// The scope with numerical value 0xA.
    ScopeA = 0xA,
    /// The scope with numerical value 0xB.
    ScopeB = 0xB,
    /// The scope with numerical value 0xC.
    ScopeC = 0xC,
    /// The scope with numerical value 0xD.
    ScopeD = 0xD,
}

impl Scope for Ipv6Scope {
    #[inline]
    fn can_have_zone(&self) -> bool {
        // Per RFC 6874 Section 4:
        //
        // > [I]mplementations MUST NOT allow use of this format except for
        // > well-defined usages, such as sending to link-local addresses under
        // > prefix fe80::/10.  At the time of writing, this is the only
        // > well-defined usage known.
        //
        // While this directive applies particularly to the human-readable
        // string representation of IPv6 addresses and zone identifiers, it
        // seems reasonable to limit the in-memory representation in the same
        // way.
        //
        // Note that, if interpreted literally, this quote would bar the use of
        // zone identifiers on link-local multicast addresses (they are not
        // under the prefix fe80::/10). However, it seems clear that this is not
        // the interpretation that was intended. Link-local multicast addresses
        // have the same need for a zone-identifier as link-local unicast
        // addresses, and indeed, real systems like Linux allow link-local
        // multicast addresses to be accompanied by zone identifiers.
        matches!(self, Ipv6Scope::LinkLocal)
    }
}

impl core::cmp::PartialOrd for Ipv6Scope {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl core::cmp::Ord for Ipv6Scope {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        // RFC 4291, section 2.7 defines these scope IDs.
        //  0   Reserved
        //  1   Interface-Local scope
        //  2   Link-Local scope
        //  3   Reserved
        //  4   Admin-Local scope
        //  5   Site-Local scope
        //  6   Unassigned
        //  7   Unassigned
        //  8   Organization-Local scope
        //  9-D Unassigned
        //  E   Global scope
        //  F   Reserved
        let scope_id = |scope: &Ipv6Scope| match scope {
            Ipv6Scope::Reserved(Ipv6ReservedScope::Scope0) => 0x00,
            Ipv6Scope::InterfaceLocal => 0x01,
            Ipv6Scope::LinkLocal => 0x02,
            Ipv6Scope::Reserved(Ipv6ReservedScope::Scope3) => 0x03,
            Ipv6Scope::AdminLocal => 0x04,
            Ipv6Scope::SiteLocal => 0x05,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::Scope6) => 0x06,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::Scope7) => 0x07,
            Ipv6Scope::OrganizationLocal => 0x08,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::Scope9) => 0x09,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeA) => 0x0A,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeB) => 0x0B,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeC) => 0x0C,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeD) => 0x0D,
            Ipv6Scope::Global => 0xE,
            Ipv6Scope::Reserved(Ipv6ReservedScope::ScopeF) => 0x0F,
        };
        scope_id(self).cmp(&scope_id(other))
    }
}

impl ScopeableAddress for Ipv6Addr {
    type Scope = Ipv6Scope;

    /// The scope of this address.
    #[inline]
    fn scope(&self) -> Ipv6Scope {
        if self.is_multicast() {
            use Ipv6ReservedScope::*;
            use Ipv6Scope::*;
            use Ipv6UnassignedScope::*;

            // The "scop" field of a multicast address is the last 4 bits of the
            // second byte of the address (see
            // https://tools.ietf.org/html/rfc4291#section-2.7).
            match self.0[1] & 0xF {
                0 => Reserved(Scope0),
                1 => InterfaceLocal,
                2 => LinkLocal,
                3 => Reserved(Scope3),
                4 => AdminLocal,
                5 => SiteLocal,
                6 => Unassigned(Scope6),
                7 => Unassigned(Scope7),
                8 => OrganizationLocal,
                9 => Unassigned(Scope9),
                0xA => Unassigned(ScopeA),
                0xB => Unassigned(ScopeB),
                0xC => Unassigned(ScopeC),
                0xD => Unassigned(ScopeD),
                0xE => Global,
                0xF => Reserved(ScopeF),
                _ => unreachable!(),
            }
        } else if self.is_linklocal() {
            Ipv6Scope::LinkLocal
        } else if self.is_site_local() {
            Ipv6Scope::SiteLocal
        } else {
            Ipv6Scope::Global
        }
    }
}

impl Scope for IpAddr<(), Ipv6Scope> {
    #[inline]
    fn can_have_zone(&self) -> bool {
        match self {
            IpAddr::V4(scope) => scope.can_have_zone(),
            IpAddr::V6(scope) => scope.can_have_zone(),
        }
    }
}

impl ScopeableAddress for IpAddr {
    type Scope = IpAddr<(), Ipv6Scope>;

    #[inline]
    fn scope(&self) -> IpAddr<(), Ipv6Scope> {
        match self {
            IpAddr::V4(_) => IpAddr::V4(()),
            IpAddr::V6(addr) => IpAddr::V6(addr.scope()),
        }
    }
}

/// The definition of each trait for `IpAddr` is equal to the definition of that
/// trait for whichever of `Ipv4Addr` and `Ipv6Addr` is actually present in the
/// enum. Thus, we can convert between `$witness<IpvXAddr>`, `$witness<IpAddr>`,
/// and `IpAddr<$witness<Ipv4Addr>, $witness<Ipv6Addr>>` arbitrarily.

/// Provides various useful `From` impls for an IP address witness type.
///
/// `impl_from_witness!($witness)` implements:
/// - `From<IpAddr<$witness<Ipv4Addr>, $witness<Ipv6Addr>>> for
///   $witness<IpAddr>`
/// - `From<$witness<IpAddr>> for IpAddr<$witness<Ipv4Addr>,
///   $witness<Ipv6Addr>>`
/// - `From<$witness<Ipv4Addr>> for $witness<IpAddr>`
/// - `From<$witness<Ipv6Addr>> for $witness<IpAddr>`
/// - `From<$witness<Ipv4Addr>> for IpAddr`
/// - `From<$witness<Ipv6Addr>> for IpAddr`
/// - `TryFrom<Ipv4Addr> for $witness<Ipv4Addr>`
/// - `TryFrom<Ipv6Addr> for $witness<Ipv6Addr>`
///
/// `impl_from_witness!($witness, $ipaddr, $new_unchecked)` implements:
/// - `From<$witness<$ipaddr>> for $witness<IpAddr>`
/// - `From<$witness<$ipaddr>> for $ipaddr`
/// - `TryFrom<$ipaddr> for $witness<$ipaddr>`
macro_rules! impl_from_witness {
    ($witness:ident) => {
        impl_from_witness!($witness, Ipv4Addr, Witness::new_unchecked);
        impl_from_witness!($witness, Ipv6Addr, Witness::new_unchecked);

        impl From<IpAddr<$witness<Ipv4Addr>, $witness<Ipv6Addr>>> for $witness<IpAddr> {
            fn from(addr: IpAddr<$witness<Ipv4Addr>, $witness<Ipv6Addr>>) -> $witness<IpAddr> {
                unsafe {
                    Witness::new_unchecked(match addr {
                        IpAddr::V4(addr) => IpAddr::V4(addr.into_addr()),
                        IpAddr::V6(addr) => IpAddr::V6(addr.into_addr()),
                    })
                }
            }
        }
        impl From<$witness<IpAddr>> for IpAddr<$witness<Ipv4Addr>, $witness<Ipv6Addr>> {
            fn from(addr: $witness<IpAddr>) -> IpAddr<$witness<Ipv4Addr>, $witness<Ipv6Addr>> {
                unsafe {
                    match addr.into_addr() {
                        IpAddr::V4(addr) => IpAddr::V4(Witness::new_unchecked(addr)),
                        IpAddr::V6(addr) => IpAddr::V6(Witness::new_unchecked(addr)),
                    }
                }
            }
        }
    };
    ($witness:ident, $ipaddr:ident, $new_unchecked:expr) => {
        impl From<$witness<$ipaddr>> for $witness<IpAddr> {
            fn from(addr: $witness<$ipaddr>) -> $witness<IpAddr> {
                let addr: $ipaddr = addr.into_addr();
                let addr: IpAddr = addr.into();
                #[allow(unused_unsafe)] // For when a closure is passed
                unsafe {
                    $new_unchecked(addr)
                }
            }
        }
        impl From<$witness<$ipaddr>> for $ipaddr {
            fn from(addr: $witness<$ipaddr>) -> $ipaddr {
                addr.into_addr()
            }
        }
        impl TryFrom<$ipaddr> for $witness<$ipaddr> {
            type Error = ();
            fn try_from(addr: $ipaddr) -> Result<$witness<$ipaddr>, ()> {
                Witness::new(addr).ok_or(())
            }
        }
    };
}

impl_from_witness!(SpecifiedAddr);
impl_from_witness!(MulticastAddr);
impl_from_witness!(LinkLocalAddr);
impl_from_witness!(LinkLocalMulticastAddr);
impl_from_witness!(UnicastAddr, Ipv6Addr, UnicastAddr::new_unchecked);
impl_from_witness!(LinkLocalUnicastAddr, Ipv6Addr, |addr| LinkLocalAddr(UnicastAddr(addr)));

/// An IPv4 address.
#[derive(Copy, Clone, Default, PartialEq, Eq, Hash, FromBytes, AsBytes, Unaligned)]
#[repr(transparent)]
pub struct Ipv4Addr([u8; 4]);

impl Ipv4Addr {
    /// Creates a new IPv4 address.
    #[inline]
    pub const fn new(bytes: [u8; 4]) -> Self {
        Ipv4Addr(bytes)
    }

    /// Gets the bytes of the IPv4 address.
    #[inline]
    pub const fn ipv4_bytes(self) -> [u8; 4] {
        self.0
    }

    /// Is this the global broadcast address?
    ///
    /// `is_global_broadcast` is a shorthand for comparing against
    /// [`Ipv4::GLOBAL_BROADCAST_ADDRESS`].
    #[inline]
    pub fn is_global_broadcast(self) -> bool {
        self == Ipv4::GLOBAL_BROADCAST_ADDRESS.into_addr()
    }

    /// Is this a Class E address?
    ///
    /// `is_class_e` is a shorthand for checking membership in
    /// [`Ipv4::CLASS_E_SUBNET`].
    #[inline]
    pub fn is_class_e(self) -> bool {
        Ipv4::CLASS_E_SUBNET.contains(&self)
    }

    /// Calculates the common prefix length between this address and `other`.
    pub fn common_prefix_length(&self, other: &Ipv4Addr) -> u8 {
        let Ipv4Addr(me) = self;
        let Ipv4Addr(other) = other;
        common_prefix_len(me.iter().copied().zip(other.iter().copied()))
    }

    /// Converts the address to an IPv4-mapped IPv6 address according to
    /// [RFC 4291 Section 2.5.5.2].
    ///
    /// [RFC 4291 Section 2.5.5.2]: https://tools.ietf.org/html/rfc4291#section-2.5.5.2
    pub fn to_v6_mapped(self) -> Ipv6Addr {
        let Self(self_bytes) = self;
        let mut bytes = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0];
        bytes[12..].copy_from_slice(&self_bytes[..]);
        Ipv6Addr::new(bytes)
    }
}

impl sealed::Sealed for Ipv4Addr {}

impl IpAddress for Ipv4Addr {
    const BYTES: u8 = 4;

    type Version = Ipv4;

    #[inline]
    fn mask(&self, bits: u8) -> Self {
        assert!(bits <= 32);
        if bits == 0 {
            // shifting left by the size of the value is undefined
            Ipv4Addr([0; 4])
        } else {
            let mask = <u32>::max_value() << (32 - bits);
            Self::new((u32::from_be_bytes(self.0) & mask).to_be_bytes())
        }
    }

    #[inline]
    fn bytes(&self) -> &[u8] {
        &self.0
    }

    #[inline]
    fn into_ip_addr(self) -> IpAddr {
        IpAddr::V4(self)
    }

    #[inline]
    fn is_unicast_in_subnet(&self, subnet: &Subnet<Self>) -> bool {
        !self.is_multicast()
            && !self.is_global_broadcast()
            // This clause implements the rules that (the subnet broadcast is
            // not unicast AND the address with an all-zeroes host part is not
            // unicast) UNLESS the prefix length is 31 or 32.
            && (subnet.prefix() == 32
            || subnet.prefix() == 31
            || (*self != subnet.broadcast() && *self != subnet.network()))
            && self.is_specified()
            && !self.is_class_e()
            && subnet.contains(self)
    }

    #[inline]
    fn with<O, F4: FnOnce(Ipv4Addr) -> O, F6: FnOnce(Ipv6Addr) -> O>(self, v4: F4, _v6: F6) -> O {
        v4(self)
    }

    fn subnet_into_either(subnet: Subnet<Ipv4Addr>) -> SubnetEither {
        SubnetEither::V4(subnet)
    }
}

impl From<[u8; 4]> for Ipv4Addr {
    #[inline]
    fn from(bytes: [u8; 4]) -> Ipv4Addr {
        Ipv4Addr(bytes)
    }
}

#[cfg(std)]
impl From<net::Ipv4Addr> for Ipv4Addr {
    #[inline]
    fn from(ip: net::Ipv4Addr) -> Ipv4Addr {
        Ipv4Addr::new(ip.octets())
    }
}

#[cfg(std)]
impl From<Ipv4Addr> for net::Ipv4Addr {
    #[inline]
    fn from(ip: Ipv4Addr) -> net::Ipv4Addr {
        Ipv4Addr::from(ip.0)
    }
}

impl Display for Ipv4Addr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        write!(f, "{}.{}.{}.{}", self.0[0], self.0[1], self.0[2], self.0[3])
    }
}

impl Debug for Ipv4Addr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// An IPv6 address.
#[derive(Copy, Clone, Default, PartialEq, Eq, Hash, FromBytes, AsBytes, Unaligned)]
#[repr(transparent)]
pub struct Ipv6Addr([u8; 16]);

impl Ipv6Addr {
    /// Creates a new IPv6 address.
    #[inline]
    pub const fn new(bytes: [u8; 16]) -> Self {
        Ipv6Addr(bytes)
    }

    /// Gets the bytes of the IPv6 address.
    #[inline]
    pub const fn ipv6_bytes(&self) -> [u8; 16] {
        self.0
    }

    /// Converts this `Ipv6Addr` to the IPv6 Solicited-Node Address, used in
    /// Neighbor Discovery. Defined in [RFC 4291 Section 2.7.1].
    ///
    /// [RFC 4291 Section 2.7.1]: https://tools.ietf.org/html/rfc4291#section-2.7.1
    #[inline]
    pub const fn to_solicited_node_address(&self) -> MulticastAddr<Self> {
        // TODO(brunodalbo) benchmark this generation and evaluate if using
        //  bit operations with u128 could be faster. This is very likely
        //  going to be on a hot path.

        // We know we are not breaking the guarantee that `MulticastAddr` provides
        // when calling `new_unchecked` because the address we provide it is
        // a multicast address as defined by RFC 4291 section 2.7.1.
        unsafe {
            MulticastAddr::new_unchecked(Self::new([
                0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xff, self.0[13], self.0[14],
                self.0[15],
            ]))
        }
    }

    /// Checks whether `self` is a valid unicast address.
    ///
    /// A valid unicast address is any unicast address that can be bound to an
    /// interface (not the unspecified or loopback addresses).
    #[inline]
    pub fn is_valid_unicast(&self) -> bool {
        !(self.is_loopback() || !self.is_specified() || self.is_multicast())
    }

    /// Is this address in the (deprecated) site-local unicast subnet?
    ///
    /// `is_site_local` returns true if `self` is in the (deprecated)
    /// [`Ipv6::SITE_LOCAL_UNICAST_SUBNET`]. See that constant's documentation
    /// for more details on deprecation and how the subnet should be used in
    /// light of deprecation.
    #[inline]
    pub fn is_site_local(&self) -> bool {
        Ipv6::SITE_LOCAL_UNICAST_SUBNET.contains(self)
    }

    /// Is this address a unicast link-local address?
    ///
    /// Shorthand for `self.is_unicast_in_subnet(Ipv6::LINK_LOCAL_UNICAST_SUBNET)`.
    #[inline]
    pub fn is_unicast_linklocal(&self) -> bool {
        self.is_unicast_in_subnet(&Ipv6::LINK_LOCAL_UNICAST_SUBNET)
    }

    /// Calculates the common prefix length between this address and `other`.
    pub fn common_prefix_length(&self, other: &Ipv6Addr) -> u8 {
        let Ipv6Addr(me) = self;
        let Ipv6Addr(other) = other;
        common_prefix_len(me.iter().copied().zip(other.iter().copied()))
    }
}

impl sealed::Sealed for Ipv6Addr {}

/// [`Ipv4Addr`] is convertible into [`Ipv6Addr`] through
/// [`Ipv4Addr::to_v6_mapped`].
impl From<Ipv4Addr> for Ipv6Addr {
    fn from(addr: Ipv4Addr) -> Self {
        addr.to_v6_mapped()
    }
}

impl IpAddress for Ipv6Addr {
    const BYTES: u8 = 16;

    type Version = Ipv6;

    #[inline]
    fn mask(&self, bits: u8) -> Self {
        assert!(bits <= 128);
        if bits == 0 {
            // shifting left by the size of the value is undefined
            Ipv6Addr([0; 16])
        } else {
            let mask = <u128>::max_value() << (128 - bits);
            Self::new((u128::from_be_bytes(self.0) & mask).to_be_bytes())
        }
    }

    #[inline]
    fn bytes(&self) -> &[u8] {
        &self.0
    }

    #[inline]
    fn into_ip_addr(self) -> IpAddr {
        IpAddr::V6(self)
    }

    #[inline]
    fn is_unicast_in_subnet(&self, subnet: &Subnet<Self>) -> bool {
        !self.is_multicast() && self.is_specified() && subnet.contains(self)
    }

    #[inline]
    fn with<O, F4: FnOnce(Ipv4Addr) -> O, F6: FnOnce(Ipv6Addr) -> O>(self, _v4: F4, v6: F6) -> O {
        v6(self)
    }

    fn subnet_into_either(subnet: Subnet<Ipv6Addr>) -> SubnetEither {
        SubnetEither::V6(subnet)
    }
}

impl UnicastAddress for Ipv6Addr {
    #[inline]
    fn is_unicast(&self) -> bool {
        !self.is_multicast() && self.is_specified()
    }
}

impl From<[u8; 16]> for Ipv6Addr {
    #[inline]
    fn from(bytes: [u8; 16]) -> Ipv6Addr {
        Ipv6Addr(bytes)
    }
}

#[cfg(std)]
impl From<net::Ipv6Addr> for Ipv6Addr {
    #[inline]
    fn from(ip: net::Ipv6Addr) -> Ipv6Addr {
        Ipv6Addr::new(ip.octets())
    }
}

#[cfg(std)]
impl From<Ipv6Addr> for net::Ipv6Addr {
    #[inline]
    fn from(ip: Ipv6Addr) -> net::Ipv6Addr {
        net::Ipv6Addr::from(ip.0)
    }
}

impl Display for Ipv6Addr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        // TODO(fxbug.dev/68672): Implement canonicalization even when the "std"
        // feature is not enabled.

        use core::convert::TryInto;

        let to_u16 = |idx| u16::from_be_bytes(self.0[idx..idx + 2].try_into().unwrap());
        #[cfg(std)]
        return Display::fmt(
            &net::Ipv6Addr::new(
                to_u16(0),
                to_u16(2),
                to_u16(4),
                to_u16(6),
                to_u16(8),
                to_u16(10),
                to_u16(12),
                to_u16(14),
            ),
            f,
        );

        #[cfg(not(std))]
        return write!(
            f,
            "{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}",
            to_u16(0),
            to_u16(2),
            to_u16(4),
            to_u16(6),
            to_u16(8),
            to_u16(10),
            to_u16(12),
            to_u16(14),
        );
    }
}

impl Debug for Ipv6Addr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// The source address from an IPv6 packet.
///
/// An `Ipv6SourceAddr` represents the source address from an IPv6 packet, which
/// may only be either unicast or unspecified.
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq)]
pub enum Ipv6SourceAddr {
    Unicast(UnicastAddr<Ipv6Addr>),
    Unspecified,
}

impl Ipv6SourceAddr {
    /// Converts this `Ipv6SourceAddr` into an `Option<UnicastAddr<Ipv6Addr>>`,
    /// mapping [`Ipv6SourceAddr::Unspecified`] to `None`.
    #[inline]
    pub fn into_option(self) -> Option<UnicastAddr<Ipv6Addr>> {
        match self {
            Ipv6SourceAddr::Unicast(addr) => Some(addr),
            Ipv6SourceAddr::Unspecified => None,
        }
    }
}

impl crate::sealed::Sealed for Ipv6SourceAddr {}

impl Ipv6SourceAddr {
    /// Constructs a new `Ipv6SourceAddr`.
    ///
    /// `new` constructs a new `Ipv6SourceAddr`, returning `None` if `addr` is
    /// neither unicast nor unspecified.
    #[inline]
    pub fn new(addr: Ipv6Addr) -> Option<Ipv6SourceAddr> {
        if let Some(addr) = UnicastAddr::new(addr) {
            Some(Ipv6SourceAddr::Unicast(addr))
        } else if !addr.is_specified() {
            Some(Ipv6SourceAddr::Unspecified)
        } else {
            None
        }
    }
}

impl Witness<Ipv6Addr> for Ipv6SourceAddr {
    #[inline]
    fn new(addr: Ipv6Addr) -> Option<Ipv6SourceAddr> {
        Ipv6SourceAddr::new(addr)
    }

    #[inline]
    unsafe fn new_unchecked(addr: Ipv6Addr) -> Ipv6SourceAddr {
        Ipv6SourceAddr::new(addr).unwrap()
    }

    #[inline]
    fn into_addr(self) -> Ipv6Addr {
        match self {
            Ipv6SourceAddr::Unicast(addr) => addr.into_addr(),
            Ipv6SourceAddr::Unspecified => Ipv6::UNSPECIFIED_ADDRESS,
        }
    }
}

impl SpecifiedAddress for Ipv6SourceAddr {
    fn is_specified(&self) -> bool {
        self != &Ipv6SourceAddr::Unspecified
    }
}

impl UnicastAddress for Ipv6SourceAddr {
    fn is_unicast(&self) -> bool {
        matches!(self, Ipv6SourceAddr::Unicast(_))
    }
}

impl LinkLocalAddress for Ipv6SourceAddr {
    fn is_linklocal(&self) -> bool {
        let addr: Ipv6Addr = self.into();
        addr.is_linklocal()
    }
}

impl From<Ipv6SourceAddr> for Ipv6Addr {
    fn from(addr: Ipv6SourceAddr) -> Ipv6Addr {
        addr.into_addr()
    }
}

impl From<&'_ Ipv6SourceAddr> for Ipv6Addr {
    fn from(addr: &Ipv6SourceAddr) -> Ipv6Addr {
        match addr {
            Ipv6SourceAddr::Unicast(addr) => addr.get(),
            Ipv6SourceAddr::Unspecified => Ipv6::UNSPECIFIED_ADDRESS,
        }
    }
}

impl From<UnicastAddr<Ipv6Addr>> for Ipv6SourceAddr {
    fn from(addr: UnicastAddr<Ipv6Addr>) -> Ipv6SourceAddr {
        Ipv6SourceAddr::Unicast(addr)
    }
}

impl TryFrom<Ipv6Addr> for Ipv6SourceAddr {
    type Error = ();
    fn try_from(addr: Ipv6Addr) -> Result<Ipv6SourceAddr, ()> {
        Ipv6SourceAddr::new(addr).ok_or(())
    }
}

impl From<Ipv6SourceAddr> for Option<UnicastAddr<Ipv6Addr>> {
    fn from(addr: Ipv6SourceAddr) -> Option<UnicastAddr<Ipv6Addr>> {
        addr.into_option()
    }
}

impl AsRef<Ipv6Addr> for Ipv6SourceAddr {
    fn as_ref(&self) -> &Ipv6Addr {
        match self {
            Ipv6SourceAddr::Unicast(addr) => addr,
            Ipv6SourceAddr::Unspecified => &Ipv6::UNSPECIFIED_ADDRESS,
        }
    }
}

impl Deref for Ipv6SourceAddr {
    type Target = Ipv6Addr;

    fn deref(&self) -> &Ipv6Addr {
        self.as_ref()
    }
}

impl Display for Ipv6SourceAddr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        match self {
            Ipv6SourceAddr::Unicast(addr) => write!(f, "{}", addr),
            // TODO(fxbug.dev/68672): Once we implement canonicalization without
            // the "std" feature, replace this with `write!(f, "::")`
            Ipv6SourceAddr::Unspecified => write!(f, "{}", Ipv6::UNSPECIFIED_ADDRESS),
        }
    }
}

impl Debug for Ipv6SourceAddr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// An IPv6 address stored as a unicast or multicast witness type.
///
/// `UnicastOrMulticastIpv6Addr` is either a [`UnicastAddr`] or a
/// [`MulticastAddr`]. It allows the user to match on the unicast-ness or
/// multicast-ness of an IPv6 address and obtain a statically-typed witness in
/// each case. This is useful if the user needs to call different functions
/// which each take a witness type.
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq)]
pub enum UnicastOrMulticastIpv6Addr {
    Unicast(UnicastAddr<Ipv6Addr>),
    Multicast(MulticastAddr<Ipv6Addr>),
}

impl UnicastOrMulticastIpv6Addr {
    /// Constructs a new `UnicastOrMulticastIpv6Addr`.
    ///
    /// `new` constructs a new `UnicastOrMulticastIpv6Addr`, returning `None` if
    /// `addr` is the unspecified address.
    pub fn new(addr: Ipv6Addr) -> Option<UnicastOrMulticastIpv6Addr> {
        SpecifiedAddr::new(addr).map(UnicastOrMulticastIpv6Addr::from_specified)
    }

    /// Constructs a new `UnicastOrMulticastIpv6Addr` from a specified address.
    pub fn from_specified(addr: SpecifiedAddr<Ipv6Addr>) -> UnicastOrMulticastIpv6Addr {
        if addr.is_unicast() {
            UnicastOrMulticastIpv6Addr::Unicast(UnicastAddr(addr.into_addr()))
        } else {
            UnicastOrMulticastIpv6Addr::Multicast(MulticastAddr(addr.into_addr()))
        }
    }
}

impl crate::sealed::Sealed for UnicastOrMulticastIpv6Addr {}

impl Witness<Ipv6Addr> for UnicastOrMulticastIpv6Addr {
    #[inline]
    fn new(addr: Ipv6Addr) -> Option<UnicastOrMulticastIpv6Addr> {
        UnicastOrMulticastIpv6Addr::new(addr)
    }

    #[inline]
    unsafe fn new_unchecked(addr: Ipv6Addr) -> UnicastOrMulticastIpv6Addr {
        UnicastOrMulticastIpv6Addr::new(addr).unwrap()
    }

    #[inline]
    fn into_addr(self) -> Ipv6Addr {
        match self {
            UnicastOrMulticastIpv6Addr::Unicast(addr) => addr.into_addr(),
            UnicastOrMulticastIpv6Addr::Multicast(addr) => addr.into_addr(),
        }
    }
}

impl UnicastAddress for UnicastOrMulticastIpv6Addr {
    fn is_unicast(&self) -> bool {
        matches!(self, UnicastOrMulticastIpv6Addr::Unicast(_))
    }
}

impl MulticastAddress for UnicastOrMulticastIpv6Addr {
    fn is_multicast(&self) -> bool {
        matches!(self, UnicastOrMulticastIpv6Addr::Multicast(_))
    }
}

impl LinkLocalAddress for UnicastOrMulticastIpv6Addr {
    fn is_linklocal(&self) -> bool {
        match self {
            UnicastOrMulticastIpv6Addr::Unicast(addr) => addr.is_linklocal(),
            UnicastOrMulticastIpv6Addr::Multicast(addr) => addr.is_linklocal(),
        }
    }
}

impl From<UnicastOrMulticastIpv6Addr> for Ipv6Addr {
    fn from(addr: UnicastOrMulticastIpv6Addr) -> Ipv6Addr {
        addr.into_addr()
    }
}

impl From<&'_ UnicastOrMulticastIpv6Addr> for Ipv6Addr {
    fn from(addr: &UnicastOrMulticastIpv6Addr) -> Ipv6Addr {
        addr.get()
    }
}

impl From<UnicastAddr<Ipv6Addr>> for UnicastOrMulticastIpv6Addr {
    fn from(addr: UnicastAddr<Ipv6Addr>) -> UnicastOrMulticastIpv6Addr {
        UnicastOrMulticastIpv6Addr::Unicast(addr)
    }
}

impl From<MulticastAddr<Ipv6Addr>> for UnicastOrMulticastIpv6Addr {
    fn from(addr: MulticastAddr<Ipv6Addr>) -> UnicastOrMulticastIpv6Addr {
        UnicastOrMulticastIpv6Addr::Multicast(addr)
    }
}

impl TryFrom<Ipv6Addr> for UnicastOrMulticastIpv6Addr {
    type Error = ();
    fn try_from(addr: Ipv6Addr) -> Result<UnicastOrMulticastIpv6Addr, ()> {
        UnicastOrMulticastIpv6Addr::new(addr).ok_or(())
    }
}

impl AsRef<Ipv6Addr> for UnicastOrMulticastIpv6Addr {
    fn as_ref(&self) -> &Ipv6Addr {
        match self {
            UnicastOrMulticastIpv6Addr::Unicast(addr) => addr,
            UnicastOrMulticastIpv6Addr::Multicast(addr) => addr,
        }
    }
}

impl Deref for UnicastOrMulticastIpv6Addr {
    type Target = Ipv6Addr;

    fn deref(&self) -> &Ipv6Addr {
        self.as_ref()
    }
}

impl Display for UnicastOrMulticastIpv6Addr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        match self {
            UnicastOrMulticastIpv6Addr::Unicast(addr) => write!(f, "{}", addr),
            UnicastOrMulticastIpv6Addr::Multicast(addr) => write!(f, "{}", addr),
        }
    }
}

impl Debug for UnicastOrMulticastIpv6Addr {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// The error returned from [`Subnet::new`] and [`SubnetEither::new`].
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum SubnetError {
    /// The network prefix is longer than the number of bits in the address (32
    /// for IPv4/128 for IPv6).
    PrefixTooLong,
    /// The network address has some bits in the host part (past the network
    /// prefix) set to one.
    HostBitsSet,
}

/// An IP subnet.
///
/// `Subnet` is a combination of an IP network address and a prefix length.
#[derive(Copy, Clone, Eq, PartialEq, Hash)]
pub struct Subnet<A> {
    // invariant: only contains prefix bits
    network: A,
    prefix: u8,
}

// TODO(joshlf): Currently, we need a separate new_unchecked because trait
// bounds other than Sized are not supported in const fns. Once that
// restriction is lifted, we can make new a const fn.

impl<A> Subnet<A> {
    /// Creates a new subnet without enforcing correctness.
    ///
    /// # Safety
    ///
    /// Unlike `new`, `new_unchecked` does not validate that `prefix` is in the
    /// proper range, and does not check that `network` has only the top
    /// `prefix` bits set. It is up to the caller to guarantee that `prefix` is
    /// in the proper range, and that none of the bits of `network` beyond the
    /// prefix are set.
    #[inline]
    pub const unsafe fn new_unchecked(network: A, prefix: u8) -> Subnet<A> {
        Subnet { network, prefix }
    }
}

impl<A: IpAddress> Subnet<A> {
    /// Creates a new subnet.
    ///
    /// `new` creates a new subnet with the given network address and prefix
    /// length. It returns `Err` if `prefix` is longer than the number of bits
    /// in this type of IP address (32 for IPv4 and 128 for IPv6) or if any of
    /// the host bits (beyond the first `prefix` bits) are set in `network`.
    #[inline]
    pub fn new(network: A, prefix: u8) -> Result<Subnet<A>, SubnetError> {
        if prefix > A::BYTES * 8 {
            return Err(SubnetError::PrefixTooLong);
        }
        // TODO(joshlf): Is there a more efficient way we can perform this
        // check?
        if network != network.mask(prefix) {
            return Err(SubnetError::HostBitsSet);
        }
        Ok(Subnet { network, prefix })
    }

    /// Gets the network address component of this subnet.
    ///
    /// `network` returns the network address component of this subnet. Any bits
    /// beyond the prefix will be zero.
    #[inline]
    pub fn network(&self) -> A {
        self.network
    }

    /// Gets the prefix length component of this subnet.
    #[inline]
    pub fn prefix(&self) -> u8 {
        self.prefix
    }

    /// Tests whether an address is in this subnet.
    ///
    /// Tests whether `address` is in this subnet by testing whether the prefix
    /// bits match the prefix bits of the subnet's network address. This is
    /// equivalent to `subnet.network() == address.mask(subnet.prefix())`.
    #[inline]
    pub fn contains(&self, address: &A) -> bool {
        self.network == address.mask(self.prefix)
    }
}

impl Subnet<Ipv4Addr> {
    // TODO(joshlf): Introduce a `BroadcastAddr` witness type, and have
    // `broadcast` return `BroadcastAddr<Ipv4Addr>`.

    /// Gets the broadcast address in this IPv4 subnet.
    #[inline]
    pub fn broadcast(self) -> Ipv4Addr {
        if self.prefix == 32 {
            // shifting right by the size of the value is undefined
            self.network
        } else {
            let mask = <u32>::max_value() >> self.prefix;
            Ipv4Addr::new((u32::from_be_bytes(self.network.0) | mask).to_be_bytes())
        }
    }
}

impl<A: IpAddress> Display for Subnet<A> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        write!(f, "{}/{}", self.network, self.prefix)
    }
}

impl<A: IpAddress> Debug for Subnet<A> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        write!(f, "{}/{}", self.network, self.prefix)
    }
}

/// An IPv4 subnet or an IPv6 subnet.
///
/// `SubnetEither` is an enum of [`Subnet<Ipv4Addr>`] and `Subnet<Ipv6Addr>`.
///
/// [`Subnet<Ipv4Addr>`]: crate::ip::Subnet
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub enum SubnetEither {
    V4(Subnet<Ipv4Addr>),
    V6(Subnet<Ipv6Addr>),
}

impl SubnetEither {
    /// Creates a new subnet.
    ///
    /// `new` creates a new subnet with the given network address and prefix
    /// length. It returns `Err` if `prefix` is longer than the number of bits
    /// in this type of IP address (32 for IPv4 and 128 for IPv6) or if any of
    /// the host bits (beyond the first `prefix` bits) are set in `network`.
    #[inline]
    pub fn new(network: IpAddr, prefix: u8) -> Result<SubnetEither, SubnetError> {
        Ok(match network {
            IpAddr::V4(network) => SubnetEither::V4(Subnet::new(network, prefix)?),
            IpAddr::V6(network) => SubnetEither::V6(Subnet::new(network, prefix)?),
        })
    }

    /// Gets the network and prefix for this `SubnetEither`.
    #[inline]
    pub fn into_net_prefix(self) -> (IpAddr, u8) {
        match self {
            SubnetEither::V4(v4) => (v4.network.into(), v4.prefix),
            SubnetEither::V6(v6) => (v6.network.into(), v6.prefix),
        }
    }
}

impl<A: IpAddress> From<Subnet<A>> for SubnetEither {
    fn from(subnet: Subnet<A>) -> SubnetEither {
        A::subnet_into_either(subnet)
    }
}

/// The error returned from [`AddrSubnet::new`] and [`AddrSubnetEither::new`].
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum AddrSubnetError {
    /// The network prefix is longer than the number of bits in the address (32
    /// for IPv4/128 for IPv6).
    PrefixTooLong,
    /// The address is not a unicast address in the given subnet (see
    /// [`IpAddress::is_unicast_in_subnet`]).
    NotUnicastInSubnet,
    /// The address does not satisfy the requirements of the witness type.
    InvalidWitness,
}

// TODO(joshlf): Is the unicast restriction always necessary, or will some users
// want the AddrSubnet functionality without that restriction?

/// An address and that address' subnet.
///
/// An `AddrSubnet` is a pair of an address and a subnet which maintains the
/// invariant that the address is guaranteed to be a unicast address in the
/// subnet. `S` is the type of address ([`Ipv4Addr`] or [`Ipv6Addr`]), and `A`
/// is the type of the address in the subnet, which is always a witness wrapper
/// around `S`. By default, it is `SpecifiedAddr<S>`.
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub struct AddrSubnet<S: IpAddress, A: Witness<S> = SpecifiedAddr<S>> {
    // TODO(joshlf): Would it be more performant to store these as just an
    // address and subnet mask? It would make the object smaller and so cheaper
    // to pass around, but it would make certain operations more expensive.
    addr: A,
    subnet: Subnet<S>,
}

impl<S: IpAddress, A: Witness<S>> AddrSubnet<S, A> {
    /// Creates a new `AddrSubnet`.
    ///
    /// `new` is like [`from_witness`], except that it also converts `addr` into
    /// the appropriate witness type, returning
    /// [`AddrSubnetError::InvalidWitness`] if the conversion fails.
    ///
    /// [`from_witness`]: Witness::from_witness
    #[inline]
    pub fn new(addr: S, prefix: u8) -> Result<AddrSubnet<S, A>, AddrSubnetError> {
        AddrSubnet::from_witness(A::new(addr).ok_or(AddrSubnetError::InvalidWitness)?, prefix)
    }

    /// Creates a new `AddrSubnet` from an existing witness.
    ///
    /// `from_witness` creates a new `AddrSubnet` with the given address and
    /// prefix length. The network address of the subnet is taken to be the
    /// first `prefix` bits of the address. It returns `Err` if `prefix` is
    /// longer than the number of bits in this type of IP address (32 for IPv4
    /// and 128 for IPv6) or if `addr` is not a unicast address in the resulting
    /// subnet (see [`IpAddress::is_unicast_in_subnet`]).
    pub fn from_witness(addr: A, prefix: u8) -> Result<AddrSubnet<S, A>, AddrSubnetError> {
        if prefix > S::BYTES * 8 {
            return Err(AddrSubnetError::PrefixTooLong);
        }
        let subnet = Subnet { network: addr.as_ref().mask(prefix), prefix };
        if !addr.as_ref().is_unicast_in_subnet(&subnet) {
            return Err(AddrSubnetError::NotUnicastInSubnet);
        }
        Ok(AddrSubnet { addr, subnet })
    }

    /// Gets the subnet.
    #[inline]
    pub fn subnet(&self) -> Subnet<S> {
        self.subnet
    }

    /// Consumes the `AddrSubnet` and returns the address.
    #[inline]
    pub fn into_addr(self) -> A {
        self.addr
    }

    /// Consumes the `AddrSubnet` and returns the subnet.
    #[inline]
    pub fn into_subnet(self) -> Subnet<S> {
        self.subnet
    }

    /// Consumes the `AddrSubnet` and returns the address and subnet
    /// individually.
    #[inline]
    pub fn into_addr_subnet(self) -> (A, Subnet<S>) {
        (self.addr, self.subnet)
    }

    /// Converts the `AddrSubnet` into an `AddrSubnet` of a different witness
    /// type.
    #[inline]
    pub fn into_witness<B: Witness<S>>(self) -> AddrSubnet<S, B>
    where
        A: Into<B>,
    {
        AddrSubnet { addr: self.addr.into(), subnet: self.subnet }
    }
}

impl<S: IpAddress, A: Witness<S> + Copy> AddrSubnet<S, A> {
    /// Gets the address.
    #[inline]
    pub fn addr(&self) -> A {
        self.addr
    }
}

impl<A: Witness<Ipv6Addr> + Copy> AddrSubnet<Ipv6Addr, A> {
    /// Gets the address as a [`UnicastAddr`] witness.
    ///
    /// Since one of the invariants on an `AddrSubnet` is that its contained
    /// address is unicast in its subnet, `ipv6_unicast_addr` can infallibly
    /// convert its stored address to a `UnicastAddr`.
    pub fn ipv6_unicast_addr(&self) -> UnicastAddr<Ipv6Addr> {
        unsafe { UnicastAddr::new_unchecked(self.addr.get()) }
    }

    /// Converts this `AddrSubnet` into one storing a [`UnicastAddr`] witness.
    ///
    /// Since one of the invariants on an `AddrSubnet` is that its contained
    /// address is unicast in its subnet, `into_unicast` can infallibly convert
    /// its stored address to a `UnicastAddr`.
    pub fn into_unicast(self) -> AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>> {
        let AddrSubnet { addr, subnet } = self;
        let addr = unsafe { UnicastAddr::new_unchecked(addr.get()) };
        AddrSubnet { addr, subnet }
    }
}

/// A type which is a witness to some property about an `IpAddress`.
///
/// `IpAddrWitness` extends [`Witness`] of [`IpAddr`] by adding associated types
/// for the IPv4- and IPv6-specific versions of the same witness type. For
/// example, the following implementation is provided for
/// `SpecifiedAddr<IpAddr>`:
///
/// ```rust,ignore
/// impl IpAddrWitness for SpecifiedAddr<IpAddr> {
///     type V4 = SpecifiedAddr<Ipv4Addr>;
///     type V6 = SpecifiedAddr<Ipv6Addr>;
/// }
/// ```
pub trait IpAddrWitness: Witness<IpAddr> {
    /// The IPv4-specific version of `Self`.
    ///
    /// For example, `SpecifiedAddr<IpAddr>: IpAddrWitness<V4 =
    /// SpecifiedAddr<Ipv4Addr>>`.
    type V4: Witness<Ipv4Addr> + Into<Self>;

    /// The IPv6-specific version of `Self`.
    ///
    /// For example, `SpecifiedAddr<IpAddr>: IpAddrWitness<V6 =
    /// SpecifiedAddr<Ipv6Addr>>`.
    type V6: Witness<Ipv6Addr> + Into<Self>;

    // TODO(https://github.com/rust-lang/rust/issues/44491): Remove these
    // functions once implied where bounds make them unnecessary.

    /// Converts an IPv4-specific witness into a general witness.
    fn from_v4(addr: Self::V4) -> Self {
        addr.into()
    }

    /// Converts an IPv6-specific witness into a general witness.
    fn from_v6(addr: Self::V6) -> Self {
        addr.into()
    }
}

macro_rules! impl_ip_addr_witness {
    ($witness:ident) => {
        impl IpAddrWitness for $witness<IpAddr> {
            type V4 = $witness<Ipv4Addr>;
            type V6 = $witness<Ipv6Addr>;
        }
    };
}

impl_ip_addr_witness!(SpecifiedAddr);
impl_ip_addr_witness!(MulticastAddr);
impl_ip_addr_witness!(LinkLocalAddr);

/// An address and that address' subnet, either IPv4 or IPv6.
///
/// `AddrSubnetEither` is an enum of [`AddrSubnet<Ipv4Addr>`] and
/// `AddrSubnet<Ipv6Addr>`.
///
/// [`AddrSubnet<Ipv4Addr>`]: crate::ip::AddrSubnet
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub enum AddrSubnetEither<A: IpAddrWitness = SpecifiedAddr<IpAddr>> {
    V4(AddrSubnet<Ipv4Addr, A::V4>),
    V6(AddrSubnet<Ipv6Addr, A::V6>),
}

impl<A: IpAddrWitness> AddrSubnetEither<A> {
    /// Creates a new `AddrSubnetEither`.
    ///
    /// `new` creates a new `AddrSubnetEither` with the given address and prefix
    /// length. It returns `Err` under the same conditions as
    /// [`AddrSubnet::new`].
    #[inline]
    pub fn new(addr: IpAddr, prefix: u8) -> Result<AddrSubnetEither<A>, AddrSubnetError> {
        Ok(match addr {
            IpAddr::V4(addr) => AddrSubnetEither::V4(AddrSubnet::new(addr, prefix)?),
            IpAddr::V6(addr) => AddrSubnetEither::V6(AddrSubnet::new(addr, prefix)?),
        })
    }

    /// Gets the contained IP address and prefix in this `AddrSubnetEither`.
    #[inline]
    pub fn into_addr_prefix(self) -> (A, u8) {
        match self {
            AddrSubnetEither::V4(v4) => (v4.addr.into(), v4.subnet.prefix),
            AddrSubnetEither::V6(v6) => (v6.addr.into(), v6.subnet.prefix),
        }
    }

    /// Gets the IP address and subnet in this `AddrSubnetEither`.
    #[inline]
    pub fn into_addr_subnet(self) -> (A, SubnetEither) {
        match self {
            AddrSubnetEither::V4(v4) => (v4.addr.into(), SubnetEither::V4(v4.subnet)),
            AddrSubnetEither::V6(v6) => (v6.addr.into(), SubnetEither::V6(v6.subnet)),
        }
    }
}

/// Helper function to calculate common prefix length in an iterator of tuple of
/// bytes.
fn common_prefix_len(it: impl Iterator<Item = (u8, u8)>) -> u8 {
    let mut len = 0;
    for (a, b) in it {
        let v = u8::leading_ones(!(a ^ b));
        // Cast to u8 is always safe because leading ones can't return more than
        // 8.
        len += v as u8;
        if v != 8 {
            break;
        }
    }
    len
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_loopback_unicast() {
        // The loopback addresses are constructed as `SpecifiedAddr`s directly,
        // bypassing the actual check against `is_specified`. Test that that's
        // actually valid.
        assert!(Ipv4::LOOPBACK_ADDRESS.0.is_specified());
        assert!(Ipv6::LOOPBACK_ADDRESS.0.is_specified());
    }

    #[test]
    fn test_specified() {
        // For types that implement SpecifiedAddress,
        // UnicastAddress::is_unicast, MulticastAddress::is_multicast, and
        // LinkLocalAddress::is_linklocal all imply
        // SpecifiedAddress::is_specified. Test that that's true for both IPv4
        // and IPv6.

        assert!(!Ipv6::UNSPECIFIED_ADDRESS.is_specified());
        assert!(!Ipv4::UNSPECIFIED_ADDRESS.is_specified());

        // Unicast

        assert!(!Ipv6::UNSPECIFIED_ADDRESS.is_unicast());

        let unicast = Ipv6Addr([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]);
        assert!(unicast.is_unicast());
        assert!(unicast.is_specified());

        // Multicast

        assert!(!Ipv4::UNSPECIFIED_ADDRESS.is_multicast());
        assert!(!Ipv6::UNSPECIFIED_ADDRESS.is_multicast());

        let multicast = Ipv4::MULTICAST_SUBNET.network;
        assert!(multicast.is_multicast());
        assert!(multicast.is_specified());
        let multicast = Ipv6::MULTICAST_SUBNET.network;
        assert!(multicast.is_multicast());
        assert!(multicast.is_specified());

        // Link-local

        assert!(!Ipv4::UNSPECIFIED_ADDRESS.is_linklocal());
        assert!(!Ipv6::UNSPECIFIED_ADDRESS.is_linklocal());

        let link_local = Ipv4::LINK_LOCAL_UNICAST_SUBNET.network;
        assert!(link_local.is_linklocal());
        assert!(link_local.is_specified());
        let link_local = Ipv4::LINK_LOCAL_MULTICAST_SUBNET.network;
        assert!(link_local.is_linklocal());
        assert!(link_local.is_specified());
        let link_local = Ipv6::LINK_LOCAL_UNICAST_SUBNET.network;
        assert!(link_local.is_linklocal());
        assert!(link_local.is_specified());
        let mut link_local = Ipv6::MULTICAST_SUBNET.network;
        link_local.0[1] = 0x02;
        assert!(link_local.is_linklocal());
        assert!(link_local.is_specified());
        assert!(Ipv6::LOOPBACK_ADDRESS.is_linklocal());
    }

    #[test]
    fn test_linklocal() {
        // IPv4
        assert!(Ipv4::LINK_LOCAL_UNICAST_SUBNET.network.is_linklocal());
        assert!(Ipv4::LINK_LOCAL_MULTICAST_SUBNET.network.is_linklocal());

        // IPv6
        assert!(Ipv6::LINK_LOCAL_UNICAST_SUBNET.network.is_linklocal());
        assert!(Ipv6::LINK_LOCAL_UNICAST_SUBNET.network.is_unicast_linklocal());
        let mut addr = Ipv6::MULTICAST_SUBNET.network;
        for flags in 0..=0x0F {
            // Set the scope to link-local and the flags to `flags`.
            addr.0[1] = (flags << 4) | 0x02;
            // Test that a link-local multicast address is always considered
            // link-local regardless of which flags are set.
            assert!(addr.is_linklocal());
            assert!(!addr.is_unicast_linklocal());
        }

        // Test that a non-multicast address (outside of the link-local subnet)
        // is never considered link-local even if the bits are set that, in a
        // multicast address, would put it in the link-local scope.
        let mut addr = Ipv6::LOOPBACK_ADDRESS.get();
        // Explicitly set the scope to link-local.
        addr.0[1] = 0x02;
        assert!(!addr.is_linklocal());
    }

    #[test]
    fn test_subnet_new() {
        Subnet::new(Ipv4Addr::new([255, 255, 255, 255]), 32).unwrap();
        // Prefix exceeds 32 bits
        assert_eq!(
            Subnet::new(Ipv4Addr::new([255, 255, 0, 0]), 33),
            Err(SubnetError::PrefixTooLong)
        );
        // Network address has more than top 8 bits set
        assert_eq!(Subnet::new(Ipv4Addr::new([255, 255, 0, 0]), 8), Err(SubnetError::HostBitsSet));

        AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv4Addr::new([1, 2, 3, 4]), 32).unwrap();
        // The unspecified address will always fail because it is not valid for
        // the `SpecifiedAddr` witness (use assert, not assert_eq, because
        // AddrSubnet doesn't impl Debug).
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv4::UNSPECIFIED_ADDRESS, 16)
                == Err(AddrSubnetError::InvalidWitness)
        );
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv6::UNSPECIFIED_ADDRESS, 64)
                == Err(AddrSubnetError::InvalidWitness)
        );
        // Prefix exceeds 32/128 bits
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv4Addr::new([1, 2, 3, 4]), 33)
                == Err(AddrSubnetError::PrefixTooLong)
        );
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(
                Ipv6Addr::new([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]),
                129,
            ) == Err(AddrSubnetError::PrefixTooLong)
        );
        // Global broadcast
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv4::GLOBAL_BROADCAST_ADDRESS.into_addr(), 16)
                == Err(AddrSubnetError::NotUnicastInSubnet)
        );
        // Subnet broadcast
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv4Addr::new([192, 168, 255, 255]), 16)
                == Err(AddrSubnetError::NotUnicastInSubnet)
        );
        // Multicast
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(Ipv4Addr::new([224, 0, 0, 1]), 16)
                == Err(AddrSubnetError::NotUnicastInSubnet)
        );
        assert!(
            AddrSubnet::<_, SpecifiedAddr<_>>::new(
                Ipv6Addr::new([0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
                64,
            ) == Err(AddrSubnetError::NotUnicastInSubnet)
        );

        // If we use the `LinkLocalAddr` witness type, then non link-local
        // addresses are rejected. Note that this address was accepted above
        // when `SpecifiedAddr` was used.
        assert!(
            AddrSubnet::<_, LinkLocalAddr<Ipv4Addr>>::new(Ipv4Addr::new([1, 2, 3, 4]), 32)
                == Err(AddrSubnetError::InvalidWitness)
        );
    }

    #[test]
    fn test_is_unicast_in_subnet() {
        // Valid unicast in subnet
        let subnet =
            Subnet::new(Ipv4Addr::new([1, 2, 0, 0]), 16).expect("1.2.0.0/16 is a valid subnet");
        assert!(Ipv4Addr::new([1, 2, 3, 4]).is_unicast_in_subnet(&subnet));
        assert!(!Ipv4Addr::new([2, 2, 3, 4]).is_unicast_in_subnet(&subnet));

        let subnet =
            Subnet::new(Ipv6Addr::new([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), 64)
                .expect("1::/64 is a valid subnet");
        assert!(Ipv6Addr::new([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1])
            .is_unicast_in_subnet(&subnet));
        assert!(!Ipv6Addr::new([2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1])
            .is_unicast_in_subnet(&subnet));

        // Unspecified address
        assert!(!Ipv4::UNSPECIFIED_ADDRESS
            .is_unicast_in_subnet(&Subnet::new(Ipv4::UNSPECIFIED_ADDRESS, 16).unwrap()));
        assert!(!Ipv6::UNSPECIFIED_ADDRESS
            .is_unicast_in_subnet(&Subnet::new(Ipv6::UNSPECIFIED_ADDRESS, 64).unwrap()));
        // The "31- or 32-bit prefix" exception doesn't apply to the unspecified
        // address (IPv4 only).
        assert!(!Ipv4::UNSPECIFIED_ADDRESS
            .is_unicast_in_subnet(&Subnet::new(Ipv4::UNSPECIFIED_ADDRESS, 31).unwrap()));
        assert!(!Ipv4::UNSPECIFIED_ADDRESS
            .is_unicast_in_subnet(&Subnet::new(Ipv4::UNSPECIFIED_ADDRESS, 32).unwrap()));
        // All-zeroes host part (IPv4 only)
        assert!(!Ipv4Addr::new([1, 2, 0, 0])
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([1, 2, 0, 0]), 16).unwrap()));
        // Exception: 31- or 32-bit prefix (IPv4 only)
        assert!(Ipv4Addr::new([1, 2, 3, 0])
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([1, 2, 3, 0]), 31).unwrap()));
        assert!(Ipv4Addr::new([1, 2, 3, 0])
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([1, 2, 3, 0]), 32).unwrap()));
        // Global broadcast (IPv4 only)
        assert!(!Ipv4::GLOBAL_BROADCAST_ADDRESS
            .into_addr()
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([128, 0, 0, 0]), 1).unwrap()));
        // Subnet broadcast (IPv4 only)
        assert!(!Ipv4Addr::new([1, 2, 255, 255])
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([1, 2, 0, 0]), 16).unwrap()));
        // Exception: 31- or 32-bit prefix (IPv4 only)
        assert!(Ipv4Addr::new([1, 2, 255, 255])
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([1, 2, 255, 254]), 31).unwrap()));
        assert!(Ipv4Addr::new([1, 2, 255, 255])
            .is_unicast_in_subnet(&Subnet::new(Ipv4Addr::new([1, 2, 255, 255]), 32).unwrap()));
        // Multicast
        assert!(!Ipv4Addr::new([224, 0, 0, 1]).is_unicast_in_subnet(&Ipv4::MULTICAST_SUBNET));
        assert!(!Ipv6Addr::new([0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1])
            .is_unicast_in_subnet(&Ipv6::MULTICAST_SUBNET));
        // Class E (IPv4 only)
        assert!(!Ipv4Addr::new([240, 0, 0, 1]).is_unicast_in_subnet(&Ipv4::CLASS_E_SUBNET));
    }

    macro_rules! add_mask_test {
            ($name:ident, $addr:ident, $from_ip:expr => {
                $($mask:expr => $to_ip:expr),*
            }) => {
                #[test]
                fn $name() {
                    let from = $addr::new($from_ip);
                    $(assert_eq!(from.mask($mask), $addr::new($to_ip), "(`{}`.mask({}))", from, $mask);)*
                }
            };
            ($name:ident, $addr:ident, $from_ip:expr => {
                $($mask:expr => $to_ip:expr),*,
            }) => {
                add_mask_test!($name, $addr, $from_ip => { $($mask => $to_ip),* });
            };
        }

    add_mask_test!(v4_full_mask, Ipv4Addr, [255, 254, 253, 252] => {
        32 => [255, 254, 253, 252],
        28 => [255, 254, 253, 240],
        24 => [255, 254, 253, 0],
        20 => [255, 254, 240, 0],
        16 => [255, 254, 0,   0],
        12 => [255, 240, 0,   0],
        8  => [255, 0,   0,   0],
        4  => [240, 0,   0,   0],
        0  => [0,   0,   0,   0],
    });

    add_mask_test!(v6_full_mask, Ipv6Addr,
        [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0] => {
            128 => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0],
            112 => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0x00, 0x00],
            96  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0x00, 0x00, 0x00, 0x00],
            80  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
            64  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
            48  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
            32  => [0xFF, 0xFE, 0xFD, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
            16  => [0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
            8   => [0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
            0   => [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
        }
    );

    #[test]
    fn test_ipv6_solicited_node() {
        let addr = Ipv6Addr::new([
            0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x52, 0xe5, 0x49, 0xff, 0xfe, 0xb5, 0x5a, 0xa0,
        ]);
        let solicited =
            Ipv6Addr::new([0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xff, 0xb5, 0x5a, 0xa0]);
        assert_eq!(addr.to_solicited_node_address().get(), solicited);
    }

    #[test]
    fn test_ipv6_address_types() {
        assert!(!Ipv6Addr::new([0; 16]).is_specified());
        assert!(Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]).is_loopback());
        let link_local = Ipv6Addr::new([
            0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x52, 0xe5, 0x49, 0xff, 0xfe, 0xb5, 0x5a, 0xa0,
        ]);
        assert!(link_local.is_linklocal());
        assert!(link_local.is_valid_unicast());
        assert!(link_local.to_solicited_node_address().is_multicast());
        let global_unicast = Ipv6Addr::new([
            0x00, 0x80, 0, 0, 0, 0, 0, 0, 0x52, 0xe5, 0x49, 0xff, 0xfe, 0xb5, 0x5a, 0xa0,
        ]);
        assert!(global_unicast.is_valid_unicast());
        assert!(global_unicast.to_solicited_node_address().is_multicast());

        let multi =
            Ipv6Addr::new([0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xff, 0xb5, 0x5a, 0xa0]);
        assert!(multi.is_multicast());
        assert!(!multi.is_valid_unicast());
    }

    #[test]
    fn test_const_witness() {
        // Test that all of the addresses that we initialize at compile time
        // using `new_unchecked` constructors are valid for their witness types.
        assert!(Ipv4::LOOPBACK_ADDRESS.0.is_specified());
        assert!(Ipv6::LOOPBACK_ADDRESS.0.is_specified());
        assert!(Ipv4::GLOBAL_BROADCAST_ADDRESS.0.is_specified());
        assert!(Ipv4::ALL_ROUTERS_MULTICAST_ADDRESS.0.is_multicast());
        assert!(Ipv6::ALL_NODES_LINK_LOCAL_MULTICAST_ADDRESS.0.is_multicast());
        assert!(Ipv6::ALL_ROUTERS_LINK_LOCAL_MULTICAST_ADDRESS.0.is_multicast());
    }

    #[test]
    fn test_ipv6_scope() {
        use Ipv6ReservedScope::*;
        use Ipv6Scope::*;
        use Ipv6UnassignedScope::*;

        // Test unicast scopes.
        assert_eq!(Ipv6::SITE_LOCAL_UNICAST_SUBNET.network.scope(), SiteLocal);
        assert_eq!(Ipv6::LINK_LOCAL_UNICAST_SUBNET.network.scope(), LinkLocal);
        assert_eq!(Ipv6::UNSPECIFIED_ADDRESS.scope(), Global);

        // Test multicast scopes.
        let assert_scope = |value, scope| {
            let mut addr = Ipv6::MULTICAST_SUBNET.network;
            // Set the "scop" field manually.
            addr.0[1] |= value;
            assert_eq!(addr.scope(), scope);
        };
        assert_scope(0, Reserved(Scope0));
        assert_scope(1, InterfaceLocal);
        assert_scope(2, LinkLocal);
        assert_scope(3, Reserved(Scope3));
        assert_scope(4, AdminLocal);
        assert_scope(5, SiteLocal);
        assert_scope(6, Unassigned(Scope6));
        assert_scope(7, Unassigned(Scope7));
        assert_scope(8, OrganizationLocal);
        assert_scope(9, Unassigned(Scope9));
        assert_scope(0xA, Unassigned(ScopeA));
        assert_scope(0xB, Unassigned(ScopeB));
        assert_scope(0xC, Unassigned(ScopeC));
        assert_scope(0xD, Unassigned(ScopeD));
        assert_scope(0xE, Global);
        assert_scope(0xF, Reserved(ScopeF));
    }

    #[test]
    fn test_ipv6_scope_ord() {
        const ALL_SCOPES: [Ipv6Scope; 16] = [
            Ipv6Scope::Reserved(Ipv6ReservedScope::Scope0),
            Ipv6Scope::InterfaceLocal,
            Ipv6Scope::LinkLocal,
            Ipv6Scope::Reserved(Ipv6ReservedScope::Scope3),
            Ipv6Scope::AdminLocal,
            Ipv6Scope::SiteLocal,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::Scope6),
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::Scope7),
            Ipv6Scope::OrganizationLocal,
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::Scope9),
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeA),
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeB),
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeC),
            Ipv6Scope::Unassigned(Ipv6UnassignedScope::ScopeD),
            Ipv6Scope::Global,
            Ipv6Scope::Reserved(Ipv6ReservedScope::ScopeF),
        ];
        for (i, a) in ALL_SCOPES.iter().enumerate() {
            for (j, b) in ALL_SCOPES.iter().enumerate() {
                assert_eq!(a.cmp(b), i.cmp(&j));
            }
        }
    }

    #[test]
    fn test_ipv6_from_ipv4() {
        assert_eq!(
            Ipv6Addr::from(Ipv4Addr::new([1, 2, 3, 4])),
            Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 1, 2, 3, 4]),
        );
        assert_eq!(
            Ipv6Addr::from(Ipv4Addr::new([192, 168, 0, 1])),
            Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 0, 1]),
        );
        assert_eq!(
            Ipv6Addr::from(Ipv4Addr::new([129, 144, 52, 38])),
            Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 129, 144, 52, 38]),
        );
    }

    #[test]
    fn test_common_prefix_len_ipv6() {
        let ip1 = Ipv6Addr::new([0xFF, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        let ip2 = Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        let ip3 = Ipv6Addr::new([0xFF, 0xFF, 0x80, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        let ip4 = Ipv6Addr::new([0xFF, 0xFF, 0xC0, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        let compare_with_ip1 = |target, expect| {
            assert_eq!(ip1.common_prefix_length(&target), expect, "{} <=> {}", ip1, target);
        };
        let () = compare_with_ip1(ip1, 128);
        let () = compare_with_ip1(ip2, 0);
        let () = compare_with_ip1(ip3, 24);
        let () = compare_with_ip1(ip4, 17);
    }

    #[test]
    fn test_common_prefix_len_ipv4() {
        let ip1 = Ipv4Addr::new([0xFF, 0xFF, 0x80, 0]);
        let ip2 = Ipv4Addr::new([0, 0, 0, 0]);
        let ip3 = Ipv4Addr::new([0xFF, 0xFF, 0x80, 0xFF]);
        let ip4 = Ipv4Addr::new([0xFF, 0xFF, 0xC0, 0x20]);
        let compare_with_ip1 = |target, expect| {
            assert_eq!(ip1.common_prefix_length(&target), expect, "{} <=> {}", ip1, target);
        };
        let () = compare_with_ip1(ip1, 32);
        let () = compare_with_ip1(ip2, 0);
        let () = compare_with_ip1(ip3, 24);
        let () = compare_with_ip1(ip4, 17);
    }
}
