#define VNC_SHARED_VERSION "0.5.0"

#include "queue.h"
#include "rfb_shared.h"
#include <rfb/rfbclient.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOGNEST_IMPLEMENTATION
#include "lognest.h"

#include "args.h"

static int connection_id = 0;
static uint8_t *shared_framebuffer = NULL;
static rfbBool handle_client_messages = TRUE;
static rfbBool keep_connected = FALSE;

typedef struct {
    RFBPointerEventData content;
    queue_handle qh;
} PointerEventsQueue;

typedef struct {
    RFBKeyEventData content;
    queue_handle qh;
} KeyEventsQueue;

static PointerEventsQueue *pointer_events_queue;
static KeyEventsQueue *key_events_queue;

static char rfb_magic[] = RFB_MAGIC;

static pthread_t connected_thread;

static FILE *message_protocol_file = NULL;

static char sz_password[100];
static rfbCredential credential;

static rfbBool resize(rfbClient *rfb_client);
static void update(rfbClient *rfb_client, const int x, const int y,
                   const int width, const int height);
static void cleanup(rfbClient *rfb_client);
static rfbCredential *get_credential(rfbClient *rfb_client, int credentialType);
static rfbBool read_magic();
static RFBToServerMessage read_message();
static rfbBool write_message(RFBFromServerMessage *message);
static void handle_commands_from_client();
static rfbBool start_connect_vnc(const RFBConnectData *connect_data);
static void *connect_vnc(void *data);
static void disconnect_vnc();

static rfbBool resize(rfbClient *rfb_client) {
    if (!shared_framebuffer) {
        char sz_file_path[300];

        sprintf(sz_file_path, "%s_%d", SHARED_MEMORY_FILE_NAME, connection_id);
        const int fd = open(sz_file_path, O_RDWR | O_CREAT, (mode_t)0664);

        if (fd != -1) {
            shared_framebuffer =
                (uint8_t *)mmap(NULL, MAX_FRAMEBUFFER_SIZE,
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

            close(fd);

            if (!shared_framebuffer) {
                return FALSE;
            }
        } else {
            lognest_error("Could not open or create shared memory file %s\n",
                          sz_file_path);
            return FALSE;
        }
    }

    rfb_client->frameBuffer = shared_framebuffer;
    rfb_client->format.redShift = 16;
    rfb_client->format.blueShift = 0;

    SetFormatAndEncodings(rfb_client);

    RFBFromServerMessage message = {0};

    message.command = RFB_FROM_SERVER_COMMAND_RESIZE;
    message.data.resize_data.framebuffer_width = rfb_client->width;
    message.data.resize_data.framebuffer_height = rfb_client->height;
    message.data.resize_data.framebuffer_bits_per_pixel =
        rfb_client->format.bitsPerPixel;

    write_message(&message);

    return TRUE;
}

static void update(rfbClient *rfb_client, const int x, const int y,
                   const int width, const int height) {
    RFBFromServerMessage message = {0};

    message.command = RFB_FROM_SERVER_COMMAND_UPDATE;
    message.data.update_data.x = x;
    message.data.update_data.y = y;
    message.data.update_data.width = width;
    message.data.update_data.height = height;

    write_message(&message);
}

static void cleanup(rfbClient *rfb_client) {
    if (rfb_client) {
        rfbClientCleanup(rfb_client);
    }
}

// ReSharper disable multiple CppParameterNeverUsed
static rfbCredential *get_credential(rfbClient *rfb_client, int credentialType) {
    return &credential;
}

// ReSharper disable once CppParameterNeverUsed
static char *get_password(rfbClient *rfb_client) {
    char *sz_temporary_password = malloc(strlen(sz_password) + 1);
    strcpy(sz_temporary_password, sz_password);
    return sz_temporary_password;
}

static rfbBool read_magic() {
    const uint8_t *magic_pointer = (uint8_t *)rfb_magic;
    uint8_t read_byte = 0;

    while (TRUE) {
        if (fread(&read_byte, 1, 1, stdin) != 1) {
            return FALSE;
        }

        if (read_byte == *magic_pointer) {
            ++magic_pointer;
            if (!*magic_pointer) {
                return TRUE;
            }
        } else {
            magic_pointer = (uint8_t *)rfb_magic;
        }
    }
}

static RFBToServerMessage read_message() {
    RFBToServerMessage message = {0};

    while (TRUE) {
        if (read_magic()) {
            uint32_t length = 0;
            if (fread(&length, 4, 1, stdin) != 1) {
                lognest_error("Could not read length from stdin\n");
                continue;
            }

            if (length < 4) {
                lognest_error("Too small length %d from RFB server\n", length);
                continue;
            }

            if (length > 200) {
                lognest_error("Too large length %d from RFB server\n", length);
                continue;
            }

            lognest_trace("Got message with length %d from RFB client", length);

            if (fread(&message.version, 4, 1, stdin) != 1) {
                lognest_error("Could not read version from stdin\n");
                continue;
            }

            switch (message.version) {
            case 1: {
                if (length != sizeof(RFBFromServerMessage)) {
                    lognest_error(
                        "Wrong length from RFB client version %d: %d vs %lu",
                        message.version, length, sizeof(RFBFromServerMessage));
                    continue;
                }

                if (fread(&(message.command),
                          sizeof(message) - sizeof(message.version), 1,
                          stdin) != 1) {
                    lognest_error("Could not read message from stdin\n");
                    continue;
                }

                return message;
            }

            default:
                lognest_error("Unknown RFB protocol version %d",
                              message.version);
            }
        }
    }
}

static rfbBool write_message(RFBFromServerMessage *message) {
    FILE *message_file = message_protocol_file ? message_protocol_file : stdout;

    if (!message->version) {
        message->version = RFB_SHARED_PROTOCOL_VERSION;
    }

    if (fwrite(&rfb_magic, 4, 1, message_file) != 1) {
        lognest_error("Could not write magic to stdout\n");
        return FALSE;
    }

    const uint32_t length = sizeof(RFBFromServerMessage);
    if (fwrite(&length, sizeof(length), 1, message_file) != 1) {
        lognest_error("Could not write length to stdout\n");
        return FALSE;
    }

    if (fwrite(message, length, 1, message_file) != 1) {
        lognest_error("Could not write %d message bytes to stdout\n", length);
        return FALSE;
    }

    fflush(message_file);

    switch (message->command) {
    case RFB_FROM_SERVER_COMMAND_CONNECTED:
        lognest_debug("Sent Connected message to client");
        break;

    case RFB_FROM_SERVER_COMMAND_DISCONNECTED:
        lognest_debug("Sent Disconnected message to client");
        break;

    case RFB_FROM_SERVER_COMMAND_RESIZE:
        lognest_debug(
            "Sent Resize(%d, %d, %d) message to client",
            (int)message->data.resize_data.framebuffer_width,
            (int)message->data.resize_data.framebuffer_height,
            (int)message->data.resize_data.framebuffer_bits_per_pixel);
        break;

    default:;
    }

    return TRUE;
}

static rfbBool start_connect_vnc(const RFBConnectData *connect_data) {
    const int result = pthread_create(&connected_thread, NULL, connect_vnc,
                                      (void *)connect_data);
    if (result != 0) {
        lognest_error("Could not start connect thread, error %d\n", result);
        return FALSE;
    }

    return TRUE;
}

static void handle_commands_from_client() {
    PointerEventsQueue pointer_events;
    KeyEventsQueue key_events;

    // ReSharper disable once CppDFALoopConditionNotUpdated
    while (handle_client_messages) {
        const RFBToServerMessage message = read_message();

        switch (message.command) {
        case RFB_TO_SERVER_COMMAND_CONNECT:
            disconnect_vnc();
            start_connect_vnc(&message.data.connect_data);
            break;

        case RFB_TO_SERVER_COMMAND_DISCONNECT:
            disconnect_vnc();
            break;

        case RFB_TO_SERVER_COMMAND_POINTER_EVENT:
            pointer_events.content = message.data.pointer_event_data;
            QUEUE_PUSH(pointer_events_queue, &pointer_events);
            break;

        case RFB_TO_SERVER_COMMAND_KEY_EVENT:
            key_events.content = message.data.key_event_data;
            QUEUE_PUSH(key_events_queue, &key_events);
            break;

        default:
            lognest_warn("Unknown command %d\n", message.command);
        }
    }
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void *connect_vnc(void *data) {
    const RFBConnectData *connect_data = data;
    char sz_host[20];
    const uint8_t *host_address_byte = (uint8_t *)&connect_data->server_address;

    sprintf(sz_host, "%d.%d.%d.%d", host_address_byte[0], host_address_byte[1],
            host_address_byte[2], host_address_byte[3]);

    lognest_debug("Connecting to %s", sz_host);
    rfbClient *rfb_client = NULL;
    keep_connected = TRUE;

    do {
        rfb_client = rfbGetClient(8, 3, 4);
        rfb_client->MallocFrameBuffer = resize;
        rfb_client->canHandleNewFBSize = TRUE;
        rfb_client->GotFrameBufferUpdate = update;
        rfb_client->GetPassword = get_password;
        rfb_client->GetCredential = get_credential;

        int argc = 2;
        char *argv[2];
        argv[0] = "KMS";
        argv[1] = sz_host;

        if (rfbInitClient(rfb_client, &argc, argv)) {
            {
                RFBFromServerMessage message = {0};
                message.command = RFB_FROM_SERVER_COMMAND_CONNECTED;
                write_message(&message);
            }

            while (keep_connected) {
                const int result = WaitForMessage(rfb_client, 500);
                if (result < 0) {
                    break;
                }
                if (!HandleRFBServerMessage(rfb_client)) {
                    break;
                }

                while (QUEUE_SIZE(pointer_events_queue)) {
                    PointerEventsQueue *pointer_events;

                    QUEUE_POP(pointer_events_queue, pointer_events);
                    if (pointer_events) {
                        SendPointerEvent(rfb_client, pointer_events->content.x,
                                         pointer_events->content.y,
                                         pointer_events->content.mask);
                        lognest_debug("Pointer at %d / %d, mask=%d",
                            pointer_events->content.x, pointer_events->content.y,
                            pointer_events->content.mask);
                    }
                }

                while (QUEUE_SIZE(key_events_queue)) {
                    KeyEventsQueue *key_events;

                    QUEUE_POP(key_events_queue, key_events);
                    if (key_events) {
                        SendKeyEvent(rfb_client, key_events->content.scancode,
                                     key_events->content.is_down);
                        lognest_debug("Key scancode=%d, down=%d",
                            key_events->content.scancode, key_events->content.is_down);
                    }
                }
            }

            {
                RFBFromServerMessage  message = {0};
                message.command = RFB_FROM_SERVER_COMMAND_DISCONNECTED;
                write_message(&message);
            }
            cleanup(rfb_client);
        } else {
            lognest_error("Could not initialize RFB client for %s", sz_host);
        }

        rfb_client = NULL;

        if (keep_connected) {
            sleep(1);
        }
    } while (keep_connected);

    RFBFromServerMessage message = {0};
    message.command = RFB_FROM_SERVER_COMMAND_DISCONNECTED;
    write_message(&message);

    return NULL;
}

static void disconnect_vnc() {
    keep_connected = FALSE;
    pthread_join(connected_thread, NULL);
}

// ReSharper disable once CppParameterNeverUsed
static void terminate(int sig) {
    handle_client_messages = FALSE;
    disconnect_vnc();

    if (message_protocol_file) {
        fclose(message_protocol_file);
    }

    exit(0);
}

static void log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    lognest_to_file(LOGNEST_FILE, "[VNC]  ", format, args);

    va_end(args);
}

static void log_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    lognest_to_file(LOGNEST_FILE, "[VNC Error]  ", format, args);

    va_end(args);
}

int main(const int argc, char **argv) {
    rfbClientLog = log;
    rfbClientErr = log_error;

    lognest_debug("Start VNC shared version %s", VNC_SHARED_VERSION);

    signal(SIGTERM, terminate);

    QUEUE_INIT(PointerEventsQueue, pointer_events_queue);
    QUEUE_INIT(KeyEventsQueue, key_events_queue);

    ArgParser* parser = ap_new_parser();
    ap_set_helptext(parser, "Usage: vnc_shared...");
    ap_set_version(parser, "0.5.0");

    ap_add_str_opt(parser, "password p", NULL);

    ap_parse(parser, argc, argv);

    const char * password = ap_get_str_value(parser, "password");
    if (password) {
        strcpy(sz_password, password);
        credential.userCredential.password = sz_password;
    }

    if (ap_count_args(parser) >= 1) {
        message_protocol_file = fopen("messages.bin", "w");

        RFBConnectData connect_data;
        const char* sz_ip_address = ap_get_arg_at_index(parser, 0);

        unsigned int c1, c2, c3, c4;
        sscanf(sz_ip_address, "%d.%d.%d.%d", &c1, &c2, &c3, &c4);

        uint8_t *address = (uint8_t *) &connect_data.server_address;
        address[0] = c1;
        address[1] = c2;
        address[2] = c3;
        address[3] = c4;

        connect_vnc(&connect_data);
    }

    handle_commands_from_client();
}