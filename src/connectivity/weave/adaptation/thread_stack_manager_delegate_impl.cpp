// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/net/routes/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>

// clang-format off
#include <Weave/DeviceLayer/internal/WeaveDeviceLayerInternal.h>
#include <Weave/DeviceLayer/internal/DeviceNetworkInfo.h>
#include <Weave/DeviceLayer/PlatformManager.h>
#include <Weave/DeviceLayer/ThreadStackManager.h>
// clang-format on

#include "thread_stack_manager_delegate_impl.h"

namespace nl {
namespace Weave {
namespace DeviceLayer {

namespace {
using fuchsia::lowpan::ConnectivityState;
using fuchsia::lowpan::Credential;
using fuchsia::lowpan::Identity;
using fuchsia::lowpan::ProvisioningParams;
using fuchsia::lowpan::Role;
using fuchsia::lowpan::device::DeviceExtraSyncPtr;
using fuchsia::lowpan::device::DeviceState;
using fuchsia::lowpan::device::DeviceSyncPtr;
using fuchsia::lowpan::device::Lookup_LookupDevice_Result;
using fuchsia::lowpan::device::LookupSyncPtr;
using fuchsia::lowpan::device::Protocols;
using fuchsia::lowpan::thread::LegacyJoiningSyncPtr;
using fuchsia::net::IpAddress;
using fuchsia::net::Ipv4Address;
using fuchsia::net::Ipv6Address;
using fuchsia::net::routes::State_Resolve_Result;

using nl::Weave::DeviceLayer::PlatformMgrImpl;
using nl::Weave::DeviceLayer::Internal::DeviceNetworkInfo;
using nl::Weave::Profiles::NetworkProvisioning::kNetworkType_Thread;

using ThreadDeviceType = ConnectivityManager::ThreadDeviceType;

constexpr uint16_t kMinThreadChannel = 11;
constexpr uint16_t kMaxThreadChannel = 26;

// Default joinable period for Thread network setup.
constexpr zx_duration_t kThreadJoinableDuration = zx_duration_from_sec(300);
// A joinable duration of 0 stops any active joinable state.
constexpr zx_duration_t kThreadJoinableStop = zx_duration_from_sec(0);

// The required size of a buffer supplied to GetPrimary802154MACAddress.
constexpr size_t k802154MacAddressBufSize =
    sizeof(Profiles::DeviceDescription::WeaveDeviceDescriptor::Primary802154MACAddress);
// Fake MAC address returned by GetPrimary802154MACAddress
constexpr uint8_t kFakeMacAddress[k802154MacAddressBufSize] = {0xFF};
}  // namespace

// Note: Since the functions within this class are intended to function
// synchronously within the Device Layer, these functions all use SyncPtrs for
// interfacing with the LoWPAN FIDL protocols.

WEAVE_ERROR ThreadStackManagerDelegateImpl::InitThreadStack() {
  // See note at top to explain these SyncPtrs.
  LookupSyncPtr lookup;
  Lookup_LookupDevice_Result result;
  Protocols protocols;
  std::vector<std::string> interface_names;
  zx_status_t status;

  // Check whether Thread support is enabled in the ConfigurationManager
  if (!ConfigurationMgrImpl().IsThreadEnabled()) {
    FX_LOGS(INFO) << "Thread support is disabled for this device.";
    is_thread_supported_ = false;
    return WEAVE_NO_ERROR;
  }

  // Access the LoWPAN service.
  status = PlatformMgrImpl().GetComponentContextForProcess()->svc()->Connect(lookup.NewRequest());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to connect to fuchsia.lowpan.device.Lookup: "
                   << zx_status_get_string(status);
    return status;
  }

  // Retrieve LoWPAN interface names.
  status = lookup->GetDevices(&interface_names);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to retrieve LoWPAN interface names: " << zx_status_get_string(status);
    return status;
  }

  // Check returned interfaces for Thread support.
  bool found_device = false;
  for (auto& name : interface_names) {
    std::vector<std::string> net_types;

    protocols.set_device(device_.NewRequest());

    // Look up the device by interface name.
    status = lookup->LookupDevice(name, std::move(protocols), &result);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to lookup device: " << zx_status_get_string(status);
      return status;
    }
    if (result.is_err()) {
      FX_LOGS(WARNING) << "LoWPAN service error during lookup: "
                       << static_cast<int32_t>(result.err());
      continue;
    }

    // Check if the device supports Thread.
    status = device_->GetSupportedNetworkTypes(&net_types);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to request supported network types from device \"" << name
                     << "\": " << zx_status_get_string(status);
      return status;
    }

    for (auto& net_type : net_types) {
      if (net_type == fuchsia::lowpan::NET_TYPE_THREAD_1_X) {
        // Found a Thread device.
        interface_name_ = name;
        found_device = true;
        break;
      }
    }

    if (found_device) {
      break;
    }
  }

  if (!found_device) {
    FX_LOGS(ERROR) << "Could not find a device that supports Thread networks!";
    return ZX_ERR_NOT_FOUND;
  }

  is_thread_supported_ = true;
  return WEAVE_NO_ERROR;
}

bool ThreadStackManagerDelegateImpl::HaveRouteToAddress(const IPAddress& destAddr) {
  fuchsia::net::routes::StateSyncPtr routes;
  State_Resolve_Result result;
  IpAddress netstack_addr;
  zx_status_t status;

  status = PlatformMgrImpl().GetComponentContextForProcess()->svc()->Connect(routes.NewRequest());
  if (status != ZX_OK) {
    // Unfortunately, no way to inform of error status.
    return false;
  }

  if (destAddr.IsIPv6()) {
    Ipv6Address netstack_v6_addr;
    static_assert(sizeof(netstack_v6_addr.addr) == sizeof(destAddr.Addr));
    std::memcpy(&netstack_v6_addr.addr, destAddr.Addr, sizeof(destAddr.Addr));
    netstack_addr.set_ipv6(std::move(netstack_v6_addr));
  } else if (destAddr.IsIPv4()) {
    Ipv4Address netstack_v4_addr;
    static_assert(sizeof(netstack_v4_addr.addr) == sizeof(destAddr.Addr[3]));
    std::memcpy(&netstack_v4_addr.addr, &destAddr.Addr[3], sizeof(destAddr.Addr[3]));
    netstack_addr.set_ipv4(std::move(netstack_v4_addr));
  } else {
    // No route to the "unspecified address".
    FX_LOGS(ERROR) << "HaveRouteToAddress recieved unspecified IP address.";
    return false;
  }

  status = routes->Resolve(std::move(netstack_addr), &result);
  if (status != ZX_OK) {
    // Unfortunately, no way to inform of error status.
    return false;
  } else if (result.is_err()) {
    // Result will be ZX_ERR_ADDRESS_UNREACHABLE if unreachable.
    if (result.err() != ZX_ERR_ADDRESS_UNREACHABLE) {
      FX_LOGS(ERROR) << "Result from resolving route was error "
                     << zx_status_get_string(result.err());
    }
    return false;
  }

  // Result resolved, a route exists.
  return true;
}

void ThreadStackManagerDelegateImpl::OnPlatformEvent(const WeaveDeviceEvent* event) {}

bool ThreadStackManagerDelegateImpl::IsThreadEnabled() {
  DeviceState device_state;

  if (!IsThreadSupported()) {
    return false;
  }

  // Get the device state.
  if (GetDeviceState(&device_state) != ZX_OK) {
    return false;
  }

  // Determine whether Thread is enabled.
  switch (device_state.connectivity_state()) {
    case ConnectivityState::OFFLINE:
    case ConnectivityState::ATTACHING:
    case ConnectivityState::ATTACHED:
    case ConnectivityState::ISOLATED:
    case ConnectivityState::COMMISSIONING:
      return true;
    default:
      return false;
  }
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::SetThreadEnabled(bool val) {
  if (!IsThreadSupported()) {
    return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
  }

  // Enable or disable the device.
  zx_status_t status = device_->SetActive(val);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to " << (val ? "enable" : "disable")
                   << " Thread: " << zx_status_get_string(status);
    return status;
  }

  return WEAVE_NO_ERROR;
}

bool ThreadStackManagerDelegateImpl::IsThreadProvisioned() {
  DeviceState device_state;

  if (!IsThreadSupported()) {
    return false;
  }

  // Get the device state.
  if (GetDeviceState(&device_state) != ZX_OK) {
    return false;
  }

  // Check for the provision.
  switch (device_state.connectivity_state()) {
    case ConnectivityState::INACTIVE:
    case ConnectivityState::OFFLINE:
      return false;
    default:
      return true;
  }
}

bool ThreadStackManagerDelegateImpl::IsThreadAttached() {
  DeviceState device_state;

  if (!IsThreadSupported()) {
    return false;
  }

  // Get the device state.
  if (GetDeviceState(&device_state) != ZX_OK) {
    return false;
  }

  return device_state.connectivity_state() == ConnectivityState::ATTACHED;
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::GetThreadProvision(DeviceNetworkInfo& netInfo,
                                                               bool includeCredentials) {
  DeviceExtraSyncPtr device_extra;
  Identity identity;
  zx_status_t status;

  if (!IsThreadSupported()) {
    return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
  }

  if (!IsThreadProvisioned()) {
    return WEAVE_ERROR_INCORRECT_STATE;
  }

  // Get the Device pointer.
  status = GetProtocols(std::move(Protocols().set_device_extra(device_extra.NewRequest())));
  if (status != ZX_OK) {
    return status;
  }

  // Get the network identity.
  status = device_extra->WatchIdentity(&identity);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not get LoWPAN network identity: " << zx_status_get_string(status);
    return status;
  }

  // TODO(fxbug.dev/67254): Restore the following block once the LoWPAN service
  // correctly returns the net_type.

  // // Check if the provision is a Thread network.
  // if (!identity.has_net_type()) {
  //   FX_LOGS(ERROR) << "No net_type provided; cannot confirm Thread network type.";
  //   return ZX_ERR_INTERNAL;
  // }
  // if (identity.net_type() != fuchsia::lowpan::NET_TYPE_THREAD_1_X) {
  //   FX_LOGS(ERROR) << "Cannot support LoWPAN network type \"" << identity.net_type()
  //                  << "\" in ThreadStackManager.";
  //   return ZX_ERR_INTERNAL;
  // }

  // Start copying provision info.
  netInfo.Reset();
  netInfo.NetworkType = kNetworkType_Thread;
  netInfo.NetworkId = Internal::kThreadNetworkId;
  netInfo.FieldPresent.NetworkId = true;

  // Copy network name.
  if (identity.has_raw_name()) {
    std::memcpy(netInfo.ThreadNetworkName, identity.raw_name().data(),
                std::min<size_t>(DeviceNetworkInfo::kMaxThreadNetworkNameLength,
                                 identity.raw_name().size()));
  }
  // Copy extended PAN id.
  if (identity.has_xpanid()) {
    std::memcpy(
        netInfo.ThreadExtendedPANId, identity.xpanid().data(),
        std::min<size_t>(DeviceNetworkInfo::kThreadExtendedPANIdLength, identity.xpanid().size()));
    netInfo.FieldPresent.ThreadExtendedPANId = true;
  }
  // Copy PAN id.
  if (!identity.has_panid()) {
    // Warn that PAN id remains unspecified.
    FX_LOGS(WARNING) << "PAN id not supplied.";
  } else {
    netInfo.ThreadPANId = identity.panid();
  }
  // Copy channel.
  if (!identity.has_channel() || identity.channel() < kMinThreadChannel ||
      identity.channel() > kMaxThreadChannel) {
    // Warn that channel remains unspecified.
    std::string channel_info =
        identity.has_channel() ? std::to_string(identity.channel()) : "(none)";
    FX_LOGS(WARNING) << "Invalid Thread channel: " << channel_info;
  } else {
    netInfo.ThreadChannel = identity.channel();
  }

  // TODO(fxbug.dev/55638) - Implement mesh prefix and pre-shared commisioning key.

  if (!includeCredentials) {
    // No futher processing needed, credentials won't be included.
    return WEAVE_NO_ERROR;
  }

  // Get credential.
  std::unique_ptr<Credential> credential;
  status = device_extra->GetCredential(&credential);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not retrieve credential: " << zx_status_get_string(status);
    return status;
  }

  // Copy credential info.
  if (!credential) {
    // Warn that credential remains unset.
    FX_LOGS(WARNING) << "Credential requested but no credential provided from LoWPAN device";
  } else {
    std::memcpy(netInfo.ThreadNetworkKey, credential->master_key().data(),
                std::min<size_t>(DeviceNetworkInfo::kMaxThreadNetworkNameLength,
                                 credential->master_key().size()));
    netInfo.FieldPresent.ThreadNetworkKey = true;
  }

  return WEAVE_NO_ERROR;
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::SetThreadProvision(const DeviceNetworkInfo& netInfo) {
  DeviceSyncPtr device;
  std::unique_ptr<Credential> credential;
  Identity identity;

  if (!IsThreadSupported()) {
    return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
  }

  // Set up identity.
  std::vector<uint8_t> network_name{
      netInfo.ThreadNetworkName,
      netInfo.ThreadNetworkName + std::strlen(netInfo.ThreadNetworkName)};

  identity.set_raw_name(std::move(network_name));
  identity.set_net_type(fuchsia::lowpan::NET_TYPE_THREAD_1_X);

  if (netInfo.FieldPresent.ThreadExtendedPANId) {
    identity.set_xpanid(std::vector<uint8_t>{
        netInfo.ThreadExtendedPANId,
        netInfo.ThreadExtendedPANId + DeviceNetworkInfo::kThreadExtendedPANIdLength});
  } else {
    FX_LOGS(ERROR) << "No XPAN ID provided to SetThreadProvision.";
    return WEAVE_ERROR_INVALID_ARGUMENT;
  }

  if (netInfo.ThreadChannel != Profiles::NetworkProvisioning::kThreadChannel_NotSpecified) {
    identity.set_channel(netInfo.ThreadChannel);
  } else {
    FX_LOGS(ERROR) << "No channel provided to SetThreadProvision.";
    return WEAVE_ERROR_INVALID_ARGUMENT;
  }

  if (netInfo.ThreadPANId != Profiles::NetworkProvisioning::kThreadPANId_NotSpecified) {
    identity.set_panid(netInfo.ThreadPANId);
  } else {
    FX_LOGS(ERROR) << "No PAN ID provided to SetThreadProvision.";
    return WEAVE_ERROR_INVALID_ARGUMENT;
  }

  // Set up credential.
  if (netInfo.FieldPresent.ThreadNetworkKey) {
    credential = std::make_unique<Credential>();
    credential->set_master_key(std::vector<uint8_t>{
        netInfo.ThreadNetworkKey,
        netInfo.ThreadNetworkKey + DeviceNetworkInfo::kThreadNetworkKeyLength});
  } else {
    FX_LOGS(ERROR) << "No network key provided to SetThreadProvision.";
    return WEAVE_ERROR_INVALID_ARGUMENT;
  }

  // Add identity and credential to provisioning params.
  ProvisioningParams params{.identity = std::move(identity), .credential = std::move(credential)};

  // Provision the thread device.
  return device_->ProvisionNetwork(std::move(params));
}

void ThreadStackManagerDelegateImpl::ClearThreadProvision() {
  // TODO(fxbug.dev/59029): When thread stack mgr is initialized, this workaround will be removed.
  if (!device_.is_bound()) {
    FX_LOGS(INFO) << "Skipping ClearThreadProvision as device is not bound";
    return;
  }

  if (!IsThreadSupported()) {
    return;
  }

  zx_status_t status = device_->LeaveNetwork();
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not clear LoWPAN provision: " << zx_status_get_string(status);
  }
}

ThreadDeviceType ThreadStackManagerDelegateImpl::GetThreadDeviceType() {
  DeviceState device_state;

  // Get the device state.
  if (GetDeviceState(&device_state) != ZX_OK || !IsThreadSupported()) {
    return ThreadDeviceType::kThreadDeviceType_NotSupported;
  }

  // Determine device type by role.
  switch (device_state.role()) {
    case Role::END_DEVICE:
      return ThreadDeviceType::kThreadDeviceType_FullEndDevice;
    case Role::SLEEPY_END_DEVICE:
      return ThreadDeviceType::kThreadDeviceType_SleepyEndDevice;
    case Role::ROUTER:
    case Role::SLEEPY_ROUTER:
    case Role::LEADER:
    case Role::COORDINATOR:
      return ThreadDeviceType::kThreadDeviceType_Router;
    default:
      return ThreadDeviceType::kThreadDeviceType_NotSupported;
  };
}

bool ThreadStackManagerDelegateImpl::HaveMeshConnectivity() { return IsThreadAttached(); }

WEAVE_ERROR ThreadStackManagerDelegateImpl::GetAndLogThreadStatsCounters() {
  return WEAVE_ERROR_NOT_IMPLEMENTED;  // TODO(fxbug.dev/55888)
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::GetAndLogThreadTopologyMinimal() {
  return WEAVE_ERROR_NOT_IMPLEMENTED;  // TODO(fxbug.dev/55888)
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::GetAndLogThreadTopologyFull() {
  return WEAVE_ERROR_NOT_IMPLEMENTED;  // TODO(fxbug.dev/55888)
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::GetPrimary802154MACAddress(uint8_t* mac_address) {
  if (!IsThreadSupported()) {
    return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
  }

  // This is setting the MAC address to FF:0:0:0:0:0:0:0; this is for a few reasons:
  //   1. The actual value of the MAC address in the descriptor is not currently used.
  //   2. The MAC address (either the factory or the current address) is PII, so it should not be
  //      transmitted unless necessary.
  //   3. Some value should still be transmitted as some tools or other devices use the presence of
  //      an 802.15.4 MAC address to determine if Thread is supported.
  // The best way to meet these requirements is to provide a faked-out MAC address instead.
  std::memcpy(mac_address, kFakeMacAddress, k802154MacAddressBufSize);
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR ThreadStackManagerDelegateImpl::SetThreadJoinable(bool enable) {
  LegacyJoiningSyncPtr thread_legacy;
  zx_status_t status;

  // Get the legacy Thread protocol
  status = GetProtocols(std::move(Protocols().set_thread_legacy_joining(thread_legacy.NewRequest())));
  if (status != ZX_OK) {
    return status;
  }

  // Set joinable or disable joinable based on the intended value.
  status = thread_legacy->MakeJoinable(enable ? kThreadJoinableDuration : kThreadJoinableStop,
                                       WEAVE_UNSECURED_PORT);
  if (status != ZX_OK) {
    return status;
  }

  // Confirm joinable state has been updated successfully.
  return WEAVE_NO_ERROR;
}

zx_status_t ThreadStackManagerDelegateImpl::GetDeviceState(DeviceState* device_state) {
  DeviceSyncPtr device;
  zx_status_t status;

  // Get device pointer.
  status = GetProtocols(std::move(Protocols().set_device(device.NewRequest())));
  if (status != ZX_OK) {
    return status;
  }

  // Grab device state.
  status = device->WatchDeviceState(device_state);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not get LoWPAN device state: " << zx_status_get_string(status);
    return status;
  }

  return ZX_OK;
}

zx_status_t ThreadStackManagerDelegateImpl::GetProtocols(Protocols protocols) {
  // See note at top to explain these SyncPtrs.
  LookupSyncPtr lookup;
  Lookup_LookupDevice_Result result;
  zx_status_t status;

  // Access the LoWPAN service.
  status = PlatformMgrImpl().GetComponentContextForProcess()->svc()->Connect(lookup.NewRequest());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to connect to fuchsia.lowpan.device.Lookup: "
                   << zx_status_get_string(status);
    return status;
  }

  // Look up the device by interface name.
  status = lookup->LookupDevice(interface_name_, std::move(protocols), &result);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to lookup device: " << zx_status_get_string(status);
    return status;
  }
  if (result.is_err()) {
    FX_LOGS(ERROR) << "LoWPAN service error during lookup: " << static_cast<int32_t>(result.err());
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

const std::string& ThreadStackManagerDelegateImpl::GetInterfaceName() const {
  return interface_name_;
}

bool ThreadStackManagerDelegateImpl::IsThreadSupported() const { return is_thread_supported_; }

}  // namespace DeviceLayer
}  // namespace Weave
}  // namespace nl
