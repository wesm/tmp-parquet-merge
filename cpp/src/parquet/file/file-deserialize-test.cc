// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "parquet/column/page.h"
#include "parquet/compression/codec.h"
#include "parquet/exception.h"
#include "parquet/file/reader-internal.h"
#include "parquet/thrift/parquet_types.h"
#include "parquet/thrift/util.h"
#include "parquet/types.h"
#include "parquet/util/input.h"
#include "parquet/util/output.h"
#include "parquet/util/test-common.h"

namespace parquet_cpp {


// Adds page statistics occupying a certain amount of bytes (for testing very
// large page headers)
static inline void AddDummyStats(size_t stat_size,
    parquet::DataPageHeader& data_page) {

  std::vector<uint8_t> stat_bytes(stat_size);
  // Some non-zero value
  std::fill(stat_bytes.begin(), stat_bytes.end(), 1);
  data_page.statistics.__set_max(std::string(
          reinterpret_cast<const char*>(stat_bytes.data()), stat_size));
  data_page.__isset.statistics = true;
}

class TestPageSerde : public ::testing::Test {
 public:
  void SetUp() {
    data_page_header_.encoding = parquet::Encoding::PLAIN;
    data_page_header_.definition_level_encoding = parquet::Encoding::RLE;
    data_page_header_.repetition_level_encoding = parquet::Encoding::RLE;

    ResetStream();
  }

  void InitSerializedPageReader(Compression::type codec =
      Compression::UNCOMPRESSED) {
    EndStream();
    std::unique_ptr<InputStream> stream;
    stream.reset(new InMemoryInputStream(out_buffer_));
    page_reader_.reset(new SerializedPageReader(std::move(stream), codec));
  }

  void WriteDataPageHeader(int max_serialized_len = 1024,
      int32_t uncompressed_size = 0, int32_t compressed_size = 0) {
    // Simplifying writing serialized data page headers which may or may not
    // have meaningful data associated with them

    // Serialize the Page header
    uint32_t serialized_len = max_serialized_len;
    page_header_.__set_data_page_header(data_page_header_);
    page_header_.uncompressed_page_size = uncompressed_size;
    page_header_.compressed_page_size = compressed_size;
    page_header_.type = parquet::PageType::DATA_PAGE;

    ASSERT_NO_THROW(SerializeThriftMsg(&page_header_, max_serialized_len,
          out_stream_.get()));
  }

  void ResetStream() {
    out_stream_.reset(new InMemoryOutputStream());
  }

  void EndStream() {
    out_buffer_ = out_stream_->GetBuffer();
  }

 protected:
  std::unique_ptr<InMemoryOutputStream> out_stream_;
  std::shared_ptr<Buffer> out_buffer_;

  std::unique_ptr<SerializedPageReader> page_reader_;
  parquet::PageHeader page_header_;
  parquet::DataPageHeader data_page_header_;
};

void CheckDataPageHeader(const parquet::DataPageHeader expected,
    const Page* page) {
  ASSERT_EQ(PageType::DATA_PAGE, page->type());

  const DataPage* data_page = static_cast<const DataPage*>(page);
  ASSERT_EQ(expected.num_values, data_page->num_values());
  ASSERT_EQ(expected.encoding, data_page->encoding());
  ASSERT_EQ(expected.definition_level_encoding,
      data_page->definition_level_encoding());
  ASSERT_EQ(expected.repetition_level_encoding,
      data_page->repetition_level_encoding());

  if (expected.statistics.__isset.max) {
    ASSERT_EQ(0, memcmp(expected.statistics.max.c_str(),
            data_page->max(), expected.statistics.max.length()));
  }
  if (expected.statistics.__isset.min) {
    ASSERT_EQ(0, memcmp(expected.statistics.min.c_str(),
            data_page->min(), expected.statistics.min.length()));
  }
}

TEST_F(TestPageSerde, DataPage) {
  parquet::PageHeader out_page_header;

  int stats_size = 512;
  AddDummyStats(stats_size, data_page_header_);
  data_page_header_.num_values = 4444;

  WriteDataPageHeader();
  InitSerializedPageReader();
  std::shared_ptr<Page> current_page = page_reader_->NextPage();
  CheckDataPageHeader(data_page_header_, current_page.get());
}

TEST_F(TestPageSerde, TestLargePageHeaders) {
  int stats_size = 256 * 1024; // 256 KB
  AddDummyStats(stats_size, data_page_header_);

  // Any number to verify metadata roundtrip
  data_page_header_.num_values = 4141;

  int max_header_size = 512 * 1024; // 512 KB
  WriteDataPageHeader(max_header_size);
  ASSERT_GE(max_header_size, out_stream_->Tell());

  // check header size is between 256 KB to 16 MB
  ASSERT_LE(stats_size, out_stream_->Tell());
  ASSERT_GE(DEFAULT_MAX_PAGE_HEADER_SIZE, out_stream_->Tell());

  InitSerializedPageReader();
  std::shared_ptr<Page> current_page = page_reader_->NextPage();
  CheckDataPageHeader(data_page_header_, current_page.get());
}

TEST_F(TestPageSerde, TestFailLargePageHeaders) {
  int stats_size = 256 * 1024; // 256 KB
  AddDummyStats(stats_size, data_page_header_);

  // Serialize the Page header
  int max_header_size = 512 * 1024; // 512 KB
  WriteDataPageHeader(max_header_size);
  ASSERT_GE(max_header_size, out_stream_->Tell());

  int smaller_max_size = 128 * 1024;
  ASSERT_LE(smaller_max_size, out_stream_->Tell());
  InitSerializedPageReader();

  // Set the max page header size to 128 KB, which is less than the current
  // header size
  page_reader_->set_max_page_header_size(smaller_max_size);
  ASSERT_THROW(page_reader_->NextPage(), ParquetException);
}

TEST_F(TestPageSerde, Compression) {
  Compression::type codec_types[2] = {Compression::GZIP, Compression::SNAPPY};

  // This is a dummy number
  data_page_header_.num_values = 32;

  int num_pages = 10;

  std::vector<std::vector<uint8_t> > faux_data;
  faux_data.resize(num_pages);
  for (int i = 0; i < num_pages; ++i) {
    // The pages keep getting larger
    int page_size = (i + 1) * 64;
    test::random_bytes(page_size, 0, &faux_data[i]);
  }
  for (auto codec_type : codec_types) {
    std::unique_ptr<Codec> codec = Codec::Create(codec_type);

    std::vector<uint8_t> buffer;
    for (int i = 0; i < num_pages; ++i) {
      const uint8_t* data = faux_data[i].data();
      size_t data_size = faux_data[i].size();

      int64_t max_compressed_size = codec->MaxCompressedLen(data_size, data);
      buffer.resize(max_compressed_size);

      int64_t actual_size = codec->Compress(data_size, data,
          max_compressed_size, &buffer[0]);

      WriteDataPageHeader(1024, data_size, actual_size);
      out_stream_->Write(buffer.data(), actual_size);
    }

    InitSerializedPageReader(codec_type);

    std::shared_ptr<Page> page;
    const DataPage* data_page;
    for (int i = 0; i < num_pages; ++i) {
      size_t data_size = faux_data[i].size();
      page = page_reader_->NextPage();
      data_page = static_cast<const DataPage*>(page.get());
      ASSERT_EQ(data_size, data_page->size());
      ASSERT_EQ(0, memcmp(faux_data[i].data(), data_page->data(), data_size));
    }

    ResetStream();
  }
}

TEST_F(TestPageSerde, LZONotSupported) {
  // Must await PARQUET-530
  int data_size = 1024;
  std::vector<uint8_t> faux_data(data_size);
  WriteDataPageHeader(1024, data_size, data_size);
  out_stream_->Write(faux_data.data(), data_size);
  ASSERT_THROW(InitSerializedPageReader(Compression::LZO), ParquetException);
}

} // namespace parquet_cpp
