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

#include "parquet/column_writer.h"

#include "arrow/util/bit-util.h"
#include "arrow/util/rle-encoding.h"

#include "parquet/encoding-internal.h"
#include "parquet/properties.h"
#include "parquet/statistics.h"
#include "parquet/util/logging.h"
#include "parquet/util/memory.h"

namespace parquet {

using BitWriter = ::arrow::BitWriter;
using RleEncoder = ::arrow::RleEncoder;

LevelEncoder::LevelEncoder() {}
LevelEncoder::~LevelEncoder() {}

void LevelEncoder::Init(Encoding::type encoding, int16_t max_level,
                        int num_buffered_values, uint8_t* data, int data_size) {
  bit_width_ = BitUtil::Log2(max_level + 1);
  encoding_ = encoding;
  switch (encoding) {
    case Encoding::RLE: {
      rle_encoder_.reset(new RleEncoder(data, data_size, bit_width_));
      break;
    }
    case Encoding::BIT_PACKED: {
      int num_bytes =
          static_cast<int>(BitUtil::Ceil(num_buffered_values * bit_width_, 8));
      bit_packed_encoder_.reset(new BitWriter(data, num_bytes));
      break;
    }
    default:
      throw ParquetException("Unknown encoding type for levels.");
  }
}

int LevelEncoder::MaxBufferSize(Encoding::type encoding, int16_t max_level,
                                int num_buffered_values) {
  int bit_width = BitUtil::Log2(max_level + 1);
  int num_bytes = 0;
  switch (encoding) {
    case Encoding::RLE: {
      // TODO: Due to the way we currently check if the buffer is full enough,
      // we need to have MinBufferSize as head room.
      num_bytes = RleEncoder::MaxBufferSize(bit_width, num_buffered_values) +
                  RleEncoder::MinBufferSize(bit_width);
      break;
    }
    case Encoding::BIT_PACKED: {
      num_bytes = static_cast<int>(BitUtil::Ceil(num_buffered_values * bit_width, 8));
      break;
    }
    default:
      throw ParquetException("Unknown encoding type for levels.");
  }
  return num_bytes;
}

int LevelEncoder::Encode(int batch_size, const int16_t* levels) {
  int num_encoded = 0;
  if (!rle_encoder_ && !bit_packed_encoder_) {
    throw ParquetException("Level encoders are not initialized.");
  }

  if (encoding_ == Encoding::RLE) {
    for (int i = 0; i < batch_size; ++i) {
      if (!rle_encoder_->Put(*(levels + i))) {
        break;
      }
      ++num_encoded;
    }
    rle_encoder_->Flush();
    rle_length_ = rle_encoder_->len();
  } else {
    for (int i = 0; i < batch_size; ++i) {
      if (!bit_packed_encoder_->PutValue(*(levels + i), bit_width_)) {
        break;
      }
      ++num_encoded;
    }
    bit_packed_encoder_->Flush();
  }
  return num_encoded;
}

// ----------------------------------------------------------------------
// ColumnWriter

std::shared_ptr<WriterProperties> default_writer_properties() {
  static std::shared_ptr<WriterProperties> default_writer_properties =
      WriterProperties::Builder().build();
  return default_writer_properties;
}

ColumnWriter::ColumnWriter(ColumnChunkMetaDataBuilder* metadata,
                           std::unique_ptr<PageWriter> pager, int64_t expected_rows,
                           bool has_dictionary, Encoding::type encoding,
                           const WriterProperties* properties)
    : metadata_(metadata),
      descr_(metadata->descr()),
      pager_(std::move(pager)),
      expected_rows_(expected_rows),
      has_dictionary_(has_dictionary),
      encoding_(encoding),
      properties_(properties),
      allocator_(properties->memory_pool()),
      pool_(properties->memory_pool()),
      num_buffered_values_(0),
      num_buffered_encoded_values_(0),
      num_rows_(0),
      total_bytes_written_(0),
      closed_(false),
      fallback_(false) {
  definition_levels_sink_.reset(new InMemoryOutputStream(allocator_));
  repetition_levels_sink_.reset(new InMemoryOutputStream(allocator_));
  definition_levels_rle_ =
      std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
  repetition_levels_rle_ =
      std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
  uncompressed_data_ =
      std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
  if (pager_->has_compressor()) {
    compressed_data_ =
        std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
  }
}

void ColumnWriter::InitSinks() {
  definition_levels_sink_->Clear();
  repetition_levels_sink_->Clear();
}

void ColumnWriter::WriteDefinitionLevels(int64_t num_levels, const int16_t* levels) {
  DCHECK(!closed_);
  definition_levels_sink_->Write(reinterpret_cast<const uint8_t*>(levels),
                                 sizeof(int16_t) * num_levels);
}

void ColumnWriter::WriteRepetitionLevels(int64_t num_levels, const int16_t* levels) {
  DCHECK(!closed_);
  repetition_levels_sink_->Write(reinterpret_cast<const uint8_t*>(levels),
                                 sizeof(int16_t) * num_levels);
}

// return the size of the encoded buffer
int64_t ColumnWriter::RleEncodeLevels(const Buffer& src_buffer,
                                      ResizableBuffer* dest_buffer, int16_t max_level) {
  // TODO: This only works with due to some RLE specifics
  int64_t rle_size = LevelEncoder::MaxBufferSize(Encoding::RLE, max_level,
                                                 static_cast<int>(num_buffered_values_)) +
                     sizeof(int32_t);

  // Use Arrow::Buffer::shrink_to_fit = false
  // underlying buffer only keeps growing. Resize to a smaller size does not reallocate.
  PARQUET_THROW_NOT_OK(dest_buffer->Resize(rle_size, false));

  level_encoder_.Init(Encoding::RLE, max_level, static_cast<int>(num_buffered_values_),
                      dest_buffer->mutable_data() + sizeof(int32_t),
                      static_cast<int>(dest_buffer->size()) - sizeof(int32_t));
  int encoded =
      level_encoder_.Encode(static_cast<int>(num_buffered_values_),
                            reinterpret_cast<const int16_t*>(src_buffer.data()));
  DCHECK_EQ(encoded, num_buffered_values_);
  reinterpret_cast<int32_t*>(dest_buffer->mutable_data())[0] = level_encoder_.len();
  int64_t encoded_size = level_encoder_.len() + sizeof(int32_t);
  return encoded_size;
}

void ColumnWriter::AddDataPage() {
  int64_t definition_levels_rle_size = 0;
  int64_t repetition_levels_rle_size = 0;

  std::shared_ptr<Buffer> values = GetValuesBuffer();

  if (descr_->max_definition_level() > 0) {
    definition_levels_rle_size =
        RleEncodeLevels(definition_levels_sink_->GetBufferRef(),
                        definition_levels_rle_.get(), descr_->max_definition_level());
  }

  if (descr_->max_repetition_level() > 0) {
    repetition_levels_rle_size =
        RleEncodeLevels(repetition_levels_sink_->GetBufferRef(),
                        repetition_levels_rle_.get(), descr_->max_repetition_level());
  }

  int64_t uncompressed_size =
      definition_levels_rle_size + repetition_levels_rle_size + values->size();

  // Use Arrow::Buffer::shrink_to_fit = false
  // underlying buffer only keeps growing. Resize to a smaller size does not reallocate.
  PARQUET_THROW_NOT_OK(uncompressed_data_->Resize(uncompressed_size, false));

  // Concatenate data into a single buffer
  uint8_t* uncompressed_ptr = uncompressed_data_->mutable_data();
  memcpy(uncompressed_ptr, repetition_levels_rle_->data(), repetition_levels_rle_size);
  uncompressed_ptr += repetition_levels_rle_size;
  memcpy(uncompressed_ptr, definition_levels_rle_->data(), definition_levels_rle_size);
  uncompressed_ptr += definition_levels_rle_size;
  memcpy(uncompressed_ptr, values->data(), values->size());

  EncodedStatistics page_stats = GetPageStatistics();
  ResetPageStatistics();

  std::shared_ptr<Buffer> compressed_data;
  if (pager_->has_compressor()) {
    pager_->Compress(*(uncompressed_data_.get()), compressed_data_.get());
    compressed_data = compressed_data_;
  } else {
    compressed_data = uncompressed_data_;
  }

  // Write the page to OutputStream eagerly if there is no dictionary or
  // if dictionary encoding has fallen back to PLAIN
  if (has_dictionary_ && !fallback_) {  // Save pages until end of dictionary encoding
    std::shared_ptr<Buffer> compressed_data_copy;
    PARQUET_THROW_NOT_OK(compressed_data->Copy(0, compressed_data->size(), allocator_,
                                               &compressed_data_copy));
    CompressedDataPage page(compressed_data_copy,
                            static_cast<int32_t>(num_buffered_values_), encoding_,
                            Encoding::RLE, Encoding::RLE, uncompressed_size, page_stats);
    data_pages_.push_back(std::move(page));
  } else {  // Eagerly write pages
    CompressedDataPage page(compressed_data, static_cast<int32_t>(num_buffered_values_),
                            encoding_, Encoding::RLE, Encoding::RLE, uncompressed_size,
                            page_stats);
    WriteDataPage(page);
  }

  // Re-initialize the sinks for next Page.
  InitSinks();
  num_buffered_values_ = 0;
  num_buffered_encoded_values_ = 0;
}

void ColumnWriter::WriteDataPage(const CompressedDataPage& page) {
  total_bytes_written_ += pager_->WriteDataPage(page);
}

int64_t ColumnWriter::Close() {
  if (!closed_) {
    closed_ = true;
    if (has_dictionary_ && !fallback_) {
      WriteDictionaryPage();
    }

    FlushBufferedDataPages();

    EncodedStatistics chunk_statistics = GetChunkStatistics();
    if (chunk_statistics.is_set()) metadata_->SetStatistics(chunk_statistics);
    pager_->Close(has_dictionary_, fallback_);
  }

  if (num_rows_ != expected_rows_) {
    std::stringstream ss;
    ss << "Written rows: " << num_rows_ << " != expected rows: " << expected_rows_
       << "in the current column chunk";
    throw ParquetException(ss.str());
  }

  return total_bytes_written_;
}

void ColumnWriter::FlushBufferedDataPages() {
  // Write all outstanding data to a new page
  if (num_buffered_values_ > 0) {
    AddDataPage();
  }
  for (size_t i = 0; i < data_pages_.size(); i++) {
    WriteDataPage(data_pages_[i]);
  }
  data_pages_.clear();
}

// ----------------------------------------------------------------------
// TypedColumnWriter

template <typename Type>
TypedColumnWriter<Type>::TypedColumnWriter(ColumnChunkMetaDataBuilder* metadata,
                                           std::unique_ptr<PageWriter> pager,
                                           int64_t expected_rows, Encoding::type encoding,
                                           const WriterProperties* properties)
    : ColumnWriter(metadata, std::move(pager), expected_rows,
                   (encoding == Encoding::PLAIN_DICTIONARY ||
                    encoding == Encoding::RLE_DICTIONARY),
                   encoding, properties) {
  switch (encoding) {
    case Encoding::PLAIN:
      current_encoder_.reset(new PlainEncoder<Type>(descr_, properties->memory_pool()));
      break;
    case Encoding::PLAIN_DICTIONARY:
    case Encoding::RLE_DICTIONARY:
      current_encoder_.reset(
          new DictEncoder<Type>(descr_, &pool_, properties->memory_pool()));
      break;
    default:
      ParquetException::NYI("Selected encoding is not supported");
  }

  if (properties->statistics_enabled(descr_->path())) {
    page_statistics_ = std::unique_ptr<TypedStats>(new TypedStats(descr_, allocator_));
    chunk_statistics_ = std::unique_ptr<TypedStats>(new TypedStats(descr_, allocator_));
  }
}

// Only one Dictionary Page is written.
// Fallback to PLAIN if dictionary page limit is reached.
template <typename Type>
void TypedColumnWriter<Type>::CheckDictionarySizeLimit() {
  auto dict_encoder = static_cast<DictEncoder<Type>*>(current_encoder_.get());
  if (dict_encoder->dict_encoded_size() >= properties_->dictionary_pagesize_limit()) {
    WriteDictionaryPage();
    // Serialize the buffered Dictionary Indicies
    FlushBufferedDataPages();
    fallback_ = true;
    // Only PLAIN encoding is supported for fallback in V1
    current_encoder_.reset(new PlainEncoder<Type>(descr_, properties_->memory_pool()));
    encoding_ = Encoding::PLAIN;
  }
}

template <typename Type>
void TypedColumnWriter<Type>::WriteDictionaryPage() {
  auto dict_encoder = static_cast<DictEncoder<Type>*>(current_encoder_.get());
  std::shared_ptr<PoolBuffer> buffer =
      AllocateBuffer(properties_->memory_pool(), dict_encoder->dict_encoded_size());
  dict_encoder->WriteDict(buffer->mutable_data());
  // TODO Get rid of this deep call
  dict_encoder->mem_pool()->FreeAll();

  DictionaryPage page(buffer, dict_encoder->num_entries(),
                      properties_->dictionary_index_encoding());
  total_bytes_written_ += pager_->WriteDictionaryPage(page);
}

template <typename Type>
EncodedStatistics TypedColumnWriter<Type>::GetPageStatistics() {
  EncodedStatistics result;
  if (page_statistics_) result = page_statistics_->Encode();
  return result;
}

template <typename Type>
EncodedStatistics TypedColumnWriter<Type>::GetChunkStatistics() {
  EncodedStatistics result;
  if (chunk_statistics_) result = chunk_statistics_->Encode();
  return result;
}

template <typename Type>
void TypedColumnWriter<Type>::ResetPageStatistics() {
  if (chunk_statistics_ != nullptr) {
    chunk_statistics_->Merge(*page_statistics_);
    page_statistics_->Reset();
  }
}

// ----------------------------------------------------------------------
// Dynamic column writer constructor

std::shared_ptr<ColumnWriter> ColumnWriter::Make(ColumnChunkMetaDataBuilder* metadata,
                                                 std::unique_ptr<PageWriter> pager,
                                                 int64_t expected_rows,
                                                 const WriterProperties* properties) {
  const ColumnDescriptor* descr = metadata->descr();
  Encoding::type encoding = properties->encoding(descr->path());
  if (properties->dictionary_enabled(descr->path()) &&
      descr->physical_type() != Type::BOOLEAN) {
    encoding = properties->dictionary_page_encoding();
  }
  switch (descr->physical_type()) {
    case Type::BOOLEAN:
      return std::make_shared<BoolWriter>(metadata, std::move(pager), expected_rows,
                                          encoding, properties);
    case Type::INT32:
      return std::make_shared<Int32Writer>(metadata, std::move(pager), expected_rows,
                                           encoding, properties);
    case Type::INT64:
      return std::make_shared<Int64Writer>(metadata, std::move(pager), expected_rows,
                                           encoding, properties);
    case Type::INT96:
      return std::make_shared<Int96Writer>(metadata, std::move(pager), expected_rows,
                                           encoding, properties);
    case Type::FLOAT:
      return std::make_shared<FloatWriter>(metadata, std::move(pager), expected_rows,
                                           encoding, properties);
    case Type::DOUBLE:
      return std::make_shared<DoubleWriter>(metadata, std::move(pager), expected_rows,
                                            encoding, properties);
    case Type::BYTE_ARRAY:
      return std::make_shared<ByteArrayWriter>(metadata, std::move(pager), expected_rows,
                                               encoding, properties);
    case Type::FIXED_LEN_BYTE_ARRAY:
      return std::make_shared<FixedLenByteArrayWriter>(
          metadata, std::move(pager), expected_rows, encoding, properties);
    default:
      ParquetException::NYI("type reader not implemented");
  }
  // Unreachable code, but supress compiler warning
  return std::shared_ptr<ColumnWriter>(nullptr);
}

// ----------------------------------------------------------------------
// Instantiate templated classes

template <typename DType>
inline int64_t TypedColumnWriter<DType>::WriteMiniBatch(int64_t num_values,
                                                        const int16_t* def_levels,
                                                        const int16_t* rep_levels,
                                                        const T* values) {
  int64_t values_to_write = 0;
  // If the field is required and non-repeated, there are no definition levels
  if (descr_->max_definition_level() > 0) {
    for (int64_t i = 0; i < num_values; ++i) {
      if (def_levels[i] == descr_->max_definition_level()) {
        ++values_to_write;
      }
    }

    WriteDefinitionLevels(num_values, def_levels);
  } else {
    // Required field, write all values
    values_to_write = num_values;
  }

  // Not present for non-repeated fields
  if (descr_->max_repetition_level() > 0) {
    // A row could include more than one value
    // Count the occasions where we start a new row
    for (int64_t i = 0; i < num_values; ++i) {
      if (rep_levels[i] == 0) {
        num_rows_++;
      }
    }

    WriteRepetitionLevels(num_values, rep_levels);
  } else {
    // Each value is exactly one row
    num_rows_ += static_cast<int>(num_values);
  }

  if (num_rows_ > expected_rows_) {
    throw ParquetException("More rows were written in the column chunk than expected");
  }

  // PARQUET-780
  if (values_to_write > 0) {
    DCHECK(nullptr != values) << "Values ptr cannot be NULL";
  }

  WriteValues(values_to_write, values);

  if (page_statistics_ != nullptr) {
    page_statistics_->Update(values, values_to_write, num_values - values_to_write);
  }

  num_buffered_values_ += num_values;
  num_buffered_encoded_values_ += values_to_write;

  if (current_encoder_->EstimatedDataEncodedSize() >= properties_->data_pagesize()) {
    AddDataPage();
  }
  if (has_dictionary_ && !fallback_) {
    CheckDictionarySizeLimit();
  }

  return values_to_write;
}

template <typename DType>
inline int64_t TypedColumnWriter<DType>::WriteMiniBatchSpaced(
    int64_t num_values, const int16_t* def_levels, const int16_t* rep_levels,
    const uint8_t* valid_bits, int64_t valid_bits_offset, const T* values,
    int64_t* num_spaced_written) {
  int64_t values_to_write = 0;
  int64_t spaced_values_to_write = 0;
  // If the field is required and non-repeated, there are no definition levels
  if (descr_->max_definition_level() > 0) {
    // Minimal definition level for which spaced values are written
    int16_t min_spaced_def_level = descr_->max_definition_level();
    if (descr_->schema_node()->is_optional()) {
      min_spaced_def_level--;
    }
    for (int64_t i = 0; i < num_values; ++i) {
      if (def_levels[i] == descr_->max_definition_level()) {
        ++values_to_write;
      }
      if (def_levels[i] >= min_spaced_def_level) {
        ++spaced_values_to_write;
      }
    }

    WriteDefinitionLevels(num_values, def_levels);
  } else {
    // Required field, write all values
    values_to_write = num_values;
    spaced_values_to_write = num_values;
  }

  // Not present for non-repeated fields
  if (descr_->max_repetition_level() > 0) {
    // A row could include more than one value
    // Count the occasions where we start a new row
    for (int64_t i = 0; i < num_values; ++i) {
      if (rep_levels[i] == 0) {
        num_rows_++;
      }
    }

    WriteRepetitionLevels(num_values, rep_levels);
  } else {
    // Each value is exactly one row
    num_rows_ += static_cast<int>(num_values);
  }

  if (num_rows_ > expected_rows_) {
    throw ParquetException("More rows were written in the column chunk than expected");
  }

  if (descr_->schema_node()->is_optional()) {
    WriteValuesSpaced(spaced_values_to_write, valid_bits, valid_bits_offset, values);
  } else {
    WriteValues(values_to_write, values);
  }
  *num_spaced_written = spaced_values_to_write;

  if (page_statistics_ != nullptr) {
    page_statistics_->UpdateSpaced(values, valid_bits, valid_bits_offset, values_to_write,
                                   num_values - values_to_write);
  }

  num_buffered_values_ += num_values;
  num_buffered_encoded_values_ += values_to_write;

  if (current_encoder_->EstimatedDataEncodedSize() >= properties_->data_pagesize()) {
    AddDataPage();
  }
  if (has_dictionary_ && !fallback_) {
    CheckDictionarySizeLimit();
  }

  return values_to_write;
}

template <typename DType>
void TypedColumnWriter<DType>::WriteBatch(int64_t num_values, const int16_t* def_levels,
                                          const int16_t* rep_levels, const T* values) {
  // We check for DataPage limits only after we have inserted the values. If a user
  // writes a large number of values, the DataPage size can be much above the limit.
  // The purpose of this chunking is to bound this. Even if a user writes large number
  // of values, the chunking will ensure the AddDataPage() is called at a reasonable
  // pagesize limit
  int64_t write_batch_size = properties_->write_batch_size();
  int num_batches = static_cast<int>(num_values / write_batch_size);
  int64_t num_remaining = num_values % write_batch_size;
  int64_t value_offset = 0;
  for (int round = 0; round < num_batches; round++) {
    int64_t offset = round * write_batch_size;
    int64_t num_values = WriteMiniBatch(write_batch_size, &def_levels[offset],
                                        &rep_levels[offset], &values[value_offset]);
    value_offset += num_values;
  }
  // Write the remaining values
  int64_t offset = num_batches * write_batch_size;
  WriteMiniBatch(num_remaining, &def_levels[offset], &rep_levels[offset],
                 &values[value_offset]);
}

template <typename DType>
void TypedColumnWriter<DType>::WriteBatchSpaced(
    int64_t num_values, const int16_t* def_levels, const int16_t* rep_levels,
    const uint8_t* valid_bits, int64_t valid_bits_offset, const T* values) {
  // We check for DataPage limits only after we have inserted the values. If a user
  // writes a large number of values, the DataPage size can be much above the limit.
  // The purpose of this chunking is to bound this. Even if a user writes large number
  // of values, the chunking will ensure the AddDataPage() is called at a reasonable
  // pagesize limit
  int64_t write_batch_size = properties_->write_batch_size();
  int num_batches = static_cast<int>(num_values / write_batch_size);
  int64_t num_remaining = num_values % write_batch_size;
  int64_t num_spaced_written = 0;
  int64_t values_offset = 0;
  for (int round = 0; round < num_batches; round++) {
    int64_t offset = round * write_batch_size;
    WriteMiniBatchSpaced(write_batch_size, &def_levels[offset], &rep_levels[offset],
                         valid_bits, valid_bits_offset + values_offset,
                         values + values_offset, &num_spaced_written);
    values_offset += num_spaced_written;
  }
  // Write the remaining values
  int64_t offset = num_batches * write_batch_size;
  WriteMiniBatchSpaced(num_remaining, &def_levels[offset], &rep_levels[offset],
                       valid_bits, valid_bits_offset + values_offset,
                       values + values_offset, &num_spaced_written);
}

template <typename DType>
void TypedColumnWriter<DType>::WriteValues(int64_t num_values, const T* values) {
  current_encoder_->Put(values, static_cast<int>(num_values));
}

template <typename DType>
void TypedColumnWriter<DType>::WriteValuesSpaced(int64_t num_values,
                                                 const uint8_t* valid_bits,
                                                 int64_t valid_bits_offset,
                                                 const T* values) {
  current_encoder_->PutSpaced(values, static_cast<int>(num_values), valid_bits,
                              valid_bits_offset);
}

template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<BooleanType>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<Int32Type>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<Int64Type>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<Int96Type>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<FloatType>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<DoubleType>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<ByteArrayType>;
template class PARQUET_TEMPLATE_EXPORT TypedColumnWriter<FLBAType>;

}  // namespace parquet
