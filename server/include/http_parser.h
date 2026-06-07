#pragma once
#include "non_owning_co_future.h"
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
  bool hostEmpty_{false};
  bool contentLengthInvalid_{false};
  bool contentLengthConflict_{false};
  bool unsupportedTransferEncoding_{false};
  bool invalidExpect_{false};
  bool hasContentLength_{false};
  size_t hostCount_{0};
  size_t headerBytes_{0};
  size_t fixedRemaining_{0};
  size_t chunkRemaining_{0};
  size_t contentLength_{0};
  int errorStatus_{0};
  std::string errorMessage_;

  void ResetForNext();
  size_t ProcessBytes(std::string_view data);
  void SetError(int status, std::string message);
  void CompleteCurrent();
  void ProcessLine();
  void AppendLineData(std::string_view data);
  void ProcessLineByte(char ch);
  void FinishHeaders();
  void TrackHeader(std::string_view name, std::string_view value);

public:
  HttpParserState();
  void Append(std::string_view data);
  size_t Consume(std::string_view data);
  bool Empty() const;
  void MarkContinueSent();
  HttpParseResult ParseNext(RequestData &request);
};

class HttpRequestParser {
  ReadIterator iterator_;
  HttpParserState state_;

public:
  HttpRequestParser(IOUring &ring, int fd);
  NonOwningCoFuture<RequestReadStatus> ReadRequest(RequestData &request);
  void MarkContinueSent();
};

} // namespace HTTP
