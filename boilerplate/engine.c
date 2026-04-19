/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ----------------------------------------------------------------
 * Bounded Buffer
 * ---------------------------------------------------------------- */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while full, but bail if shutting down */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while empty; wake up if shutting down so we can drain */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0) {
        /* Shutting down and nothing left */
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ----------------------------------------------------------------
 * Logging consumer thread
 * ---------------------------------------------------------------- */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break; /* shutdown + empty */

        /* Build log file path: logs/<container_id>.log */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }
        ssize_t written = 0;
        while (written < (ssize_t)item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) break;
            written += n;
        }
        close(fd);
    }

    return NULL;
}

/* ----------------------------------------------------------------
 * Log reader thread: reads container stdout/stderr pipe and pushes
 * chunks into the bounded buffer.
 * ---------------------------------------------------------------- */

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} log_reader_arg_t;

static void *log_reader_thread(void *arg)
{
    log_reader_arg_t *lra = (log_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, lra->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(lra->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(lra->log_buffer, &item);
        memset(item.data, 0, LOG_CHUNK_SIZE);
    }

    close(lra->read_fd);
    free(lra);
    return NULL;
}

/* ----------------------------------------------------------------
 * Container child entrypoint (runs inside clone()'d process)
 * ---------------------------------------------------------------- */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Redirect stdout and stderr to the log pipe write end */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* Set hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child_fn: chdir");
        return 1;
    }

    /* Mount /proc so the container has process visibility */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal — /proc may already exist */
    }

    /* Execute the container command */
    char *argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv);

    /* execv only returns on error */
    perror("child_fn: execv");
    return 1;
}

/* ----------------------------------------------------------------
 * Monitor helpers (already mostly provided)
 * ---------------------------------------------------------------- */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ----------------------------------------------------------------
 * Supervisor helpers
 * ---------------------------------------------------------------- */

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static void add_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next = ctx->containers;
    ctx->containers = rec;
}

/* Launch a new container; called from the supervisor event loop */
static int do_start_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    /* Create log pipe */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    /* Allocate child stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    /* Build child config */
    child_config_t *cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) {
        free(stack);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1]; /* child writes here */

    /* Clone with new PID, UTS, and mount namespaces */
    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    /* Parent: close the write end of the pipe */
    close(pipefd[1]);
    free(stack);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        free(cfg);
        return -1;
    }

    /* Record container metadata */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    if (!rec) {
        close(pipefd[0]);
        free(cfg);
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    add_container(ctx, rec);
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    /* Spawn log reader thread */
    log_reader_arg_t *lra = calloc(1, sizeof(log_reader_arg_t));
    if (lra) {
        lra->read_fd = pipefd[0];
        strncpy(lra->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        lra->log_buffer = &ctx->log_buffer;
        pthread_t tid;
        pthread_create(&tid, NULL, log_reader_thread, lra);
        pthread_detach(tid);
    } else {
        close(pipefd[0]);
    }

    free(cfg);
    fprintf(stdout, "Container '%s' started with pid %d\n", req->container_id, pid);
    return 0;
}

/* Reap any exited children and update metadata */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->state = (WTERMSIG(status) == SIGKILL)
                                   ? CONTAINER_KILLED
                                   : CONTAINER_STOPPED;
                    c->exit_signal = WTERMSIG(status);
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ----------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------- */

static volatile sig_atomic_t g_sigchld_flag = 0;
static volatile sig_atomic_t g_shutdown_flag = 0;

static void handle_sigchld(int sig)
{
    (void)sig;
    g_sigchld_flag = 1;
}

static void handle_shutdown(int sig)
{
    (void)sig;
    g_shutdown_flag = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa_chld, sa_term;

    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_shutdown;
    sa_term.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
}

/* ----------------------------------------------------------------
 * Supervisor event loop — handles one client connection
 * ---------------------------------------------------------------- */

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    control_response_t resp;
    char buf[4096];
    int len = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    len += snprintf(buf + len, sizeof(buf) - len,
                    "%-20s %-8s %-10s %-10s\n", "ID", "PID", "STATE", "EXIT");
    while (c && len < (int)sizeof(buf) - 128) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        "%-20s %-8d %-10s %-10d\n",
                        c->id, c->host_pid, state_to_string(c->state), c->exit_code);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    memset(&resp, 0, sizeof(resp));
    resp.status = 0;
    strncpy(resp.message, buf, sizeof(resp.message) - 1);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd, const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    char log_path[PATH_MAX];
    if (c)
        strncpy(log_path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Container '%s' not found", req->container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    /* Read last CONTROL_MESSAGE_LEN bytes from log file */
    int fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Log not available yet");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, resp.message, sizeof(resp.message) - 1);
    if (n < 0) n = 0;
    resp.message[n] = '\0';
    resp.status = 0;
    close(fd);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd, const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    pid_t pid = c ? c->host_pid : -1;
    if (c) c->state = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pid < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Container '%s' not found", req->container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    /* Graceful stop: SIGTERM first, then SIGKILL after 3 seconds */
    kill(pid, SIGTERM);
    struct timespec ts = {3, 0};
    nanosleep(&ts, NULL);
    kill(pid, SIGKILL); /* no-op if already exited */

    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message), "Stopped '%s'", req->container_id);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        close(client_fd);
        return;
    }

    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {
    case CMD_START:
    case CMD_RUN: {
        int rc = do_start_container(ctx, &req);
        resp.status = rc;
        if (rc == 0)
            snprintf(resp.message, sizeof(resp.message), "Started '%s'", req.container_id);
        else
            snprintf(resp.message, sizeof(resp.message), "Failed to start '%s'", req.container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
    case CMD_PS:
        handle_ps(ctx, client_fd);
        break;
    case CMD_LOGS:
        handle_logs(ctx, client_fd, &req);
        break;
    case CMD_STOP:
        handle_stop(ctx, client_fd, &req);
        break;
    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    close(client_fd);
}

/* ----------------------------------------------------------------
 * Supervisor: main entry point
 * ---------------------------------------------------------------- */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    /* 1) Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor (%s). Memory monitoring disabled.\n",
                strerror(errno));

    /* 2) Create log directory */
    mkdir(LOG_DIR, 0755);

    /* 3) Create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1; }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); return 1; }

    /* 4) Install signal handlers */
    install_signal_handlers();

    /* 5) Spawn logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { errno = rc; perror("pthread_create logger"); return 1; }

    fprintf(stdout, "Supervisor listening on %s\n", CONTROL_PATH);
    fflush(stdout);

    /* 6) Event loop */
    while (!g_shutdown_flag) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            reap_children(&ctx);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = {1, 0};

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (sel > 0 && FD_ISSET(ctx.server_fd, &rfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0)
                handle_client(&ctx, client_fd);
        }
    }

    fprintf(stdout, "\nSupervisor shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait a moment then force-kill */
    sleep(2);
    reap_children(&ctx);

    /* Shutdown logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        if (ctx.monitor_fd >= 0)
            unregister_from_monitor(ctx.monitor_fd, c->id, c->host_pid);
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Cleanup */
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stdout, "Supervisor exited cleanly.\n");
    return 0;
}

/* ----------------------------------------------------------------
 * Client-side: send a control request and print the response
 * ---------------------------------------------------------------- */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    control_response_t resp;
    ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    close(fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Incomplete response from supervisor\n");
        return 1;
    }

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

/* ----------------------------------------------------------------
 * CLI commands
 * ---------------------------------------------------------------- */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
