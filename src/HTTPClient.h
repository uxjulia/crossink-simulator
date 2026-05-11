#pragma once

#include <map>
#include <string>

#include "SimHttpFetch.h"
#include "Stream.h"
#include "WString.h"

class NetworkClient;

enum { HTTPC_STRICT_FOLLOW_REDIRECTS, HTTP_CODE_OK = 200 };

class HTTPClient {
 public:
  HTTPClient() {}
  ~HTTPClient() {}

  void begin(NetworkClient &client, const char *url) {
    (void)client;
    url_ = url ? url : "";
    responseBody_.s.clear();
    statusCode_ = 0;
  }
  void setFollowRedirects(int mode) {}
  void addHeader(const char *name, const String &value) {
    if (name)
      headers_[name] = value.c_str();
  }
  void addHeader(const char *name, const char *value) {
    if (name)
      headers_[name] = value ? value : "";
  }
  void setAuthorization(const char *user, const char *pass) {
    if (user && pass)
      basicAuth_ = std::string(user) + ":" + pass;
  }

  int GET() { return perform("GET", nullptr); }
  int POST() { return perform("POST", ""); }
  int POST(const char *body) { return perform("POST", body ? body : ""); }
  int PUT(const char *body) { return perform("PUT", body ? body : ""); }
  int PUT(const String &body) { return perform("PUT", body.c_str()); }

  String getString() { return responseBody_; }
  int getSize() { return static_cast<int>(responseBody_.length()); }
  int writeToStream(Stream *stream) {
    if (!stream)
      return 0;
    return static_cast<int>(
        stream->write(reinterpret_cast<const uint8_t *>(responseBody_.c_str()),
                      responseBody_.length()));
  }

  void end() {
    url_.clear();
    headers_.clear();
    basicAuth_.clear();
    responseBody_ = "";
    statusCode_ = 0;
  }

 private:
  std::string url_;
  std::map<std::string, std::string> headers_;
  std::string basicAuth_;
  String responseBody_;
  int statusCode_ = 0;

  int perform(const char *method, const char *body) {
    if (url_.empty())
      return 0;

    sim_http_fetch::Response response;
    if (!sim_http_fetch::fetch(url_, method, headers_, basicAuth_, body, response))
      return 0;

    responseBody_ = response.body;
    statusCode_ = response.statusCode;
    return statusCode_;
  }
};
