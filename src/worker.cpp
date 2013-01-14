
#include "worker.hpp"

#include <cocaine/context.hpp>
#include <cocaine/logging.hpp>
#include <cocaine/manifest.hpp>
#include <cocaine/profile.hpp>

#include <cocaine/api/sandbox.hpp>

#include <cocaine/traits/unique_id.hpp>
#include <cocaine/traits.hpp>
#include <cocaine/traits/json.hpp>

#include <boost/filesystem/path.hpp>

using namespace cocaine;
using namespace cocaine::engine;
using namespace cocaine::io;
using namespace cocaine::logging;

namespace fs = boost::filesystem;

namespace {
  struct upstream_t:
    public api::stream_t
  {
    upstream_t(const unique_id_t& id,
               worker_t * const worker):
      m_id(id),
      m_worker(worker),
      m_state(state_t::open)
      { }

    virtual
    ~upstream_t() {
      if(m_state != state_t::closed) {
        close();
      }
    }

    virtual
    void
    push(const char * chunk,
         size_t size) {
      switch(m_state) {
        case state_t::open:
          send<rpc::chunk>(std::string(chunk, size));
                    
          break;

        case state_t::closed:
          throw cocaine::error_t("the stream has been closed");
      }
    }

    virtual
    void
    error(error_code code,
          const std::string& message) {
      switch(m_state) {
        case state_t::open:
          m_state = state_t::closed;

          send<rpc::error>(static_cast<int>(code), message);
          send<rpc::choke>();

          break;

        case state_t::closed:
          throw cocaine::error_t("the stream has been closed");
      }
    }

    virtual
    void
    close() {
      switch(m_state) {
        case state_t::open:
          m_state = state_t::closed;

          send<rpc::choke>();

          break;

        case state_t::closed:
          throw cocaine::error_t("the stream has been closed");
      }
    }

  private:
    template<class Event, typename... Args>
    void
    send(Args&&... args) {
      m_worker->send<Event>(m_id, std::forward<Args>(args)...);
    }

  private:
    const unique_id_t m_id;
    worker_t * const m_worker;

    enum class state_t: int {
      open,
        closed
        };

    state_t m_state;
  };
}

worker_t::worker_t(context_t& context,
                   worker_config_t config):
  m_context(context),
  m_log(new log_t(context, cocaine::format("app/%s", config.app))),
  m_id(config.uuid),
  m_channel(context, ZMQ_DEALER, m_id),
  m_loop(uv_default_loop())
{
  std::string endpoint = cocaine::format(
    "ipc://%1%/engines/%2%",
    m_context.config.path.runtime,
    config.app);

  printf("i'm %p\n",(void*)this);
  std::cout << "connecting to:" << endpoint << std::endl;
    
  m_channel.connect(endpoint);

  COCAINE_LOG_ERROR(
    m_log,
    "%s: evening everybody",
    m_id
    );
  
  printf("pending %d\n",m_channel.pending());
  //m_watcher.set<worker_t, &worker_t::on_event>(this);
  //m_watcher.start(m_channel.fd(), uv::READ);
  m_watcher_uv = new uv_poll_t;
  uv_poll_init(m_loop,m_watcher_uv,m_channel.fd());
  m_watcher_uv->data=this;
  uv_poll_start(m_watcher_uv, UV_READABLE, worker_t::uv_on_event);

  //m_checker.set<worker_t, &worker_t::on_check>(this);
  //m_checker.start();
  m_checker_uv = new uv_prepare_t;
  uv_prepare_init(m_loop,m_checker_uv);
  m_checker_uv->data=this;
  uv_prepare_start(m_checker_uv,worker_t::uv_on_check);

  //m_heartbeat_timer.set<worker_t, &worker_t::on_heartbeat>(this);
  //m_heartbeat_timer.start(0.0f, 5.0f);
  m_heartbeat_timer_uv = new uv_timer_t;
  uv_timer_init(m_loop,m_heartbeat_timer_uv);
  m_heartbeat_timer_uv->data=this;
  uv_timer_start(m_heartbeat_timer_uv,
                 worker_t::uv_on_heartbeat,
                 0,5000);

  // Launching the app

  try {
    m_manifest.reset(new manifest_t(m_context, config.app));
    m_profile.reset(new profile_t(m_context, config.profile));
        
    fs::path path = fs::path(m_context.config.path.spool) / config.app;
         
    // m_sandbox = new JsSandbox(
    //   m_contect,
    //   m_manifest->name,
    //   m_manifest->sandbox.args,
    //   path.string())
    
  } catch(const std::exception& e) {
    terminate(rpc::suicide::abnormal, e.what());
    throw;
  } catch(...) {
    terminate(rpc::suicide::abnormal, "unexpected exception");
    throw;
  }
    
  m_disown_timer_uv = new uv_timer_t;
  uv_timer_init(m_loop,m_disown_timer_uv);
  m_disown_timer_uv->data=this;
  uv_timer_start(m_disown_timer_uv,
                 worker_t::uv_on_disown,
                 m_profile->heartbeat_timeout*1000,0);
}

worker_t::~worker_t() {
  // Empty.
}

void
worker_t::run() {
  // Empty.
}

void
worker_t::uv_on_event(uv_poll_t* hdl, int status, int events) {
  //BOOST_ASSERT(status==0);
  printf("worker %p got event\n",hdl->data);
  worker_t *w = static_cast<worker_t*>(hdl->data);
  w->on_event();
}

void
worker_t::on_event(){
  uv_prepare_stop(m_checker_uv);

  if(m_channel.pending()) {
    uv_prepare_start(m_checker_uv,worker_t::uv_on_check);
    process();
  }
}

void
worker_t::uv_on_check(uv_prepare_t *hdl,int) {
  //XXX
  //m_loop.feed_fd_event(m_channel.fd(), ev::READ);
  //worker_t *w = static_cast<worker_t*>(hdl->data);
  //uv_feed_fd_event(uv_default_loop(),
  //                 w->m_channel.fd(),
  //                 UV_READABLE);
}

void
worker_t::uv_on_heartbeat(uv_timer_t *hdl,int) {
  worker_t *w = static_cast<worker_t*>(hdl->data);
  printf("beat\n");
  scoped_option<
    options::send_timeout
    > option(w->m_channel, 0);
  w->send<rpc::heartbeat>();
}

void
worker_t::uv_on_disown(uv_timer_t *hdl,int) {
  worker_t *w = static_cast<worker_t*>(hdl->data);
  COCAINE_LOG_ERROR(
    w->m_log,
    "worker %s has lost the controlling engine",
    w->m_id
    );

  //m_loop.unloop(uv::ALL);
  //XXX
  //signal all sessions to exit
  //smth like m_dispatch.emit("disown");
}

void
worker_t::process() {
  int counter = defaults::io_bulk_size;

  do {
    // TEST: Ensure that we haven't missed something in a previous iteration.
    BOOST_ASSERT(!m_channel.more());
       
    int message_id = -1;

      {
        scoped_option<
          options::receive_timeout
          > option(m_channel, 0);

        if(!m_channel.recv(message_id)) {
          return;
        }
      }

      COCAINE_LOG_ERROR(
        m_log,
        "worker %s received type %d message",
        m_id,
        message_id);

      switch(message_id) {
        case event_traits<rpc::heartbeat>::id:
          uv_timer_stop(m_disown_timer_uv);
          uv_timer_start(m_disown_timer_uv,
                         worker_t::uv_on_heartbeat,
                         (int)(m_profile->heartbeat_timeout*1000),0);
          break;

        case event_traits<rpc::invoke>::id: {
          unique_id_t session_id(uninitialized);
          std::string event;

          m_channel.recv<rpc::invoke>(session_id, event);

          COCAINE_LOG_ERROR(
            m_log,
            "worker %s session %s: received event %s",
            m_id,session_id.string(),event);

          boost::shared_ptr<api::stream_t> upstream(
            boost::make_shared<upstream_t>(session_id, this)
            );

          try {
            // io_pair_t io = {
            //   upstream,
            //   m_sandbox->invoke(event, upstream)
            // };

            //m_streams.emplace(session_id, io);
            m_upstreams.emplace(session_id, upstream);

            Json::Value v;
            v["code"] = 200;
            Json::Value hdr;
            hdr.append("Content-Type");
            hdr.append("text/plain");
            v["headers"].append(hdr);
            std::ostringstream s;
            msgpack::packer<std::ostringstream> packer(s);
            io::type_traits<Json::Value>::pack(packer, v);
            std::string response = s.str();

            COCAINE_LOG_ERROR(
              m_log,
              "worker %s session %s: responding",
              m_id,session_id.string());

            upstream->push(response.data(),response.length());
            upstream->push("Evening everybody",17);
            //it->second.downstream->close();
            //upstream->close();

          } catch(const std::exception& e) {
            upstream->error(invocation_error, e.what());
          } catch(...) {
            upstream->error(invocation_error, "unexpected exception");
          }

          break;
        }

        case event_traits<rpc::chunk>::id: {
          unique_id_t session_id(uninitialized);
          std::string message;

          m_channel.recv<rpc::chunk>(session_id, message);
          COCAINE_LOG_ERROR(
            m_log,
            "worker %s session %s: received chunk length %d",
            m_id,session_id.string(),message.size());
            
          // //stream_map_t::iterator it(m_streams.find(session_id));
          // upstream_map_t::iterator it(m_upstreams.find(session_id));

          // // NOTE: This may be a chunk for a failed invocation, in which case there
          // // will be no active stream, so drop the message.
          // if(it != m_upstreams.end()) {
          //   try {
          //     //it->second.downstream->push(message.data(), message.size());
          //     //m_streams.erase(it);
          //     //m_upstreams.erase(it);
          //   } catch(const std::exception& e) {
          //     //it->second.upstream->error(invocation_error, e.what());
          //     it->second->error(invocation_error, e.what());
          //     //m_streams.erase(it);
          //     m_upstreams.erase(it);
          //   } catch(...) {
          //     //it->second.upstream->error(invocation_error, "unexpected exception");
          //     it->second->error(invocation_error, "unexpected exception");
          //     //m_streams.erase(it);
          //     m_upstreams.erase(it);
          //   }
          // }

          break;
        }

        case event_traits<rpc::choke>::id: {
          unique_id_t session_id(uninitialized);

          m_channel.recv<rpc::choke>(session_id);

          COCAINE_LOG_ERROR(
            m_log,
            "worker %s session %s: received close",
            m_id,session_id.string());

          //stream_map_t::iterator it = m_streams.find(session_id);
          upstream_map_t::iterator it = m_upstreams.find(session_id);

          // NOTE: This may be a choke for a failed invocation, in which case there
          // will be no active stream, so drop the message.
          if(it != m_upstreams.end()) {
            try {
              // std::map<
              //   std::string,
              //   std::string> res;

              // std::map<
              //   std::string,
              //   std::string> hh;
          
              // hh[(std::string)"X-By"]=(std::string)"coca-worker";

              // res[(std::string)"code"]=(std::string)"200";
              // //res[(std::string)"headers"]=hh;

              // msgpack::sbuffer buffer;
              // msgpack::packer<msgpack::sbuffer> packer(buffer);

              // packer << res;
              // COCAINE_LOG_ERROR(
              //   m_log,
              //   "worker %s session %s: responding",
              //   m_id,session_id.string());

              // it->second->push(buffer.data(),buffer.size());
              // it->second->push("Evening everybody",17);
              // //it->second.downstream->close();
              it->second->close();
              //
            } catch(const std::exception& e) {
              //it->second.upstream->error(invocation_error, e.what());
              it->second->error(invocation_error, e.what());
            } catch(...) {
              //it->second.upstream->error(invocation_error, "unexpected exception");
              it->second->error(invocation_error, "unexpected exception");
            }
                    
            //m_streams.erase(it);
            m_upstreams.erase(it);
          }

          break;
        }
            
        case event_traits<rpc::terminate>::id:
          terminate(rpc::suicide::normal, "per request");
          break;

        default:
          COCAINE_LOG_WARNING(
            m_log,
            "worker %s dropping unknown type %d message", 
            m_id,
            message_id
            );
                
          m_channel.drop();
      }
  } while(--counter);

  // Feed the event loop.
  //m_loop.feed_fd_event(m_channel.fd(), uv::READ);
  //XXX

}

void
worker_t::terminate(rpc::suicide::reasons reason,
                    const std::string& message)
{
  send<rpc::suicide>(static_cast<int>(reason), message);
  //m_loop.unloop(uv::ALL);
  //XXX
  exit(0);
}
