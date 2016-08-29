#include "XXX/pre_session.h"

#include "XXX/io_handle.h"
#include "XXX/protocol.h"
#include "XXX/event_loop.h"
#include "XXX/log_macros.h"
#include "XXX/utils.h"
#include "XXX/kernel.h"
#include "XXX/rawsocket_protocol.h"
#include "XXX/websocket_protocol.h"
#include "XXX/http_parser.h"

#include <iomanip>

#include <string.h>
#include <unistd.h>


#define MAX_PENDING_OPEN_MS 5000

namespace XXX {


static std::atomic<uint64_t> m_next_pre_session_id(1); // start at 1, so that 0 implies invalid ID
static uint64_t generate_unique_session_id()
{
  return m_next_pre_session_id++;
}


bool pre_session::is_pending_open() const
{
  return (m_state != eClosed && m_state != eClosing);
}


int pre_session::duration_since_creation() const
{
  return (time(NULL) - m_time_create);
}


pre_session::pre_session(kernel& __kernel,
                         std::unique_ptr<io_handle> h,
                         on_closed_fn   __on_closed_cb,
                         on_protocol_fn protocol_cb)
  : m_state( eInit ),
    __logger(__kernel.get_logger()),
    m_kernel(__kernel),
    m_sid(generate_unique_session_id()),
    m_buf(1,1024),
    m_io_handle( std::move(h) ),
    m_time_create(time(NULL)),
    m_notify_closed_cb(__on_closed_cb),
    m_protocol_cb(protocol_cb)
{
}


std::shared_ptr<pre_session> pre_session::create(kernel& k,
                                                 std::unique_ptr<io_handle> ioh,
                                                 on_closed_fn __on_closed_cb,
                                                 on_protocol_fn protocol_cb )
{
  std::shared_ptr<pre_session> sp(
    new pre_session(k, std::move(ioh), __on_closed_cb, protocol_cb)
    );

  // can't put this initialisation step inside wamp_sesssion constructor,
  // because the shared pointer wont be created & available inside the
  // constructor
  sp->m_io_handle->start_read( sp );

  // set up a timer to expire this session if it has not been successfully
  // opened with a maximum time duration
  std::weak_ptr<pre_session> wp = sp;
  k.get_event_loop()->dispatch(
    std::chrono::milliseconds(MAX_PENDING_OPEN_MS),
    [wp]()
    {
      if (auto sp = wp.lock())
      {
//        LOG_WARN("closing pre-session; has failed to login within time limit");
        sp->close();
      }
      return 0;
    });

  return sp;
}


pre_session::~pre_session()
{
  // note: dont log in here, just in case logger has been deleted
}


void pre_session::close()
{
  // ANY thread

  /* Initiate the asynchronous close request. The sequence starts with a call to
   * close the underlying socket object.
   */

  if (m_state == eClosing || m_state == eClosed || m_state == eTransferedIO)
  {
    /* dont need to do anything */
  }
  else
  {
    change_state(eClosing, eClosing);
    m_io_handle->request_close();
  }
}


/* Invoked on IO thread when underlying socket has closed.
 *
 */
void pre_session::io_on_close()
{
  change_state(eClosing,eClosing);

  // push the final EV operation
  auto sp = shared_from_this();
  m_kernel.get_event_loop()->dispatch(
    [sp]()
    {
      /* EV thread */

      // Wait until the IO async object is fully closed before proceeding. It is
      // not enough that the underlying socket has notified end of
      // stream. Danger here is that we proceed to close the pre_session,
      // leading to its deletion, while the IO object still has a thread inside
      // it.
      sp->m_io_handle->request_close().wait();

      // When called, this should be the last callback from the EV, and marks
      // the end of asynchronous events targeted at and generated by self. This
      // session is now closed.
      sp->change_state(eClosed,eClosed);
      sp->m_notify_closed_cb(sp);
    } );
}


/* Invoked on IO thread when underlying socket has recevied data.
 *
 */
void pre_session::io_on_read(char* src, size_t len)
{
  try
  {
    io_on_read_impl(src,len);
    return;
  }
  catch ( std::exception& e )
  {
    LOG_WARN("closing pre-session: " << e.what());
  }
  catch (...)
  {
    LOG_WARN("closing pre-session due to unknown exception");
  }
  this->close();
}


void pre_session::io_on_read_impl(char* src, size_t len)
{
  protocol * proto_actual = nullptr;
  protocol_builder_fn builder_fn;

  while (len)
  {
    size_t consumed = m_buf.consume(src, len);
    src += consumed;
    len -= consumed;

    // Ensure that all bytes were consumed (ie len==0), since we will need to
    // pass everything onto any protocol instance that gets created. We don't
    // have any way to handle left over data in 'src' after the concrete
    // protocol is constructed (since it will then take over socket read
    // callbacks).
    if (len)
      throw handshake_error("failed to consume all bytes read from socket");

    auto rd = m_buf.read_ptr();

    if (rd.avail() >= rawsocket_protocol::HEADER_SIZE
        && rd[0] == rawsocket_protocol::MAGIC)
    {
      rawsocket_protocol::options default_opts;

      builder_fn = [&proto_actual, default_opts](io_handle* io,
                                                 protocol::t_msg_cb _msg_cb)
        {
          std::unique_ptr<protocol> up (
            new rawsocket_protocol(io, _msg_cb,
                                   protocol::connection_mode::ePassive,
                                   default_opts)
            );
          proto_actual = up.get();
          return up;
        };

      break;
    }
    else if (rd.avail() >= websocket_protocol::HEADER_SIZE &&
             http_parser::is_http_get(rd.ptr(), rd.avail()))
    {
      builder_fn = [&proto_actual](io_handle* io,protocol::t_msg_cb _msg_cb)
        {
          std::unique_ptr<protocol> up (
            new websocket_protocol(io, _msg_cb,
                                   protocol::connection_mode::ePassive));
          proto_actual = up.get();
          return up;
        };

      break;
    }
    else if (rd.avail() >= rawsocket_protocol::HEADER_SIZE)
    {
      throw handshake_error("unknown wire protocol");
    }
  }

  /* If we have identified the wire protocol, then invoke the callback object
   * which should result in the protocol instance being created.  Once created,
   * we immediately pass into it all of the bytes so far received. */
  if (builder_fn)
  {
    // this creates the concrete protocol, and a pointer to it is available
    // locally (in proto_actual)
    m_protocol_cb( builder_fn, std::move(m_io_handle) );

    if (proto_actual)
    {
      m_io_handle.reset();
      m_state = eTransferedIO;
      proto_actual->io_on_read(m_buf.data(), m_buf.data_size());
    }
    else
      throw handshake_error("protocol identified but instance was not created");
  }

}


void pre_session::change_state(SessionState expected, SessionState next)
{
  if (m_state == eClosed) return;

  if (next == eClosed)
  {
    LOG_INFO("pre-session closed, #" << m_sid);
    m_state = eClosed;
    return;
  }

  m_state = eClosing;
}






} // namespace XXX
