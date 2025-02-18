// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/compression/compressor.h"

#include <stdlib.h>
#include <zircon/assert.h>
#include <zircon/errors.h>

#include <algorithm>
#include <memory>

#include <block-client/cpp/fake-device.h>
#include <gtest/gtest.h>

#include "src/lib/digest/digest.h"
#include "src/lib/digest/merkle-tree.h"
#include "src/storage/blobfs/blobfs.h"
#include "src/storage/blobfs/common.h"
#include "src/storage/blobfs/compression/blob_compressor.h"
#include "src/storage/blobfs/compression/decompressor.h"
#include "src/storage/blobfs/compression/lz4.h"
#include "src/storage/blobfs/compression/zstd_plain.h"
#include "src/storage/blobfs/compression/zstd_seekable.h"
#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/mkfs.h"
#include "src/storage/blobfs/test/blob_utils.h"

namespace blobfs {
namespace {

enum class DataType {
  Compressible,
  Random,
};

std::unique_ptr<char[]> GenerateInput(DataType data_type, unsigned seed, size_t size) {
  std::unique_ptr<char[]> input(new char[size]);
  switch (data_type) {
    case DataType::Compressible: {
      size_t i = 0;
      while (i < size) {
        size_t run_length = 1 + (rand_r(&seed) % (size - i));
        char value = static_cast<char>(rand_r(&seed) % std::numeric_limits<char>::max());
        memset(input.get() + i, value, run_length);
        i += run_length;
      }
      break;
    }
    case DataType::Random:
      for (size_t i = 0; i < size; i++) {
        input[i] = static_cast<char>(rand_r(&seed));
      }
      break;
    default:
      EXPECT_TRUE(false) << "Bad Data Type";
  }
  return input;
}

void CompressionHelper(CompressionAlgorithm algorithm, const char* input, size_t size, size_t step,
                       std::optional<BlobCompressor>* out) {
  CompressionSettings settings{.compression_algorithm = algorithm};
  auto compressor = BlobCompressor::Create(settings, size);
  ASSERT_TRUE(compressor);

  size_t offset = 0;
  while (offset != size) {
    const void* data = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(input) + offset);
    const size_t incremental_size = std::min(step, size - offset);
    ASSERT_EQ(compressor->Update(data, incremental_size), ZX_OK);
    offset += incremental_size;
  }
  ASSERT_EQ(compressor->End(), ZX_OK);
  EXPECT_GT(compressor->Size(), 0ul);

  *out = std::move(compressor);
}

void DecompressionHelper(CompressionAlgorithm algorithm, const void* compressed_buf,
                         size_t compressed_size, const void* expected, size_t expected_size) {
  std::unique_ptr<char[]> uncompressed_buf(new char[expected_size]);
  size_t uncompressed_size = expected_size;
  std::unique_ptr<Decompressor> decompressor;
  ASSERT_EQ(Decompressor::Create(algorithm, &decompressor), ZX_OK);
  ASSERT_EQ(decompressor->Decompress(uncompressed_buf.get(), &uncompressed_size, compressed_buf,
                                     compressed_size),
            ZX_OK);
  EXPECT_EQ(expected_size, uncompressed_size);
  EXPECT_EQ(memcmp(expected, uncompressed_buf.get(), expected_size), 0);
}

// Tests a contained case of compression and decompression.
//
// size: The size of the input buffer.
// step: The step size of updating the compression buffer.
void RunCompressDecompressTest(CompressionAlgorithm algorithm, DataType data_type, size_t size,
                               size_t step) {
  ASSERT_LE(step, size) << "Step size too large";

  // Generate input.
  std::unique_ptr<char[]> input(GenerateInput(data_type, 0, size));

  // Compress a buffer.
  std::optional<BlobCompressor> compressor;
  CompressionHelper(algorithm, input.get(), size, step, &compressor);
  ASSERT_TRUE(compressor);

  // Decompress the buffer.

  DecompressionHelper(algorithm, compressor->Data(), compressor->Size(), input.get(), size);
}

TEST(CompressorTests, CompressDecompressLZ4Random1) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressLZ4Random2) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressLZ4Random3) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressLZ4Random4) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressDecompressLZ4Compressible1) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressLZ4Compressible2) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressLZ4Compressible3) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressLZ4Compressible4) {
  RunCompressDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressDecompressZSTDRandom1) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDRandom2) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDRandom3) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressZSTDRandom4) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressDecompressZSTDCompressible1) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDCompressible2) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDCompressible3) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressZSTDCompressible4) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableRandom1) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableRandom2) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableRandom3) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableRandom4) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 15,
                            1 << 10);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableCompressible1) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableCompressible2) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableCompressible3) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressZSTDSeekableCompressible4) {
  RunCompressDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 15,
                            1 << 10);
}

TEST(CompressorTests, CompressDecompressChunkRandom1) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressChunkRandom2) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressChunkRandom3) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressChunkRandom4) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressDecompressChunkCompressible1) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressDecompressChunkCompressible2) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressDecompressChunkCompressible3) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressDecompressChunkCompressible4) {
  RunCompressDecompressTest(CompressionAlgorithm::CHUNKED, DataType::Random, 1 << 15, 1 << 10);
}

void RunUpdateNoDataTest(CompressionAlgorithm algorithm) {
  const size_t input_size = 1024;
  CompressionSettings settings{.compression_algorithm = algorithm};
  auto compressor = BlobCompressor::Create(settings, input_size);
  ASSERT_TRUE(compressor);

  std::unique_ptr<char[]> input(new char[input_size]);
  memset(input.get(), 'a', input_size);

  // Test that using "Update(data, 0)" acts a no-op, rather than corrupting the buffer.
  ASSERT_EQ(compressor->Update(input.get(), 0), ZX_OK);
  ASSERT_EQ(compressor->Update(input.get(), input_size), ZX_OK);
  ASSERT_EQ(compressor->End(), ZX_OK);

  // Ensure that even with the addition of a zero-length buffer, we still decompress
  // to the expected output.
  DecompressionHelper(algorithm, compressor->Data(), compressor->Size(), input.get(), input_size);
}

TEST(CompressorTests, UpdateNoDataLZ4) { RunUpdateNoDataTest(CompressionAlgorithm::LZ4); }

TEST(CompressorTests, UpdateNoDataZSTD) { RunUpdateNoDataTest(CompressionAlgorithm::ZSTD); }

TEST(CompressorTests, UpdateNoDataZSTDSeekable) {
  RunUpdateNoDataTest(CompressionAlgorithm::ZSTD_SEEKABLE);
}

void DecompressionRoundHelper(CompressionAlgorithm algorithm, const void* compressed_buf,
                              size_t rounded_compressed_size, const void* expected,
                              size_t expected_size) {
  std::unique_ptr<char[]> uncompressed_buf(new char[expected_size]);
  size_t uncompressed_size = expected_size;
  size_t compressed_size = rounded_compressed_size;
  std::unique_ptr<Decompressor> decompressor;
  ASSERT_EQ(Decompressor::Create(algorithm, &decompressor), ZX_OK);
  ASSERT_EQ(decompressor->Decompress(uncompressed_buf.get(), &uncompressed_size, compressed_buf,
                                     compressed_size),
            ZX_OK);
  EXPECT_EQ(expected_size, uncompressed_size);
  EXPECT_EQ(memcmp(expected, uncompressed_buf.get(), expected_size), 0);
}

// Tests decompression's ability to handle receiving a compressed size that is rounded
// up to the nearest block size. This mimics blobfs' usage, where the exact compressed size
// is not stored explicitly.
//
// size: The size of the input buffer.
// step: The step size of updating the compression buffer.
void RunCompressRoundDecompressTest(CompressionAlgorithm algorithm, DataType data_type, size_t size,
                                    size_t step) {
  ASSERT_LE(step, size) << "Step size too large";

  // Generate input.
  std::unique_ptr<char[]> input(GenerateInput(data_type, 0, size));

  // Compress a buffer.
  std::optional<BlobCompressor> compressor;
  CompressionHelper(algorithm, input.get(), size, step, &compressor);
  ASSERT_TRUE(compressor);

  // Round up compressed size to nearest block size;
  size_t rounded_size = fbl::round_up(compressor->Size(), kBlobfsBlockSize);

  // Decompress the buffer while giving the rounded compressed size.

  DecompressionRoundHelper(algorithm, compressor->Data(), rounded_size, input.get(), size);
}

TEST(CompressorTests, CompressRoundDecompressLZ4Random1) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressRoundDecompressLZ4Random2) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressRoundDecompressLZ4Random3) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressRoundDecompressLZ4Random4) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::LZ4, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressRoundDecompressZSTDRandom1) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 0, 1 << 0);
}

TEST(CompressorTests, CompressRoundDecompressZSTDRandom2) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 1, 1 << 0);
}

TEST(CompressorTests, CompressRoundDecompressZSTDRandom3) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 10, 1 << 5);
}

TEST(CompressorTests, CompressRoundDecompressZSTDRandom4) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD, DataType::Random, 1 << 15, 1 << 10);
}

TEST(CompressorTests, CompressRoundDecompressZSTDSeekableRandom1) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 0,
                                 1 << 0);
}

TEST(CompressorTests, CompressRoundDecompressZSTDSeekableRandom2) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 1,
                                 1 << 0);
}

TEST(CompressorTests, CompressRoundDecompressZSTDSeekableRandom3) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 10,
                                 1 << 5);
}

TEST(CompressorTests, CompressRoundDecompressZSTDSeekableRandom4) {
  RunCompressRoundDecompressTest(CompressionAlgorithm::ZSTD_SEEKABLE, DataType::Random, 1 << 15,
                                 1 << 10);
}

// Regression test class: Ensure that decompression that is not advancing buffers terminates, even
// if the "size of next recommended input" hint from zstd is a non-zero, non-error value. It turns
// out, this hint is not intended to be authoritative, in the sense that it can be a non-zero,
// non-error value even though subsequent invocations of `ZSTD_decompressStream` will make no
// progress.
class NonZeroHintNonAdvancingZSTDDecompressor : public AbstractZSTDDecompressor {
 public:
  NonZeroHintNonAdvancingZSTDDecompressor() = default;

  // AbstractZSTDDecompressor interface.
  size_t DecompressStream(ZSTD_DStream* zds, ZSTD_outBuffer* output,
                          ZSTD_inBuffer* input) const final {
    // Do not advance streams, but return non-zero, non-error value.
    EXPECT_FALSE(ZSTD_isError(kDecompressStreamReturn));
    return kDecompressStreamReturn;
  }

 private:
  static const size_t kDecompressStreamReturn = 1;
};

// Regression test for fxbug.dev/44603.
// This test prevents regressing to the following *incorrect* logic:
//
//     do { ... r = ZSTD_decompressStream(...) ... } while (r != 0);
//
// The value of `r`, when not an error code, is a hint at the size of the next chunk to pass to
// `ZSTD_decompressStream`. Sometimes, this value is non-zero even though invoking
// `ZSTD_decompressStream` again will make no progress, inducing an infinite loop.
// See fxbug.dev/44603 for details.
TEST(CompressorTests, DecompressZSTDNonZeroNonAdvancing) {
  constexpr size_t kCompressedSize = 1;
  constexpr size_t kUncompressedSize = 2;
  uint8_t compressed_buf[kCompressedSize] = {0x00};
  uint8_t uncompressed_buf[kUncompressedSize] = {0x00, 0x00};
  size_t compressed_size = kCompressedSize;
  size_t uncompressed_size = kUncompressedSize;
  NonZeroHintNonAdvancingZSTDDecompressor decompressor;
  ASSERT_EQ(decompressor.Decompress(uncompressed_buf, &uncompressed_size, compressed_buf,
                                    compressed_size),
            ZX_OK);
}

class BlobfsTestFixture : public testing::Test {
 protected:
  BlobfsTestFixture() {
    constexpr uint64_t kBlockCount = 1024;
    auto device = std::make_unique<block_client::FakeBlockDevice>(kBlockCount, kBlobfsBlockSize);
    EXPECT_EQ(FormatFilesystem(device.get(), FilesystemOptions{}), ZX_OK);
    EXPECT_EQ(Blobfs::Create(nullptr, std::move(device), blobfs::MountOptions(), zx::resource(),
                             &blobfs_),
              ZX_OK);
    fbl::RefPtr<fs::Vnode> root;
    EXPECT_EQ(blobfs_->OpenRootNode(&root), ZX_OK);
    root_ = fbl::RefPtr<Directory>::Downcast(std::move(root));
  }

  fbl::RefPtr<fs::Vnode> AddBlobToBlobfs(size_t data_size, DataType type) {
    std::unique_ptr<BlobInfo> blob_info = GenerateBlob(
        [type](uint8_t* data, size_t length) {
          auto generated_data = GenerateInput(type, 0, length);
          memcpy(data, generated_data.get(), length);
        },
        "", data_size);

    fbl::RefPtr<fs::Vnode> file;
    zx_status_t status = root_->Create(blob_info->path + 1, 0, &file);
    EXPECT_EQ(status, ZX_OK) << "Could not create file";
    if (status != ZX_OK) {
      return nullptr;
    }

    status = file->Truncate(data_size);
    EXPECT_EQ(status, ZX_OK) << "Could not truncate file";
    if (status != ZX_OK) {
      return nullptr;
    }
    size_t actual = 0;
    status = file->Write(blob_info->data.get(), data_size, 0, &actual);
    EXPECT_EQ(status, ZX_OK) << "Could not write file";
    if (status != ZX_OK) {
      return nullptr;
    }
    EXPECT_EQ(actual, data_size) << "Unexpected amount of written data";
    if (actual != data_size) {
      return nullptr;
    }

    return file;
  }

 private:
  std::unique_ptr<blobfs::Blobfs> blobfs_;
  fbl::RefPtr<Directory> root_;
};

using CompressorBlobfsTests = BlobfsTestFixture;

// Test that we do compress small blobs with compressible content.
TEST_F(CompressorBlobfsTests, CompressSmallCompressibleBlobs) {
  struct TestCase {
    size_t data_size;
    size_t expected_max_storage_size;
  };

  TestCase test_cases[] = {
      {
          16 * 1024 - 1,
          16 * 1024,
      },
      {
          16 * 1024,
          16 * 1024,
      },
      {
          16 * 1024 + 1,
          16 * 1024,
      },
  };

  for (const TestCase& test_case : test_cases) {
    printf("Test case: data size %zu\n", test_case.data_size);
    fbl::RefPtr<fs::Vnode> file = AddBlobToBlobfs(test_case.data_size, DataType::Compressible);

    fs::VnodeAttributes attributes;
    ASSERT_EQ(file->GetAttributes(&attributes), ZX_OK);

    EXPECT_EQ(attributes.content_size, test_case.data_size);
    EXPECT_LE(attributes.storage_size, test_case.expected_max_storage_size);

    ASSERT_EQ(file->Close(), ZX_OK);
  }
}

TEST_F(CompressorBlobfsTests, DoNotInflateIncompressibleBlobs) {
  size_t data_sizes[] = {
      8 * 1024 - 1, 8 * 1024, 8 * 1024 + 1, 16 * 1024 - 1, 16 * 1024, 16 * 1024 + 1, 128 * 8192 + 1,
  };

  for (size_t data_size : data_sizes) {
    if (data_size != 8193)
      continue;
    printf("Test case: data size %zu\n", data_size);
    fbl::RefPtr<fs::Vnode> file = AddBlobToBlobfs(data_size, DataType::Random);

    fs::VnodeAttributes attributes;
    ASSERT_EQ(file->GetAttributes(&attributes), ZX_OK);

    EXPECT_EQ(attributes.content_size, data_size);
    // Beyond 1 block, we need 1 block for the Merkle tree.
    size_t expected_max_storage_size = fbl::round_up(data_size, kBlobfsBlockSize) +
                                       (data_size > kBlobfsBlockSize ? kBlobfsBlockSize : 0);

    EXPECT_LE(attributes.storage_size, expected_max_storage_size);

    ASSERT_EQ(file->Close(), ZX_OK);
  }
}

}  // namespace
}  // namespace blobfs
