#include "http_parser.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

HTTP::HttpParseResult Parse(HTTP::HttpParserState &parser,
                            HTTP::RequestData &request) {
  return parser.ParseNext(request);
}
} // namespace

int main() {
  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("GET /echo?msg=hello HTTP/1.1\r\nHost: example.com\r\n\r\n");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Complete, "valid GET parses");
    Check(request.method == HTTP::GET, "GET method is recognized");
    Check(request.path == "/echo", "origin path is extracted");
    Check(request.query == "msg=hello", "query is extracted");
    Check(request.params["msg"] == "hello", "query parameter is populated");
    Check(request.Header("host") && *request.Header("host") == "example.com",
          "Header lookup is case-insensitive");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData first;
    HTTP::RequestData second;
    parser.Append("GET /one HTTP/1.1\r\nHost: a\r\n\r\n"
                  "GET /two HTTP/1.1\r\nHost: a\r\n\r\n");
    Check(Parse(parser, first).status == HTTP::HttpParseStatus::Complete,
          "first pipelined request parses");
    Check(first.path == "/one", "first pipelined path");
    Check(Parse(parser, second).status == HTTP::HttpParseStatus::Complete,
          "second pipelined request parses from buffered bytes");
    Check(second.path == "/two", "second pipelined path");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    for (char ch : raw) {
      parser.Append(std::string_view(&ch, 1));
    }
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Complete,
          "one-byte-at-a-time request parses");
    Check(request.body == "hello", "one-byte-at-a-time body is read");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("POST /echo HTTP/1.1\r\nHost: a\r\n"
                  "Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Complete,
          "duplicate equal Content-Length is accepted");
    Check(request.body == "hello", "fixed body is read");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("POST /echo HTTP/1.1\r\nHost: a\r\n"
                  "Content-Length: 5\r\nContent-Length: 7\r\n\r\nhello");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Error &&
              result.errorStatus == 400,
          "conflicting Content-Length is rejected");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("POST /echo HTTP/1.1\r\nHost: a\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n"
                  "5;ignored=true\r\nhello\r\n0\r\nTrace: yes\r\n\r\n");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Complete,
          "chunked body parses");
    Check(request.body == "hello", "chunked body is decoded");
    Check(request.trailers["Trace"] == "yes", "trailer is parsed");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("POST /echo HTTP/1.1\r\nHost: a\r\n"
                  "Expect: 100-continue\r\nContent-Length: 5\r\n\r\n");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::NeedContinue,
          "Expect 100-continue is surfaced before body read");
    parser.MarkContinueSent();
    parser.Append("hello");
    result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Complete,
          "request completes after continue and body");
    Check(request.body == "hello", "continued body is read");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("GET / HTTP/1.1\r\n\r\n");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Error &&
              result.errorStatus == 400,
          "HTTP/1.1 without Host is rejected");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("GET / HTTP/2.0\r\nHost: a\r\n\r\n");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Error &&
              result.errorStatus == 505,
          "unsupported HTTP version returns 505 parse error");
  }

  {
    HTTP::HttpParserState parser;
    HTTP::RequestData request;
    parser.Append("GET / HTTP/1.1\r\nHost: a\r\n folded: no\r\n\r\n");
    auto result = Parse(parser, request);
    Check(result.status == HTTP::HttpParseStatus::Error &&
              result.errorStatus == 400,
          "obsolete folded header is rejected");
  }

  return 0;
}
