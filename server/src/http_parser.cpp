#include "http_parser.h"
#include "http_error.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <optional>
#include <string_view>

namespace HTTP {

namespace {
constexpr size_t kMaxStartLineSize = 8192;
constexpr size_t kMaxHeaderSectionSize = 64 * 1024;
constexpr size_t kMaxBodySize = 10 * 1024 * 1024;

HttpParseResult NeedMore() {
  return {HttpParseStatus::NeedMoreData, 0, ""};
}

HttpParseResult NeedContinue() {
  return {HttpParseStatus::NeedContinue, 0, ""};
}

HttpParseResult Complete() {
  return {HttpParseStatus::Complete, 0, ""};
}

HttpParseResult Error(int status, std::string message) {
  return {HttpParseStatus::Error, status, std::move(message)};
}

constexpr unsigned char ascii_lower(unsigned char ch) {
  return ch >= 'A' && ch <= 'Z' ? static_cast<unsigned char>(ch + ('a' - 'A'))
                                : ch;
}

bool iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower(static_cast<unsigned char>(lhs[i])) !=
        ascii_lower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool starts_with_ci(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         iequals(value.substr(0, prefix.size()), prefix);
}

std::string_view trim_ows_view(std::string_view value) {
  size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  size_t end = value.size();
  while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string trim_ows(std::string_view value) {
  return std::string(trim_ows_view(value));
}

bool ascii_is_alpha(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool ascii_is_digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }

bool ascii_is_alnum(unsigned char ch) {
  return ascii_is_alpha(ch) || ascii_is_digit(ch);
}

bool ascii_is_xdigit(unsigned char ch) {
  return ascii_is_digit(ch) || (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

bool is_tchar(unsigned char ch) {
  if (ascii_is_alnum(ch)) {
    return true;
  }
  switch (ch) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '{':
  case '|':
  case '}':
  case '~':
    return true;
  default:
    return false;
  }
}

bool is_token(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char ch) {
    return is_tchar(static_cast<unsigned char>(ch));
  });
}

bool valid_field_value(std::string_view value) {
  for (unsigned char ch : value) {
    if (ch == '\t') {
      continue;
    }
    if (ch < 0x20 || ch == 0x7f) {
      return false;
    }
  }
  return true;
}

Method method_from_token(std::string_view token) {
  if (token == "GET") {
    return GET;
  }
  if (token == "PUT") {
    return PUT;
  }
  if (token == "POST") {
    return POST;
  }
  if (token == "PATCH") {
    return PATCH;
  }
  if (token == "DELETE") {
    return DELETE;
  }
  if (token == "HEAD") {
    return HEAD;
  }
  if (token == "OPTIONS") {
    return OPTIONS;
  }
  return UNKNOWN;
}

bool parse_http_version(std::string_view version, int &major, int &minor) {
  if (version.size() != 8 || version.substr(0, 5) != "HTTP/" ||
      !ascii_is_digit(static_cast<unsigned char>(version[5])) ||
      version[6] != '.' ||
      !ascii_is_digit(static_cast<unsigned char>(version[7]))) {
    return false;
  }
  major = version[5] - '0';
  minor = version[7] - '0';
  return true;
}

std::vector<std::string> header_values(
    const std::vector<std::pair<std::string, std::string>> &headers,
    std::string_view name) {
  std::vector<std::string> result;
  for (const auto &[fieldName, value] : headers) {
    if (iequals(fieldName, name)) {
      result.push_back(value);
    }
  }
  return result;
}

std::vector<std::string> split_comma_values(
    const std::vector<std::string> &values) {
  std::vector<std::string> result;
  for (const std::string &value : values) {
    size_t pos = 0;
    while (pos <= value.size()) {
      const size_t comma = value.find(',', pos);
      const size_t end = comma == std::string::npos ? value.size() : comma;
      result.push_back(trim_ows(std::string_view(value).substr(pos, end - pos)));
      if (comma == std::string::npos) {
        break;
      }
      pos = comma + 1;
    }
  }
  return result;
}

std::optional<size_t> parse_decimal_size(std::string_view value) {
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](char ch) {
        return ascii_is_digit(static_cast<unsigned char>(ch));
      })) {
    return std::nullopt;
  }
  size_t parsed = 0;
  const auto *first = value.data();
  const auto *last = value.data() + value.size();
  auto [ptr, ec] = std::from_chars(first, last, parsed, 10);
  if (ec != std::errc() || ptr != last) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<size_t> parse_hex_size(std::string_view value) {
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](char ch) {
        return ascii_is_xdigit(static_cast<unsigned char>(ch));
      })) {
    return std::nullopt;
  }
  size_t parsed = 0;
  const auto *first = value.data();
  const auto *last = value.data() + value.size();
  auto [ptr, ec] = std::from_chars(first, last, parsed, 16);
  if (ec != std::errc() || ptr != last) {
    return std::nullopt;
  }
  return parsed;
}

HttpParseResult parse_field_line(std::string_view line, std::string_view &name,
                                 std::string_view &value) {
  if (line.empty()) {
    return Error(400, "Empty header field");
  }
  if (line.front() == ' ' || line.front() == '\t') {
    return Error(400, "Obsolete folded headers are not supported");
  }
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return Error(400, "Invalid header field");
  }
  name = line.substr(0, colon);
  if (!is_token(name)) {
    return Error(400, "Invalid header name");
  }
  value = trim_ows_view(line.substr(colon + 1));
  if (!valid_field_value(value)) {
    return Error(400, "Invalid header value");
  }
  return Complete();
}

void add_header(RequestData &request, std::string_view name,
                std::string_view value) {
  std::string storedName(name);
  std::string storedValue(value);
  request.rawHeaders.emplace_back(storedName, storedValue);
  auto existing = request.headers.find(storedName);
  if (existing == request.headers.end()) {
    request.headers.emplace(std::move(storedName), std::move(storedValue));
  } else {
    existing->second += ", ";
    existing->second += storedValue;
  }
}

void add_trailer(RequestData &request, std::string_view name,
                 std::string_view value) {
  std::string storedName(name);
  std::string storedValue(value);
  auto existing = request.trailers.find(storedName);
  if (existing == request.trailers.end()) {
    request.trailers.emplace(std::move(storedName), std::move(storedValue));
  } else {
    existing->second += ", ";
    existing->second += storedValue;
  }
}

void parse_query_params(RequestData &request) {
  request.params.reserve(4);
  size_t pos = 0;
  while (pos <= request.query.size()) {
    const size_t amp = request.query.find('&', pos);
    const size_t end = amp == std::string::npos ? request.query.size() : amp;
    const std::string_view part =
        std::string_view(request.query).substr(pos, end - pos);
    if (!part.empty()) {
      const size_t eq = part.find('=');
      const std::string_view name =
          eq == std::string_view::npos ? part : part.substr(0, eq);
      const std::string_view value =
          eq == std::string_view::npos ? std::string_view()
                                       : part.substr(eq + 1);
      if (!name.empty()) {
        auto [it, inserted] =
            request.params.emplace(std::string(name), std::string(value));
        if (!inserted) {
          it->second.assign(value);
        }
      }
    }
    if (amp == std::string::npos) {
      break;
    }
    pos = amp + 1;
  }
}

HttpParseResult parse_target(RequestData &request) {
  if (request.target.empty()) {
    return Error(400, "Empty request target");
  }

  if (request.target == "*") {
    request.path = "*";
    return Complete();
  }

  if (starts_with_ci(request.target, "http://") ||
      starts_with_ci(request.target, "https://")) {
    const size_t authorityStart = request.target.find("://") + 3;
    const size_t targetStart = request.target.find_first_of("/?", authorityStart);
    if (targetStart == authorityStart) {
      return Error(400, "Invalid absolute request target");
    }
    if (targetStart == std::string::npos) {
      request.path = "/";
      return Complete();
    }
    if (request.target[targetStart] == '?') {
      request.path = "/";
      request.query = request.target.substr(targetStart + 1);
      parse_query_params(request);
      return Complete();
    }
    const size_t queryStart = request.target.find('?', targetStart);
    request.path = queryStart == std::string::npos
                       ? request.target.substr(targetStart)
                       : request.target.substr(targetStart,
                                               queryStart - targetStart);
    if (queryStart != std::string::npos) {
      request.query = request.target.substr(queryStart + 1);
      parse_query_params(request);
    }
    return Complete();
  }

  if (request.target.front() != '/') {
    return Error(400, "Unsupported request target form");
  }

  const size_t queryStart = request.target.find('?');
  request.path = queryStart == std::string::npos
                     ? request.target
                     : request.target.substr(0, queryStart);
  if (queryStart != std::string::npos) {
    request.query = request.target.substr(queryStart + 1);
    parse_query_params(request);
  }
  return Complete();
}

HttpParseResult parse_request_line(std::string_view line,
                                   RequestData &request) {
  const size_t firstSpace = line.find(' ');
  if (firstSpace == std::string_view::npos || firstSpace == 0) {
    return Error(400, "Invalid request line");
  }
  const size_t secondSpace = line.find(' ', firstSpace + 1);
  if (secondSpace == std::string_view::npos || secondSpace == firstSpace + 1 ||
      line.find(' ', secondSpace + 1) != std::string_view::npos) {
    return Error(400, "Invalid request line");
  }

  request.methodToken = std::string(line.substr(0, firstSpace));
  request.target =
      std::string(line.substr(firstSpace + 1, secondSpace - firstSpace - 1));
  request.version = std::string(line.substr(secondSpace + 1));

  if (!is_token(request.methodToken)) {
    return Error(400, "Invalid method token");
  }
  request.method = method_from_token(request.methodToken);

  int major = 0;
  int minor = 0;
  if (!parse_http_version(request.version, major, minor)) {
    return Error(400, "Invalid HTTP version");
  }
  if (major != 1 || (minor != 0 && minor != 1)) {
    return Error(505, "HTTP Version Not Supported");
  }

  return parse_target(request);
}

HttpParseResult parse_content_length(
    const std::vector<std::pair<std::string, std::string>> &headers,
    std::optional<size_t> &contentLength) {
  const auto values = split_comma_values(header_values(headers, "Content-Length"));
  if (values.empty()) {
    return Complete();
  }

  std::optional<size_t> expected;
  for (const std::string &value : values) {
    const auto parsed = parse_decimal_size(value);
    if (!parsed) {
      return Error(400, "Invalid Content-Length");
    }
    if (!expected) {
      expected = *parsed;
    } else if (*expected != *parsed) {
      return Error(400, "Conflicting Content-Length values");
    }
  }
  contentLength = expected;
  if (contentLength && *contentLength > kMaxBodySize) {
    return Error(413, "Content Too Large");
  }
  return Complete();
}

HttpParseResult parse_transfer_encoding(
    const std::vector<std::pair<std::string, std::string>> &headers,
    bool &chunked) {
  const auto values =
      split_comma_values(header_values(headers, "Transfer-Encoding"));
  if (values.empty()) {
    return Complete();
  }
  if (values.back().empty() || !iequals(values.back(), "chunked")) {
    return Error(501, "Unsupported transfer encoding");
  }
  for (const std::string &value : values) {
    if (value.empty() || !iequals(value, "chunked")) {
      return Error(501, "Unsupported transfer encoding");
    }
  }
  chunked = true;
  return Complete();
}

HttpParseResult validate_host(const RequestData &request) {
  const auto hosts = header_values(request.rawHeaders, "Host");
  if (request.version == "HTTP/1.1" && hosts.size() != 1) {
    return Error(400, "HTTP/1.1 requires exactly one Host header");
  }
  if (hosts.size() > 1) {
    return Error(400, "Multiple Host headers are not allowed");
  }
  if (!hosts.empty() && hosts.front().empty()) {
    return Error(400, "Host header must not be empty");
  }
  return Complete();
}

HttpParseResult validate_expect(const RequestData &request,
                                bool &expectsContinue) {
  auto value = request.Header("Expect");
  if (!value) {
    return Complete();
  }
  const std::string trimmed = trim_ows(*value);
  if (!iequals(trimmed, "100-continue")) {
    return Error(417, "Expectation Failed");
  }
  expectsContinue = true;
  return Complete();
}
} // namespace

std::optional<std::string> RequestData::Header(std::string_view name) const {
  std::string result;
  for (const auto &[fieldName, value] : rawHeaders) {
    if (!iequals(fieldName, name)) {
      continue;
    }
    if (!result.empty()) {
      result += ", ";
    }
    result += value;
  }
  if (!result.empty()) {
    return result;
  }

  for (const auto &[fieldName, value] : headers) {
    if (iequals(fieldName, name)) {
      return value;
    }
  }
  return std::nullopt;
}

HttpParserState::HttpParserState() {
  line_.reserve(256);
  pending_.reserve(256);
  current_.headers.reserve(8);
  current_.rawHeaders.reserve(8);
}

void HttpParserState::ResetForNext() {
  state_ = State::StartLine;
  current_ = RequestData{};
  current_.headers.reserve(8);
  current_.rawHeaders.reserve(8);
  line_.clear();
  sawCr_ = false;
  continueSent_ = false;
  waitingForContinue_ = false;
  continueCandidate_ = false;
  chunkNeedsLf_ = false;
  started_ = false;
  chunked_ = false;
  expectsContinue_ = false;
  hostEmpty_ = false;
  contentLengthInvalid_ = false;
  contentLengthConflict_ = false;
  unsupportedTransferEncoding_ = false;
  invalidExpect_ = false;
  hasContentLength_ = false;
  hostCount_ = 0;
  headerBytes_ = 0;
  fixedRemaining_ = 0;
  chunkRemaining_ = 0;
  contentLength_ = 0;
  errorStatus_ = 0;
  errorMessage_.clear();
}

void HttpParserState::SetError(int status, std::string message) {
  state_ = State::Error;
  errorStatus_ = status;
  errorMessage_ = std::move(message);
}

void HttpParserState::CompleteCurrent() {
  state_ = State::Complete;
}

void HttpParserState::TrackHeader(std::string_view name,
                                  std::string_view value) {
  if (iequals(name, "Host")) {
    ++hostCount_;
    hostEmpty_ = hostEmpty_ || value.empty();
    return;
  }

  if (iequals(name, "Content-Length")) {
    size_t pos = 0;
    while (pos <= value.size()) {
      const size_t comma = value.find(',', pos);
      const size_t end = comma == std::string_view::npos ? value.size() : comma;
      const std::string_view token = trim_ows_view(value.substr(pos, end - pos));
      const auto parsed = parse_decimal_size(token);
      if (!parsed) {
        contentLengthInvalid_ = true;
      } else if (!hasContentLength_) {
        hasContentLength_ = true;
        contentLength_ = *parsed;
      } else if (contentLength_ != *parsed) {
        contentLengthConflict_ = true;
      }
      if (comma == std::string_view::npos) {
        break;
      }
      pos = comma + 1;
    }
    return;
  }

  if (iequals(name, "Transfer-Encoding")) {
    size_t pos = 0;
    bool sawToken = false;
    bool lastWasChunked = false;
    while (pos <= value.size()) {
      const size_t comma = value.find(',', pos);
      const size_t end = comma == std::string_view::npos ? value.size() : comma;
      const std::string_view token = trim_ows_view(value.substr(pos, end - pos));
      if (token.empty()) {
        unsupportedTransferEncoding_ = true;
      } else {
        sawToken = true;
        lastWasChunked = iequals(token, "chunked");
        if (!lastWasChunked) {
          unsupportedTransferEncoding_ = true;
        }
      }
      if (comma == std::string_view::npos) {
        break;
      }
      pos = comma + 1;
    }
    if (!sawToken || !lastWasChunked) {
      unsupportedTransferEncoding_ = true;
    }
    chunked_ = chunked_ || lastWasChunked;
    return;
  }

  if (iequals(name, "Expect")) {
    const std::string_view token = trim_ows_view(value);
    if (!iequals(token, "100-continue")) {
      invalidExpect_ = true;
    } else {
      expectsContinue_ = true;
    }
    return;
  }

  if (iequals(name, "Connection")) {
    size_t pos = 0;
    while (pos <= value.size()) {
      const size_t comma = value.find(',', pos);
      const size_t end = comma == std::string_view::npos ? value.size() : comma;
      const std::string_view token = trim_ows_view(value.substr(pos, end - pos));
      if (iequals(token, "close")) {
        current_.connectionClose = true;
      } else if (iequals(token, "keep-alive")) {
        current_.connectionKeepAlive = true;
      }
      if (comma == std::string_view::npos) {
        break;
      }
      pos = comma + 1;
    }
  }
}

void HttpParserState::ProcessLine() {
  if (state_ == State::StartLine) {
    if (line_.empty() && !started_) {
      line_.clear();
      return;
    }
    started_ = true;
    HttpParseResult result = parse_request_line(line_, current_);
    line_.clear();
    if (result.status != HttpParseStatus::Complete) {
      SetError(result.errorStatus, std::move(result.errorMessage));
      return;
    }
    state_ = State::HeaderLine;
    return;
  }

  if (state_ == State::HeaderLine) {
    headerBytes_ += line_.size() + 2;
    if (headerBytes_ > kMaxHeaderSectionSize) {
      line_.clear();
      SetError(431, "Header section too large");
      return;
    }
    if (line_.empty()) {
      line_.clear();
      FinishHeaders();
      return;
    }
    std::string_view name;
    std::string_view value;
    HttpParseResult result = parse_field_line(line_, name, value);
    if (result.status != HttpParseStatus::Complete) {
      line_.clear();
      SetError(result.errorStatus, std::move(result.errorMessage));
      return;
    }
    TrackHeader(name, value);
    add_header(current_, name, value);
    line_.clear();
    return;
  }

  if (state_ == State::ChunkSize) {
    const size_t extension = line_.find(';');
    std::string sizeToken =
        trim_ows(extension == std::string::npos
                     ? std::string_view(line_)
                     : std::string_view(line_).substr(0, extension));
    line_.clear();
    const auto chunkSize = parse_hex_size(sizeToken);
    if (!chunkSize) {
      SetError(400, "Invalid chunk size");
      return;
    }
    if (*chunkSize > kMaxBodySize ||
        current_.body.size() > kMaxBodySize - *chunkSize) {
      SetError(413, "Content Too Large");
      return;
    }
    chunkRemaining_ = *chunkSize;
    state_ = chunkRemaining_ == 0 ? State::TrailerLine : State::ChunkData;
    return;
  }

  if (state_ == State::TrailerLine) {
    headerBytes_ += line_.size() + 2;
    if (headerBytes_ > kMaxHeaderSectionSize) {
      line_.clear();
      SetError(431, "Trailer section too large");
      return;
    }
    if (line_.empty()) {
      line_.clear();
      CompleteCurrent();
      return;
    }
    std::string_view name;
    std::string_view value;
    HttpParseResult result = parse_field_line(line_, name, value);
    if (result.status != HttpParseStatus::Complete) {
      line_.clear();
      SetError(result.errorStatus, std::move(result.errorMessage));
      return;
    }
    add_trailer(current_, name, value);
    line_.clear();
  }
}

void HttpParserState::AppendLineData(std::string_view data) {
  if (data.empty()) {
    return;
  }

  line_.append(data.data(), data.size());
  if (state_ == State::StartLine && line_.size() > kMaxStartLineSize) {
    SetError(414, "URI Too Long");
  } else if ((state_ == State::HeaderLine || state_ == State::TrailerLine) &&
             headerBytes_ + line_.size() > kMaxHeaderSectionSize) {
    SetError(431, state_ == State::HeaderLine ? "Header section too large"
                                              : "Trailer section too large");
  }
}

void HttpParserState::ProcessLineByte(char ch) {
  if (sawCr_) {
    sawCr_ = false;
    if (ch != '\n') {
      SetError(400, "Invalid line ending");
      return;
    }
    ProcessLine();
    return;
  }

  if (ch == '\n') {
    SetError(400, "Invalid line ending");
    return;
  }
  if (ch == '\r') {
    sawCr_ = true;
    return;
  }

  line_.push_back(ch);
  if (state_ == State::StartLine && line_.size() > kMaxStartLineSize) {
    SetError(414, "URI Too Long");
  } else if ((state_ == State::HeaderLine || state_ == State::TrailerLine) &&
             headerBytes_ + line_.size() > kMaxHeaderSectionSize) {
    SetError(state_ == State::HeaderLine ? 431 : 431,
             state_ == State::HeaderLine ? "Header section too large"
                                         : "Trailer section too large");
  }
}

void HttpParserState::FinishHeaders() {
  if (current_.version == "HTTP/1.1" && hostCount_ != 1) {
    SetError(400, "HTTP/1.1 requires exactly one Host header");
    return;
  }
  if (hostCount_ > 1) {
    SetError(400, "Multiple Host headers are not allowed");
    return;
  }
  if (hostEmpty_) {
    SetError(400, "Host header must not be empty");
    return;
  }

  if (contentLengthInvalid_) {
    SetError(400, "Invalid Content-Length");
    return;
  }
  if (contentLengthConflict_) {
    SetError(400, "Conflicting Content-Length values");
    return;
  }
  if (hasContentLength_ && contentLength_ > kMaxBodySize) {
    SetError(413, "Content Too Large");
    return;
  }
  if (unsupportedTransferEncoding_) {
    SetError(501, "Unsupported transfer encoding");
    return;
  }
  if (chunked_ && hasContentLength_) {
    SetError(400, "Transfer-Encoding and Content-Length conflict");
    return;
  }
  if (invalidExpect_) {
    SetError(417, "Expectation Failed");
    return;
  }

  if (chunked_) {
    state_ = State::ChunkSize;
    continueCandidate_ = expectsContinue_ && !continueSent_;
    return;
  }

  fixedRemaining_ = hasContentLength_ ? contentLength_ : 0;
  if (fixedRemaining_ == 0) {
    CompleteCurrent();
    return;
  }
  current_.body.reserve(fixedRemaining_);
  state_ = State::FixedBody;
  continueCandidate_ = expectsContinue_ && !continueSent_;
}

size_t HttpParserState::ProcessBytes(std::string_view data) {
  size_t pos = 0;
  while (pos < data.size()) {
    if (state_ == State::Complete || state_ == State::Error ||
        waitingForContinue_) {
      return pos;
    }

    if (state_ == State::FixedBody) {
      continueCandidate_ = false;
      const size_t take = std::min(fixedRemaining_, data.size() - pos);
      current_.body.append(data.data() + pos, take);
      fixedRemaining_ -= take;
      pos += take;
      if (fixedRemaining_ == 0) {
        CompleteCurrent();
        return pos;
      }
      continue;
    }

    if (state_ == State::ChunkData) {
      continueCandidate_ = false;
      const size_t take = std::min(chunkRemaining_, data.size() - pos);
      current_.body.append(data.data() + pos, take);
      chunkRemaining_ -= take;
      pos += take;
      if (chunkRemaining_ == 0) {
        state_ = State::ChunkDataCrlf;
        chunkNeedsLf_ = false;
      }
      continue;
    }

    if (state_ == State::StartLine || state_ == State::HeaderLine ||
        state_ == State::ChunkSize || state_ == State::TrailerLine) {
      if (sawCr_) {
        sawCr_ = false;
        if (data[pos++] != '\n') {
          SetError(400, "Invalid line ending");
          return pos;
        }
        ProcessLine();
        if (state_ == State::Complete || state_ == State::Error ||
            waitingForContinue_) {
          return pos;
        }
        if (continueCandidate_ && pos == data.size() &&
            (state_ == State::FixedBody || state_ == State::ChunkSize)) {
          waitingForContinue_ = true;
          continueCandidate_ = false;
          return pos;
        }
        if (continueCandidate_ && pos < data.size()) {
          continueCandidate_ = false;
        }
        continue;
      }

      const char *begin = data.data() + pos;
      const size_t remaining = data.size() - pos;
      const char *cr = static_cast<const char *>(std::memchr(begin, '\r', remaining));
      const char *lf = static_cast<const char *>(std::memchr(begin, '\n', remaining));
      const char *lineEnd = nullptr;
      if (cr && lf) {
        lineEnd = cr < lf ? cr : lf;
      } else {
        lineEnd = cr ? cr : lf;
      }

      if (!lineEnd) {
        AppendLineData(std::string_view(begin, remaining));
        return data.size();
      }

      const size_t prefixSize = static_cast<size_t>(lineEnd - begin);
      AppendLineData(std::string_view(begin, prefixSize));
      pos += prefixSize + 1;
      if (state_ == State::Error) {
        return pos;
      }

      if (*lineEnd == '\n') {
        SetError(400, "Invalid line ending");
        return pos;
      }

      if (pos == data.size()) {
        sawCr_ = true;
        return pos;
      }

      if (data[pos++] != '\n') {
        SetError(400, "Invalid line ending");
        return pos;
      }

      ProcessLine();
      if (state_ == State::Complete || state_ == State::Error ||
          waitingForContinue_) {
        return pos;
      }
      if (continueCandidate_ && pos == data.size() &&
          (state_ == State::FixedBody || state_ == State::ChunkSize)) {
        waitingForContinue_ = true;
        continueCandidate_ = false;
        return pos;
      }
      if (continueCandidate_ && pos < data.size()) {
        continueCandidate_ = false;
      }
      continue;
    }

    char ch = data[pos++];
    if (state_ == State::ChunkDataCrlf) {
      if (!chunkNeedsLf_) {
        if (ch != '\r') {
          SetError(400, "Invalid chunk data terminator");
          return pos;
        }
        chunkNeedsLf_ = true;
        continue;
      }
      if (ch != '\n') {
        SetError(400, "Invalid chunk data terminator");
        return pos;
      }
      chunkNeedsLf_ = false;
      state_ = State::ChunkSize;
      continue;
    }

    ProcessLineByte(ch);
    if (state_ == State::Complete || state_ == State::Error ||
        waitingForContinue_) {
      return pos;
    }
    if (continueCandidate_ && pos == data.size() &&
        (state_ == State::FixedBody || state_ == State::ChunkSize)) {
      waitingForContinue_ = true;
      continueCandidate_ = false;
      return pos;
    }
    if (continueCandidate_ && pos < data.size()) {
      continueCandidate_ = false;
    }
  }
  return pos;
}

void HttpParserState::Append(std::string_view data) {
  const size_t consumed = ProcessBytes(data);
  if (consumed < data.size()) {
    pending_.append(data.substr(consumed));
  }
}

size_t HttpParserState::Consume(std::string_view data) {
  return ProcessBytes(data);
}

bool HttpParserState::Empty() const {
  return state_ == State::StartLine && line_.empty() && pending_.empty() &&
         !sawCr_;
}

void HttpParserState::MarkContinueSent() {
  if (waitingForContinue_) {
    waitingForContinue_ = false;
  }
  continueSent_ = true;
}

HttpParseResult HttpParserState::ParseNext(RequestData &request) {
  if (state_ == State::Complete) {
    request = std::move(current_);
    ResetForNext();
    return Complete();
  }

  if (state_ == State::Error) {
    return Error(errorStatus_, errorMessage_);
  }

  if (waitingForContinue_) {
    return NeedContinue();
  }

  if (!pending_.empty()) {
    std::string data = std::move(pending_);
    pending_.clear();
    const size_t consumed = ProcessBytes(data);
    if (consumed < data.size()) {
      pending_.append(std::string_view(data).substr(consumed));
    }
    if (state_ == State::Complete) {
      request = std::move(current_);
      ResetForNext();
      return Complete();
    }
    if (state_ == State::Error) {
      return Error(errorStatus_, errorMessage_);
    }
    if (waitingForContinue_) {
      return NeedContinue();
    }
  }

  return NeedMore();
}

HttpRequestParser::HttpRequestParser(IOUring &ring, int fd)
    : iterator_(ring, fd) {}

void HttpRequestParser::MarkContinueSent() { state_.MarkContinueSent(); }

CoFuture<RequestReadStatus> HttpRequestParser::ReadRequest(
    RequestData &request) {
  while (true) {
    HttpParseResult parsed = state_.ParseNext(request);
    if (parsed.status == HttpParseStatus::Complete) {
      co_return RequestReadStatus::Complete;
    }
    if (parsed.status == HttpParseStatus::NeedContinue) {
      co_return RequestReadStatus::NeedContinue;
    }
    if (parsed.status == HttpParseStatus::Error) {
      throw HTTPError(parsed.errorStatus, parsed.errorMessage);
    }

    if (!iterator_) {
      co_await ++iterator_;
    }

    if (!iterator_) {
      if (iterator_.Eof() && state_.Empty()) {
        co_return RequestReadStatus::Closed;
      }
      throw HTTPError(400, "Incomplete HTTP message");
    }

    const size_t available = iterator_.Available();
    const size_t consumed =
        state_.Consume(std::string_view(iterator_.CurrentPtr(), available));
    iterator_.Advance(consumed);
  }
}

} // namespace HTTP
