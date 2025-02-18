// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_NETWORK_DRIVERS_NETWORK_DEVICE_DEVICE_STATUS_WATCHER_H_
#define SRC_CONNECTIVITY_NETWORK_DRIVERS_NETWORK_DEVICE_DEVICE_STATUS_WATCHER_H_

#include <fuchsia/hardware/network/device/cpp/banjo.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/llcpp/server.h>
#include <lib/stdcompat/optional.h>
#include <lib/sync/completion.h>
#include <threads.h>

#include <queue>

#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>

#include "definitions.h"

namespace network::internal {

/// A helper class to build FIDL fuchsia.hardware.network.Status from the banjo status definition
/// `status_t``.
class FidlStatus {
 public:
  explicit FidlStatus(const status_t& status)
      : mtu_(status.mtu), flags_(netdev::StatusFlags::TruncatingUnknown(status.flags)) {
    builder_.set_flags(fidl::unowned_ptr(&flags_));
    builder_.set_mtu(fidl::unowned_ptr(&mtu_));
  }

  netdev::Status view() { return builder_.build(); }

 private:
  uint32_t mtu_;
  netdev::StatusFlags flags_;
  netdev::Status::UnownedBuilder builder_;
};

class StatusWatcher : public fbl::DoublyLinkedListable<std::unique_ptr<StatusWatcher>>,
                      public netdev::StatusWatcher::Interface {
 public:
  explicit StatusWatcher(uint32_t max_queue);
  ~StatusWatcher() override;

  zx_status_t Bind(async_dispatcher_t* dispatcher, fidl::ServerEnd<netdev::StatusWatcher> channel,
                   fit::callback<void(StatusWatcher*)> closed_callback);
  void Unbind();

  void PushStatus(const status_t& status);

 private:
  void WatchStatus(WatchStatusCompleter::Sync& _completer) override;

  fbl::Mutex lock_;
  uint32_t max_queue_;
  cpp17::optional<status_t> last_observed_ __TA_GUARDED(lock_);
  std::queue<status_t> queue_ __TA_GUARDED(lock_);
  cpp17::optional<WatchStatusCompleter::Async> pending_txn_ __TA_GUARDED(lock_);
  cpp17::optional<fidl::ServerBindingRef<netdev::StatusWatcher>> binding_ __TA_GUARDED(lock_);
  fit::callback<void(StatusWatcher*)> closed_cb_;
};

using StatusWatcherList = fbl::DoublyLinkedList<std::unique_ptr<StatusWatcher>>;

}  // namespace network::internal

#endif  // SRC_CONNECTIVITY_NETWORK_DRIVERS_NETWORK_DEVICE_DEVICE_STATUS_WATCHER_H_
