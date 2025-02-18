// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FS_PAGER_THREAD_POOL_H_
#define FS_PAGER_THREAD_POOL_H_

#include <lib/zx/port.h>
#include <lib/zx/status.h>

#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace fs {

class PagedVfs;

// Implements a simple background thread pool that listens for pager requests and dispatches page
// requests and notifications.
//
// This avoids libasync because the pager is both performance-critical and its needs are very
// simple. libasync associates additional tracking information and has lambdas for every watched
// object that are not required for this use-case. It is easy enough to listen for pager requests on
// the port directly, and this also allows us to service the same port from potentially multiple
// threads.
class PagerThreadPool {
 public:
  // The VFS must outlive this class (in practice it owns us). Init() must be called and must
  // succeed before using this class.
  PagerThreadPool(PagedVfs& vfs, int num_threads);

  // This object must be destroyed before the associated PagedVfs.
  ~PagerThreadPool();

  const zx::port& port() const { return port_; }

  zx::status<> Init();

 private:
  // This function runs each background thread.
  void ThreadProc();

  PagedVfs& vfs_;  // Non-owning.
  const int num_threads_;

  // Use from the main thread only.
  std::vector<std::unique_ptr<std::thread>> threads_;

  zx::port port_;  // Port associated with page requests.
};

}  // namespace fs

#endif  // FS_PAGER_THREAD_POOL_H_
