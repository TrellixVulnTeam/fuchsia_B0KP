// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package constants

const (
	FailedToStartTargetMsg    = "start target error"
	QEMUInvocationErrorMsg    = "QEMU invocation error"
	ReadConfigFileErrorMsg    = "could not open config file"
	FailedToResolveIPErrorMsg = "could not resolve target IP address"
	PackageRepoSetupErrorMsg  = "failed to set up a package repository"
	SerialReadErrorMsg        = "error reading serial log line"

	NodenameEnvKey     = "FUCHSIA_NODENAME"
	SSHKeyEnvKey       = "FUCHSIA_SSH_KEY"
	SerialSocketEnvKey = "FUCHSIA_SERIAL_SOCKET"
	DeviceAddrEnvKey   = "FUCHSIA_DEVICE_ADDR"
	IPv4AddrEnvKey     = "FUCHSIA_IPV4_ADDR"
	IPv6AddrEnvKey     = "FUCHSIA_IPV6_ADDR"
)
