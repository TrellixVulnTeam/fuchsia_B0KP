// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FVM_DRIVER_VPARTITION_MANAGER_H_
#define SRC_STORAGE_FVM_DRIVER_VPARTITION_MANAGER_H_

#include <fuchsia/hardware/block/cpp/banjo.h>
#include <fuchsia/hardware/block/partition/cpp/banjo.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <lib/fidl-utils/bind.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/status.h>
#include <lib/zx/vmo.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <zircon/types.h>

#include <atomic>
#include <memory>
#include <optional>

#include <ddk/device.h>
#include <ddktl/device.h>
#include <fbl/algorithm.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/mutex.h>
#include <fbl/vector.h>

#include "src/storage/fvm/driver/diagnostics.h"
#include "src/storage/fvm/driver/slice_extent.h"
#include "src/storage/fvm/driver/vpartition.h"
#include "src/storage/fvm/fvm.h"
#include "src/storage/fvm/metadata.h"

namespace fvm {

using volume_info_t = fuchsia_hardware_block_volume_VolumeInfo;

// Forward declaration
class VPartitionManager;
using ManagerDeviceType =
    ddk::Device<VPartitionManager, ddk::Initializable, ddk::Messageable, ddk::Unbindable>;

class VPartitionManager : public ManagerDeviceType {
 public:
  DISALLOW_COPY_ASSIGN_AND_MOVE(VPartitionManager);
  static zx_status_t Bind(void*, zx_device_t* dev);

  // Read the underlying block device, initialize the recorded VPartitions.
  zx_status_t Load();

  // Block Protocol
  size_t BlockOpSize() const { return block_op_size_; }
  void Queue(block_op_t* txn, block_impl_queue_callback completion_cb, void* cookie) const {
    bp_.ops->queue(bp_.ctx, txn, completion_cb, cookie);
  }

  // Acquire access to a VPart Entry which has already been modified (and
  // will, as a consequence, not be de-allocated underneath us).
  VPartitionEntry* GetAllocatedVPartEntry(size_t index) const TA_NO_THREAD_SAFETY_ANALYSIS {
    auto entry = GetVPartEntryLocked(index);
    ZX_ASSERT(entry->slices > 0);
    return entry;
  }

  // Allocate 'count' slices, write back the FVM.
  zx_status_t AllocateSlices(VPartition* vp, size_t vslice_start, size_t count) TA_EXCL(lock_);

  // Deallocate 'count' slices, write back the FVM.
  // If a request is made to remove vslice_count = 0, deallocates the entire
  // VPartition.
  zx_status_t FreeSlices(VPartition* vp, size_t vslice_start, size_t count) TA_EXCL(lock_);

  // Returns global information about the FVM.
  void Query(volume_info_t* info) TA_EXCL(lock_);

  zx_status_t GetPartitionLimit(const uint8_t* guid, uint64_t* byte_count) const;
  zx_status_t SetPartitionLimit(const uint8_t* guid, uint64_t byte_count);

  size_t DiskSize() const { return info_.block_count * info_.block_size; }
  size_t slice_size() const { return slice_size_; }
  uint64_t VSliceMax() const { return fvm::kMaxVSlices; }
  const block_info_t& Info() const { return info_; }

  // Returns a copy of the current header.
  fvm::Header GetHeader() const;

  void DdkInit(ddk::InitTxn txn);
  zx_status_t DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  VPartitionManager(zx_device_t* parent, const block_info_t& info, size_t block_op_size,
                    const block_impl_protocol_t* bp);
  ~VPartitionManager();

  // Allocates the partition, returning it without adding it to the device manager. Production code
  // will go through the FIDL API, this is exposed separately to allow testing without FIDL.
  zx::status<std::unique_ptr<VPartition>> AllocatePartition(
      uint64_t slice_count, const fuchsia_hardware_block_partition_GUID* type,
      const fuchsia_hardware_block_partition_GUID* instance, const char* name_data,
      size_t name_size, uint32_t flags);

  // Returns a reference to the Diagnostics that this instance publishes to.
  Diagnostics& diagnostics() { return diagnostics_; }

 private:
  static const fuchsia_hardware_block_volume_VolumeManager_ops* Ops() {
    using Binder = fidl::Binder<VPartitionManager>;
    static const fuchsia_hardware_block_volume_VolumeManager_ops kOps = {
        .AllocatePartition = Binder::BindMember<&VPartitionManager::FIDLAllocatePartition>,
        .Query = Binder::BindMember<&VPartitionManager::FIDLQuery>,
        .GetInfo = Binder::BindMember<&VPartitionManager::FIDLGetInfo>,
        .Activate = Binder::BindMember<&VPartitionManager::FIDLActivate>,
        .GetPartitionLimit = Binder::BindMember<&VPartitionManager::FIDLGetPartitionLimit>,
        .SetPartitionLimit = Binder::BindMember<&VPartitionManager::FIDLSetPartitionLimit>,
    };
    return &kOps;
  }

  // FIDL interface VolumeManager
  zx_status_t FIDLAllocatePartition(uint64_t slice_count,
                                    const fuchsia_hardware_block_partition_GUID* type,
                                    const fuchsia_hardware_block_partition_GUID* instance,
                                    const char* name_data, size_t name_size, uint32_t flags,
                                    fidl_txn_t* txn);
  zx_status_t FIDLQuery(fidl_txn_t* txn);
  zx_status_t FIDLGetInfo(fidl_txn_t* txn);
  zx_status_t FIDLActivate(const fuchsia_hardware_block_partition_GUID* old_guid,
                           const fuchsia_hardware_block_partition_GUID* new_guid, fidl_txn_t* txn);
  zx_status_t FIDLGetPartitionLimit(const fuchsia_hardware_block_partition_GUID* guid,
                                    fidl_txn_t* txn);
  zx_status_t FIDLSetPartitionLimit(const fuchsia_hardware_block_partition_GUID* guid,
                                    uint64_t byte_count, fidl_txn_t* txn);

  // Marks the partition with instance GUID |old_guid| as inactive,
  // and marks partitions with instance GUID |new_guid| as active.
  //
  // If a partition with |old_guid| does not exist, it is ignored.
  // If |old_guid| equals |new_guid|, then |old_guid| is ignored.
  // If a partition with |new_guid| does not exist, |ZX_ERR_NOT_FOUND|
  // is returned.
  //
  // Updates the FVM metadata atomically.
  zx_status_t Upgrade(const uint8_t* old_guid, const uint8_t* new_guid) TA_EXCL(lock_);

  // Given a VPartition object, add a corresponding ddk device.
  zx_status_t AddPartition(std::unique_ptr<VPartition> vp) const;

  // Update, hash, and write back the current copy of the FVM metadata.
  // Automatically handles alternating writes to primary / backup copy of FVM.
  zx_status_t WriteFvmLocked() TA_REQ(lock_);

  zx_status_t AllocateSlicesLocked(VPartition* vp, size_t vslice_start, size_t count) TA_REQ(lock_);

  zx_status_t FreeSlicesLocked(VPartition* vp, size_t vslice_start, size_t count) TA_REQ(lock_);

  zx_status_t FindFreeVPartEntryLocked(size_t* out) const TA_REQ(lock_);
  zx_status_t FindFreeSliceLocked(size_t* out, size_t hint) const TA_REQ(lock_);

  Header* GetFvmLocked() const TA_REQ(lock_) { return &metadata_.GetHeader(); }

  // Mark a slice as free in the metadata structure.
  // Update free slice accounting.
  void FreePhysicalSlice(VPartition* vp, size_t pslice) TA_REQ(lock_);

  // Mark a slice as allocated in the metadata structure.
  // Update allocated slice accounting.
  void AllocatePhysicalSlice(VPartition* vp, size_t pslice, uint64_t vslice) TA_REQ(lock_);

  // Given a physical slice (acting as an index into the slice table),
  // return the associated slice entry.
  SliceEntry* GetSliceEntryLocked(size_t index) const TA_REQ(lock_);

  // Given an index into the vpartition table, return the associated
  // virtual partition entry.
  VPartitionEntry* GetVPartEntryLocked(size_t index) const TA_REQ(lock_);

  // Returns the number of the partition with the given GUID. If there are multiple ones (there
  // should not be), returns the first one. If there are no matches, returns 0 (partitions are
  // 1-indexed).
  size_t GetPartitionNumberLocked(const uint8_t* guid) const TA_REQ(lock_);

  zx_status_t DoIoLocked(zx_handle_t vmo, size_t off, size_t len, uint32_t command) const;

  // Writes the current partition information out to the system log.
  void LogPartitionsLocked() const TA_REQ(lock_);

  thrd_t initialization_thread_;
  std::atomic_bool initialization_thread_started_ = false;
  block_info_t info_;  // Cached info from parent device

  mutable fbl::Mutex lock_;
  Metadata metadata_ TA_GUARDED(lock_);
  // Number of currently allocated slices.
  size_t pslice_allocated_count_ TA_GUARDED(lock_) = 0;

  Diagnostics diagnostics_;

  // Set when the driver is loaded and never changed.
  size_t slice_size_ = 0;

  // Stores the maximum size in bytes for each partition, 1-indexed (0 elt is not used) the same as
  // GetVPartEntryLocked(). A 0 max size means there is no maximum for this partition.
  //
  // These are 0-initialized and set by the FIDL call SetPartitionLimit. It would be better in the
  // future if this information could be persisted in the partition table. But currently we want
  // to keep the max size without changing the on-disk format. fshost will set these on startup
  // when configured to do so.
  uint64_t max_partition_sizes_[fvm::kMaxVPartitions] TA_GUARDED(lock_) = {0};

  // Block Protocol
  const size_t block_op_size_;
  block_impl_protocol_t bp_;

  // For replying to the device init hook. Empty when not initialized by the DDK yet and when run
  // in unit tests. To allow for test operation, null check this and ignore the txn if unset.
  std::optional<ddk::InitTxn> init_txn_;

  // Worker completion.
  sync_completion_t worker_completed_;
};

}  // namespace fvm

#endif  // SRC_STORAGE_FVM_DRIVER_VPARTITION_MANAGER_H_
