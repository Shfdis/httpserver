#include "request_data.h"
#include "server.h"
#include <future>
int main() {
  HTTP::ServerBuilder builder;
  builder.SetPort(8080);
  builder.SetThreads(22);
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
  builder.AddRequest(HTTP::GET, "/echo/*/echo",
                     [](const HTTP::RequestData &request) {
                       HTTP::ResponseData response;
                       response.body = request.urlVariables.at(0);
                       response.status = 200;
                       return response;
                     });
  auto server = builder.Build();
  server.Start();
  std::promise<void> keep_alive;
  keep_alive.get_future().wait();
  return 0;
}
