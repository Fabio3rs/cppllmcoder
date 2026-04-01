#pragma once

#include <openai/openai.hpp>

#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace embedding_utils {

using embedding_t = std::vector<float>;
using embedding_batch_t = std::vector<embedding_t>;

inline embedding_t parse_embedding_vector(const nlohmann::json &emb) {
    if (!emb.is_array()) {
        throw std::runtime_error("embedding field is not an array");
    }

    embedding_t out;
    out.reserve(emb.size());

    for (const auto &x : emb) {
        out.push_back(x.get<float>());
    }

    return out;
}

inline embedding_batch_t
parse_embedding_response(const nlohmann::json &response) {
    const auto it = response.find("data");
    if (it == response.end() || !it->is_array()) {
        throw std::runtime_error(
            "embedding response missing array field 'data'");
    }

    embedding_batch_t out;
    out.reserve(it->size());

    for (const auto &item : *it) {
        const auto emb_it = item.find("embedding");
        if (emb_it == item.end()) {
            throw std::runtime_error(
                "embedding response item missing field 'embedding'");
        }

        out.push_back(parse_embedding_vector(*emb_it));
    }

    return out;
}

inline embedding_t embed_text(openai::OpenAI &connection,
                              std::string_view model, std::string_view text) {
    auto response = connection.embedding.create(
        {{"model", std::string(model)}, {"input", std::string(text)}});

    auto batch = parse_embedding_response(response);
    if (batch.empty()) {
        throw std::runtime_error("embedding response returned no vectors");
    }

    return std::move(batch.front());
}

inline embedding_batch_t embed_texts(openai::OpenAI &connection,
                                     std::string model,
                                     std::span<const std::string> texts) {
    nlohmann::json input = nlohmann::json::array();
    input.get_ref<nlohmann::json::array_t &>().reserve(texts.size());

    for (const auto &text : texts) {
        input.push_back(text);
    }

    auto response = connection.embedding.create(
        {{"model", std::move(model)}, {"input", std::move(input)}});

    return parse_embedding_response(response);
}

inline embedding_batch_t
embed_texts(openai::OpenAI &connection, std::string model,
            std::initializer_list<std::string_view> texts) {
    nlohmann::json input = nlohmann::json::array();
    input.get_ref<nlohmann::json::array_t &>().reserve(texts.size());

    for (std::string_view text : texts) {
        input.push_back(std::string(text));
    }

    auto response = connection.embedding.create(
        {{"model", std::move(model)}, {"input", std::move(input)}});

    return parse_embedding_response(response);
}

inline embedding_batch_t embed_texts(openai::OpenAI &connection,
                                     std::string model,
                                     nlohmann::json::array_t texts) {
    auto res = connection.embedding.create(
        {{"model", std::move(model)}, {"input", std::move(texts)}});

    const auto &data = res["data"];
    std::vector<std::vector<float>> out;
    out.reserve(data.size());

    for (const auto &item : data) {
        const auto &emb = item["embedding"];
        std::vector<float> vec;
        vec.reserve(emb.size());
        for (const auto &x : emb) {
            vec.push_back(x.get<float>());
        }
        out.push_back(std::move(vec));
    }

    return out;
}

inline std::string to_json_array(const std::span<const float> &v) {
    std::string s;
    s.reserve(v.size() * 12);
    s.push_back('[');

    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += std::to_string(v[i]);
    }

    s.push_back(']');
    return s;
}

} // namespace embedding_utils
