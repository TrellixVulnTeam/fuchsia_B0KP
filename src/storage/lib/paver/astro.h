// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_STORAGE_LIB_PAVER_ASTRO_H_
#define SRC_STORAGE_LIB_PAVER_ASTRO_H_

#include <lib/sysconfig/sync-client.h>

#include "src/storage/lib/paver/abr-client.h"
#include "src/storage/lib/paver/partition-client.h"
#include "src/storage/lib/paver/paver-context.h"
#include "src/storage/lib/paver/skip-block.h"

namespace paver {

class AstroPartitioner : public DevicePartitioner {
 public:
  enum class AbrWearLevelingOption { ON, OFF };

  static zx::status<std::unique_ptr<DevicePartitioner>> Initialize(
      fbl::unique_fd devfs_root, fidl::UnownedClientEnd<::llcpp::fuchsia::io::Directory> svc_root,
      std::shared_ptr<Context> context);

  bool IsFvmWithinFtl() const override { return true; }

  bool SupportsPartition(const PartitionSpec& spec) const override;

  zx::status<std::unique_ptr<PartitionClient>> AddPartition(
      const PartitionSpec& spec) const override;

  zx::status<std::unique_ptr<PartitionClient>> FindPartition(
      const PartitionSpec& spec) const override;

  zx::status<> FinalizePartition(const PartitionSpec& spec) const override { return zx::ok(); }

  zx::status<> WipeFvm() const override;

  zx::status<> InitPartitionTables() const override;

  zx::status<> WipePartitionTables() const override;

  zx::status<> ValidatePayload(const PartitionSpec& spec,
                               fbl::Span<const uint8_t> data) const override;

  zx::status<> Flush() const override;

 private:
  AstroPartitioner(std::unique_ptr<SkipBlockDevicePartitioner> skip_block,
                   std::shared_ptr<Context> context)
      : skip_block_(std::move(skip_block)), context_(context) {}

  static zx::status<> InitializeContext(const fbl::unique_fd& devfs_root,
                                        AbrWearLevelingOption abr_wear_leveling_opt,
                                        std::shared_ptr<Context> context);

  static bool CanSafelyUpdateLayout(std::shared_ptr<Context> context);

  std::unique_ptr<SkipBlockDevicePartitioner> skip_block_;

  std::shared_ptr<Context> context_;
};

class AstroPartitionerFactory : public DevicePartitionerFactory {
 public:
  zx::status<std::unique_ptr<DevicePartitioner>> New(
      fbl::unique_fd devfs_root, fidl::UnownedClientEnd<::llcpp::fuchsia::io::Directory> svc_root,
      Arch arch, std::shared_ptr<Context> context, const fbl::unique_fd& block_device) final;
};

class AstroAbrClientFactory : public abr::ClientFactory {
 public:
  zx::status<std::unique_ptr<abr::Client>> New(
      fbl::unique_fd devfs_root, fidl::UnownedClientEnd<::llcpp::fuchsia::io::Directory> svc_root,
      std::shared_ptr<paver::Context> context) final;
};

// Specialized astro sysconfig partition client built on SyncClientBuffered.
class AstroSysconfigPartitionClientBuffered : public BlockDevicePartitionClient {
 public:
  AstroSysconfigPartitionClientBuffered(std::shared_ptr<Context> context,
                                        ::sysconfig::SyncClient::PartitionType partition)
      : context_(context), partition_(partition) {}

  zx::status<size_t> GetBlockSize() final;
  zx::status<size_t> GetPartitionSize() final;
  zx::status<> Read(const zx::vmo& vmo, size_t size) final;
  zx::status<> Write(const zx::vmo& vmo, size_t vmo_size) final;
  zx::status<> Trim() final;
  zx::status<> Flush() final;
  fidl::ClientEnd<::llcpp::fuchsia::hardware::block::Block> GetChannel() final;
  fbl::unique_fd block_fd() final;

  // No copy, no move.
  AstroSysconfigPartitionClientBuffered(const AstroSysconfigPartitionClientBuffered&) = delete;
  AstroSysconfigPartitionClientBuffered& operator=(const AstroSysconfigPartitionClientBuffered&) =
      delete;
  AstroSysconfigPartitionClientBuffered(AstroSysconfigPartitionClientBuffered&&) = delete;
  AstroSysconfigPartitionClientBuffered& operator=(AstroSysconfigPartitionClientBuffered&&) =
      delete;

 private:
  std::shared_ptr<Context> context_;
  ::sysconfig::SyncClient::PartitionType partition_;
};

// Specialized layer on top of SkipBlockPartitionClient to deal with page0 quirk and block size
// quirk.
class Bl2PartitionClient final : public SkipBlockPartitionClient {
 public:
  explicit Bl2PartitionClient(
      fidl::ClientEnd<::llcpp::fuchsia::hardware::skipblock::SkipBlock> partition)
      : SkipBlockPartitionClient(std::move(partition)) {}

  zx::status<size_t> GetBlockSize() final;
  zx::status<size_t> GetPartitionSize() final;
  zx::status<> Read(const zx::vmo& vmo, size_t size) final;
  zx::status<> Write(const zx::vmo& vmo, size_t vmo_size) final;

  // No copy, no move.
  Bl2PartitionClient(const Bl2PartitionClient&) = delete;
  Bl2PartitionClient& operator=(const Bl2PartitionClient&) = delete;
  Bl2PartitionClient(Bl2PartitionClient&&) = delete;
  Bl2PartitionClient& operator=(Bl2PartitionClient&&) = delete;

 private:
  static constexpr size_t kNandPageSize = 4 * 1024;
  static constexpr size_t kBl2Size = 64 * 1024;
};

class AstroPartitionerContext : public ContextBase {
 public:
  std::unique_ptr<::sysconfig::SyncClientBuffered> client_;

  AstroPartitionerContext(std::unique_ptr<::sysconfig::SyncClientBuffered> client)
      : client_{std::move(client)} {}
};

}  // namespace paver

#endif  // SRC_STORAGE_LIB_PAVER_ASTRO_H_
