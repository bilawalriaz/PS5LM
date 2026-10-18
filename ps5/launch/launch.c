/* Starts the PS5LM title from the PC, without a controller. GPL-3.0-or-later.
 *
 * Send this to elfldr on port 9021; it reads /data/PS5LM/launch.txt, which the
 * host writes over FTP first:
 *
 *     PPSA99575                       <- title id to launch
 *     /data/homebrew/PPSA99575        <- optional: register this directory with
 *                                        ShadowMountPlus before launching
 *
 * Both are bounded, and nothing else is read. The launch ABI (the LaunchContext
 * layout and the two user-service calls) follows John Tornblom's hbldr example,
 * the same one ps5-homebrew-dev-protocol's launch payload is built from.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LAUNCH_TXT "/data/PS5LM/launch.txt"
#define SHADOW_HOST "127.0.0.1"
#define SHADOW_PORT 10101

/* Layout and zero-initialized fields follow the upstream hbldr example. */
typedef struct {
    uint32_t  structsize;
    uint32_t  user_id;
    uint32_t  app_opt;
    uint64_t  crash_report;
    uint32_t  check_flag;
    char    **argv;
} LaunchContext;
_Static_assert(sizeof(LaunchContext) == 40, "launch ABI");

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *);
int sceUserServiceTerminate(void);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceLaunchApp(const char *, char **, LaunchContext *);

/* Reads at most `cap-1` bytes of one line, without trailing CR/LF. Returns the length. */
static size_t read_line(int fd, char * out, size_t cap, size_t * offset) {
    char byte = 0;
    size_t n = 0;
    for (;;) {
        const ssize_t got = read(fd, &byte, 1);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        *offset += 1;
        if (byte == '\n') {
            break;
        }
        if (byte == '\r' || byte == ' ' || byte == '\t') {
            continue;
        }
        if (byte < 0x20 || (unsigned char) byte > 0x7e) {
            return 0; /* not a plausible title id or path */
        }
        if (n + 1 >= cap) {
            return 0;
        }
        out[n++] = byte;
    }
    out[n] = '\0';
    return n;
}

/* POST one JSON body to ShadowMountPlus's loopback API and report what it answered. */
static int shadow_post(const char * body) {
    char request[512];
    const int n = snprintf(request, sizeof(request),
                           "POST /api/v1/manual/add HTTP/1.1\r\nHost: %s:%d\r\n"
                           "Content-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                           SHADOW_HOST, SHADOW_PORT, (unsigned) strlen(body), body);
    if (n <= 0 || (size_t) n >= sizeof(request)) {
        puts("PS5LM launch: register body too long");
        return -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct timeval timeout = { 5, 0 };
    struct sockaddr_in address = { 0 };
    address.sin_len    = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port   = htons(SHADOW_PORT);
    if (inet_pton(AF_INET, SHADOW_HOST, &address.sin_addr) != 1 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        perror("ShadowMount loopback");
        close(fd);
        return -1;
    }
    if (write(fd, request, (size_t) n) != n) {
        perror("write");
        close(fd);
        return -1;
    }
    char reply[256];
    const ssize_t got = read(fd, reply, sizeof(reply) - 1);
    close(fd);
    if (got > 0) {
        reply[got] = '\0';
        char * body_start = strstr(reply, "\r\n\r\n");
        printf("PS5LM launch: ShadowMount said %s\n", body_start != NULL ? body_start + 4 : reply);
        return 0;
    }
    puts("PS5LM launch: ShadowMount gave no answer (is it running?)");
    return -1;
}

int main(void) {
    const int fd = open(LAUNCH_TXT, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        perror("open " LAUNCH_TXT);
        return 1;
    }

    char title[64] = { 0 };
    char path[256]  = { 0 };
    size_t offset = 0;
    const size_t title_len = read_line(fd, title, sizeof(title), &offset);
    if (title_len == 0) {
        puts("PS5LM launch: first line of " LAUNCH_TXT " must be a title id");
        close(fd);
        return 1;
    }
    read_line(fd, path, sizeof(path), &offset); /* optional */
    close(fd);

    if (path[0] != '\0') {
        if (strncmp(path, "/data/homebrew/", 16) != 0) {
            puts("PS5LM launch: the path to register must be under /data/homebrew/");
            return 1;
        }
        char body[384];
        snprintf(body, sizeof(body), "{\"path\":\"%s\"}", path);
        shadow_post(body); /* best effort: the title launches without it */
    }

    const int owned = sceUserServiceInitialize(NULL) == 0;
    LaunchContext context = { 0 };
    const int running = sceSystemServiceGetAppIdOfRunningBigApp();
    printf("PS5LM launch: running_big_app=%d title=%s\n", running, title);
    if (running > 0 || sceUserServiceGetForegroundUser(&context.user_id) != 0) {
        puts("PS5LM launch: a title is already running, or nobody is signed in - close it first");
        if (owned) {
            sceUserServiceTerminate();
        }
        return 1;
    }

    const int result = sceSystemServiceLaunchApp(title, NULL, &context);
    printf("PS5LM launch: launch_result=0x%x\n", (unsigned) result);
    if (owned) {
        sceUserServiceTerminate();
    }
    /* the launch request is asynchronous: the caller waits for the app's port */
    return 0;
}
