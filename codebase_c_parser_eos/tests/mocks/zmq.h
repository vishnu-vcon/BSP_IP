#ifndef MOCK_ZMQ_H
#define MOCK_ZMQ_H

#include <stddef.h>

#define ZMQ_PUB 1
#define ZMQ_SUB 2
#define ZMQ_SUBSCRIBE 3
#define ZMQ_RCVTIMEO 4
#define ZMQ_SNDMORE 5
#define EAGAIN 11

void *zmq_ctx_new(void);
int zmq_ctx_destroy(void *context);
void *zmq_socket(void *context, int type);
int zmq_close(void *socket);
int zmq_bind(void *socket, const char *endpoint);
int zmq_connect(void *socket, const char *endpoint);
int zmq_setsockopt(void *socket, int option, const void *optval, size_t optvallen);
int zmq_send(void *socket, const void *buf, size_t len, int flags);
int zmq_recv(void *socket, void *buf, size_t len, int flags);
int zmq_errno(void);
const char *zmq_strerror(int errnum);

#endif
