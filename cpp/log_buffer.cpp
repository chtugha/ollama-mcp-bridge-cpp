#include "log_buffer.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/pattern_formatter.h>

namespace omb {

LogBuffer::LogBuffer(size_t max_lines)
    : max_lines_(max_lines) {}

void LogBuffer::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    base_sink<std::mutex>::formatter_->format(msg, formatted);
    std::string line(formatted.data(), formatted.size());
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines_.push_back(std::move(line));
    if (lines_.size() > max_lines_) {
        lines_.pop_front();
    }
}

std::vector<std::string> LogBuffer::get_lines(int n) {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (n <= 0 || static_cast<size_t>(n) >= lines_.size()) {
        return std::vector<std::string>(lines_.begin(), lines_.end());
    }
    auto start = lines_.end() - n;
    return std::vector<std::string>(start, lines_.end());
}

}
