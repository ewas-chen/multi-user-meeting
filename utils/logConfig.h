#pragma once
#include <string>
#include "spdlog/spdlog.h"

struct logConfig
{
    std::string logger_name = "ewasrpc";
    std::string log_file = "../logs/rpc.log";
    size_t max_file_size = 10 * 1024 * 1024;
    size_t max_files = 5;

    bool console_output = true;
    bool file_output = true;

    spdlog::level::level_enum level = spdlog::level::debug;
};
