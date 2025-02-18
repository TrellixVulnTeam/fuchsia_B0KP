// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/update/verify/llcpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <string>

#include <gtest/gtest.h>

#include "src/storage/blobfs/test/integration/blobfs_fixtures.h"

namespace blobfs {
namespace {

namespace fuv = ::llcpp::fuchsia::update::verify;

class HealthCheckTest : public ParameterizedBlobfsTest {
 protected:
  fuv::BlobfsVerifier::SyncClient ConnectToHealthCheckService() {
    auto endpoints = fidl::CreateEndpoints<fuv::BlobfsVerifier>();
    EXPECT_EQ(endpoints.status_value(), ZX_OK);
    auto [client_end, server_end] = *std::move(endpoints);

    std::string service_path = std::string("svc/") + fuv::BlobfsVerifier::Name;
    EXPECT_EQ(fdio_service_connect_at(fs().GetOutgoingDirectory()->get(), service_path.c_str(),
                                      server_end.TakeChannel().release()),
              ZX_OK);
    return fuv::BlobfsVerifier::SyncClient(std::move(client_end));
  }
};

TEST_P(HealthCheckTest, EmptyFilesystem) {
  fuv::VerifyOptions options;
  auto status = ConnectToHealthCheckService().Verify(std::move(options));
  ASSERT_EQ(status.status(), ZX_OK) << status.error();
}

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, HealthCheckTest,
                         testing::Values(BlobfsDefaultTestParam(), BlobfsWithFvmTestParam(),
                                         BlobfsWithCompactLayoutTestParam()),
                         testing::PrintToStringParamName());

}  // namespace
}  // namespace blobfs
