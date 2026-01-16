#include "http_server.h"
#include "iasio.h"
#include "io_uring.h"
#include <future>
#include <string_view>

int main() {
  HTTP::ServerBuilder builder;
  builder.SetPort(8080);
  builder.AddRequest(HTTP::POST, "/echo", [](const HTTP::RequestData &request) {
    HTTP::ResponseData response;
    response.status = 200;
    response.body = request.body;
    return response;
  });

  builder.AddRequest(HTTP::GET, "/echo", [](const HTTP::RequestData &request) {
    HTTP::ResponseData response;
    response.status = 200;
    auto it = request.params.find("msg");
    if (it != request.params.end()) {
      response.body = it->second;
    }
    return response;
  });
  builder.SetAsio(
      std::unique_ptr<HTTP::IAsio>((HTTP::IAsio *)new HTTP::IOUring()));
  auto server = builder.Build();
  server.Start();
  std::promise<void> keep_alive;
  keep_alive.get_future().wait();
  return 0;
}
