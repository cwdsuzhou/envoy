#include "source/server/hot_restarting_parent.h"

#include "envoy/server/instance.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/memory/stats.h"
#include "source/common/network/connection_impl.h"
#include "source/common/network/utility.h"
#include "source/common/stats/stat_merger.h"
#include "source/common/stats/symbol_table_impl.h"
#include "source/common/stats/utility.h"
#include "source/common/upstream/cluster_manager_impl.h"
#include "source/server/listener_impl.h"
#include "source/server/active_tcp_listener.h"
#include "source/server/connection_handler_impl.h"
#include "source/server/server.h"

namespace Envoy {
namespace Server {

using HotRestartMessage = envoy::HotRestartMessage;
using SocketInfo = envoy::HotRestartMessage_Reply_SocketInfo;

const int MAX_FD_SIZE = 100;

HotRestartingParent::HotRestartingParent(int base_id, int restart_epoch,
                                         const std::string& socket_path, mode_t socket_mode)
    : HotRestartingBase(base_id), restart_epoch_(restart_epoch) {
  child_address_ = createDomainSocketAddress(restart_epoch_ + 1, "child", socket_path, socket_mode);
  bindDomainSocket(restart_epoch_, "parent", socket_path, socket_mode);
}

void HotRestartingParent::initialize(Event::Dispatcher& dispatcher, Server::Instance& server) {
  socket_event_ = dispatcher.createFileEvent(
      myDomainSocket(),
      [this](uint32_t events) -> void {
        ASSERT(events == Event::FileReadyType::Read);
        onSocketEvent();
      },
      Event::FileTriggerType::Edge, Event::FileReadyType::Read);
  internal_ = std::make_unique<Internal>(&server);
}

void HotRestartingParent::onSocketEvent() {
  std::unique_ptr<HotRestartMessage> wrapped_request;
  while ((wrapped_request = receiveHotRestartMessage(Blocking::No, nullptr))) {
    ENVOY_LOG(info, "receive request {} content {}", wrapped_request->request().request_case(),
              wrapped_request->request().SerializeAsString());
    if (wrapped_request->requestreply_case() == HotRestartMessage::kReply) {
      ENVOY_LOG(error, "child sent us a HotRestartMessage reply (we want requests); ignoring.");
      HotRestartMessage wrapped_reply;
      wrapped_reply.set_didnt_recognize_your_last_message(true);
      sendHotRestartMessage(child_address_, wrapped_reply);
      continue;
    }
    switch (wrapped_request->request().request_case()) {
    case HotRestartMessage::Request::kShutdownAdmin: {
      sendHotRestartMessage(child_address_, internal_->shutdownAdmin());
      break;
    }

    case HotRestartMessage::Request::kPassListenSocket: {
      sendHotRestartMessage(child_address_,
                            internal_->getListenSocketsForChild(wrapped_request->request()));
      break;
    }

    case HotRestartMessage::Request::kPassConnectionSocket: {
      sendHotRestartMessage(child_address_,
                            internal_->getConnectionSocketsForChild(wrapped_request->request()));
      // internal_->disableConnections();
      break;
    }

    case HotRestartMessage::Request::kPassConnectionData: {
      sendHotRestartMessage(child_address_,
                            internal_->getConnectionDataForChild(wrapped_request->request()));
      break;
    }

    case HotRestartMessage::Request::kStats: {
      HotRestartMessage wrapped_reply;
      internal_->exportStatsToChild(wrapped_reply.mutable_reply()->mutable_stats());
      sendHotRestartMessage(child_address_, wrapped_reply);
      break;
    }

    case HotRestartMessage::Request::kDrainListeners: {
      internal_->drainListeners();
      break;
    }

    case HotRestartMessage::Request::kTerminate: {
      ENVOY_LOG(info, "shutting down due to child request");
      kill(getpid(), SIGTERM);
      break;
    }

    default: {
      ENVOY_LOG(error, "child sent us an unfamiliar type of HotRestartMessage; ignoring.");
      HotRestartMessage wrapped_reply;
      wrapped_reply.set_didnt_recognize_your_last_message(true);
      sendHotRestartMessage(child_address_, wrapped_reply);
      break;
    }
    }
  }
}

void HotRestartingParent::shutdown() { socket_event_.reset(); }

HotRestartingParent::Internal::Internal(Server::Instance* server) : server_(server) {
  Stats::Gauge& hot_restart_generation = hotRestartGeneration(server->stats());
  hot_restart_generation.inc();
}

HotRestartMessage HotRestartingParent::Internal::shutdownAdmin() {
  server_->shutdownAdmin();
  HotRestartMessage wrapped_reply;
  wrapped_reply.mutable_reply()->mutable_shutdown_admin()->set_original_start_time_unix_seconds(
      server_->startTimeFirstEpoch());
  wrapped_reply.mutable_reply()->mutable_shutdown_admin()->set_enable_reuse_port_default(
      server_->enableReusePortDefault());
  return wrapped_reply;
}

HotRestartMessage
HotRestartingParent::Internal::getListenSocketsForChild(const HotRestartMessage::Request& request) {
  HotRestartMessage wrapped_reply;
  wrapped_reply.mutable_reply()->mutable_pass_listen_socket()->set_fd(-1);
  Network::Address::InstanceConstSharedPtr addr =
      Network::Utility::resolveUrl(request.pass_listen_socket().address());
  for (const auto& listener : server_->listenerManager().listeners()) {
    Network::ListenSocketFactory& socket_factory = listener.get().listenSocketFactory();
    auto sockets = socket_factory.getListenSockets();
    if (*socket_factory.localAddress() == *addr && listener.get().bindToPort()) {
      // worker_index() will default to 0 if not set which is the behavior before this field
      // was added. Thus, this should be safe for both roll forward and roll back.
      if (request.pass_listen_socket().worker_index() < server_->options().concurrency()) {
        wrapped_reply.mutable_reply()->mutable_pass_listen_socket()->set_fd(
            socket_factory.getListenSocket(request.pass_listen_socket().worker_index())
                ->ioHandle()
                .fdDoNotUse());
      }
      break;
    }
  }
  return wrapped_reply;
}

WaitGroup wgcon;

HotRestartMessage
HotRestartingParent::Internal::getConnectionSocketsForChild(const HotRestartMessage::Request&) {
  HotRestartMessage wrapped_reply;
  wrapped_reply.mutable_reply()->mutable_pass_connection_socket()->sockets();
  auto lmi = dynamic_cast<Envoy::Server::ListenerManagerImpl*>(&(server_->listenerManager()));
  auto& wkrs = lmi->getWorkers();
  for (auto& wk : wkrs) {
    auto wki = dynamic_cast<Envoy::Server::WorkerImpl*>(wk.get());
    auto con_handler = dynamic_cast<Envoy::Server::ConnectionHandlerImpl*>(wki->getHandler().get());
    auto& lss = con_handler->getListeners();
    for (auto& listenerPair : lss) {
      if (std::move(listenerPair.second).tcpListener() == absl::nullopt) {
        continue;
      }
      auto& tcpListener = std::move(listenerPair.second).tcpListener()->get();
      for (auto& cont : tcpListener.connections_by_context_) {
        for (auto& con : cont.second->connections_) {
          auto sc = dynamic_cast<Envoy::Network::ConnectionImpl*>(con->connection_.get());
          if (sc == nullptr) {
            continue;
          }
          if (!sc->ioHandle().isOpen()) {
            continue;
          }
          wgcon.Add();
          con_handler->dispatcher().post([sc]() {
            sc->readDisable(true);
            wgcon.Done();
          });
          wgcon.Wait();
          ENVOY_LOG(info, "parent: add socket {}, local {}, remote {}", sc->ioHandle().fdDoNotUse(),
                    sc->ioHandle().localAddress()->asString(),
                    sc->ioHandle().peerAddress()->asString());
          std::string key(sc->ioHandle().localAddress()->asString() + "_" +
                          sc->ioHandle().peerAddress()->asString());
          if (handlers_.find(key) != handlers_.end()) {
            continue;
          }
          int fd = sc->ioHandle().fdDoNotUse();
          Buffer::OwnedImpl buf(sc->getReadBuffer().buffer.toString());
          ENVOY_LOG(info, "read buffer {} from socket {}", buf.length(), fd);
          auto add_socket =
              wrapped_reply.mutable_reply()->mutable_pass_connection_socket()->add_sockets();
          add_socket->set_fd(fd);
          if (buf.length() > 0) {
            add_socket->set_buffer(buf.toString());
          }
          handlers_.insert(std::pair<std::string, Network::IoHandle&>(key, sc->ioHandle()));
          if (wrapped_reply.reply().pass_connection_socket().sockets_size() >= MAX_FD_SIZE) {
            wrapped_reply.mutable_reply()->mutable_pass_connection_socket()->set_has_more_fd(true);
            return wrapped_reply;
          }
        }
      }
    }
  }
  //
  //  for (auto& c : server_->clusterManager().clusters().active_clusters_) {
  //    auto fds = server_->clusterManager().findConnections(c.first);
  //    ENVOY_LOG(debug, "clusterManager: cluster {} fd size {}", c.first, fds.size());
  //    for (auto fd : fds) {
  //      ENVOY_LOG(debug, "clusterManager: fd {} ", fd);
  //    }
  //  }

  return wrapped_reply;
}
void HotRestartingParent::Internal::disableConnections() {
  HotRestartMessage wrapped_reply;
  auto lmi = dynamic_cast<Envoy::Server::ListenerManagerImpl*>(&(server_->listenerManager()));
  auto& wkrs = lmi->getWorkers();
  for (auto& wk : wkrs) {
    auto wki = dynamic_cast<Envoy::Server::WorkerImpl*>(wk.get());
    auto con_handler = dynamic_cast<Envoy::Server::ConnectionHandlerImpl*>(wki->getHandler().get());
    auto& lss = con_handler->getListeners();
    for (auto& listenerPair : lss) {
      if (std::move(listenerPair.second).tcpListener() == absl::nullopt) {
        continue;
      }
      auto& tcp_listener = std::move(listenerPair.second).tcpListener()->get();
      for (auto& cont : tcp_listener.connections_by_context_) {
        for (auto& con : cont.second->connections_) {
          auto sc = dynamic_cast<Envoy::Network::ConnectionImpl*>(con->connection_.get());
          if (sc == nullptr) {
            continue;
          }
          if (sc->state() != Network::Connection::State::Open || !sc->ioHandle().isOpen()) {
            continue;
          }
          sc->readDisable(true);
        }
      }
      tcp_listener.pauseListening();
    }
  }
}

HotRestartMessage HotRestartingParent::Internal::getConnectionDataForChild(
    const HotRestartMessage::Request& request) {
  HotRestartMessage wrapped_reply;
  Buffer::OwnedImpl buffer;
  auto id = request.pass_connection_data().connection_id();
  wrapped_reply.mutable_reply()->mutable_pass_connection_data()->set_connection_id(id);
  auto iter = handlers_.find(id);
  if (iter == handlers_.end()) {
    return wrapped_reply;
  }
  auto& handler = iter->second;
  if (handler.isOpen()) {
    Api::IoCallUint64Result result = handler.read(buffer, absl::nullopt);
    if (!result.ok()) {
      ENVOY_LOG(error, "reader from handler failed {}",
                result.err_.get()->getErrorDetails().data());
    }
  }
  auto buf = buffer.toString();
  ENVOY_LOG(debug, "reader from handler bytes {}", buf.length());
  if (buf.length() > 0) {
    wrapped_reply.mutable_reply()->mutable_pass_connection_data()->set_connection_data(buf);
  }
  return wrapped_reply;
}

// TODO(fredlas) if there are enough stats for stat name length to become an issue, this current
// implementation can negate the benefit of symbolized stat names by periodically reaching the
// magnitude of memory usage that they are meant to avoid, since this map holds full-string
// names. The problem can be solved by splitting the export up over many chunks.
void HotRestartingParent::Internal::exportStatsToChild(HotRestartMessage::Reply::Stats* stats) {
  for (const auto& gauge : server_->stats().gauges()) {
    if (gauge->used()) {
      const std::string name = gauge->name();
      (*stats->mutable_gauges())[name] = gauge->value();
      recordDynamics(stats, name, gauge->statName());
    }
  }

  for (const auto& counter : server_->stats().counters()) {
    if (counter->used()) {
      // The hot restart parent is expected to have stopped its normal stat exporting (and so
      // latching) by the time it begins exporting to the hot restart child.
      uint64_t latched_value = counter->latch();
      if (latched_value > 0) {
        const std::string name = counter->name();
        (*stats->mutable_counter_deltas())[name] = latched_value;
        recordDynamics(stats, name, counter->statName());
      }
    }
  }
  stats->set_memory_allocated(Memory::Stats::totalCurrentlyAllocated());
  stats->set_num_connections(server_->listenerManager().numConnections());
}

void HotRestartingParent::Internal::recordDynamics(HotRestartMessage::Reply::Stats* stats,
                                                   const std::string& name,
                                                   Stats::StatName stat_name) {
  // Compute an array of spans describing which components of the stat name are
  // dynamic. This is needed so that when the child recovers the StatName, it
  // correlates with how the system generates those stats, with the same exact
  // components using a dynamic representation.
  //
  // See https://github.com/envoyproxy/envoy/issues/9874 for more details.
  Stats::DynamicSpans spans = server_->stats().symbolTable().getDynamicSpans(stat_name);

  // Convert that C++ structure (controlled by stat_merger.cc) into a protobuf
  // for serialization.
  if (!spans.empty()) {
    HotRestartMessage::Reply::RepeatedSpan spans_proto;
    for (const Stats::DynamicSpan& span : spans) {
      HotRestartMessage::Reply::Span* span_proto = spans_proto.add_spans();
      span_proto->set_first(span.first);
      span_proto->set_last(span.second);
    }
    (*stats->mutable_dynamics())[name] = spans_proto;
  }
}

void HotRestartingParent::Internal::drainListeners() { server_->drainListeners(); }

} // namespace Server
} // namespace Envoy
