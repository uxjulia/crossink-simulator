#if defined(CROSSPOINT_SIM_USE_SDK_HTTP) && defined(FREEINK_NET_EXTERNAL_TLS)
#include <SecureClient.h>
#include <algorithm>
#include <climits>
#include <csignal>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

// The simulator uses native OpenSSL for TLS while exercising the SDK's actual
// HTTP framing, redirect and resumable-download implementation.
namespace freeink {
namespace {
bool wantsIo(SSL *ssl, const int result) {
  const int error = SSL_get_error(ssl, result);
  return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}
} // namespace
bool hostResolves(const char *host) {
  if (!host || !*host)
    return false;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const int result = getaddrinfo(host, nullptr, &hints, &addresses);
  if (addresses)
    freeaddrinfo(addresses);
  return result == 0;
}

SecureClient::~SecureClient() { stop(); }
bool SecureClient::tls13Available() { return true; }
void SecureClient::setCACert(const char *pem) {
  _rootCA = pem;
  _insecure = false;
}
void SecureClient::setInsecure() { _insecure = true; }
bool SecureClient::abortRequested() {
  if (_aborted || (_shouldAbort && _shouldAbort()))
    _aborted = true;
  return _aborted;
}
int SecureClient::connect(IPAddress ip, uint16_t port) {
  return connect(ip.toString().c_str(), port);
}
int SecureClient::connect(const char *host, uint16_t port) {
  // OpenSSL's socket BIO uses write(), which can raise SIGPIPE on Linux.
  // Treat closed sockets as transport errors for this simulator process.
  static const bool signalsReady = [] {
    std::signal(SIGPIPE, SIG_IGN);
    return true;
  }();
  (void)signalsReady;
  stop();
  _aborted = false;
  if (!host || abortRequested())
    return 0;
  _transport.setConnectionTimeout(getTimeout());
  if (!_transport.connect(host, port) || abortRequested()) {
    stop();
    return 0;
  }
  auto *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    stop();
    return 0;
  }
  _ctx = ctx;
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  if (_insecure) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (_rootCA) {
      BIO *bio = BIO_new_mem_buf(_rootCA, -1);
      bool loaded = false;
      while (bio) {
        X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!cert)
          break;
        loaded |= X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), cert) == 1;
        X509_free(cert);
      }
      BIO_free(bio);
      ERR_clear_error();
      if (!loaded) {
        stop();
        return 0;
      }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      stop();
      return 0;
    }
  }
  auto *ssl = SSL_new(ctx);
  if (!ssl) {
    stop();
    return 0;
  }
  _ssl = ssl;
  if (SSL_set_fd(ssl, _transport.socketFd()) != 1 ||
      SSL_set_tlsext_host_name(ssl, host) != 1 ||
      (!_insecure && SSL_set1_host(ssl, host) != 1)) {
    stop();
    return 0;
  }
  const auto started = millis();
  for (;;) {
    if (abortRequested()) {
      stop();
      return 0;
    }
    ERR_clear_error();
    const int result = SSL_connect(ssl);
    if (result == 1) {
      _connected = true;
      return 1;
    }
    if (!wantsIo(ssl, result) || millis() - started >= getTimeout()) {
      stop();
      return 0;
    }
    delay(1);
  }
}
size_t SecureClient::write(uint8_t value) { return write(&value, 1); }
size_t SecureClient::write(const uint8_t *data, size_t size) {
  size_t written = 0;
  const auto started = millis();
  while (_connected && written < size) {
    auto *ssl = static_cast<SSL *>(_ssl);
    ERR_clear_error();
    const int n =
        SSL_write(ssl, data + written,
                  static_cast<int>(std::min<size_t>(size - written, INT_MAX)));
    if (n > 0) {
      written += n;
      continue;
    }
    if (!wantsIo(ssl, n) || abortRequested() ||
        millis() - started >= getTimeout()) {
      stop();
      break;
    }
    delay(1);
  }
  return written;
}
int SecureClient::read(uint8_t *data, size_t size) {
  if (!_connected || size == 0)
    return -1;
  auto *ssl = static_cast<SSL *>(_ssl);
  ERR_clear_error();
  const int n =
      SSL_read(ssl, data, static_cast<int>(std::min<size_t>(size, INT_MAX)));
  if (n > 0)
    return n;
  if (!wantsIo(ssl, n))
    stop();
  return -1;
}
int SecureClient::read() {
  uint8_t value;
  return read(&value, 1) == 1 ? value : -1;
}
int SecureClient::peek() {
  if (!_connected)
    return -1;
  auto *ssl = static_cast<SSL *>(_ssl);
  uint8_t value;
  ERR_clear_error();
  const int n = SSL_peek(ssl, &value, 1);
  if (n == 1)
    return value;
  if (!wantsIo(ssl, n))
    stop();
  return -1;
}
int SecureClient::available() {
  // SSL_pending alone does not consume encrypted records already on the socket.
  if (peek() < 0)
    return 0;
  return SSL_pending(static_cast<SSL *>(_ssl));
}
uint8_t SecureClient::connected() {
  if (_connected)
    peek();
  return _connected;
}
void SecureClient::flush() {}
void SecureClient::stop() {
  if (_ssl)
    SSL_free(static_cast<SSL *>(_ssl));
  if (_ctx)
    SSL_CTX_free(static_cast<SSL_CTX *>(_ctx));
  _ssl = _ctx = nullptr;
  _connected = false;
  _transport.stop();
}
} // namespace freeink
#endif
