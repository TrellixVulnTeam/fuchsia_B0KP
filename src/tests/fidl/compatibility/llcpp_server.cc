// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/stdcompat/optional.h>
#include <lib/sys/cpp/component_context.h>
#include <zircon/status.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include <fidl/test/compatibility/llcpp/fidl.h>

constexpr const char kEchoInterfaceName[] = "fidl.test.compatibility.Echo";

namespace llcpp {
namespace fidl {
namespace test {
namespace compatibility {

class EchoClientApp {
 public:
  EchoClientApp(::fidl::StringView&& server_url)
      : context_(sys::ComponentContext::CreateAndServeOutgoingDirectory()),
        client_(Echo::SyncClient(ConnectTo(std::move(server_url)))) {}

  // Half the methods are testing the managed flavor; the other half are testing caller-allocate.

  Echo::ResultOf::EchoStruct EchoStruct(Struct value, ::fidl::StringView forward_to_server) {
    return client_.EchoStruct(std::move(value), std::move(forward_to_server));
  }

  Echo::ResultOf::EchoStructWithError EchoStructWithError(Struct value, default_enum err,
                                                          ::fidl::StringView forward_to_server,
                                                          RespondWith result_variant) {
    return client_.EchoStructWithError(std::move(value), err, std::move(forward_to_server),
                                       result_variant);
  }

  zx_status_t EchoStructNoRetVal(Struct value, ::fidl::StringView forward_to_server,
                                 Echo::SyncEventHandler& event_handler) {
    auto result = client_.EchoStructNoRetVal(std::move(value), std::move(forward_to_server));
    if (result.status() != ZX_OK) {
      return result.status();
    }
    return client_.HandleOneEvent(event_handler).status();
  }

  Echo::UnownedResultOf::EchoArrays EchoArrays(::fidl::BufferSpan request_buffer,
                                               ArraysStruct value,
                                               ::fidl::StringView forward_to_server,
                                               ::fidl::BufferSpan response_buffer) {
    return client_.EchoArrays(request_buffer, std::move(value), std::move(forward_to_server),
                              response_buffer);
  }

  Echo::ResultOf::EchoArraysWithError EchoArraysWithError(ArraysStruct value, default_enum err,
                                                          ::fidl::StringView forward_to_server,
                                                          RespondWith result_variant) {
    return client_.EchoArraysWithError(std::move(value), err, std::move(forward_to_server),
                                       result_variant);
  }

  Echo::ResultOf::EchoVectors EchoVectors(VectorsStruct value,
                                          ::fidl::StringView forward_to_server) {
    return client_.EchoVectors(std::move(value), std::move(forward_to_server));
  }

  Echo::ResultOf::EchoVectorsWithError EchoVectorsWithError(VectorsStruct value, default_enum err,
                                                            ::fidl::StringView forward_to_server,
                                                            RespondWith result_variant) {
    return client_.EchoVectorsWithError(std::move(value), err, std::move(forward_to_server),
                                        result_variant);
  }

  Echo::UnownedResultOf::EchoTable EchoTable(::fidl::BufferSpan request_buffer, AllTypesTable value,
                                             ::fidl::StringView forward_to_server,
                                             ::fidl::BufferSpan response_buffer) {
    return client_.EchoTable(request_buffer, std::move(value), std::move(forward_to_server),
                             response_buffer);
  }

  Echo::UnownedResultOf::EchoTableWithError EchoTableWithError(::fidl::BufferSpan request_buffer,
                                                               AllTypesTable value,
                                                               default_enum err,
                                                               ::fidl::StringView forward_to_server,
                                                               RespondWith result_variant,
                                                               ::fidl::BufferSpan response_buffer) {
    return client_.EchoTableWithError(request_buffer, std::move(value), err,
                                      std::move(forward_to_server), result_variant,
                                      response_buffer);
  }

  Echo::ResultOf::EchoXunions EchoXunions(::fidl::VectorView<AllTypesXunion> value,
                                          ::fidl::StringView forward_to_server) {
    return client_.EchoXunions(std::move(value), std::move(forward_to_server));
  }

  Echo::ResultOf::EchoXunionsWithError EchoXunionsWithError(
      ::fidl::VectorView<AllTypesXunion> value, default_enum err,
      ::fidl::StringView forward_to_server, RespondWith result_variant) {
    return client_.EchoXunionsWithError(std::move(value), err, std::move(forward_to_server),
                                        result_variant);
  }

  EchoClientApp(const EchoClientApp&) = delete;
  EchoClientApp& operator=(const EchoClientApp&) = delete;

 private:
  // Called once upon construction to launch and connect to the server.
  zx::channel ConnectTo(::fidl::StringView server_url) {
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = std::string(server_url.data(), server_url.size());
    echo_provider_ = sys::ServiceDirectory::CreateWithRequest(&launch_info.directory_request);

    fuchsia::sys::LauncherPtr launcher;
    context_->svc()->Connect(launcher.NewRequest());
    launcher->CreateComponent(std::move(launch_info), controller_.NewRequest());

    zx::channel server_end, client_end;
    ZX_ASSERT(zx::channel::create(0, &client_end, &server_end) == ZX_OK);
    ZX_ASSERT(echo_provider_->Connect(kEchoInterfaceName, std::move(server_end)) == ZX_OK);

    return client_end;
  }

  std::unique_ptr<sys::ComponentContext> context_;
  std::shared_ptr<sys::ServiceDirectory> echo_provider_;
  fuchsia::sys::ComponentControllerPtr controller_;
  Echo::SyncClient client_;
};

class EchoConnection final : public Echo::Interface {
 public:
  EchoConnection() = default;

  void EchoStruct(Struct value, ::fidl::StringView forward_to_server,
                  EchoStructCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      completer.Reply(std::move(value));
    } else {
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoStruct(std::move(value), "");
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s", result.error());
      completer.Reply(std::move(result.Unwrap()->value));
    }
  }

  void EchoStructWithError(Struct value, default_enum err, ::fidl::StringView forward_to_server,
                           RespondWith result_variant,
                           EchoStructWithErrorCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      if (result_variant == RespondWith::ERR) {
        completer.ReplyError(err);
      } else {
        completer.ReplySuccess(std::move(value));
      }
    } else {
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoStructWithError(std::move(value), err, "", result_variant);
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s", result.error());
      completer.Reply(std::move(result->result));
    }
  }

  void EchoStructNoRetVal(Struct value, ::fidl::StringView forward_to_server,
                          EchoStructNoRetValCompleter::Sync&) override {
    if (forward_to_server.empty()) {
      auto status = server_binding_.value()->EchoEvent(std::move(value));
      ZX_ASSERT_MSG(status == ZX_OK, "Replying with event failed: %s",
                    zx_status_get_string(status));
    } else {
      class EventHandler : public Echo::SyncEventHandler {
       public:
        explicit EventHandler(EchoConnection* connection) : connection_(connection) {}

        zx_status_t status() const { return status_; }

        void EchoEvent(Echo::EchoEventResponse* event) override {
          status_ = connection_->server_binding_.value()->EchoEvent(std::move(event->value));
        }

        zx_status_t Unknown() override {
          ZX_PANIC("Received unexpected event");
          return ZX_ERR_INVALID_ARGS;
        }

       private:
        EchoConnection* const connection_;
        zx_status_t status_ = ZX_OK;
      };

      EchoClientApp app(std::move(forward_to_server));
      EventHandler event_handler(this);
      zx_status_t status = app.EchoStructNoRetVal(std::move(value), "", event_handler);
      ZX_ASSERT_MSG(status == ZX_OK, "Replying with event failed direct: %s",
                    zx_status_get_string(status));
      ZX_ASSERT_MSG(event_handler.status() == ZX_OK, "Replying with event failed indirect: %s",
                    zx_status_get_string(event_handler.status()));
    }
  }

  void EchoArrays(ArraysStruct value, ::fidl::StringView forward_to_server,
                  EchoArraysCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      completer.Reply(std::move(value));
    } else {
      std::vector<uint8_t> request_buffer(ZX_CHANNEL_MAX_MSG_BYTES);
      std::vector<uint8_t> response_buffer(ZX_CHANNEL_MAX_MSG_BYTES);
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoArrays(
          ::fidl::BufferSpan(&request_buffer[0], static_cast<uint32_t>(request_buffer.size())),
          std::move(value), "",
          ::fidl::BufferSpan(&response_buffer[0], static_cast<uint32_t>(response_buffer.size())));
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s", result.error());
      completer.Reply(std::move(result.Unwrap()->value));
    }
  }

  void EchoArraysWithError(ArraysStruct value, default_enum err,
                           ::fidl::StringView forward_to_server, RespondWith result_variant,
                           EchoArraysWithErrorCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      if (result_variant == RespondWith::ERR) {
        completer.ReplyError(err);
      } else {
        completer.ReplySuccess(std::move(value));
      }
    } else {
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoArraysWithError(std::move(value), err, "", result_variant);
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result->result));
    }
  }

  void EchoVectors(VectorsStruct value, ::fidl::StringView forward_to_server,
                   EchoVectorsCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      completer.Reply(std::move(value));
    } else {
      EchoClientApp app(std::move(forward_to_server));
      VectorsStruct out_value;
      auto result = app.EchoVectors(std::move(value), "");
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result.Unwrap()->value));
    }
  }

  void EchoVectorsWithError(VectorsStruct value, default_enum err,
                            ::fidl::StringView forward_to_server, RespondWith result_variant,
                            EchoVectorsWithErrorCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      if (result_variant == RespondWith::ERR) {
        completer.ReplyError(err);
      } else {
        completer.ReplySuccess(std::move(value));
      }
    } else {
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoVectorsWithError(std::move(value), err, "", result_variant);
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result->result));
    }
  }

  void EchoTable(AllTypesTable value, ::fidl::StringView forward_to_server,
                 EchoTableCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      completer.Reply(std::move(value));
    } else {
      std::vector<uint8_t> request_buffer(ZX_CHANNEL_MAX_MSG_BYTES);
      std::vector<uint8_t> response_buffer(ZX_CHANNEL_MAX_MSG_BYTES);
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoTable(
          ::fidl::BufferSpan(&request_buffer[0], static_cast<uint32_t>(request_buffer.size())),
          std::move(value), "",
          ::fidl::BufferSpan(&response_buffer[0], static_cast<uint32_t>(response_buffer.size())));
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result.Unwrap()->value));
    }
  }

  void EchoTableWithError(AllTypesTable value, default_enum err,
                          ::fidl::StringView forward_to_server, RespondWith result_variant,
                          EchoTableWithErrorCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      if (result_variant == RespondWith::ERR) {
        completer.ReplyError(err);
      } else {
        completer.ReplySuccess(std::move(value));
      }
    } else {
      std::vector<uint8_t> request_buffer(ZX_CHANNEL_MAX_MSG_BYTES);
      std::vector<uint8_t> response_buffer(ZX_CHANNEL_MAX_MSG_BYTES);
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoTableWithError(
          ::fidl::BufferSpan(&request_buffer[0], static_cast<uint32_t>(request_buffer.size())),
          std::move(value), err, "", result_variant,
          ::fidl::BufferSpan(&response_buffer[0], static_cast<uint32_t>(response_buffer.size())));
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result->result));
    }
  }

  void EchoXunions(::fidl::VectorView<AllTypesXunion> value, ::fidl::StringView forward_to_server,
                   EchoXunionsCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      completer.Reply(std::move(value));
    } else {
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoXunions(std::move(value), "");
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result.Unwrap()->value));
    }
  }

  void EchoXunionsWithError(::fidl::VectorView<AllTypesXunion> value, default_enum err,
                            ::fidl::StringView forward_to_server, RespondWith result_variant,
                            EchoXunionsWithErrorCompleter::Sync& completer) override {
    if (forward_to_server.empty()) {
      if (result_variant == RespondWith::ERR) {
        completer.ReplyError(err);
      } else {
        completer.ReplySuccess(std::move(value));
      }
    } else {
      EchoClientApp app(std::move(forward_to_server));
      auto result = app.EchoXunionsWithError(std::move(value), err, "", result_variant);
      ZX_ASSERT_MSG(result.status() == ZX_OK, "Forwarding failed: %s: %s",
                    zx_status_get_string(result.status()), result.error());
      completer.Reply(std::move(result->result));
    }
  }

  void set_server_binding(::fidl::ServerBindingRef<Echo> binding) {
    server_binding_.emplace(binding);
  }

 private:
  cpp17::optional<::fidl::ServerBindingRef<Echo>> server_binding_;
};

}  // namespace compatibility
}  // namespace test
}  // namespace fidl
}  // namespace llcpp

int main(int argc, const char** argv) {
  // The FIDL support lib requires async_get_default_dispatcher() to return
  // non-null.
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
  std::vector<std::unique_ptr<llcpp::fidl::test::compatibility::EchoConnection>> connections;

  context->outgoing()->AddPublicService(
      std::make_unique<vfs::Service>([&](zx::channel request, async_dispatcher_t* dispatcher) {
        auto conn = std::make_unique<llcpp::fidl::test::compatibility::EchoConnection>();
        auto result = ::fidl::BindServer(dispatcher, std::move(request), conn.get());
        ZX_ASSERT(result.is_ok());
        conn->set_server_binding(result.take_value());
        connections.push_back(std::move(conn));
      }),
      kEchoInterfaceName);

  loop.Run();
  return EXIT_SUCCESS;
}
