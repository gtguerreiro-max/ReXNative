// The control file (--rexnative_d3d_control): commands a script writes for the running game, one
// per line, read and removed once per frame (rexnative_d3d_hooks.cpp's PollControl; the trace
// player's `set` lines).
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace rexnative::control_file {

// The file's lines once it is whole, and the file gone; none while it is not. A writer that
// appends creates the file, then writes into it, and may add to it later: read and removed in
// between, its commands were lost (half the pad presses of a route on the Steam Deck, written
// over SSH; 3 % of a script's appends on Windows). So the file is first renamed to
// <file>.taken, which fails while a writer has it open on Windows and leaves later appends a
// file of their own; then read, and taken once it is whole: one still empty or cut in its last
// line (a writer under Wine or on Linux writes on into the renamed file) waits for a later
// frame, a second at most. Scripts also write it whole (live.ps1 / live.py: written aside, then
// moved in place).
inline std::vector<std::string> Take(const std::string& path) {
  std::vector<std::string> lines;
  if (path.empty()) return lines;
  std::error_code ec;
  const std::filesystem::path taken = path + ".taken";
  if (!std::filesystem::exists(taken, ec)) {
    if (!std::filesystem::exists(path, ec)) return lines;
    std::filesystem::rename(path, taken, ec);
    if (ec) return lines;  // open in a writer: a later frame
  }
  std::string text;
  {
    std::ifstream in(taken, std::ios::binary);
    if (!in) return lines;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (text.empty() || text.back() != '\n') {
    const auto written = std::filesystem::last_write_time(taken, ec);
    if (ec || std::filesystem::file_time_type::clock::now() - written < std::chrono::seconds(1))
      return lines;
  }
  if (!std::filesystem::remove(taken, ec) || ec) return lines;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
    at = end + 1;
  }
  return lines;
}

}  // namespace rexnative::control_file
