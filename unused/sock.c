/*
Copyright: Boaz Segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sock.h"
#include "spnlock.inc"
/* *****************************************************************************
Includes and state
***************************************************************************** */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#if BUFFER_PACKET_SIZE < (BUFFER_FILE_READ_SIZE + 64)
#error BUFFER_PACKET_POOL must be bigger than BUFFER_FILE_READ_SIZE + 64.
#endif

/* *****************************************************************************
OS Sendfile settings.
*/

#ifndef USE_SENDFILE

#if defined(__linux__) /* linux sendfile works  */
#include <sys/sendfile.h>
#define USE_SENDFILE 1
#elif defined(__unix__) /* BSD sendfile should work, but isn't tested */
#include <sys/uio.h>
#define USE_SENDFILE 0
#elif defined(__APPLE__) /* Is the apple sendfile still broken? */
#include <sys/uio.h>
#define USE_SENDFILE 1
#else /* sendfile might not be available - always set to 0 */
#define USE_SENDFILE 0
#endif

#endif

/* *****************************************************************************
Support `libreact` on_close callback, if exist.
*/

#pragma weak reactor_on_close
void reactor_on_close(intptr_t uuid) { (void)(uuid); }
#pragma weak reactor_remove
int reactor_remove(intptr_t uuid) {
  (void)(uuid);
  return -1;
}

/* *****************************************************************************
Support timeout setting.
*/
#pragma weak sock_touch
void sock_touch(intptr_t uuid) { (void)(uuid); }

/* *****************************************************************************
User-Land Buffer and Packets
***************************************************************************** */

typedef struct packet_s {
  struct packet_metadata_s {
    int (*write_func)(int fd, struct packet_s *packet);
    void (*free_func)(struct packet_s *packet);
    struct packet_s *next;
  } metadata;
  sock_buffer_s buffer;
} packet_s;

struct {
  packet_s *next;
  spn_lock_i lock;
  uint8_t init;
  packet_s mem[BUFFER_PACKET_POOL];
} packet_pool;

static void sock_packet_free_none(struct packet_s *packet) { (void)packet; }

static inline void sock_packet_clear(packet_s *packet) {
  packet->metadata.free_func(packet);
  packet->metadata =
      (struct packet_metadata_s){.free_func = sock_packet_free_none};
  packet->buffer.len = 0;
}

static inline void sock_packet_free(packet_s *packet) {
  sock_packet_clear(packet);
  if (packet >= packet_pool.mem &&
      packet <= packet_pool.mem + (BUFFER_PACKET_POOL - 1)) {
    spn_lock(&packet_pool.lock);
    packet->metadata.next = packet_pool.next;
    packet_pool.next = packet;
    spn_unlock(&packet_pool.lock);
  } else
    free(packet);
}

static inline packet_s *sock_packet_try_grab(void) {
  packet_s *packet = NULL;
  spn_lock(&packet_pool.lock);
  packet = packet_pool.next;
  if (packet)
    packet_pool.next = packet->metadata.next;
  else if (!packet_pool.init)
    goto init;
  spn_unlock(&packet_pool.lock);
  packet->metadata =
      (struct packet_metadata_s){.free_func = sock_packet_free_none};
  packet->buffer.len = 0;
  return packet;
init:
  packet_pool.init = 1;
  packet_pool.mem[0].metadata.free_func = sock_packet_free_none;
  for (size_t i = 2; i < BUFFER_PACKET_POOL; i++) {
    packet_pool.mem[i - 1].metadata.next = packet_pool.mem + i;
    packet_pool.mem[i - 1].metadata.free_func = sock_packet_free_none;
  }
  packet_pool.mem[BUFFER_PACKET_POOL - 1].metadata.free_func =
      sock_packet_free_none;
  spn_unlock(&packet_pool.lock);
  packet = packet_pool.mem;
  packet->metadata =
      (struct packet_metadata_s){.free_func = sock_packet_free_none};
  packet->buffer.len = 0;
  return packet;
}

static inline packet_s *sock_packet_grab(void) {
  packet_s *packet = sock_packet_try_grab();
  if (packet)
    return packet;
  while (packet == NULL) {
    sock_flush_all();
    packet = sock_packet_try_grab();
  };
  return packet;
}

/* *****************************************************************************
Default Socket Read/Write Hook
***************************************************************************** */

static ssize_t sock_default_hooks_read(intptr_t uuid, void *buf, size_t count) {
  return read(sock_uuid2fd(uuid), buf, count);
}
static ssize_t sock_default_hooks_write(intptr_t uuid, const void *buf,
                                        size_t count) {
  return write(sock_uuid2fd(uuid), buf, count);
}

static void sock_default_hooks_on_clear(intptr_t fduuid,
                                        struct sock_rw_hook_s *rw_hook) {
  (void)rw_hook;
  (void)fduuid;
}

static ssize_t sock_default_hooks_flush(intptr_t uuid) {
  return (((void)(uuid)), 0);
}

sock_rw_hook_s sock_default_hooks = {
    .read = sock_default_hooks_read,
    .write = sock_default_hooks_write,
    .flush = sock_default_hooks_flush,
    .on_clear = sock_default_hooks_on_clear,
};
/* *****************************************************************************
Socket Data Structures
***************************************************************************** */
struct fd_data_s {
  /** Connection counter - collision protection. */
  uint8_t counter;
  /** Connection lock */
  spn_lock_i lock;
  /** Connection is open */
  unsigned open : 1;
  /** indicated that the connection should be closed. */
  unsigned close : 1;
  /** indicated that the connection experienced an error. */
  unsigned err : 1;
  /** future flags. */
  unsigned rsv : 5;
  /** data sent from current packet - this is per packet. */
  size_t sent;
  /** the currently active packet to be sent. */
  packet_s *packet;
  /** RW hooks. */
  sock_rw_hook_s *rw_hooks;
};

struct sock_data_s {
  size_t capacity;
  struct fd_data_s *fds;
} sock_data_s;

#define fd2uuid(fd) (((uintptr_t)(fd) << 8) | (sock_data_s.fds[(fd)].counter))
#define fdinfo(fd) sock_data_s.fds[(fd)]

#define lock_fd(fd) spn_lock(&sock_data_s.fds[(fd)].lock)
#define unlock_fd(fd) spn_unlock(&sock_data_s.fds[(fd)].lock)

static inline int validate_uuid(uintptr_t uuid) {
  uintptr_t fd = sock_uuid2fd(uuid);
  if (sock_data_s.capacity <= fd || fdinfo(fd).counter != (uuid & 0xFF))
    return -1;
  return 0;
}

static inline void sock_packet_rotate_unsafe(uintptr_t fd) {
  packet_s *packet = fdinfo(fd).packet;
  fdinfo(fd).packet = packet->metadata.next;
  fdinfo(fd).sent = 0;
  sock_packet_free(packet);
}

static inline int clear_fd(uintptr_t fd, uint8_t is_open) {
  if (sock_data_s.capacity <= fd)
    goto reinitialize;
  packet_s *packet;
clear:
  spn_lock(&(fdinfo(fd).lock));
  struct fd_data_s old_data = fdinfo(fd);
  sock_data_s.fds[fd] = (struct fd_data_s){.open = is_open,
                                           .lock = fdinfo(fd).lock,
                                           .rw_hooks = &sock_default_hooks,
                                           .counter = fdinfo(fd).counter + 1};
  spn_unlock(&(fdinfo(fd).lock));
  packet = old_data.packet;
  while (old_data.packet) {
    old_data.packet = old_data.packet->metadata.next;
    sock_packet_free(packet);
    packet = old_data.packet;
  }
  if (old_data.rw_hooks)
    old_data.rw_hooks->on_clear(((fd << 8) | old_data.counter),
                                old_data.rw_hooks);
  return 0;
reinitialize : {
  struct fd_data_s *new_collection =
      realloc(sock_data_s.fds, sizeof(struct fd_data_s) * (fd << 1));
  if (new_collection) {
    memset(new_collection + sock_data_s.capacity, 0,
           ((fd << 1) - sock_data_s.capacity) * sizeof(struct fd_data_s));
    sock_data_s.capacity = fd << 1;
    sock_data_s.fds = new_collection;
  } else
    return -1;
  goto clear;
}
}

/* *****************************************************************************
Writing - from memory
***************************************************************************** */

struct sock_packet_ext_data_s {
  uint8_t *buffer;
  uint8_t *to_free;
  void (*dealloc)(void *);
};

static int sock_write_buffer(int fd, struct packet_s *packet) {
  int written = fdinfo(fd).rw_hooks->write(
      fd2uuid(fd), packet->buffer.buf + fdinfo(fd).sent,
      packet->buffer.len - fdinfo(fd).sent);
  if (written > 0) {
    fdinfo(fd).sent += written;
    if (fdinfo(fd).sent == packet->buffer.len)
      sock_packet_rotate_unsafe(fd);
    return written;
  }
  return written;
}

static int sock_write_buffer_ext(int fd, struct packet_s *packet) {
  struct sock_packet_ext_data_s *ext = (void *)packet->buffer.buf;
  int written =
      fdinfo(fd).rw_hooks->write(fd2uuid(fd), ext->buffer + fdinfo(fd).sent,
                                 packet->buffer.len - fdinfo(fd).sent);
  if (written > 0) {
    fdinfo(fd).sent += written;
    if (fdinfo(fd).sent == packet->buffer.len)
      sock_packet_rotate_unsafe(fd);
    return written;
  }
  if (written < 0 && (errno == EWOULDBLOCK || errno == EAGAIN ||
                      errno == EINTR || errno == ENOTCONN))
    return 0;
  return -1;
}

static void sock_free_buffer_ext(packet_s *packet) {
  struct sock_packet_ext_data_s *ext = (void *)packet->buffer.buf;
  ext->dealloc(ext->to_free);
}

/* *****************************************************************************
Writing - from files
***************************************************************************** */

struct sock_packet_file_data_s {
  intptr_t fd;
  off_t offset;
  void (*close)(void *);
  uint8_t buffer[];
};

static void sock_close_from_fd(packet_s *packet) {
  struct sock_packet_file_data_s *ext = (void *)packet->buffer.buf;
  ext->close((void *)ext->fd);
}

static void sock_mock_close_fd(intptr_t fd) { close(fd); }

static int sock_write_from_fd(int fd, struct packet_s *packet) {
  struct sock_packet_file_data_s *ext = (void *)packet->buffer.buf;
  ssize_t count = 0;
  do {
    fdinfo(fd).sent += count;
    packet->buffer.len -= count;
  retry:
    count = (packet->buffer.len < BUFFER_FILE_READ_SIZE)
                ? pread(ext->fd, ext->buffer, packet->buffer.len,
                        ext->offset + fdinfo(fd).sent)
                : pread(ext->fd, ext->buffer, BUFFER_FILE_READ_SIZE,
                        ext->offset + fdinfo(fd).sent);
    if (count <= 0)
      goto read_error;
    count = fdinfo(fd).rw_hooks->write(fd, ext->buffer, count);
  } while (count == BUFFER_FILE_READ_SIZE && packet->buffer.len);
  if (count < 0)
    return -1;
  fdinfo(fd).sent += count;
  packet->buffer.len -= count;
  if (!packet->buffer.len) {
    sock_packet_rotate_unsafe(fd);
    return 1;
  }
  return count;

read_error:
  if (count == 0) {
    sock_packet_rotate_unsafe(fd);
    return 1;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    goto retry;
  return -1;
}

#if USE_SENDFILE == 1 && 0

#if defined(__linux__) /* linux sendfile API */
static int sock_sendfile_from_fd(int fd, struct packet_s *packet) {
  ssize_t sent;
  sock_packet_s *packet = fd_info[fd].packet;
  sent =
      sendfile64(fd, (int)((ssize_t)packet->buffer), &packet->metadata.offset,
                 packet->length - fd_info[fd].sent);

  if (sent < 0) {
    if (ERR_OK)
      return -1;
    else if (ERR_TRY_AGAIN)
      return 0;
    else
      return sock_flush_fd_failed(fd);
  }
  if (sent == 0)
    fd_info[fd].sent = packet->length;
  fd_info[fd].sent += sent;
  return 0;
}

#elif defined(__APPLE__) || defined(__unix__) /* BSD / Apple API */

static int sock_sendfile_from_fd(int fd, struct packet_s *packet) {
  off_t act_sent;
  sock_packet_s *packet = fd_info[fd].packet;
  act_sent = packet->length - fd_info[fd].sent;

#if defined(__APPLE__)
  if (sendfile((int)((ssize_t)packet->buffer), fd, packet->metadata.offset,
               &act_sent, NULL, 0) < 0 &&
      act_sent == 0)
#else
  if (sendfile((int)((ssize_t)packet->buffer), fd, packet->metadata.offset,
               (size_t)act_sent, NULL, &act_sent, 0) < 0 &&
      act_sent == 0)
#endif
  {
    if (ERR_OK)
      return -1;
    else if (ERR_TRY_AGAIN)
      return 0;
    else
      return sock_flush_fd_failed(fd);
  }
  if (act_sent == 0) {
    fd_info[fd].sent = packet->length;
    return 0;
  }
  packet->metadata.offset += act_sent;
  fd_info[fd].sent += act_sent;
  return 0;
}
#endif

#else

static int (*sock_sendfile_from_fd)(int fd, struct packet_s *packet) =
    sock_write_from_fd;

#endif

/* *****************************************************************************
The API
***************************************************************************** */

/* *****************************************************************************
Process wide and helper sock_API.
*/

/**
Sets a socket to non blocking state.

This function is called automatically for the new socket, when using
`sock_accept` or `sock_connect`.
*/
int sock_set_non_block(int fd) {
/* If they have O_NONBLOCK, use the Posix way to do it */
#if defined(O_NONBLOCK)
  /* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
    flags = 0;
  // printf("flags initial value was %d\n", flags);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
  /* Otherwise, use the old way of doing it */
  static int flags = 1;
  return ioctl(fd, FIOBIO, &flags);
#endif
}

/**
Gets the maximum number of file descriptors this process can be allowed to
access (== maximum fd value + 1).

If the "soft" limit is lower then the "hard" limit, the process's limits will be
extended to the allowed "hard" limit.
*/
ssize_t sock_max_capacity(void) {
  // get current limits
  static ssize_t flim = 0;
  if (flim)
    return flim;
#ifdef _SC_OPEN_MAX
  flim = sysconf(_SC_OPEN_MAX);
#elif defined(OPEN_MAX)
  flim = OPEN_MAX;
#endif
  // try to maximize limits - collect max and set to max
  struct rlimit rlim = {.rlim_max = 0};
  getrlimit(RLIMIT_NOFILE, &rlim);
// printf("Meximum open files are %llu out of %llu\n", rlim.rlim_cur,
//        rlim.rlim_max);
#if defined(__APPLE__) /* Apple's getrlimit is broken. */
  rlim.rlim_cur = rlim.rlim_max >= OPEN_MAX ? OPEN_MAX : rlim.rlim_max;
#else
  rlim.rlim_cur = rlim.rlim_max;
#endif

  setrlimit(RLIMIT_NOFILE, &rlim);
  getrlimit(RLIMIT_NOFILE, &rlim);
  // printf("Meximum open files are %llu out of %llu\n", rlim.rlim_cur,
  //        rlim.rlim_max);
  // if the current limit is higher than it was, update
  if (flim < ((ssize_t)rlim.rlim_cur))
    flim = rlim.rlim_cur;
  // return what we have
  return flim;
}

/* *****************************************************************************
The main sock_API.
*/

/**
Opens a listening non-blocking socket. Return's the socket's UUID.

Returns -1 on error. Returns a valid socket (non-random) UUID.

UUIDs with values less then -1 are valid values, depending on the system's
byte-ordering.

Socket UUIDs are predictable and shouldn't be used outside the local system.
They protect against connection mixups on concurrent systems (i.e. when saving
client data for "broadcasting" or when an old client task is preparing a
response in the background while a disconnection and a new connection occur on
the same `fd`).
*/
intptr_t sock_listen(const char *address, const char *port) {
  int srvfd;
  // setup the address
  struct addrinfo hints;
  struct addrinfo *servinfo;       // will point to the results
  memset(&hints, 0, sizeof hints); // make sure the struct is empty
  hints.ai_family = AF_UNSPEC;     // don't care IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
  hints.ai_flags = AI_PASSIVE;     // fill in my IP for me
  if (getaddrinfo(address, port, &hints, &servinfo)) {
    // perror("addr err");
    return -1;
  }
  // get the file descriptor
  srvfd =
      socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
  if (srvfd <= 0) {
    // perror("socket err");
    freeaddrinfo(servinfo);
    return -1;
  }
  // make sure the socket is non-blocking
  if (sock_set_non_block(srvfd) < 0) {
    // perror("couldn't set socket as non blocking! ");
    freeaddrinfo(servinfo);
    close(srvfd);
    return -1;
  }
  // avoid the "address taken"
  {
    int optval = 1;
    setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
  }
  // bind the address to the socket
  {
    int bound = 0;
    for (struct addrinfo *p = servinfo; p != NULL; p = p->ai_next) {
      if (!bind(srvfd, p->ai_addr, p->ai_addrlen))
        bound = 1;
    }

    if (!bound) {
      // perror("bind err");
      freeaddrinfo(servinfo);
      close(srvfd);
      return -1;
    }
  }
  freeaddrinfo(servinfo);
  // listen in
  if (listen(srvfd, SOMAXCONN) < 0) {
    // perror("couldn't start listening");
    close(srvfd);
    return -1;
  }
  clear_fd(srvfd, 1);
  return fd2uuid(srvfd);
}

/**
`sock_accept` accepts a new socket connection from the listening socket
`server_fd`, allowing the use of `sock_` functions with this new file
descriptor.

When using `libreact`, remember to call `int reactor_add(intptr_t uuid);` to
listen for events.

Returns -1 on error. Returns a valid socket (non-random) UUID.

Socket UUIDs are predictable and shouldn't be used outside the local system.
They protect against connection mixups on concurrent systems (i.e. when saving
client data for "broadcasting" or when an old client task is preparing a
response in the background while a disconnection and a new connection occur on
the same `fd`).
*/
intptr_t sock_accept(intptr_t srv_uuid) {
  static socklen_t cl_addrlen = 0;
  int client;
#ifdef SOCK_NONBLOCK
  client = accept4(sock_uuid2fd(srv_uuid), NULL, &cl_addrlen, SOCK_NONBLOCK);
  if (client <= 0)
    return -1;
#else
  client = accept(sock_uuid2fd(srv_uuid), NULL, &cl_addrlen);
  if (client <= 0)
    return -1;
  sock_set_non_block(client);
#endif
  clear_fd(client, 1);
  return fd2uuid(client);
}

/**
`sock_connect` is similar to `sock_accept` but should be used to initiate a
client connection to the address requested.

Returns -1 on error. Returns a valid socket (non-random) UUID.

Socket UUIDs are predictable and shouldn't be used outside the local system.
They protect against connection mixups on concurrent systems (i.e. when saving
client data for "broadcasting" or when an old client task is preparing a
response in the background while a disconnection and a new connection occur on
the same `fd`).

When using `libreact`, remember to call `int reactor_add(intptr_t uuid);` to
listen for events.

NOTICE:

This function is non-blocking, meaning that the connection probably wasn't
established by the time the function returns (this prevents the function from
hanging while waiting for a network timeout).

Use select, poll, `libreact` or other solutions to review the connection state
before attempting to write to the socket.
*/
intptr_t sock_connect(char *address, char *port) {
  int fd;
  // setup the address
  struct addrinfo hints;
  struct addrinfo *addrinfo;       // will point to the results
  memset(&hints, 0, sizeof hints); // make sure the struct is empty
  hints.ai_family = AF_UNSPEC;     // don't care IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
  hints.ai_flags = AI_PASSIVE;     // fill in my IP for me
  if (getaddrinfo(address, port, &hints, &addrinfo)) {
    return -1;
  }
  // get the file descriptor
  fd =
      socket(addrinfo->ai_family, addrinfo->ai_socktype, addrinfo->ai_protocol);
  if (fd <= 0) {
    freeaddrinfo(addrinfo);
    return -1;
  }
  // make sure the socket is non-blocking
  if (sock_set_non_block(fd) < 0) {
    freeaddrinfo(addrinfo);
    close(fd);
    return -1;
  }

  if (connect(fd, addrinfo->ai_addr, addrinfo->ai_addrlen) < 0 &&
      errno != EINPROGRESS) {
    close(fd);
    freeaddrinfo(addrinfo);
    return -1;
  }
  freeaddrinfo(addrinfo);
  clear_fd(fd, 1);
  return fd2uuid(fd);
}

/**
`sock_open` takes an existing file descriptor `fd` and initializes it's status
as open and available for `sock_API` calls, returning a valid UUID.

This will reinitialize the data (user buffer etc') for the file descriptor
provided, calling the `reactor_on_close` callback if the `fd` was previously
marked as used.

When using `libreact`, remember to call `int reactor_add(intptr_t uuid);` to
listen for events.

Returns -1 on error. Returns a valid socket (non-random) UUID.

Socket UUIDs are predictable and shouldn't be used outside the local system.
They protect against connection mixups on concurrent systems (i.e. when saving
client data for "broadcasting" or when an old client task is preparing a
response in the background while a disconnection and a new connection occur on
the same `fd`).
*/
intptr_t sock_open(int fd) {
  clear_fd(fd, 1);
  return fd2uuid(fd);
}

/**
Returns 1 if the uuid refers to a valid and open, socket.

Returns 0 if not.
*/
int sock_isvalid(intptr_t uuid) {
  return validate_uuid(uuid) == 0 && fdinfo(sock_uuid2fd(uuid)).open;
}

/**
`sock_fd2uuid` takes an existing file decriptor `fd` and returns it's active
`uuid`.

If the file descriptor is marked as closed (wasn't opened / registered with
`libsock`) the function returns -1;

If the file descriptor was closed remotely (or not using `libsock`), a false
positive will be possible. This is not an issue, since the use of an invalid fd
will result in the registry being updated and the fd being closed.

Returns -1 on error. Returns a valid socket (non-random) UUID.
*/
intptr_t sock_fd2uuid(int fd) {
  return (fd > 0 && sock_data_s.capacity > (size_t)fd &&
          sock_data_s.fds[fd].open)
             ? fd2uuid(fd)
             : -1;
}

/**
`sock_read` attempts to read up to count bytes from the socket into the buffer
starting at buf.

On a connection error (NOT EAGAIN or EWOULDBLOCK), signal interrupt, or when the
connection was closed, `sock_read` returns -1.

The value 0 is the valid value indicating no data was read.

Data might be available in the kernel's buffer while it is not available to be
read using `sock_read` (i.e., when using a transport layer, such as TLS).
*/
ssize_t sock_read(intptr_t uuid, void *buf, size_t count) {
  if (validate_uuid(uuid) || !fdinfo(sock_uuid2fd(uuid)).open) {
    errno = EBADF;
    return -1;
  }
  lock_fd(sock_uuid2fd(uuid));
  if (!fdinfo(sock_uuid2fd(uuid)).open) {
    unlock_fd(sock_uuid2fd(uuid));
    errno = EBADF;
    return -1;
  }
  ssize_t ret = fdinfo(sock_uuid2fd(uuid)).rw_hooks->read(uuid, buf, count);
  unlock_fd(sock_uuid2fd(uuid));
  if (ret > 0)
    return ret;
  if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR ||
                  errno == ENOTCONN))
    return 0;
  int old_errno = errno;
  sock_force_close(uuid);
  errno = ret ? old_errno : ECONNRESET;
  return -1;
}

/**
`sock_write2_fn` is the actual function behind the macro `sock_write2`.
*/
ssize_t sock_write2_fn(sock_write_info_s options) {
  int fd = sock_uuid2fd(options.uuid);
  if (validate_uuid(options.uuid))
    clear_fd(fd, 0);
  // avoid work when an error is expected to occur.
  if (!fdinfo(fd).open || options.offset < 0) {
    if (options.move == 0 && options.is_fd == 0) {
      errno = (options.offset < 0) ? ERANGE : EBADF;
      return -1;
    }
    if (options.move)
      (options.dealloc ? options.dealloc : free)((void *)options.buffer);
    else
      (options.dealloc ? (void (*)(intptr_t))options.dealloc
                       : sock_mock_close_fd)((intptr_t)options.buffer);
    errno = (options.offset < 0) ? ERANGE : EBADF;
    return -1;
  }
  // if (options.offset < 0)
  //   options.offset = 0;
  packet_s *packet = sock_packet_grab();
  packet->buffer.len = options.length;
  if (options.is_fd == 0) {  /* is data */
    if (options.move == 0) { /* memory is copied. */
      if (options.length <= BUFFER_PACKET_SIZE) {
        /* small enough for internal buffer */
        memcpy(packet->buffer.buf, (uint8_t *)options.buffer + options.offset,
               options.length);
        packet->metadata =
            (struct packet_metadata_s){.write_func = sock_write_buffer,
                                       .free_func = sock_packet_free_none};
        goto place_packet_in_queue;
      }
      /* too big for the pre-allocated buffer */
      void *copy = malloc(options.length);
      memcpy(copy, (uint8_t *)options.buffer + options.offset, options.length);
      options.offset = 0;
      options.buffer = copy;
    }
    /* memory moved, not copied. */
    struct sock_packet_ext_data_s *ext = (void *)packet->buffer.buf;
    ext->buffer = (uint8_t *)options.buffer + options.offset;
    ext->to_free = (uint8_t *)options.buffer;
    ext->dealloc = options.dealloc ? options.dealloc : free;
    packet->metadata = (struct packet_metadata_s){
        .write_func = sock_write_buffer_ext, .free_func = sock_free_buffer_ext};
  } else { /* is file */
    struct sock_packet_file_data_s *ext = (void *)packet->buffer.buf;
    ext->fd = (intptr_t)options.buffer;
    ext->close = options.dealloc ? options.dealloc : (void (*)(void *))close;
    ext->offset = options.offset;
    packet->metadata = (struct packet_metadata_s){
        .write_func =
            (fdinfo(sock_uuid2fd(options.uuid)).rw_hooks == &sock_default_hooks
                 ? sock_sendfile_from_fd
                 : sock_write_from_fd),
        .free_func = options.move ? sock_close_from_fd : sock_packet_free_none};
  }
/* place packet in queue */
place_packet_in_queue:
  if (validate_uuid(options.uuid))
    goto error;
  lock_fd(fd);
  if (!fdinfo(fd).open) {
    unlock_fd(fd);
    goto error;
  }
  packet_s **pos = &fdinfo(fd).packet;
  if (options.urgent == 0) {
    while (*pos)
      pos = &(*pos)->metadata.next;
  } else {
    if (*pos && fdinfo(fd).sent)
      pos = &(*pos)->metadata.next;
    packet->metadata.next = *pos;
  }
  *pos = packet;
  unlock_fd(fd);
  sock_flush(options.uuid);
  return 0;

error:
  sock_packet_free(packet);
  errno = EBADF;
  return -1;
}
#define sock_write2(...) sock_write2_fn((sock_write_info_s){__VA_ARGS__})

/**
`sock_flush` writes the data in the internal buffer to the underlying file
descriptor and closes the underlying fd once it's marked for closure (and all
the data was sent).

Return value: 0 will be returned on success and -1 will be returned on an error
or when the connection is closed.

**Please Note**: when using `libreact`, the `sock_flush` will be called
automatically when the socket is ready.
*/
ssize_t sock_flush(intptr_t uuid) {
  int fd = sock_uuid2fd(uuid);
  if (validate_uuid(uuid) || !fdinfo(fd).open || !fdinfo(fd).packet)
    return -1;
  ssize_t ret;
  lock_fd(fd);
retry:
  while ((ret = fdinfo(fd).rw_hooks->flush(fd)) > 0)
    ;
  if (ret == -1) {
    if (errno == EINTR)
      goto retry;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOTCONN)
      goto finish;
    goto error;
  }
  while (fdinfo(fd).packet && (ret = fdinfo(fd).packet->metadata.write_func(
                                   fd, fdinfo(fd).packet)) > 0)
    ;
  if (ret == -1) {
    if (errno == EINTR)
      goto retry;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOTCONN)
      goto finish;
    goto error;
  }
finish:
  unlock_fd(fd);
  return 0;
error:
  unlock_fd(fd);
  sock_force_close(uuid);
  return -1;
}
/**
`sock_flush_strong` performs the same action as `sock_flush` but returns only
after all the data was sent. This is a "busy" wait, polling isn't performed.
*/
void sock_flush_strong(intptr_t uuid) {
  while (sock_flush(uuid) == 0)
    ;
}
/**
Calls `sock_flush` for each file descriptor that's buffer isn't empty.
*/
void sock_flush_all(void) {
  for (size_t fd = 0; fd < sock_data_s.capacity; fd++) {
    if (!fdinfo(fd).open || !fdinfo(fd).packet)
      continue;
    sock_flush(fd2uuid(fd));
  }
}
/**
`sock_close` marks the connection for disconnection once all the data was sent.
The actual disconnection will be managed by the `sock_flush` function.

`sock_flash` will automatically be called.
*/
void sock_close(intptr_t uuid) {
  if (validate_uuid(uuid) || !fdinfo(sock_uuid2fd(uuid)).open)
    return;
  fdinfo(sock_uuid2fd(uuid)).close = 1;
  sock_flush(uuid);
}
/**
`sock_force_close` closes the connection immediately, without adhering to any
protocol restrictions and without sending any remaining data in the connection
buffer.
*/
void sock_force_close(intptr_t uuid) {
  if (validate_uuid(uuid))
    return;
  shutdown(sock_uuid2fd(uuid), SHUT_RDWR);
  close(sock_uuid2fd(uuid));
  clear_fd(sock_uuid2fd(uuid), 0);
}

/* *****************************************************************************
Direct user level buffer API.

The following API allows data to be written directly to the packet, minimizing
memory copy operations.
*/

/**
Checks out a `sock_buffer_s` from the buffer pool.
*/
sock_buffer_s *sock_buffer_checkout(void) {
  packet_s *ret = sock_packet_grab();
  return &ret->buffer;
}
/**
Attaches a packet to a socket's output buffer and calls `sock_flush` for the
socket.

Returns -1 on error. Returns 0 on success. The `buffer` memory is always
automatically managed.
*/
ssize_t sock_buffer_send(intptr_t uuid, sock_buffer_s *buffer) {
  if (validate_uuid(uuid) || !fdinfo(sock_uuid2fd(uuid)).open) {
    sock_buffer_free(buffer);
    return -1;
  }
  packet_s **tmp, *packet = (packet_s *)((uintptr_t)(buffer) -
                                         sizeof(struct packet_metadata_s));
  packet->metadata =
      (struct packet_metadata_s){.write_func = sock_write_buffer};
  int fd = sock_uuid2fd(uuid);
  lock_fd(fd);
  tmp = &fdinfo(fd).packet;
  while (*tmp)
    tmp = &(*tmp)->metadata.next;
  *tmp = packet;
  unlock_fd(fd);
  return 0;
}

/**
Returns TRUE (non 0) if there is data waiting to be written to the socket in the
user-land buffer.
*/
int sock_has_pending(intptr_t uuid) {
  return validate_uuid(uuid) == 0 && fdinfo(sock_uuid2fd(uuid)).open &&
         fdinfo(sock_uuid2fd(uuid)).packet;
}

/**
Use `sock_buffer_free` to free unused buffers that were checked-out using
`sock_buffer_checkout`.
*/
void sock_buffer_free(sock_buffer_s *buffer) {
  packet_s *packet =
      (packet_s *)((uintptr_t)(buffer) - sizeof(struct packet_metadata_s));
  sock_packet_free(packet);
}

/* *****************************************************************************
TLC - Transport Layer Callback.

Experimental
*/

/** Gets a socket hook state (a pointer to the struct). */
struct sock_rw_hook_s *sock_rw_hook_get(intptr_t uuid) {
  if (validate_uuid(uuid) || !fdinfo(sock_uuid2fd(uuid)).open ||
      ((uuid = sock_uuid2fd(uuid)),
       fdinfo(uuid).rw_hooks == &sock_default_hooks))
    return NULL;
  return fdinfo(uuid).rw_hooks;
}

/** Sets a socket hook state (a pointer to the struct). */
int sock_rw_hook_set(intptr_t uuid, sock_rw_hook_s *rw_hooks) {
  if (validate_uuid(uuid) || !fdinfo(sock_uuid2fd(uuid)).open)
    return -1;
  if (!rw_hooks->read)
    rw_hooks->read = sock_default_hooks_read;
  if (!rw_hooks->write)
    rw_hooks->write = sock_default_hooks_write;
  if (!rw_hooks->flush)
    rw_hooks->flush = sock_default_hooks_flush;
  if (!rw_hooks->on_clear)
    rw_hooks->on_clear = sock_default_hooks_on_clear;
  uuid = sock_uuid2fd(uuid);
  lock_fd(sock_uuid2fd(uuid));
  fdinfo(uuid).rw_hooks = rw_hooks;
  unlock_fd(uuid);
  return 0;
}

/* *****************************************************************************
test
*/
#ifdef DEBUG
void sock_libtest(void) {
  char request[] = "GET / HTTP/1.1\r\n"
                   "Host: www.google.com\r\n"
                   "\r\n";
  char buff[1024];
  ssize_t i_read;
  intptr_t uuid = sock_connect("www.google.com", "80");
  if (uuid == -1)
    perror("sock_connect failed"), exit(1);
  if (sock_write(uuid, request, sizeof(request) - 1) < 0)
    perror("sock_write error ");

  while ((i_read = sock_read(uuid, buff, 1024)) >= 0) {
    if (i_read == 0) { // could be we hadn't finished connecting yet.
      sock_flush(uuid);
      reschedule_thread();
    } else {
      fprintf(stderr, "\n%.*s\n\n", (int)i_read, buff);
      break;
    }
  }
  if (i_read < 0)
    perror("Error with sock_read ");
  fprintf(stderr, "done.\n");
  sock_close(uuid);
}
#endif
