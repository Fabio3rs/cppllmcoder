#pragma once

#include <string>
namespace app {

struct Options {
    bool verbose = false;
    bool version = false;
    bool auto_approve = false;
    int max_iterations = 10;
    std::string db_path = ".cppllmcoder/brain.db";
    std::string workdir = ".";
    std::string model = "qwen2.5-coder:7b";
    std::string endpoint = "http://localhost:11434/v1/";
};

} // namespace app
