// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/boot/c/fidl.h>
#include <fuchsia/boot/llcpp/fidl.h>
#include <fuchsia/exception/llcpp/fidl.h>
#include <fuchsia/kernel/c/fidl.h>
#include <fuchsia/power/manager/llcpp/fidl.h>
#include <fuchsia/scheduler/c/fidl.h>
#include <fuchsia/sys2/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/devmgr-launcher/launch.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fidl-async/bind.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/service/llcpp/service.h>
#include <lib/vfs/cpp/remote_dir.h>
#include <lib/zx/exception.h>
#include <stdint.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls/exception.h>

#include <map>
#include <thread>
#include <utility>

#include <fbl/algorithm.h>
#include <fbl/ref_ptr.h>
#include <fbl/string_printf.h>
#include <fs/pseudo_dir.h>
#include <fs/service.h>
#include <fs/synchronous_vfs.h>
#include <fs/vfs_types.h>
#include <mock-boot-arguments/server.h>

namespace {

using GetBootItemFunction = devmgr_launcher::GetBootItemFunction;

// TODO(http://fxbug.dev/33183): Replace this with a test component_manager.
class FakeRealm : public llcpp::fuchsia::sys2::Realm::Interface {
 public:
  void BindChild(llcpp::fuchsia::sys2::ChildRef child,
                 fidl::ServerEnd<llcpp::fuchsia::io::Directory> exposed_dir,
                 BindChildCompleter::Sync& completer) override {
    exposed_dir_ = std::move(exposed_dir);
    completer.ReplySuccess();
  }

  void CreateChild(llcpp::fuchsia::sys2::CollectionRef collection,
                   llcpp::fuchsia::sys2::ChildDecl decl,
                   CreateChildCompleter::Sync& completer) override {
    completer.ReplySuccess();
  }

  void DestroyChild(llcpp::fuchsia::sys2::ChildRef child,
                    DestroyChildCompleter::Sync& completer) override {}

  void ListChildren(llcpp::fuchsia::sys2::CollectionRef collection,
                    fidl::ServerEnd<llcpp::fuchsia::sys2::ChildIterator> iter,
                    ListChildrenCompleter::Sync& completer) override {}

 private:
  fidl::ServerEnd<llcpp::fuchsia::io::Directory> exposed_dir_;
};

class FakePowerRegistration
    : public llcpp::fuchsia::power::manager::DriverManagerRegistration::Interface {
 public:
  void Register(fidl::ClientEnd<llcpp::fuchsia::device::manager::SystemStateTransition> transition,
                fidl::ClientEnd<llcpp::fuchsia::io::Directory> dir,
                RegisterCompleter::Sync& completer) override {
    // Store these so the other side doesn't see the channels close.
    transition_ = std::move(transition);
    dir_ = std::move(dir);
    completer.ReplySuccess();
  }

 private:
  fidl::ClientEnd<llcpp::fuchsia::device::manager::SystemStateTransition> transition_;
  fidl::ClientEnd<llcpp::fuchsia::io::Directory> dir_;
};

zx_status_t ItemsGet(void* ctx, uint32_t type, uint32_t extra, fidl_txn_t* txn) {
  const auto& get_boot_item = *static_cast<GetBootItemFunction*>(ctx);
  zx::vmo vmo;
  uint32_t length = 0;
  if (get_boot_item) {
    zx_status_t status = get_boot_item(type, extra, &vmo, &length);
    if (status != ZX_OK) {
      return status;
    }
  }
  return fuchsia_boot_ItemsGet_reply(txn, vmo.release(), length);
}

constexpr fuchsia_boot_Items_ops kItemsOps = {
    .Get = ItemsGet,
};

zx_status_t RootJobGet(void* ctx, fidl_txn_t* txn) {
  const auto& root_job = *static_cast<zx::unowned_job*>(ctx);
  zx::job job;
  zx_status_t status = root_job->duplicate(ZX_RIGHT_SAME_RIGHTS, &job);
  if (status != ZX_OK) {
    return status;
  }
  return fuchsia_kernel_RootJobGet_reply(txn, job.release());
}

constexpr fuchsia_kernel_RootJob_ops kRootJobOps = {
    .Get = RootJobGet,
};

template <class Protocol>
void CreateFakeCppService(fbl::RefPtr<fs::PseudoDir> root, async_dispatcher_t* dispatcher,
                          std::unique_ptr<typename Protocol::Interface> server) {
  auto node = fbl::MakeRefCounted<fs::Service>(
      [dispatcher, server{std::move(server)}](fidl::ServerEnd<Protocol> channel) {
        return fidl::BindSingleInFlightOnly(dispatcher, std::move(channel), server.get());
      });
  root->AddEntry(Protocol::Name, node);
}

void CreateFakeService(fbl::RefPtr<fs::PseudoDir> root, const char* name,
                       async_dispatcher_t* dispatcher, fidl_dispatch_t* dispatch, void* ctx,
                       const void* ops) {
  auto node =
      fbl::MakeRefCounted<fs::Service>([dispatcher, dispatch, ctx, ops](zx::channel channel) {
        return fidl_bind(dispatcher, channel.release(), dispatch, ctx, ops);
      });
  root->AddEntry(name, node);
}

void ForwardService(fbl::RefPtr<fs::PseudoDir> root, const char* name,
                    fidl::ClientEnd<llcpp::fuchsia::io::Directory> svc_client) {
  root->AddEntry(name, fbl::MakeRefCounted<fs::Service>(
                           [name, svc_client = std::move(svc_client)](zx::channel request) {
                             return fdio_service_connect_at(svc_client.channel().get(), name,
                                                            request.release());
                           }));
}

fidl::ClientEnd<llcpp::fuchsia::io::Directory> CloneDirectory(
    fidl::UnownedClientEnd<llcpp::fuchsia::io::Directory> end) {
  return fidl::ClientEnd<llcpp::fuchsia::io::Directory>(
      zx::channel(fdio_service_clone(end.channel())));
}

}  // namespace

namespace devmgr_integration_test {

// We keep this structure opaque so that we don't grow a bunch of public dependencies for the
// implementation of this loop
struct IsolatedDevmgr::SvcLoopState {
  ~SvcLoopState() {
    // We must shut down the loop before we operate on vfs and bootsvc_wait in order to prevent
    // concurrent access to them
    loop.Shutdown();
  }

  GetBootItemFunction get_boot_item;

  async::Loop loop{&kAsyncLoopConfigNoAttachToCurrentThread};
  fbl::RefPtr<fs::PseudoDir> root{fbl::MakeRefCounted<fs::PseudoDir>()};
  fs::SynchronousVfs vfs{loop.dispatcher()};
  async::Wait bootsvc_wait;
};

struct IsolatedDevmgr::ExceptionLoopState {
  ExceptionLoopState(async_dispatcher_t* dispatcher, zx::channel exception_channel)
      : dispatcher_(dispatcher),
        exception_channel_(std::move(exception_channel)),
        watcher_(this, exception_channel_.get(), ZX_CHANNEL_READABLE) {
    if (dispatcher_ == nullptr) {
      loop_.emplace(&kAsyncLoopConfigNoAttachToCurrentThread);
      dispatcher_ = loop_->dispatcher();
    }
    watcher_.Begin(dispatcher_);
  }
  ~ExceptionLoopState() {
    // We must shut down the loop before we operate on watcher_ in order to prevent
    // concurrent access to them. If dispatcher is passed in, this should be done beforehand.
    if (loop_) {
      loop_->Shutdown();
    }
  }

  void HandleException() {
    printf("Handling devmgr exception\n");
    zx_exception_info_t info;
    zx::exception exception;
    zx_status_t status = exception_channel_.read(0, &info, exception.reset_and_get_address(),
                                                 sizeof(info), 1, nullptr, nullptr);
    if (status != ZX_OK) {
      return;
    }

    // Send exceptions to the ambient fuchsia.exception.Handler.
    auto local = service::Connect<::llcpp::fuchsia::exception::Handler>();
    if (!local.is_ok()) {
      return;
    }
    ::llcpp::fuchsia::exception::Handler::SyncClient handler(std::move(*local));
    ::llcpp::fuchsia::exception::ExceptionInfo einfo;
    einfo.process_koid = info.pid;
    einfo.thread_koid = info.tid;
    einfo.type = static_cast<::llcpp::fuchsia::exception::ExceptionType>(info.type);
    handler.OnException(std::move(exception), einfo);
  }

  void DevmgrException(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                       const zx_packet_signal_t* signal) {
    if (status == ZX_ERR_CANCELED) {
      return;
    }
    crashed_ = true;
    HandleException();
    if (exception_callback_) {
      exception_callback_();
    }
  }

  std::optional<async::Loop> loop_;
  async_dispatcher_t* dispatcher_ = nullptr;

  zx::channel exception_channel_;
  std::atomic<bool> crashed_ = false;
  fit::closure exception_callback_;
  async::WaitMethod<ExceptionLoopState, &ExceptionLoopState::DevmgrException> watcher_;
};

zx_status_t IsolatedDevmgr::SetupExceptionLoop(async_dispatcher_t* dispatcher,
                                               zx::channel exception_channel) {
  exception_loop_state_ =
      std::make_unique<ExceptionLoopState>(dispatcher, std::move(exception_channel));

  if (dispatcher == nullptr) {
    return exception_loop_state_->loop_->StartThread("isolated-devmgr-exceptionloop");
  } else {
    return ZX_OK;
  }
}

// Create and host a /svc directory for the devcoordinator process we're creating.
// TODO(fxbug.dev/35991): IsolatedDevmgr and devmgr_launcher should be rewritten to make use of
// Components v2/Test Framework concepts as soon as those are ready enough. For now this has to be
// manually kept in sync with devcoordinator's manifest in //src/sys/root/devcoordinator.cml
// (although it already seems to be incomplete).
zx_status_t IsolatedDevmgr::SetupSvcLoop(
    fidl::ServerEnd<llcpp::fuchsia::io::Directory> bootsvc_server,
    fidl::ClientEnd<llcpp::fuchsia::io::Directory> fshost_outgoing_client,
    GetBootItemFunction get_boot_item, std::map<std::string, std::string>&& boot_args) {
  svc_loop_state_ = std::make_unique<SvcLoopState>();
  svc_loop_state_->get_boot_item = std::move(get_boot_item);

  // Quit the loop when the channel is closed.
  svc_loop_state_->bootsvc_wait.set_object(bootsvc_server.channel().get());
  svc_loop_state_->bootsvc_wait.set_trigger(ZX_CHANNEL_PEER_CLOSED);
  svc_loop_state_->bootsvc_wait.set_handler([loop = &svc_loop_state_->loop](...) { loop->Quit(); });
  svc_loop_state_->bootsvc_wait.Begin(svc_loop_state_->loop.dispatcher());

  // Connect to /svc in the current namespace.
  auto svc_client = *service::OpenServiceRoot();

  // Connect to /svc in fshost's outgoing directory
  auto fshost_endpoints = fidl::CreateEndpoints<llcpp::fuchsia::io::Directory>();
  if (!fshost_endpoints.is_ok()) {
    return fshost_endpoints.status_value();
  }
  auto [fshost_svc_client, fshost_svc_server] = *std::move(fshost_endpoints);

  zx_status_t status =
      fdio_open_at(fshost_outgoing_client.channel().get(), "svc",
                   ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE | ZX_FS_FLAG_DIRECTORY,
                   fshost_svc_server.TakeChannel().release());
  if (status != ZX_OK) {
    return status;
  }

  // Forward required services from the current namespace.
  ForwardService(svc_loop_state_->root, "fuchsia.process.Launcher", CloneDirectory(svc_client));
  ForwardService(svc_loop_state_->root, "fuchsia.logger.LogSink", CloneDirectory(svc_client));
  ForwardService(svc_loop_state_->root, "fuchsia.boot.RootResource", std::move(svc_client));
  ForwardService(svc_loop_state_->root, "fuchsia.fshost.Loader", std::move(fshost_svc_client));

  boot_args.try_emplace("virtcon.disable", "true");

  // Host fake instances of some services normally provided by bootsvc and routed to devcoordinator
  // by component_manager. The difference between these fakes and the optional services above is
  // that these 1) are fakeable (unlike fuchsia.process.Launcher) and 2) seem to be required
  // services for devcoordinator.
  auto items_dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_Items_dispatch);
  CreateFakeService(svc_loop_state_->root, fuchsia_boot_Items_Name,
                    svc_loop_state_->loop.dispatcher(), items_dispatch,
                    &svc_loop_state_->get_boot_item, &kItemsOps);

  auto root_job_dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_kernel_RootJob_dispatch);
  CreateFakeService(svc_loop_state_->root, fuchsia_kernel_RootJob_Name,
                    svc_loop_state_->loop.dispatcher(), root_job_dispatch, &job_, &kRootJobOps);

  // Create fake Boot Arguments.
  CreateFakeCppService<llcpp::fuchsia::boot::Arguments>(
      svc_loop_state_->root, svc_loop_state_->loop.dispatcher(),
      std::make_unique<mock_boot_arguments::Server>(std::move(boot_args)));

  // Create fake Power Registration.
  CreateFakeCppService<llcpp::fuchsia::power::manager::DriverManagerRegistration>(
      svc_loop_state_->root, svc_loop_state_->loop.dispatcher(),
      std::make_unique<FakePowerRegistration>());

  CreateFakeCppService<llcpp::fuchsia::sys2::Realm>(
      svc_loop_state_->root, svc_loop_state_->loop.dispatcher(), std::make_unique<FakeRealm>());

  // Serve VFS on channel.
  svc_loop_state_->vfs.ServeDirectory(svc_loop_state_->root, std::move(bootsvc_server),
                                      fs::Rights::ReadWrite());

  return svc_loop_state_->loop.StartThread("isolated-devmgr-svcloop");
}

__EXPORT
zx_status_t IsolatedDevmgr::AddDevfsToOutgoingDir(vfs::PseudoDir* outgoing_root_dir) {
  zx::channel client, server;
  auto status = zx::channel::create(0, &client, &server);
  if (status != ZX_OK) {
    return status;
  }
  fdio_cpp::UnownedFdioCaller fd(devfs_root_.get());
  fdio_service_clone_to(fd.borrow_channel(), server.release());

  // Add devfs to out directory.
  auto devfs_out = std::make_unique<vfs::RemoteDir>(std::move(client));
  outgoing_root_dir->AddEntry("dev", std::move(devfs_out));
  return ZX_OK;
}

__EXPORT
devmgr_launcher::Args IsolatedDevmgr::DefaultArgs() {
  devmgr_launcher::Args args;
  args.sys_device_driver = kSysdevDriver;
  args.load_drivers.push_back("/boot/driver/test.so");
  args.driver_search_paths.push_back("/boot/driver/test");
  return args;
}

__EXPORT
IsolatedDevmgr::IsolatedDevmgr() = default;

__EXPORT
IsolatedDevmgr::~IsolatedDevmgr() { Terminate(); }

__EXPORT
IsolatedDevmgr::IsolatedDevmgr(IsolatedDevmgr&& other)
    : job_(std::move(other.job_)),
      svc_root_dir_(std::move(other.svc_root_dir_)),
      fshost_outgoing_dir_(std::move(other.fshost_outgoing_dir_)),
      devfs_root_(std::move(other.devfs_root_)),
      component_lifecycle_client_(std::move(other.component_lifecycle_client_)),
      svc_loop_state_(std::move(other.svc_loop_state_)),
      exception_loop_state_(std::move(other.exception_loop_state_)) {}

__EXPORT
IsolatedDevmgr& IsolatedDevmgr::operator=(IsolatedDevmgr&& other) {
  Terminate();
  job_ = std::move(other.job_);
  component_lifecycle_client_ = std::move(other.component_lifecycle_client_);
  devfs_root_ = std::move(other.devfs_root_);
  svc_root_dir_ = std::move(other.svc_root_dir_);
  fshost_outgoing_dir_ = std::move(other.fshost_outgoing_dir_);
  svc_loop_state_ = std::move(other.svc_loop_state_);
  exception_loop_state_ = std::move(other.exception_loop_state_);
  return *this;
}

__EXPORT
void IsolatedDevmgr::Terminate() {
  if (job_.is_valid()) {
    job_.kill();

    // Best-effort; ignores error.
    zx_signals_t observed = 0;
    job_.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), &observed);
  }
  job_.reset();
}

__EXPORT
zx_status_t IsolatedDevmgr::Create(devmgr_launcher::Args args, IsolatedDevmgr* out) {
  return Create(std::move(args), nullptr, out);
}

__EXPORT
zx_status_t IsolatedDevmgr::Create(devmgr_launcher::Args args, async_dispatcher_t* dispatcher,
                                   IsolatedDevmgr* out) {
  auto svc_endpoints = fidl::CreateEndpoints<llcpp::fuchsia::io::Directory>();
  if (!svc_endpoints.is_ok()) {
    return svc_endpoints.status_value();
  }
  auto [svc_client, svc_server] = *std::move(svc_endpoints);

  auto fshost_endpoints = fidl::CreateEndpoints<llcpp::fuchsia::io::Directory>();
  if (!fshost_endpoints.is_ok()) {
    return fshost_endpoints.status_value();
  }
  auto [fshost_outgoing_client, fshost_outgoing_server] = *std::move(fshost_endpoints);

  GetBootItemFunction get_boot_item = std::move(args.get_boot_item);
  auto component_lifecycle = fidl::CreateEndpoints<llcpp::fuchsia::process::lifecycle::Lifecycle>();
  if (!component_lifecycle.is_ok()) {
    return component_lifecycle.status_value();
  }

  IsolatedDevmgr devmgr;
  zx::channel devfs;
  fidl::ClientEnd<llcpp::fuchsia::io::Directory> outgoing_svc_root;
  std::map<std::string, std::string> boot_args = std::move(args.boot_args);
  zx_status_t status = devmgr_launcher::Launch(std::move(args), svc_client.TakeChannel(),
                                               fshost_outgoing_server.TakeChannel(),
                                               component_lifecycle->server.TakeChannel(),
                                               &devmgr.job_, &devfs, &outgoing_svc_root.channel());
  if (status != ZX_OK) {
    return status;
  }

  zx::channel exception_channel;
  devmgr.containing_job().create_exception_channel(0, &exception_channel);

  status = devmgr.SetupExceptionLoop(dispatcher, std::move(exception_channel));
  if (status != ZX_OK) {
    return status;
  }

  status = devmgr.SetupSvcLoop(std::move(svc_server), CloneDirectory(fshost_outgoing_client),
                               std::move(get_boot_item), std::move(boot_args));
  if (status != ZX_OK) {
    return status;
  }

  int fd;
  status = fdio_fd_create(devfs.release(), &fd);
  if (status != ZX_OK) {
    return status;
  }
  devmgr.devfs_root_.reset(fd);
  devmgr.component_lifecycle_client_ = std::move(component_lifecycle->client);
  devmgr.svc_root_dir_ = std::move(outgoing_svc_root);
  devmgr.fshost_outgoing_dir_ = std::move(fshost_outgoing_client);
  *out = std::move(devmgr);
  return ZX_OK;
}

__EXPORT void IsolatedDevmgr::SetExceptionCallback(fit::closure exception_callback) {
  exception_loop_state_->exception_callback_ = std::move(exception_callback);
}

__EXPORT bool IsolatedDevmgr::crashed() const { return exception_loop_state_->crashed_; }

}  // namespace devmgr_integration_test
