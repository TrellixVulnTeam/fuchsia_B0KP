// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_CONSTANTS_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_CONSTANTS_H_

#include <fuchsia/feedback/cpp/fidl.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "src/developer/forensics/utils/cobalt/metrics.h"

namespace forensics {
namespace feedback_data {

///////////////////////////////////////////////////////////////////////////////////////////////////
// Annotations
///////////////////////////////////////////////////////////////////////////////////////////////////

const uint32_t kMaxNumPlatformAnnotations = 32u;
const uint32_t kMaxNumNonPlatformAnnotations = 30u;
const uint32_t kMaxNumDebugAnnotations = 2u;
static_assert(kMaxNumPlatformAnnotations + kMaxNumNonPlatformAnnotations +
                      kMaxNumDebugAnnotations ==
                  fuchsia::feedback::MAX_NUM_ANNOTATIONS_PROVIDED,
              "The max number of provided annotations has to be split between a max number of "
              "platform annotations, a max number of non-platform annotations, and a max number of "
              "debug annotations");

// Platform annotation keys.
constexpr char kAnnotationBuildBoard[] = "build.board";
constexpr char kAnnotationBuildIsDebug[] = "build.is_debug";
constexpr char kAnnotationBuildLatestCommitDate[] = "build.latest-commit-date";
constexpr char kAnnotationBuildProduct[] = "build.product";
constexpr char kAnnotationBuildVersion[] = "build.version";
constexpr char kAnnotationDeviceBoardName[] = "device.board-name";
constexpr char kAnnotationDeviceFeedbackId[] = "device.feedback-id";
constexpr char kAnnotationDeviceUptime[] = "device.uptime";
constexpr char kAnnotationDeviceUtcTime[] = "device.utc-time";
constexpr char kAnnotationHardwareBoardName[] = "hardware.board.name";
constexpr char kAnnotationHardwareBoardRevision[] = "hardware.board.revision";
constexpr char kAnnotationHardwareProductLanguage[] = "hardware.product.language";
constexpr char kAnnotationHardwareProductLocaleList[] = "hardware.product.locale-list";
constexpr char kAnnotationHardwareProductManufacturer[] = "hardware.product.manufacturer";
constexpr char kAnnotationHardwareProductModel[] = "hardware.product.model";
constexpr char kAnnotationHardwareProductName[] = "hardware.product.name";
constexpr char kAnnotationHardwareProductRegulatoryDomain[] = "hardware.product.regulatory-domain";
constexpr char kAnnotationHardwareProductSKU[] = "hardware.product.sku";
constexpr char kAnnotationSystemBootIdCurrent[] = "system.boot-id.current";
constexpr char kAnnotationSystemBootIdPrevious[] = "system.boot-id.previous";
constexpr char kAnnotationSystemLastRebootReason[] = "system.last-reboot.reason";
constexpr char kAnnotationSystemLastRebootUptime[] = "system.last-reboot.uptime";
constexpr char kAnnotationSystemUpdateChannelCurrent[] = "system.update-channel.current";

// Reserved namespaces for platform annotations. Components are not allowed to use these namespaces
// when supplying non-platform annotations.
const std::set<const std::string> kReservedAnnotationNamespaces({
    "build",
    "device",
    "hardware",
    "hardware.board",
    "hardware.product",
    "misc",
    "system",
});

///////////////////////////////////////////////////////////////////////////////////////////////////
// Attachments
///////////////////////////////////////////////////////////////////////////////////////////////////

// Platform attachments keys.
constexpr char kAttachmentAnnotations[] = "annotations.json";
constexpr char kAttachmentBuildSnapshot[] = "build.snapshot.xml";
constexpr char kAttachmentInspect[] = "inspect.json";
constexpr char kAttachmentLogKernel[] = "log.kernel.txt";
constexpr char kAttachmentLogSystem[] = "log.system.txt";
constexpr char kAttachmentLogSystemPrevious[] = "log.system.previous_boot.txt";
constexpr char kAttachmentMetadata[] = "metadata.json";

// Snapshot key.
constexpr char kSnapshotFilename[] = "snapshot.zip";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Device ID
///////////////////////////////////////////////////////////////////////////////////////////////////

constexpr char kDeviceIdPath[] = "/data/device_id.txt";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Boot ID
///////////////////////////////////////////////////////////////////////////////////////////////////

constexpr char kBootIdFileName[] = "boot_id.txt";

///////////////////////////////////////////////////////////////////////////////////////////////////
// UTC-monotonic difference
///////////////////////////////////////////////////////////////////////////////////////////////////

constexpr char kUtcMonotonicDifferenceFile[] = "current_utc_monotonic_difference.txt";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Logs from previous boot cycle.
///////////////////////////////////////////////////////////////////////////////////////////////////

constexpr char kPreviousLogsFilePath[] = "/tmp/log.system.previous_boot.txt";

// We use the 8 files below to store up to 512 kb of logs. So, assuming all components have logged
// at least 512 kb of data, we can expect between 448 kb and 512 kb of logs to be persisted due to
// the log rotation.
constexpr uint64_t kPersistentLogsMaxSizeInKb = 512;
constexpr char kCurrentLogsDir[] = "/cache/current_system_logs";
constexpr size_t kMaxNumLogFiles = 8u;

// At most 16KB of logs will be persisted each second.
constexpr size_t kMaxWriteSizeInBytes = 16 * 1024;

// Repeated messge format
constexpr char kRepeatedStrPrefix[] = "!!! MESSAGE REPEATED ";
constexpr char kRepeatedOnceFormatStr[] = "!!! MESSAGE REPEATED 1 MORE TIME !!!\n";
constexpr char kRepeatedFormatStr[] = "!!! MESSAGE REPEATED %lu MORE TIMES !!!\n";

// One repeated message can occupy up to "kMaxRepeatedBuffers" buffers.
constexpr size_t kMaxRepeatedBuffers = 30;

// The current version of the snapshot. Update these values together!
struct SnapshotVersion {
  static constexpr cobalt::SnapshotVersion kCobalt = cobalt::SnapshotVersion::kV_01;
  static constexpr const char* kString = "1";
};

// Use this file to determine whether or not a previous instance of the component was instructed to
// terminated system log recording.
constexpr char kDoNotLaunchSystemLogRecorder[] = "/tmp/do_not_launch_system_log_recorder.txt";

}  // namespace feedback_data
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_CONSTANTS_H_
