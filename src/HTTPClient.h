#pragma once

#include <cstdint>
#include <map>
#include <memory>
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
    responseStream_.reset();
    statusCode_ = 0;
  }
  void setFollowRedirects(int mode) { (void)mode; }
  void setReuse(bool reuse) { (void)reuse; }
  void setConnectTimeout(int32_t timeout) { (void)timeout; }
  void setTimeout(uint16_t timeout) { (void)timeout; }
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
  Stream *getStreamPtr() {
    if (!responseStream_)
      responseStream_ = std::make_unique<ResponseBodyStream>(responseBody_);
    return responseStream_.get();
  }
  bool connected() {
    return responseStream_ ? responseStream_->available() > 0 : statusCode_ > 0;
  }
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
    responseStream_.reset();
    statusCode_ = 0;
  }

  static String errorToString(int error) {
    return String(std::to_string(error));
  }

private:
  class ResponseBodyStream final : public Stream {
  public:
    explicit ResponseBodyStream(const String &body) : body_(body.s) {}

    int available() override {
      return position_ < body_.size()
                 ? static_cast<int>(body_.size() - position_)
                 : 0;
    }
    int read() override {
      if (position_ >= body_.size())
        return -1;
      return static_cast<uint8_t>(body_[position_++]);
    }
    int peek() override {
      if (position_ >= body_.size())
        return -1;
      return static_cast<uint8_t>(body_[position_]);
    }
    size_t write(uint8_t value) override {
      (void)value;
      return 0;
    }

  private:
    std::string body_;
    size_t position_ = 0;
  };

  std::string url_;
  std::map<std::string, std::string> headers_;
  std::string basicAuth_;
  String responseBody_;
  std::unique_ptr<ResponseBodyStream> responseStream_;
  int statusCode_ = 0;

  int perform(const char *method, const char *body) {
    if (url_.empty())
      return 0;

    sim_http_fetch::Response response;
    if (!sim_http_fetch::fetch(url_, method, headers_, basicAuth_, body,
                               response))
      return 0;

    responseBody_ = response.body;
    responseStream_.reset();
    statusCode_ = response.statusCode;
    return statusCode_;
  }
};
