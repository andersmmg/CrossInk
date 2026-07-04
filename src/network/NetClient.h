#pragma once
#include <string>

namespace NetClient {

bool get(const std::string& url, std::string& outBody);

const std::string& lastError();

}  // namespace NetClient
