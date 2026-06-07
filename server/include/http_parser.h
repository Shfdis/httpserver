#pragma once
#include "co_future.h"
#include "read_iterator.h"
#include "request_data.h"
#include <optional>
#include <string>
#include <string_view>

namespace HTTP {

enum class HttpParseStatus { NeedMoreData, NeedContinue, Complete, Error };
enum class RequestReadStatus { Complete, NeedContinue, Closed };

struct HttpParseResult {
  HttpParseStatus status{HttpParseStatus::NeedMoreData};
  int errorStatus{0};
  std::string errorMessage;
};

class HttpParserState {
  enum class State {
    StartLine,
    HeaderLine,
    FixedBody,
    ChunkSize,
    ChunkData,
    ChunkDataCrlf,
    TrailerLine,
    Complete,
    Error
  };

  State state_{State::StartLine};
  RequestData current_;
  std::optional<RequestData> complete_;
  std::string line_;
  std::string pending_;
  bool sawCr_{false};
  bool continueSent_{false};
  bool waitingForContinue_{false};
  bool continueCandidate_{false};
  bool chunkNeedsLf_{false};
  bool started_{false};
  bool chunked_{false};
  bool expectsContinue_{false};
  size_t headerBytes_{0};
  size_t fixedRemaining_{0};
  size_t chunkRemaining_{0};
  int errorStatus_{0};
  std::string errorMessage_;

  void ResetForNext();
  void ProcessBytes(std::string_view data);
  void SetError(int status, std::string message);
  void CompleteCurrent();
  void ProcessLine();
  void ProcessLineByte(char ch);
  void FinishHeaders();

public:
  void Append(std::string_view data);
  bool Empty() const;
  void MarkContinueSent();
  HttpParseResult ParseNext(RequestData &request);
};

class HttpRequestParser {
  ReadIterator iterator_;
  HttpParserState state_;

public:
  HttpRequestParser(IOUring &ring, int fd);
  CoFuture<RequestReadStatus> ReadRequest(RequestData &request);
  void MarkContinueSent();
};

} // namespace HTTP
