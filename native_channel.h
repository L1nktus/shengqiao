#ifndef NATIVE_CHANNEL_H
#define NATIVE_CHANNEL_H
#include <stddef.h>
typedef struct native_channel native_channel;
native_channel *native_channel_open(const char *protocol);
void native_channel_close(native_channel *channel);
int native_channel_fd(native_channel *channel);
int native_channel_send(native_channel *channel, const void *data, size_t length);
int native_channel_receive(native_channel *channel, void *data, size_t capacity);
#endif
