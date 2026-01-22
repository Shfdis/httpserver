#include "http_error.h"
#include <string_view>
using namespace HTTP;
HTTPError::HTTPError(int status, std::string_view message)
    : status(status), message(message) {}
