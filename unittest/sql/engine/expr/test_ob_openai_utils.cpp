#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"

namespace oceanbase
{
namespace common
{
class ProviderTest : public ::testing::Test
{
protected:
  ObJsonObject *object(const char *text)
  {
    ObJsonObject *obj = nullptr;
    ObString json(text);
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, json, obj));
    return obj;
  }
  ObArenaAllocator allocator_;
};

TEST_F(ProviderTest, embedding_recovers_response_indices)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, object(
      R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  ObString text;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, result, text));
  EXPECT_EQ("[[1, 2], [3, 4]]", std::string(text.ptr(), text.length()));
}

TEST_F(ProviderTest, embedding_rejects_invalid_response_indices_and_shapes)
{
  const char *inputs[] = {
    R"({"data":{}})", R"({"data":[]})", R"({"data":[[]]})",
    R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})",
    R"({"data":[{"index":0.5,"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})"
  };
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *input : inputs) {
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, object(input), result)) << input;
  }
}

TEST_F(ProviderTest, vector_validation_rejects_wrong_count_dimensions_and_elements)
{
  const char *inputs[] = {R"({"v":null})", R"({"v":[null]})", R"({"v":[[]]})",
                          R"({"v":[[1,"x"]]})", R"({"v":[[1,true]]})", R"({"v":[[1]]})"};
  for (const char *input : inputs) {
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, object(input)->get_value("v"), 1, 2));
  }
  auto *vectors = object(R"({"v":[[1,2],[3,4]]})")->get_value("v");
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, vectors, 2, 0));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, vectors, 1, 2));
  ObJsonArray *vector = nullptr;
  ObJsonArray *outer = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, vector));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, outer));
  ObJsonDouble invalid(std::numeric_limits<double>::infinity());
  ASSERT_EQ(OB_SUCCESS, vector->append(&invalid));
  ASSERT_EQ(OB_SUCCESS, outer->append(vector));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, outer, 1, 1));
}

TEST_F(ProviderTest, completion_rejects_empty_null_and_wrong_type)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  const char *inputs[] = {"{}", R"({"choices":[]})", R"({"choices":[{"message":{"content":""}}]})",
                          R"({"choices":[{"message":{"content":null}}]})",
                          R"({"choices":[{"message":{"content":[]}}]})"};
  for (const char *input : inputs) {
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, object(input), result));
  }
}

TEST_F(ProviderTest, ollama_native_request_and_provider_selection)
{
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("OlLaMa"), provider));
  ObString model("model"), system("system"), content("hello"), key;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator_, key, headers));
  ASSERT_EQ(1, headers.count());
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, system, content,
      object(R"({"temperature":0.25,"max_tokens":9})"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  EXPECT_EQ(ObString("hello"), static_cast<ObJsonString *>(body->get_value("prompt"))->get_str());
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  EXPECT_DOUBLE_EQ(0.25, options->get_value("temperature")->get_double());
  EXPECT_EQ(9, options->get_value("num_predict")->get_int());
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, ObString("OLLAMA"), embed));
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(content));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(system));
  ASSERT_EQ(OB_SUCCESS, embed->get_body(allocator_, model, inputs, nullptr, body));
  EXPECT_EQ(2, body->get_value("input")->element_count());
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed->parse_output(allocator_, object(R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, result, 2, 2));
}

TEST_F(ProviderTest, rerank_validates_and_stably_sorts)
{
  auto *result = static_cast<ObJsonArray *>(object(R"({"v":[{"index":2,"relevance_score":1},{"index":1,"relevance_score":2},{"index":0,"relevance_score":1}]})")->get_value("v"));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator_, result, 3));
  const int expected[] = {1, 0, 2};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(expected[i], static_cast<ObJsonObject *>(result->get_value(i))->get_value("index")->get_int());
  }
  const char *inputs[] = {
    R"({"v":[{"index":0,"relevance_score":1},{"index":0,"relevance_score":2}]})",
    R"({"v":[{"index":2,"relevance_score":1},{"index":0,"relevance_score":2}]})",
    R"({"v":[{"index":0,"relevance_score":"1"},{}]})"
  };
  for (const char *input : inputs) {
    result = static_cast<ObJsonArray *>(object(input)->get_value("v"));
    EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::normalize_rerank_result(allocator_, result, 2));
  }
}

// A one-response loopback peer exercises the real HTTP client's type boundary.
TEST_F(ProviderTest, http_rejects_non_object_json_before_provider_cast)
{
  const char *payloads[] = {"[]", "null", "1", "\"text\"", "{}"};
  for (int scenario = 0; scenario < 10; ++scenario) {
    const char *payload = payloads[scenario / 2];
    bool batch = scenario % 2 != 0;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(0, bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    socklen_t length = sizeof(address);
    ASSERT_EQ(0, getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length));
    ASSERT_EQ(0, listen(listener, 1));
    std::string message = std::string("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: ") +
        std::to_string(strlen(payload)) + "\r\n\r\n" + payload;
    std::thread peer([listener, message]() {
      int connection = accept(listener, nullptr, nullptr);
      if (connection >= 0) {
        char request[4096];
        recv(connection, request, sizeof(request), 0);
        send(connection, message.data(), message.size(), MSG_NOSIGNAL);
        close(connection);
      }
      close(listener);
    });
    std::string url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    ObString endpoint(url.size(), url.data());
    ObArray<ObString> headers;
    headers.push_back(ObString("Content-Type: application/json"));
    ObJsonObject *response = nullptr;
    ObAIFuncClient client;
    int ret = OB_SUCCESS;
    ObArray<ObJsonObject *> responses;
    if (batch) {
      ObArray<ObJsonObject *> requests;
      ASSERT_EQ(OB_SUCCESS, requests.push_back(object("{}")));
      ret = client.send_post_batch(allocator_, endpoint, headers, requests, responses);
    } else {
      ret = client.send_post(allocator_, endpoint, headers, object("{}"), response);
    }
    peer.join();
    int expected = scenario / 2 == 4 ? OB_SUCCESS : OB_INVALID_DATA;
    EXPECT_EQ(expected, ret) << payload << ", batch=" << batch;
    if (expected == OB_INVALID_DATA) {
      EXPECT_EQ(nullptr, response);
      EXPECT_EQ(0, responses.count());
    }
  }
}
} // namespace common
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
