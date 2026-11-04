#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H
#define HAVE_ASSERT
#define HAVE_MALLOC
#define HAVE_POSIX_TIME
#define HAVE_PORTAUDIO
#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
/* A2DP and AVRCP control use Basic Mode. Do not advertise ERTM without
 * providing ERTM-configured services; Windows then chooses a broken fallback. */
#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define NVM_NUM_LINK_KEYS 4
#endif
