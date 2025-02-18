// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peer_cache.h"

#include <zircon/assert.h>

#include <fbl/function.h>

#include "lib/async/default.h"
#include "src/connectivity/bluetooth/core/bt-host/common/random.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/peer.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/low_energy_scanner.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/types.h"

namespace bt::gap {

namespace {

// Return an address with the same value as given, but with type kBREDR for
// kLEPublic addresses and vice versa.
DeviceAddress GetAliasAddress(const DeviceAddress& address) {
  if (address.type() == DeviceAddress::Type::kBREDR) {
    return {DeviceAddress::Type::kLEPublic, address.value()};
  } else if (address.type() == DeviceAddress::Type::kLEPublic) {
    return {DeviceAddress::Type::kBREDR, address.value()};
  }
  return address;
}

}  // namespace

Peer* PeerCache::NewPeer(const DeviceAddress& address, bool connectable) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  auto* const peer = InsertPeerRecord(RandomPeerId(), address, connectable);
  if (peer) {
    UpdateExpiry(*peer);
    NotifyPeerUpdated(*peer, Peer::NotifyListenersChange::kBondNotUpdated);
  }
  return peer;
}

void PeerCache::ForEach(PeerCallback f) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(f);
  for (const auto& iter : peers_) {
    f(*iter.second.peer());
  }
}

bool PeerCache::AddBondedPeer(BondingData bd) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(bd.address.type() != DeviceAddress::Type::kLEAnonymous);

  const bool bond_le =
      bd.le_pairing_data.peer_ltk || bd.le_pairing_data.local_ltk || bd.le_pairing_data.csrk;
  const bool bond_bredr = bd.bredr_link_key.has_value();

  // |bd.le_pairing_data| must contain either a LTK or CSRK for LE
  // Security Mode 1 or 2.
  if (bd.address.IsLowEnergy() && !bond_le) {
    bt_log(ERROR, "gap-le", "mandatory keys missing: no LTK or CSRK (id: %s)",
           bt_str(bd.identifier));
    return false;
  }

  if (bd.address.IsBrEdr() && !bond_bredr) {
    bt_log(ERROR, "gap-bredr", "mandatory link key missing (id: %s)", bt_str(bd.identifier));
    return false;
  }

  auto* peer = InsertPeerRecord(bd.identifier, bd.address, true);
  if (!peer) {
    return false;
  }

  // A bonded peer must have its identity known.
  peer->set_identity_known(true);

  if (bd.name.has_value()) {
    peer->SetName(bd.name.value());
  }

  if (bond_le) {
    peer->MutLe().SetBondData(bd.le_pairing_data);
    ZX_DEBUG_ASSERT(peer->le()->bonded());

    // Add the peer to the resolving list if it has an IRK.
    if (bd.le_pairing_data.irk) {
      le_resolving_list_.Add(peer->address(), bd.le_pairing_data.irk->value());
    }
  }

  if (bond_bredr) {
    for (auto& service : bd.bredr_services) {
      peer->MutBrEdr().AddService(std::move(service));
    }

    peer->MutBrEdr().SetBondData(*bd.bredr_link_key);
    ZX_DEBUG_ASSERT(peer->bredr()->bonded());
  }

  if (peer->technology() == TechnologyType::kDualMode) {
    address_map_[GetAliasAddress(bd.address)] = bd.identifier;
  }

  ZX_DEBUG_ASSERT(!peer->temporary());
  ZX_DEBUG_ASSERT(peer->bonded());
  bt_log(TRACE, "gap", "restored bonded peer: %s, id: %s", bt_str(bd.address),
         bt_str(bd.identifier));

  // Don't call UpdateExpiry(). Since a bonded peer starts out as
  // non-temporary it is not necessary to ever set up the expiration callback.
  NotifyPeerUpdated(*peer, Peer::NotifyListenersChange::kBondNotUpdated);
  return true;
}

bool PeerCache::StoreLowEnergyBond(PeerId identifier, const sm::PairingData& bond_data) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  auto* peer = FindById(identifier);
  if (!peer) {
    bt_log(DEBUG, "gap-le", "failed to store bond for unknown peer: %s", bt_str(identifier));
    return false;
  }

  // Either a LTK or CSRK is mandatory for bonding (the former is needed for LE
  // Security Mode 1 and the latter is needed for Mode 2).
  if (!bond_data.peer_ltk && !bond_data.local_ltk && !bond_data.csrk) {
    bt_log(DEBUG, "gap-le", "mandatory keys missing: no LTK or CSRK (id: %s)", bt_str(identifier));
    return false;
  }

  if (bond_data.identity_address) {
    auto existing_id = FindIdByAddress(*bond_data.identity_address);
    if (!existing_id) {
      // Map the new address to |peer|. We leave old addresses that map to
      // this peer in the cache in case there are any pending controller
      // procedures that expect them.
      // TODO(armansito): Maybe expire the old address after a while?
      address_map_[*bond_data.identity_address] = identifier;
    } else if (*existing_id != identifier) {
      bt_log(DEBUG, "gap-le", "identity address %s for peer %s belongs to another peer %s!",
             bt_str(*bond_data.identity_address), bt_str(identifier), bt_str(*existing_id));
      return false;
    }
    // We have either created a new mapping or the identity address already
    // maps to this peer.
  }

  // TODO(fxbug.dev/1212): Check that we're not downgrading the security level before
  // overwriting the bond.
  peer->MutLe().SetBondData(bond_data);
  ZX_DEBUG_ASSERT(!peer->temporary());
  ZX_DEBUG_ASSERT(peer->le()->bonded());

  // Add the peer to the resolving list if it has an IRK.
  if (peer->identity_known() && bond_data.irk) {
    le_resolving_list_.Add(peer->address(), bond_data.irk->value());
  }

  if (bond_data.cross_transport_key) {
    peer->StoreBrEdrCrossTransportKey(*bond_data.cross_transport_key);
  }

  // Report the bond for persisting only if the identity of the peer is known.
  if (peer->identity_known()) {
    NotifyPeerBonded(*peer);
  }
  return true;
}

bool PeerCache::StoreBrEdrBond(const DeviceAddress& address, const sm::LTK& link_key) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(address.type() == DeviceAddress::Type::kBREDR);
  auto* peer = FindByAddress(address);
  if (!peer) {
    bt_log(DEBUG, "gap-bredr", "failed to store bond for unknown peer: %s",
           address.ToString().c_str());
    return false;
  }

  // TODO(fxbug.dev/1212): Check that we're not downgrading the security level before
  // overwriting the bond.
  peer->MutBrEdr().SetBondData(link_key);
  ZX_DEBUG_ASSERT(!peer->temporary());
  ZX_DEBUG_ASSERT(peer->bredr()->bonded());

  NotifyPeerBonded(*peer);
  return true;
}

bool PeerCache::SetAutoConnectBehaviorForIntentionalDisconnect(PeerId peer_id) {
  Peer* const peer = FindById(peer_id);
  if (!peer) {
    bt_log(INFO, "gap-le",
           "failed to update auto-connect behavior (disconnect) for unknown peer: %s",
           bt_str(peer_id));
    return false;
  }

  bt_log(DEBUG, "gap-le", "updated auto-connect behavior (disconnect) for peer: %s",
         bt_str(peer_id));

  peer->MutLe().set_auto_connect_behavior(Peer::AutoConnectBehavior::kSkipUntilNextConnection);

  // TODO(fxbug.dev/37584): When implementing auto-connect behavior tracking for classic bluetooth,
  // consider tracking this policy for the peer as a whole unless we think this policy should be
  // applied separately for each transport (per armansito@).

  return true;
}

bool PeerCache::SetAutoConnectBehaviorForSuccessfulConnection(PeerId peer_id) {
  Peer* const peer = FindById(peer_id);
  if (!peer) {
    bt_log(INFO, "gap-le",
           "failed to update auto-connect behavior (connection) for unknown peer: %s",
           bt_str(peer_id));
    return false;
  }

  bt_log(DEBUG, "gap-le", "updated auto-connect behavior (connection) for peer: %s",
         bt_str(peer_id));

  peer->MutLe().set_auto_connect_behavior(Peer::AutoConnectBehavior::kAlways);

  // TODO(fxbug.dev/37584): Implement auto-connect behavior tracking for classic bluetooth.

  return true;
}

bool PeerCache::RemoveDisconnectedPeer(PeerId peer_id) {
  Peer* const peer = FindById(peer_id);
  if (!peer) {
    return true;
  }

  if (peer->connected()) {
    return false;
  }

  RemovePeer(peer);
  return true;
}

Peer* PeerCache::FindById(PeerId peer_id) const {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  auto iter = peers_.find(peer_id);
  return iter != peers_.end() ? iter->second.peer() : nullptr;
}

Peer* PeerCache::FindByAddress(const DeviceAddress& in_address) const {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());

  std::optional<DeviceAddress> address;
  if (in_address.IsResolvablePrivate()) {
    address = le_resolving_list_.Resolve(in_address);
  }

  // Fall back to the input if an identity wasn't resolved.
  if (!address) {
    address = in_address;
  }

  ZX_DEBUG_ASSERT(address);
  auto identifier = FindIdByAddress(*address);
  if (!identifier) {
    return nullptr;
  }

  auto* p = FindById(*identifier);
  ZX_DEBUG_ASSERT(p);
  return p;
}

void PeerCache::AttachInspect(inspect::Node& parent, std::string name) {
  node_ = parent.CreateChild(name);

  if (!node_) {
    return;
  }

  for (auto& [_, record] : peers_) {
    record.peer()->AttachInspect(node_, node_.UniqueName("peer_"));
  }
}

// Private methods below.

Peer* PeerCache::InsertPeerRecord(PeerId identifier, const DeviceAddress& address,
                                  bool connectable) {
  if (FindIdByAddress(address)) {
    bt_log(WARN, "gap", "tried to insert peer with existing address: %s",
           address.ToString().c_str());
    return nullptr;
  }

  std::unique_ptr<Peer> peer(new Peer(fit::bind_member(this, &PeerCache::NotifyPeerUpdated),
                                      fit::bind_member(this, &PeerCache::UpdateExpiry),
                                      fit::bind_member(this, &PeerCache::MakeDualMode), identifier,
                                      address, connectable));
  if (node_) {
    peer->AttachInspect(node_, node_.UniqueName("peer_"));
  }

  // Note: we must construct the PeerRecord in-place, because it doesn't
  // support copy or move.
  auto [iter, inserted] = peers_.try_emplace(peer->identifier(), std::move(peer),
                                             [this, p = peer.get()] { RemovePeer(p); });
  if (!inserted) {
    bt_log(WARN, "gap", "tried to insert peer with existing ID: %s", bt_str(identifier));
    return nullptr;
  }

  address_map_[address] = identifier;
  return iter->second.peer();
}

void PeerCache::NotifyPeerBonded(const Peer& peer) {
  ZX_DEBUG_ASSERT(peers_.find(peer.identifier()) != peers_.end());
  ZX_DEBUG_ASSERT(peers_.at(peer.identifier()).peer() == &peer);
  ZX_DEBUG_ASSERT_MSG(peer.identity_known(), "peers not allowed to bond with unknown identity!");

  bt_log(INFO, "gap", "peer bonded %s", peer.ToString().c_str());
  if (peer_bonded_callback_) {
    peer_bonded_callback_(peer);
  }
}

void PeerCache::NotifyPeerUpdated(const Peer& peer, Peer::NotifyListenersChange change) {
  ZX_DEBUG_ASSERT(peers_.find(peer.identifier()) != peers_.end());
  ZX_DEBUG_ASSERT(peers_.at(peer.identifier()).peer() == &peer);
  if (peer_updated_callback_) {
    peer_updated_callback_(peer);
  }
  if (change == Peer::NotifyListenersChange::kBondUpdated) {
    ZX_ASSERT(peer.bonded());
    bt_log(INFO, "gap", "peer bond updated %s", bt_str(peer));
    if (peer_bonded_callback_) {
      peer_bonded_callback_(peer);
    }
  }
}

void PeerCache::UpdateExpiry(const Peer& peer) {
  auto peer_record_iter = peers_.find(peer.identifier());
  ZX_DEBUG_ASSERT(peer_record_iter != peers_.end());

  auto& peer_record = peer_record_iter->second;
  ZX_DEBUG_ASSERT(peer_record.peer() == &peer);

  const auto cancel_res = peer_record.removal_task()->Cancel();
  ZX_DEBUG_ASSERT(cancel_res == ZX_OK || cancel_res == ZX_ERR_NOT_FOUND);

  // Previous expiry task has been canceled. Re-schedule only if the peer is
  // temporary.
  if (peer.temporary()) {
    const auto schedule_res =
        peer_record.removal_task()->PostDelayed(async_get_default_dispatcher(), kCacheTimeout);
    ZX_DEBUG_ASSERT(schedule_res == ZX_OK || schedule_res == ZX_ERR_BAD_STATE);
  }
}

void PeerCache::MakeDualMode(const Peer& peer) {
  ZX_ASSERT(address_map_.at(peer.address()) == peer.identifier());
  const auto address_alias = GetAliasAddress(peer.address());
  auto [iter, inserted] = address_map_.try_emplace(address_alias, peer.identifier());
  ZX_ASSERT_MSG(inserted || iter->second == peer.identifier(),
                "%s can't become dual-mode because %s maps to %s", bt_str(peer.identifier()),
                bt_str(address_alias), bt_str(iter->second));
  bt_log(INFO, "gap", "peer became dual mode (peer: %s, address: %s, alias: %s)",
         bt_str(peer.identifier()), bt_str(peer.address()), bt_str(address_alias));

  // The peer became dual mode in lieu of adding a new peer but is as
  // significant, so notify listeners of the change.
  NotifyPeerUpdated(peer, Peer::NotifyListenersChange::kBondNotUpdated);
}

void PeerCache::RemovePeer(Peer* peer) {
  ZX_DEBUG_ASSERT(peer);

  auto peer_record_it = peers_.find(peer->identifier());
  ZX_DEBUG_ASSERT(peer_record_it != peers_.end());
  ZX_DEBUG_ASSERT(peer_record_it->second.peer() == peer);

  PeerId id = peer->identifier();
  bt_log(TRACE, "gap", "removing peer %s", bt_str(id));
  for (auto iter = address_map_.begin(); iter != address_map_.end();) {
    if (iter->second == id) {
      iter = address_map_.erase(iter);
    } else {
      iter++;
    }
  }

  if (peer->le() && peer->le()->bonded()) {
    if (auto& address = peer->le()->bond_data()->identity_address) {
      le_resolving_list_.Remove(*address);
    }
  }

  peers_.erase(peer_record_it);  // Destroys |peer|.
  if (peer_removed_callback_) {
    peer_removed_callback_(id);
  }
}

std::optional<PeerId> PeerCache::FindIdByAddress(const DeviceAddress& address) const {
  auto iter = address_map_.find(address);
  if (iter == address_map_.end()) {
    // Search again using the other technology's address. This is necessary when
    // a dual-mode peer is known by only one technology and is then discovered
    // or connected on its other technology.
    iter = address_map_.find(GetAliasAddress(address));
  }

  if (iter == address_map_.end()) {
    return {};
  }
  return {iter->second};
}

}  // namespace bt::gap
