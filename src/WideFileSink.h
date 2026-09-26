#pragma once

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <share.h>

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

namespace FO4CS
{
    // spdlog's file sink opens narrow paths through the ANSI code page, which
    // cannot represent a Documents folder in another script (for example a
    // Cyrillic or CJK user name on a Western-locale Windows). This sink opens
    // the UTF-16 path directly and truncates it like basic_file_sink(true).
    class WideFileSink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        explicit WideFileSink(const std::filesystem::path& path)
        {
            file_ = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
            if (!file_)
                throw spdlog::spdlog_ex("cannot open log file");
        }

        ~WideFileSink() override
        {
            if (file_)
                std::fclose(file_);
        }

        WideFileSink(const WideFileSink&) = delete;
        WideFileSink& operator=(const WideFileSink&) = delete;

    protected:
        void sink_it_(const spdlog::details::log_msg& message) override
        {
            spdlog::memory_buf_t formatted;
            formatter_->format(message, formatted);
            std::fwrite(formatted.data(), 1, formatted.size(), file_);
        }

        void flush_() override
        {
            std::fflush(file_);
        }

    private:
        std::FILE* file_{};
    };
}
