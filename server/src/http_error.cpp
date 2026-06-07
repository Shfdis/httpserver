#include "http_error.h"
#include <utility>
using namespace HTTP;
HTTPError::HTTPError(int status, std::string message)
    : message(std::move(message)), status(status) {}
