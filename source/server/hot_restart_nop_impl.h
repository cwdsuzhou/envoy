#pragma once

#include <string>

#include "envoy/server/hot_restart.h"

#include "source/common/common/thread.h"
#include "source/common/stats/allocator_impl.h"

namespace Envoy {
namespace Server {

/**
 * No-op implementation of HotRestart.
 */
class HotRestartNopImpl : public Server::HotRestart {
public:
  // Server::HotRestart
  void drainParentListeners() override {}
  int duplicateParentListenSocket(const std::string&, uint32_t) override { return -1; }

  std::vector<envoy::HotRestartMessage_Reply_SocketInfo>
  duplicateParentConnectionSockets(const std::string&) override {
    std::vector<envoy::HotRestartMessage_Reply_SocketInfo> a;
    return a;
  }
  const std::string getConnectionData(std::string) override {
    std::string empty;
    return empty;
  }
  void initialize(Event::Dispatcher&, Server::Instance&) override {}
  absl::optional<AdminShutdownResponse> sendParentAdminShutdownRequest() override {
    return absl::nullopt;
  }
  void sendParentTerminateRequest() override {}
  ServerStatsFromParent mergeParentStatsIfAny(Stats::StoreRoot&) override { return {}; }
  void shutdown() override {}
  uint32_t baseId() override { return 0; }
  std::string version() override { return "disabled"; }
  Thread::BasicLockable& logLock() override { return log_lock_; }
  Thread::BasicLockable& accessLogLock() override { return access_log_lock_; }

private:
  Thread::MutexBasicLockable log_lock_;
  Thread::MutexBasicLockable access_log_lock_;
};

} // namespace Server
} // namespace Envoy
