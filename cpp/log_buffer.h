#pragma once

#include <deque>
#include <string>
#include <vector>

#include <spdlog/sinks/base_sink.h>

namespace omb {

class LogBuffer : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit LogBuffer(size_t max_lines = 500);
    // non-const: base_sink::mutex_ is not mutable, so locking it requires a
    // non-const reference even though this method only reads lines_.
    std::vector<std::string> get_lines(int n);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    size_t max_lines_;
    std::deque<std::string> lines_;
};

}
