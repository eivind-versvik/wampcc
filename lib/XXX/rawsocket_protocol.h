#ifndef XXX_RAWSOCKET_PROTOCOL_H
#define XXX_RAWSOCKET_PROTOCOL_H

#include "XXX/protocol.h"

namespace XXX {

class rawsocket_protocol : public protocol
{
public:

  enum max_msg_size_flag
  {
    e_512_b = 0,
    e_1_kb,
    e_2_kb,
    e_4_kb,
    e_8_kb,
    e_16_kb,
    e_32_kb,
    e_64_kb,
    e_128_kb,
    e_256_kb,
    e_512_kb,
    e_1_mb,
    e_2_mb,
    e_4_mb,
    e_8_mb,
    e_16_mb
  };

  enum serialiser_flag
  {
    e_INVALID = 0,
    e_JSON    = 1,
    e_MSGPACK = 2
  };

  struct options
  {
    max_msg_size_flag inbound_max_msg_size;
    options(max_msg_size_flag __inbound_max_msg_size=e_512_kb)
      : inbound_max_msg_size(__inbound_max_msg_size)
    {}
  };

  static constexpr const char* NAME = "rawsocket";

  static constexpr unsigned char HANDSHAKE_SIZE = 4;
  static constexpr unsigned char HEADER_SIZE = HANDSHAKE_SIZE;
  static constexpr unsigned char MAGIC = 0x7F;

  rawsocket_protocol(io_handle*, t_msg_cb, connection_mode, options);

  void io_on_read(char*, size_t) override;
  void initiate(t_initiate_cb) override;
  const char* name() const override { return NAME; }
  void send_msg(const jalson::json_array& j);

private:

  void decode(const char*, size_t);

  enum handshake_error_code
  {
    e_SerialiserUnsupported    = 1,
    e_MaxMsgLengthUnacceptable = 2,
    e_UseOfReservedBits        = 3,
    e_MaxConnectionCountReach  = 4
  };

  void reply_handshake(int, int);

  enum Status
  {
    eHandshaking = 0,
    eOpen,
    eClosing,
    eClosed
  } m_state = eHandshaking;

  t_initiate_cb m_initiate_cb;

  /* maximum messages that this-side and remote-side can accept */
  unsigned int m_self_max_msg_size;
  unsigned int m_peer_max_msg_size;
  options m_options;
};



}

#endif
