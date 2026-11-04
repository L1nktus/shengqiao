/* Private Skywalk ABI verified against Apple xnu's os_channel.h and this
 * machine's BlueTool. No hard-coded nexus IDs or kernel offsets. */
#include "native_channel.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>

typedef void *channel_t;
typedef void *ring_t;
typedef void *slot_t;
typedef struct {
    uint16_t flags, len;
    uint32_t index;
    uint64_t ext, buffer, metadata;
    uint32_t reserved[8];
} slot_prop_t;
_Static_assert(sizeof(slot_prop_t) == 64, "Skywalk slot ABI");

#define API(ret, name, args) static ret (*name) args
API(channel_t, os_channel_create, (const uuid_t, uint32_t));
API(void, os_channel_destroy, (channel_t));
API(int, os_channel_get_fd, (channel_t));
API(uint32_t, os_channel_ring_id, (channel_t, int));
API(ring_t, os_channel_tx_ring, (channel_t, uint32_t));
API(ring_t, os_channel_rx_ring, (channel_t, uint32_t));
API(slot_t, os_channel_get_next_slot, (ring_t, slot_t, slot_prop_t *));
API(void, os_channel_set_slot_properties, (ring_t, slot_t, const slot_prop_t *));
API(int, os_channel_advance_slot, (ring_t, slot_t));
API(int, os_channel_sync, (channel_t, int));
#define LOAD(name) do { *(void **)(&name) = dlsym(RTLD_DEFAULT, #name); \
    if (!name) { fprintf(stderr, "Missing API: %s\n", #name); return -1; } } while (0)

struct native_channel { channel_t handle; ring_t tx, rx; };

static int load_api(void) {
    LOAD(os_channel_create); LOAD(os_channel_destroy); LOAD(os_channel_get_fd);
    LOAD(os_channel_ring_id); LOAD(os_channel_tx_ring); LOAD(os_channel_rx_ring);
    LOAD(os_channel_get_next_slot); LOAD(os_channel_set_slot_properties);
    LOAD(os_channel_advance_slot); LOAD(os_channel_sync);
    return 0;
}

static int find_uuid(const char *wanted, uuid_t uuid) {
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("AppleConvergedIPCRTIInterface"), &iter)) return -1;
    int found = -1; io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        CFTypeRef protocol = IORegistryEntryCreateCFProperty(service,
            CFSTR("ACIPCInterfaceProtocol"), kCFAllocatorDefault, 0);
        char protocol_text[32] = {0};
        if (protocol && CFGetTypeID(protocol) == CFStringGetTypeID() &&
            CFStringGetCString(protocol, protocol_text, sizeof(protocol_text),
                kCFStringEncodingUTF8) && strcmp(protocol_text, wanted) == 0) {
            CFTypeRef value = IORegistryEntrySearchCFProperty(service,
                kIOServicePlane, CFSTR("IOSkywalkNexusUUID"),
                kCFAllocatorDefault, kIORegistryIterateRecursively);
            char text[80] = {0};
            if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
                CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8))
                found = uuid_parse(text, uuid);
            if (value) CFRelease(value);
        }
        if (protocol) CFRelease(protocol);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return found;
}

native_channel *native_channel_open(const char *protocol) {
    if (load_api()) return NULL;
    uuid_t uuid;
    if (find_uuid(protocol, uuid)) { fprintf(stderr, "%s: no nexus\n", protocol); return NULL; }
    channel_t handle = os_channel_create(uuid, 0);
    if (!handle) { fprintf(stderr, "%s: open failed: %s\n", protocol, strerror(errno)); return NULL; }
    native_channel *channel = calloc(1, sizeof(*channel));
    if (!channel) { os_channel_destroy(handle); return NULL; }
    channel->handle = handle;
    channel->tx = os_channel_tx_ring(handle, os_channel_ring_id(handle, 0));
    channel->rx = os_channel_rx_ring(handle, os_channel_ring_id(handle, 2));
    if (!channel->tx || !channel->rx) { native_channel_close(channel); return NULL; }
    printf("NATIVE_OPEN %s\n", protocol);
    return channel;
}

void native_channel_close(native_channel *channel) {
    if (!channel) return;
    os_channel_destroy(channel->handle);
    free(channel);
}

int native_channel_fd(native_channel *channel) { return os_channel_get_fd(channel->handle); }

int native_channel_send(native_channel *channel, const void *data, size_t length) {
    slot_prop_t prop = {0};
    slot_t slot = os_channel_get_next_slot(channel->tx, NULL, &prop);
    if (!slot || !prop.buffer) return -1;
    if (length > prop.len || length > UINT16_MAX) return -1;
    memcpy((void *)(uintptr_t)prop.buffer, data, length);
    prop.len = (uint16_t)length;
    os_channel_set_slot_properties(channel->tx, slot, &prop);
    if (os_channel_advance_slot(channel->tx, slot)) return -1;
    return os_channel_sync(channel->handle, 0);
}

int native_channel_receive(native_channel *channel, void *data, size_t capacity) {
    if (os_channel_sync(channel->handle, 1)) return -1;
    slot_prop_t prop = {0};
    slot_t slot = os_channel_get_next_slot(channel->rx, NULL, &prop);
    if (!slot) return 0;
    int result = -1;
    if (prop.buffer && prop.len <= capacity) {
        memcpy(data, (void *)(uintptr_t)prop.buffer, prop.len);
        result = prop.len;
    }
    if (os_channel_advance_slot(channel->rx, slot) || os_channel_sync(channel->handle, 1)) return -1;
    return result;
}
