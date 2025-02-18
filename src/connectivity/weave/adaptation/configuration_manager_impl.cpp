// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
#include <Weave/DeviceLayer/internal/WeaveDeviceLayerInternal.h>
#include <Weave/DeviceLayer/ConfigurationManager.h>
#include <Weave/Core/WeaveKeyIds.h>
#include "src/connectivity/weave/adaptation/configuration_manager_impl.h"
#include "src/connectivity/weave/adaptation/group_key_store_impl.h"
#include <Weave/Profiles/security/WeaveApplicationKeys.h>

#include <Weave/Core/WeaveVendorIdentifiers.hpp>
#include <Weave/DeviceLayer/internal/GenericConfigurationManagerImpl.ipp>
// clang-format on

#include <lib/syslog/cpp/macros.h>

namespace nl {
namespace Weave {
namespace DeviceLayer {
namespace {
using GroupKeyStoreBase = ::nl::Weave::Profiles::Security::AppKeys::GroupKeyStoreBase;
using Key = ::nl::Weave::Platform::PersistedStorage::Key;
}  // namespace

/* Singleton instance of the ConfigurationManager implementation object for the Fuchsia. */
ConfigurationManagerImpl ConfigurationManagerImpl::sInstance;

WEAVE_ERROR ConfigurationManagerImpl::_Init(void) {
  FX_CHECK(delegate_ != nullptr) << "ConfigurationManager delegate not set before Init.";
  return delegate_->Init();
}

WEAVE_ERROR ConfigurationManagerImpl::_GetDeviceId(uint64_t& device_id) {
  return delegate_->GetDeviceId(device_id);
}

WEAVE_ERROR ConfigurationManagerImpl::_GetFirmwareRevision(char* buf, size_t buf_size,
                                                           size_t& out_len) {
  return delegate_->GetFirmwareRevision(buf, buf_size, out_len);
}

WEAVE_ERROR ConfigurationManagerImpl::_GetManufacturerDeviceCertificate(uint8_t* buf,
                                                                        size_t buf_size,
                                                                        size_t& out_len) {
  return delegate_->GetManufacturerDeviceCertificate(buf, buf_size, out_len);
}

WEAVE_ERROR ConfigurationManagerImpl::_GetProductId(uint16_t& product_id) {
  return delegate_->GetProductId(product_id);
}

WEAVE_ERROR ConfigurationManagerImpl::_GetPrimaryWiFiMACAddress(uint8_t* mac_address) {
  return delegate_->GetPrimaryWiFiMACAddress(mac_address);
}

WEAVE_ERROR ConfigurationManagerImpl::_GetVendorId(uint16_t& vendor_id) {
  return delegate_->GetVendorId(vendor_id);
}

bool ConfigurationManagerImpl::_IsFullyProvisioned() { return delegate_->IsFullyProvisioned(); }

bool ConfigurationManagerImpl::_IsPairedToAccount() { return delegate_->IsPairedToAccount(); }

bool ConfigurationManagerImpl::_IsMemberOfFabric() { return delegate_->IsMemberOfFabric(); }

GroupKeyStoreBase* ConfigurationManagerImpl::_GetGroupKeyStore(void) {
  return delegate_->GetGroupKeyStore();
}

bool ConfigurationManagerImpl::_CanFactoryReset(void) { return delegate_->CanFactoryReset(); }

void ConfigurationManagerImpl::_InitiateFactoryReset(void) { delegate_->InitiateFactoryReset(); }

WEAVE_ERROR ConfigurationManagerImpl::_ReadPersistedStorageValue(Key key, uint32_t& value) {
  return delegate_->ReadPersistedStorageValue(key, value);
}

WEAVE_ERROR ConfigurationManagerImpl::_WritePersistedStorageValue(Key key, uint32_t value) {
  return delegate_->WritePersistedStorageValue(key, value);
}

WEAVE_ERROR ConfigurationManagerImpl::_GetDeviceDescriptorTLV(uint8_t* buf, size_t buf_size,
                                                              size_t& encoded_len) {
  return delegate_->GetDeviceDescriptorTLV(buf, buf_size, encoded_len);
}

void ConfigurationManagerImpl::SetDelegate(std::unique_ptr<Delegate> delegate) {
  FX_CHECK(!(delegate && delegate_)) << "Attempt to set an already set delegate. Must explicitly "
                                        "clear the existing delegate first.";
  delegate_ = std::move(delegate);
  if (delegate_) {
    delegate_->SetConfigurationManagerImpl(this);
  }
}

ConfigurationManagerImpl::Delegate* ConfigurationManagerImpl::GetDelegate() {
  return delegate_.get();
}

WEAVE_ERROR ConfigurationManagerImpl::GetBleDeviceNamePrefix(char* device_name_prefix,
                                                             size_t device_name_prefix_size,
                                                             size_t* out_len) {
  return delegate_->GetBleDeviceNamePrefix(device_name_prefix, device_name_prefix_size, out_len);
}

bool ConfigurationManagerImpl::IsThreadEnabled() { return delegate_->IsThreadEnabled(); }

bool ConfigurationManagerImpl::IsWoBLEEnabled() { return delegate_->IsWoBLEEnabled(); }

bool ConfigurationManagerImpl::IsWoBLEAdvertisementEnabled() {
  return delegate_->IsWoBLEAdvertisementEnabled();
}

zx_status_t ConfigurationManagerImpl::GetPrivateKeyForSigning(std::vector<uint8_t>* signing_key) {
  return delegate_->GetPrivateKeyForSigning(signing_key);
}

zx_status_t ConfigurationManagerImpl::GetAppletPathList(std::vector<std::string>& out) {
  return delegate_->GetAppletPathList(out);
}

WEAVE_ERROR ConfigurationManagerImpl::GetThreadJoinableDuration(uint32_t* duration) {
  return delegate_->GetThreadJoinableDuration(duration);
}

}  // namespace DeviceLayer
}  // namespace Weave
}  // namespace nl
