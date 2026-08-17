#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

#include "logConfig.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"

//SPDLOG_INFO自带level过滤，和sink,logger set_level不同，需要在cmakelists指定
#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

#define CLIENT_INFO(...) SPDLOG_INFO("[CLIENT] " __VA_ARGS__)
#define CLIENT_DEBUG(...) SPDLOG_DEBUG("[CLIENT] " __VA_ARGS__)
#define CLIENT_TRACE(...) SPDLOG_TRACE("[CLIENT] " __VA_ARGS__)
#define CLIENT_WARN(...) SPDLOG_WARN("[CLIENT] " __VA_ARGS__)
#define CLIENT_ERROR(...) SPDLOG_ERROR("[CLIENT] " __VA_ARGS__)
#define CLIENT_CRITICAL(...) SPDLOG_CRITICAL("[CLIENT] " __VA_ARGS__)

class Logger
{
private:
    Logger() = default;
    ~Logger() = default;
    std::shared_ptr<spdlog::logger> log_ptr;
    bool is_init = false;
    
public:
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& Instance() {
       static Logger logeer;
       return logeer; 
    }

    bool Init(logConfig& config) {
        namespace fs = std::filesystem;

        std::vector<spdlog::sink_ptr> sinks{};

        if (config.console_output) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            if (!console_sink) {
                std::cerr << "console_sink error \n";
                return false;
            }
            
            console_sink->set_level(config.level);
            sinks.emplace_back(console_sink);
        }
        
        if (config.file_output) {
            fs::path p = config.log_file;
            if (!fs::exists(p.parent_path())) {
                if (!fs::create_directories(p.parent_path())) {
                    std::cerr << "path not exit and create fail \n";
                    return false;
                }
            }

            auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.log_file,
                config.max_file_size,
                config.max_files,
                false
            );

            if (!rotating_sink) {
                std::cerr << "rotating_sink error \n";
                return false;
            }
            
            rotating_sink->set_level(config.level);
            sinks.emplace_back(rotating_sink);
        }
        
        log_ptr = std::make_shared<spdlog::logger>(
            config.logger_name,
            sinks.begin(),
            sinks.end()
        );
        if (!log_ptr) {
            std::cerr << "log_ptr error \n";
            return false;
        }

        log_ptr->set_level(config.level);
        log_ptr->set_pattern(
            "[%Y-%m-%d %H:%M:%S.%e] "
            "[%^%l%$] "
            "[thread %t] "
            "[%s:%#] "
            "%v"
        );
        spdlog::set_default_logger(log_ptr);
        spdlog::flush_on(spdlog::level::err);
        is_init = true;
        return true;
    }

    std::shared_ptr<spdlog::logger> GetLogger() {
        return log_ptr;
    }

    void Fush() {
        log_ptr->flush();
    }

    void Shutdown() {
        spdlog::shutdown();
    }

    void info(const std::string& str) {
        if (is_init) {
            spdlog::info(str);
        } else {
            std::cerr << "not init \n";
        }
    }

    void debug(const std::string& str) {
        if (is_init) {
            spdlog::debug(str);
        } else {
            std::cerr << "not init \n";
        }
    }

    void trace(const std::string& str) {
        if (is_init) {
            spdlog::trace(str);
        } else {
            std::cerr << "not init \n";
        }
    }

    void warn(const std::string& str) {
        if (is_init) {
            spdlog::warn(str);
        } else {
            std::cerr << "not init \n";
        }
    }

    void error(const std::string& str) {
        if (is_init) {
            spdlog::error(str);
        } else {
            std::cerr << "not init \n";
        }
    }

    void critical(const std::string& str) {
        if (is_init) {
            spdlog::critical(str);
        } else {
            std::cerr << "not init \n";
        }
    }
};

