/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (ascii_only_) {
    ret = get_next_ascii_token(word, word_len, char_len, word_freq);
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::get_next_ascii_token(
    const char *&word, int64_t &word_len, int64_t &char_len, int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  const ObCharsetInfo *cs = analysis_ctx_.cs_;
  const char *text = doc_.ptr_;
  const int64_t length = doc_.len_;
  auto delimiter = [cs](char ch) {
    return ob_isspace(cs, ch) || ob_iscntrl(cs, ch) || ob_ispunct(cs, ch);
  };
  bool found = false;
  while (!found && ascii_pos_ < length) {
    while (ascii_pos_ < length && delimiter(text[ascii_pos_])) {
      ++ascii_pos_;
    }
    int64_t begin = ascii_pos_;
    while (ascii_pos_ < length && !delimiter(text[ascii_pos_])) {
      ++ascii_pos_;
    }
    int64_t end = ascii_pos_;
    // Preserve the normalizer's exact character classification and trimming.
    while (begin < end && !ob_isalnum(cs, text[begin])) { ++begin; }
    while (end > begin && !ob_isalnum(cs, text[end - 1])) { --end; }
    if (begin < end) {
      ObString normalized;
      // The two common collations share the ASCII byte case table. Keep the
      // general charset conversion for every other UTF-8 collation.
      if (cs == ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI)
          || cs == ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN)) {
        char *buf = static_cast<char *>(allocator_.alloc(end - begin));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int64_t i = begin; i < end; ++i) {
            buf[i - begin] = ob_tolower(cs, text[i]);
          }
          normalized.assign_ptr(buf, end - begin);
        }
      } else {
        ret = ObCharset::tolower(cs, ObString(end - begin, text + begin), normalized, allocator_);
      }
      if (OB_FAIL(ret)) {
        break;
      } else {
        word = normalized.ptr();
        word_len = normalized.length();
        char_len = word_len;
        word_freq = 1;
        found = true;
      }
    }
  }
  if (OB_SUCC(ret) && !found) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    ascii_pos_ = 0;
    ascii_only_ = CHARSET_UTF8MB4 == ObCharset::charset_type_by_coll(
        static_cast<ObCollationType>(param->cs_->number));
    for (int64_t i = 0; ascii_only_ && i < param->ft_length_; ++i) {
      ascii_only_ = static_cast<unsigned char>(param->fulltext_[i]) < 0x80;
    }
    if (ascii_only_) {
      is_inited_ = true;
    } else if (OB_FAIL(english_analyzer_.init(analysis_ctx_, *param->allocator_))) {
    } else if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  is_inited_ = false;
  ascii_only_ = false;
  ascii_pos_ = 0;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    abort_unless(nullptr != param->allocator_);
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
