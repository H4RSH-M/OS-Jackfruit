/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 * - command-line shape is defined
 * - key runtime data structures are defined
 * - bounded-buffer skeleton is defined
 * - supervisor / client split is outlined
 *
 * Students are expected to design:
 * - the control-plane IPC implementation
 * - container lifecycle and metadata synchronization
 * - clone + namespace setup for each container
 * - producer/consumer behavior for log buffering
 * - signal handling and graceful shutdown
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
#include <sys/resource.h>
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

// Arguments for the producer thread
typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_args_t;

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
    case CONTAINER_RUNNING: return "running";
    case CONTAINER_STOPPED: return "stopped";
    case CONTAINER_KILLED: return "killed";
    case CONTAINER_EXITED: return "exited";
    default: return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

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

/*
 * TODO: Implement producer-side insertion into the bounded buffer.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->head] = *item;
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO: Implement consumer-side removal from the bounded buffer.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->tail];
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO: Implement the logging consumer thread.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char path[PATH_MAX];

    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        
        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

void *container_log_producer(void *arg) 
{
    producer_args_t *pargs = (producer_args_t *)arg;
    log_item_t item;
    strncpy(item.container_id, pargs->container_id, CONTAINER_ID_LEN);
    
    int bytes_read;
    while ((bytes_read = read(pargs->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = bytes_read;
        if (bounded_buffer_push(pargs->buffer, &item) != 0) break;
    }
    
    close(pargs->read_fd);
    free(pargs);
    return NULL;
}

/*
 * TODO: Implement the clone child entrypoint.
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // Apply the nice value right as the container boots
    setpriority(PRIO_PROCESS, 0, config->nice_value);

    if (config->log_write_fd > 0) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }

    sethostname(config->id, strlen(config->id));

    if (chroot(config->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }
    
    if (chdir("/") != 0) {
        perror("chdir failed");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc failed");
        return 1;
    }

    char *exec_args[32]; 
    char *token = strtok(config->command, " ");
    int i = 0;
    while (token != NULL && i < 31) {
        exec_args[i++] = token;
        token = strtok(NULL, " ");
    }
    exec_args[i] = NULL; 

    execvp(exec_args[0], exec_args);

    perror("execvp failed");
    return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid, unsigned long soft_limit_bytes, unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

static void sigchld_handler(int sig) {
    (void)sig;
}

/*
 * TODO: Implement the long-running supervisor process.
 */
static int run_supervisor(const char *rootfs)
{
    (void)rootfs;
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    // 1) open /dev/container_monitor
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Could not open /dev/container_monitor. Memory limits won't be enforced.\n");
    }

    // 4) Spawn the main consumer logger thread
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    // 3) install SIGCHLD handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    // 2) create the control socket
    unlink(CONTROL_PATH); 
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }
    
    listen(ctx.server_fd, 5);
    printf("[Supervisor] Daemon running. Listening on %s...\n", CONTROL_PATH);

    // 5) enter the supervisor event loop
    while (!ctx.should_stop) {
        
        // --- ZOMBIE REAPING & UNREGISTERING ---
        int status;
        pid_t dead_pid;
        while ((dead_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            while (curr != NULL) {
                if (curr->host_pid == dead_pid && curr->state == CONTAINER_RUNNING) {
                    curr->state = CONTAINER_EXITED;
                    if (WIFEXITED(status)) curr->exit_code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) curr->exit_signal = WTERMSIG(status);
                    
                    // UNREGISTER FROM KERNEL!
                    if (ctx.monitor_fd >= 0) {
                        unregister_from_monitor(ctx.monitor_fd, curr->id, dead_pid);
                    }

                    printf("[Supervisor] Reaped container %s (PID: %d)\n", curr->id, dead_pid);
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) continue; 

        control_request_t req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        if (read(client_fd, &req, sizeof(req)) == sizeof(req)) {
            
            if (req.kind == CMD_START || req.kind == CMD_RUN) {
                child_config_t *child_cfg = malloc(sizeof(child_config_t));
                strncpy(child_cfg->id, req.container_id, CONTAINER_ID_LEN);
                strncpy(child_cfg->rootfs, req.rootfs, PATH_MAX);
                strncpy(child_cfg->command, req.command, CHILD_COMMAND_LEN);
                child_cfg->nice_value = req.nice_value; // PASS NICE VALUE

                int pipefd[2];
                if (pipe(pipefd) == 0) {
                    child_cfg->log_write_fd = pipefd[1];
                } else {
                    child_cfg->log_write_fd = -1;
                }

                char *child_stack = malloc(STACK_SIZE);
                if (!child_stack) {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message), "ERR: Out of memory");
                } else {
                    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
                    pid_t child_pid = clone(child_fn, child_stack + STACK_SIZE, flags, child_cfg);

                    if (child_pid == -1) {
                        resp.status = 1;
                        snprintf(resp.message, sizeof(resp.message), "ERR: Failed to spawn");
                        free(child_stack);
                        free(child_cfg);
                    } else {
                        if (child_cfg->log_write_fd > 0) close(pipefd[1]);

                        producer_args_t *pargs = malloc(sizeof(producer_args_t));
                        pargs->read_fd = pipefd[0];
                        strncpy(pargs->container_id, child_cfg->id, CONTAINER_ID_LEN);
                        pargs->buffer = &ctx.log_buffer;
                        
                        pthread_t ptid;
                        pthread_create(&ptid, NULL, container_log_producer, pargs);
                        pthread_detach(ptid); 

                        // REGISTER WITH KERNEL!
                        if (ctx.monitor_fd >= 0) {
                            register_with_monitor(ctx.monitor_fd, child_cfg->id, child_pid, req.soft_limit_bytes, req.hard_limit_bytes);
                        }

                        container_record_t *rec = calloc(1, sizeof(container_record_t));
                        strncpy(rec->id, child_cfg->id, CONTAINER_ID_LEN - 1);
                        rec->host_pid = child_pid;
                        rec->state = CONTAINER_RUNNING;
                        rec->soft_limit_bytes = req.soft_limit_bytes;
                        rec->hard_limit_bytes = req.hard_limit_bytes;
                        rec->started_at = time(NULL);

                        pthread_mutex_lock(&ctx.metadata_lock);
                        rec->next = ctx.containers;
                        ctx.containers = rec;
                        pthread_mutex_unlock(&ctx.metadata_lock);

                        resp.status = 0;
                        snprintf(resp.message, sizeof(resp.message), "ACK: Container %s started with PID %d", child_cfg->id, child_pid);
                    }
                }
            } 
            else if (req.kind == CMD_PS) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                char ps_output[CONTROL_MESSAGE_LEN] = "ID\tPID\tSTATE\n--------------------------------\n";
                
                while (curr != NULL && strlen(ps_output) < CONTROL_MESSAGE_LEN - 64) {
                    char line[64];
                    snprintf(line, sizeof(line), "%s\t%d\t%s\n", curr->id, curr->host_pid, state_to_string(curr->state));
                    strncat(ps_output, line, sizeof(ps_output) - strlen(ps_output) - 1);
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);

                resp.status = 0;
                strncpy(resp.message, ps_output, sizeof(resp.message) - 1);
            }
            else if (req.kind == CMD_LOGS) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
                int fd = open(path, O_RDONLY);
                if (fd < 0) {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message), "ERR: Log file not found for %s", req.container_id);
                } else {
                    char tail_buf[CONTROL_MESSAGE_LEN - 32] = {0};
                    read(fd, tail_buf, sizeof(tail_buf) - 1);
                    close(fd);
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message), "LOGS:\n%s", tail_buf);
                }
            }
            else if (req.kind == CMD_STOP && strcmp(req.container_id, "daemon") == 0) {
                ctx.should_stop = 1;
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "ACK: Shutting down supervisor daemon.");
            } else {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "ACK: Command not fully wired up yet.");
            }
        } else {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "ERR: Invalid request.");
        }

        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }

    printf("[Supervisor] Shutting down cleanly...\n");
    unlink(CONTROL_PATH);
    close(ctx.server_fd);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd); // Close device
    
    return 1;
}

/*
 * TODO: Implement the client-side control request path.
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket creation failed");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Failed to connect to supervisor");
        close(sock);
        return 1;
    }

    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        perror("Failed to write request to socket");
        close(sock);
        return 1;
    }

    if (read(sock, &resp, sizeof(resp)) > 0) {
        if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
            printf("%s\n", resp.message);
        } else {
            printf("Supervisor Response: %s\n", resp.message);
        }
    } else {
        fprintf(stderr, "No response received from supervisor.\n");
    }

    close(sock);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
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
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
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

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}