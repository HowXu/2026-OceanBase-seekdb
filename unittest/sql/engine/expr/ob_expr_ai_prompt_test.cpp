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

#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{
class PromptTest : public ::testing::Test
{
protected:
  void render(const char *json, const char *expected)
  {
    ObJsonObject *prompt = nullptr;
    ObString input(json);
    ObString result;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, prompt));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, result));
    EXPECT_EQ(std::string(expected), std::string(result.ptr(), result.length()));
  }
  ObArenaAllocator allocator_;
};

TEST_F(PromptTest, repeated_placeholder_expands_every_occurrence)
{
  render(R"({"template":"{0}{0}{0}{0}{0}{0}{0}{0}","args":["abcdefghij"]})",
         "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij");
}

TEST_F(PromptTest, nested_prompt_and_unicode)
{
  render(R"({"template":"outer:{0}","args":[{"template":"你好，{0}","args":["世界"]}]})", "outer:你好，世界");
}

TEST_F(PromptTest, json_object_and_array_arguments)
{
  render(R"({"template":"{0}:{1}","args":[{"a":1},["x",2]]})", "{\"a\": 1}:[\"x\", 2]");
}

TEST_F(PromptTest, escapes_and_embedded_nul)
{
  ObJsonObject *prompt = nullptr;
  ObString input(R"({"template":"{0}/{0}","args":["\u0000\n\"\\"]})");
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, result));
  const char expected[] = {'\0', '\n', '"', '\\', '/', '\0', '\n', '"', '\\'};
  EXPECT_EQ(std::string(expected, sizeof(expected)), std::string(result.ptr(), result.length()));
}

TEST_F(PromptTest, invalid_numeric_subscript_rejected_without_overflow)
{
  const char *templates[] = {"{1}", "{18446744073709551616}", "{999999999999999999999999999999999999}"};
  for (const char *text : templates) {
    ObJsonArray *args = nullptr;
    ObJsonObject *prompt = nullptr;
    ObJsonString *arg = nullptr;
    ObString value("x");
    ObString pattern(text);
    ObString result;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, value, arg));
    ASSERT_EQ(OB_SUCCESS, args->append(arg));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, pattern, args, prompt));
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, result));
  }
}

TEST_F(PromptTest, non_numeric_braces_remain_literal)
{
  render(R"({"template":"{} {-1} {x} {0","args":[]})", "{} {-1} {x} {0");
}

TEST_F(PromptTest, malformed_prompt_rejected_before_cast)
{
  const char *inputs[] = {R"({"template": [], "args": []})", R"({"template":"{0}","args":{}})",
                          R"({"template":"{0}","args":[null]})"};
  for (const char *json : inputs) {
    ObJsonObject *prompt = nullptr;
    ObString input(json);
    ObString result;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, prompt));
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, result));
  }
}
} // namespace common
} // namespace oceanbase
