#ifndef VNC_SHARED_MEMORY_H
#define VNC_SHARED_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_MEMORY_FILE_NAME "rfb_shared"
#define RFB_SHARED_PROTOCOL_VERSION (1)

#define RFB_MAGIC "kmRF"

#define MAX_FRAMEBUFFER_WIDTH (2048)
#define MAX_FRAMEBUFFER_HEIGHT (1024)
#define MAX_FRAMEBUFFER_BYTES_PER_PIXEL (4)
#define MAX_FRAMEBUFFER_SIZE (MAX_FRAMEBUFFER_WIDTH * MAX_FRAMEBUFFER_HEIGHT * MAX_FRAMEBUFFER_BYTES_PER_PIXEL)

#define FIFO_FROM_SERVER "rfb_from_server.fifo"
#define FIFO_TO_SERVER "rfb_to_server.fifo"

#define RFB_FROM_SERVER_COMMAND_NONE (0)
#define RFB_FROM_SERVER_COMMAND_CONNECTED (1)
#define RFB_FROM_SERVER_COMMAND_DISCONNECTED (2)
#define RFB_FROM_SERVER_COMMAND_RESIZE (3)
#define RFB_FROM_SERVER_COMMAND_UPDATE (4)

typedef struct __attribute__((__packed__)) {
	uint16_t framebuffer_width;
	uint16_t framebuffer_height;
	uint8_t framebuffer_bits_per_pixel;
} RFBResizeData;

typedef struct __attribute__((__packed__)) {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
} RFBUpdateData;

typedef struct __attribute__((__packed__)) {
	uint32_t version;
	uint32_t command;

	union {
		RFBResizeData resize_data;
		RFBUpdateData update_data;
	} data;
} RFBFromServerMessage;

#define RFB_TO_SERVER_COMMAND_NONE (0)
#define RFB_TO_SERVER_COMMAND_CONNECT (101)
#define RFB_TO_SERVER_COMMAND_DISCONNECT (102)
#define RFB_TO_SERVER_COMMAND_POINTER_EVENT (103)
#define RFB_TO_SERVER_COMMAND_KEY_EVENT (104)
#define RFB_TO_SERVER_COMMAND_WATCHDOG (105)

typedef struct __attribute__((__packed__)) {
	uint32_t server_address;
	uint16_t server_port;
	uint16_t reserved;
} RFBConnectData;

typedef struct __attribute__((__packed__)) {
	uint16_t x;
	uint16_t y;
	uint16_t mask;
} RFBPointerEventData;

typedef struct __attribute__((__packed__)) {
	uint16_t scancode;
	uint16_t is_down;
} RFBKeyEventData;

typedef struct __attribute__((__packed__)) {
	uint32_t version;
	uint32_t command;

	union {
		RFBConnectData connect_data;
		RFBPointerEventData pointer_event_data;
		RFBKeyEventData key_event_data;
	} data;
} RFBToServerMessage;

#ifdef __cplusplus
}
#endif

#endif //VNC_SHARED_MEMORY_H
