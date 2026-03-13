#include "log_file.hpp"

namespace minilog {

LogFile::LogFile(boost::asio::io_context& ioc, OutputConfig cfg)
    : cfg_(std::move(cfg)), strand_(boost::asio::make_strand(ioc)) {}

LogFile::~LogFile() {
    close_files();
}

void LogFile::write(const SyslogMessage& msg) {
    boost::asio::post(strand_, [this, msg]() { do_write(msg); });
}

void LogFile::close() {
    boost::asio::post(strand_, [this]() { close_files(); });
}

void LogFile::do_write(const SyslogMessage& /*msg*/) {
    // TODO: implement rotation check, text write, jsonl write
}

void LogFile::rotate_if_needed() {
    // TODO: check file sizes, trigger rotate() if needed
}

void LogFile::rotate() {
    // TODO: close, shift .N files, reopen
}

void LogFile::open_files() {
    // TODO: open text_file and jsonl_file for append
}

void LogFile::close_files() {
    // TODO: flush and close open file handles
}

} // namespace minilog
