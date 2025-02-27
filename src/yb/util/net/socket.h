// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_UTIL_NET_SOCKET_H
#define YB_UTIL_NET_SOCKET_H

#include <sys/uio.h>
#include <string>

#include <boost/container/small_vector.hpp>

#include "yb/gutil/macros.h"

#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"

namespace yb {

class MonoDelta;
class MonoTime;

// Vector of io buffers. Could be used with receive, already received data etc.
typedef boost::container::small_vector<::iovec, 4> IoVecs;

size_t IoVecsFullSize(const IoVecs& io_vecs);
// begin and end are positions in concatenated io_vecs.
void IoVecsToBuffer(const IoVecs& io_vecs, size_t begin, size_t end, std::vector<char>* result);
void IoVecsToBuffer(const IoVecs& io_vecs, size_t begin, size_t end, char* result);
inline const char* IoVecBegin(const iovec& inp) { return static_cast<const char*>(inp.iov_base); }
inline const char* IoVecEnd(const iovec& inp) { return IoVecBegin(inp) + inp.iov_len; }

inline void IoVecRemovePrefix(size_t len, iovec* iov) {
  iov->iov_len -= len;
  iov->iov_base = static_cast<char*>(iov->iov_base) + len;
}

class Socket {
 public:
  static const int FLAG_NONBLOCKING = 0x1;
  static const int FLAG_IPV6 = 0x02;

  // Create a new invalid Socket object.
  Socket();

  Socket(Socket&& rhs) noexcept : fd_(rhs.fd_) { rhs.Release(); }

  // Start managing a socket.
  explicit Socket(int fd);

  // Close the socket.  Errors will be ignored.
  ~Socket();

  // Close the Socket, checking for errors.
  CHECKED_STATUS Close();

  // call shutdown() on the socket
  CHECKED_STATUS Shutdown(bool shut_read, bool shut_write);

  // Start managing a socket.
  void Reset(int fd);

  // Stop managing the socket and return it.
  int Release();

  // Get the raw file descriptor, or -1 if there is no file descriptor being
  // managed.
  int GetFd() const;

  CHECKED_STATUS Init(int flags); // See FLAG_NONBLOCKING

  // Set or clear TCP_NODELAY
  CHECKED_STATUS SetNoDelay(bool enabled);

  // Set or clear O_NONBLOCK
  CHECKED_STATUS SetNonBlocking(bool enabled);
  CHECKED_STATUS IsNonBlocking(bool* is_nonblock) const;

  // Set SO_SENDTIMEO to the specified value. Should only be used for blocking sockets.
  CHECKED_STATUS SetSendTimeout(const MonoDelta& timeout);

  // Set SO_RCVTIMEO to the specified value. Should only be used for blocking sockets.
  CHECKED_STATUS SetRecvTimeout(const MonoDelta& timeout);

  // Sets SO_REUSEADDR to 'flag'. Should be used prior to Bind().
  CHECKED_STATUS SetReuseAddr(bool flag);

  // Convenience method to invoke the common sequence:
  // 1) SetReuseAddr(true)
  // 2) Bind()
  // 3) Listen()
  CHECKED_STATUS BindAndListen(const Endpoint& endpoint, int listen_queue_size);

  // Start listening for new connections, with the given backlog size.
  // Requires that the socket has already been bound using Bind().
  CHECKED_STATUS Listen(int listen_queue_size);

  // Call getsockname to get the address of this socket.
  CHECKED_STATUS GetSocketAddress(Endpoint* out) const;

  // Call getpeername to get the address of the connected peer.
  CHECKED_STATUS GetPeerAddress(Endpoint* out) const;

  // Call bind() to bind the socket to a given address.
  // If bind() fails and indicates that the requested port is already in use,
  // and if explain_addr_in_use is set to true, generates an informative log message by calling
  // 'lsof' if available.
  CHECKED_STATUS Bind(const Endpoint& bind_addr, bool explain_addr_in_use = true);

  // Call accept(2) to get a new connection.
  CHECKED_STATUS Accept(Socket *new_conn, Endpoint* remote, int flags);

  // start connecting this socket to a remote address.
  CHECKED_STATUS Connect(const Endpoint& remote);

  // get the error status using getsockopt(2)
  CHECKED_STATUS GetSockError() const;

  CHECKED_STATUS Write(const uint8_t *buf, int32_t amt, int32_t *nwritten);

  CHECKED_STATUS Writev(const struct ::iovec *iov, int iov_len, int32_t *nwritten);

  // Blocking Write call, returns IOError unless full buffer is sent.
  // Underlying Socket expected to be in blocking mode. Fails if any Write() sends 0 bytes.
  // Returns OK if buflen bytes were sent, otherwise IOError.
  // Upon return, num_written will contain the number of bytes actually written.
  // See also writen() from Stevens (2004) or Kerrisk (2010)
  CHECKED_STATUS BlockingWrite(const uint8_t *buf, size_t buflen, size_t *num_written,
      const MonoTime& deadline);

  Result<int32_t> Recv(uint8_t* buf, int32_t amt);

  // Receives into multiple buffers, returns number of bytes received.
  Result<int32_t> Recvv(IoVecs* vecs);

  // Blocking Recv call, returns IOError unless requested amt bytes are read.
  // Underlying Socket expected to be in blocking mode. Fails if any Recv() reads 0 bytes.
  // Returns OK if amt bytes were read, otherwise IOError.
  // Upon return, nread will contain the number of bytes actually read.
  // See also readn() from Stevens (2004) or Kerrisk (2010)
  CHECKED_STATUS BlockingRecv(uint8_t *buf, size_t amt, size_t *nread, const MonoTime& deadline);

  // Implements the SOL_SOCKET/SO_RCVBUF socket option.
  Result<int32_t> GetReceiveBufferSize();
  CHECKED_STATUS SetReceiveBufferSize(int32_t size);

 private:
  // Called internally from SetSend/RecvTimeout().
  CHECKED_STATUS SetTimeout(int opt, std::string optname, const MonoDelta& timeout);

  // Called internally during socket setup.
  CHECKED_STATUS SetCloseOnExec();

  // Bind the socket to a local address before making an outbound connection,
  // based on the value of FLAGS_local_ip_for_outbound_sockets.
  CHECKED_STATUS BindForOutgoingConnection();

  int fd_;

  DISALLOW_COPY_AND_ASSIGN(Socket);
};

} // namespace yb

#endif // YB_UTIL_NET_SOCKET_H
