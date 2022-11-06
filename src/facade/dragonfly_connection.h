// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/fixed_array.h>

#include <deque>
#include <variant>

#include "base/io_buf.h"
#include "facade/facade_types.h"
#include "facade/resp_expr.h"
#include "util/connection.h"
#include "util/fibers/fibers_ext.h"
#include "util/http/http_handler.h"

typedef struct ssl_ctx_st SSL_CTX;
typedef struct mi_heap_s mi_heap_t;

namespace facade {

class ConnectionContext;
class RedisParser;
class ServiceInterface;
class MemcacheParser;

class Connection : public util::Connection {
 public:
  Connection(Protocol protocol, util::HttpListenerBase* http_listener, SSL_CTX* ctx,
             ServiceInterface* service);
  ~Connection();

  using error_code = std::error_code;
  using ShutdownCb = std::function<void()>;
  using ShutdownHandle = unsigned;

  ShutdownHandle RegisterShutdownHook(ShutdownCb cb);
  void UnregisterShutdownHook(ShutdownHandle id);

  Protocol protocol() const {
    return protocol_;
  }

  using BreakerCb = std::function<void(uint32_t)>;
  void RegisterOnBreak(BreakerCb breaker_cb);

  // This interface is used to pass a published message directly to the socket without
  // copying strings.
  // Once the msg is sent "bc" will be decreased so that caller could release the underlying
  // storage for the message.
  // virtual - to allow the testing code to override it.

  struct PubMessage {
    // if empty - means its a regular message, otherwise it's pmessage.
    std::string_view pattern;
    std::string_view channel;
    std::string_view message;
  };

  // this function is overriden at test_utils TestConnection
  virtual void SendMsgVecAsync(const PubMessage& pub_msg, util::fibers_ext::BlockingCounter bc);

  // Please note, this accept the message by value, since we really want to
  // create a new copy here, so that we would not need to "worry" about memory
  // management, we are assuming that we would not have many copy for this, and that
  // we would not need in this way to sync on the lifetime of the message
  void SendMonitorMsg(std::string monitor_msg);

  void SetName(std::string_view name) {
    CopyCharBuf(name, sizeof(name_), name_);
  }

  const char* GetName() const {
    return name_;
  }

  void SetPhase(std::string_view phase) {
    CopyCharBuf(phase, sizeof(phase_), phase_);
  }

  std::string GetClientInfo() const;
  std::string RemoteEndpointStr() const;
  uint32 GetClientId() const;

  void ShutdownSelf();

 protected:
  void OnShutdown() override;
  void OnPreMigrateThread() override;
  void OnPostMigrateThread() override;

 private:
  enum ParserStatus { OK, NEED_MORE, ERROR };

  void HandleRequests() final;

  static void CopyCharBuf(std::string_view src, unsigned dest_len, char* dest) {
    src = src.substr(0, dest_len - 1);
    if (!src.empty())
      memcpy(dest, src.data(), src.size());
    dest[src.size()] = '\0';
  }

  //
  io::Result<bool> CheckForHttpProto(util::FiberSocketBase* peer);

  void ConnectionFlow(util::FiberSocketBase* peer);
  std::variant<std::error_code, ParserStatus> IoLoop(util::FiberSocketBase* peer);

  void DispatchFiber(util::FiberSocketBase* peer);

  ParserStatus ParseRedis();
  ParserStatus ParseMemcache();
  void OnBreakCb(int32_t mask);

  base::IoBuf io_buf_;
  std::unique_ptr<RedisParser> redis_parser_;
  std::unique_ptr<MemcacheParser> memcache_parser_;
  util::HttpListenerBase* http_listener_;
  SSL_CTX* ctx_;
  ServiceInterface* service_;
  time_t creation_time_, last_interaction_;
  char name_[16];
  char phase_[16];

  std::unique_ptr<ConnectionContext> cc_;

  struct Request;
  struct DispatchOperations;
  struct DispatchCleanup;
  struct RequestDeleter;

  using RequestPtr = std::unique_ptr<Request, RequestDeleter>;

  // args are passed deliberately by value - to pass the ownership.
  static RequestPtr FromArgs(RespVec args, mi_heap_t* heap);

  std::deque<RequestPtr> dispatch_q_;  // coordinated via evc_.
  util::fibers_ext::EventCount evc_;

  RespVec parse_args_;
  CmdArgVec cmd_vec_;

  unsigned parser_error_ = 0;
  uint32_t id_;
  uint32_t break_poll_id_ = UINT32_MAX;

  Protocol protocol_;

  struct Shutdown;
  std::unique_ptr<Shutdown> shutdown_;
  BreakerCb breaker_cb_;
};

void RespToArgList(const RespVec& src, CmdArgVec* dest);

}  // namespace facade
