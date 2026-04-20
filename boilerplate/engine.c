/*
 * engine.c — Container runtime supervisor 
 *
 * Usage:
 *   engine supervisor <base-rootfs>
 *   engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]
 *   engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]
 *   engine ps
 *   engine logs  <id>
 *   engine stop  <id>
 *
 * Build:
 *   gcc -o engine engine.c -lpthread -Wall -Wextra
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>

/* ─────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────── */
#define MAX_CONTAINERS   16
#define LOG_BUF_SIZE     256
#define LOG_LINE_MAX     1024
#define SOCKET_PATH      "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine-logs"
#define STACK_SIZE       (1024 * 1024)

/* ─────────────────────────────────────────────
 * Container state
 * ───────────────────────────────────────────── */
typedef enum {
    STATE_EMPTY = 0,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED
} ContainerState;

/* ─────────────────────────────────────────────
 * Bounded log ring buffer (Task 3)
 * ───────────────────────────────────────────── */
typedef struct {
    char            lines[LOG_BUF_SIZE][LOG_LINE_MAX];
    int             head;
    int             tail;
    int             count;
    int             done;
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} LogBuffer;

/* ─────────────────────────────────────────────
 * Per-container metadata (Task 1)
 * ───────────────────────────────────────────── */
typedef struct {
    char            id[64];
    pid_t           host_pid;
    time_t          start_time;
    ContainerState  state;
    int             soft_mib;
    int             hard_mib;
    int             nice_val;
    char            log_path[256];
    int             exit_status;
    int             stop_requested;

    int             pipe_read_fd;
    LogBuffer      *log_buf;
    pthread_t       producer_tid;
    pthread_t       consumer_tid;

    pthread_mutex_t lock;
} Container;

/* ─────────────────────────────────────────────
 * Child args — includes pipe write fd (THE FIX)
 * ───────────────────────────────────────────── */
typedef struct {
    char *rootfs;
    char *command;
    char **argv;
    int   argc;
    int   pipe_write_fd;   /* child redirects stdout/stderr here */
} ChildArgs;

/* ─────────────────────────────────────────────
 * Globals
 * ───────────────────────────────────────────── */
static Container       containers[MAX_CONTAINERS];
static pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    supervisor_running = 1;
static int             sigchld_pipe[2];

/* ─────────────────────────────────────────────
 * Helpers
 * ───────────────────────────────────────────── */
static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_EMPTY:             return "empty";
        case STATE_STARTING:          return "starting";
        case STATE_RUNNING:           return "running";
        case STATE_STOPPED:           return "stopped";
        case STATE_KILLED:            return "killed";
        case STATE_HARD_LIMIT_KILLED: return "hard_limit_killed";
        default:                      return "unknown";
    }
}

static Container *find_container(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != STATE_EMPTY &&
            strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

static Container *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == STATE_EMPTY)
            return &containers[i];
    return NULL;
}

/* ─────────────────────────────────────────────
 * Log buffer (Task 3)
 * ───────────────────────────────────────────── */
static LogBuffer *log_buf_create(void) {
    LogBuffer *b = calloc(1, sizeof(LogBuffer));
    if (!b) return NULL;
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_full,  NULL);
    pthread_cond_init(&b->not_empty, NULL);
    return b;
}

static void log_buf_push(LogBuffer *b, const char *line) {
    pthread_mutex_lock(&b->lock);
    while (b->count == LOG_BUF_SIZE)
        pthread_cond_wait(&b->not_full, &b->lock);

    strncpy(b->lines[b->tail], line, LOG_LINE_MAX - 1);
    b->lines[b->tail][LOG_LINE_MAX - 1] = '\0';
    b->tail = (b->tail + 1) % LOG_BUF_SIZE;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}

static int log_buf_pop(LogBuffer *b, char *out) {
    pthread_mutex_lock(&b->lock);
    while (b->count == 0 && !b->done)
        pthread_cond_wait(&b->not_empty, &b->lock);

    if (b->count == 0 && b->done) {
        pthread_mutex_unlock(&b->lock);
        return -1;
    }

    strncpy(out, b->lines[b->head], LOG_LINE_MAX);
    b->head = (b->head + 1) % LOG_BUF_SIZE;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

static void log_buf_set_done(LogBuffer *b) {
    pthread_mutex_lock(&b->lock);
    b->done = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}

/* ─────────────────────────────────────────────
 * Producer thread: pipe → ring buffer (Task 3)
 * ───────────────────────────────────────────── */
typedef struct {
    int        fd;
    LogBuffer *buf;
    char       container_id[64];
} ProducerArgs;

static void *producer_thread(void *arg) {
    ProducerArgs *a = (ProducerArgs *)arg;
    char line[LOG_LINE_MAX];
    int  line_len = 0;
    char ch;
    ssize_t n;

    fprintf(stderr, "[producer:%s] started, reading from pipe fd=%d\n",
            a->container_id, a->fd);

    while ((n = read(a->fd, &ch, 1)) > 0) {
        if (line_len < LOG_LINE_MAX - 1)
            line[line_len++] = ch;

        if (ch == '\n' || line_len == LOG_LINE_MAX - 1) {
            line[line_len] = '\0';
            fprintf(stderr, "[producer:%s] pushing: %s", a->container_id, line);
            log_buf_push(a->buf, line);
            line_len = 0;
        }
    }
    /* flush partial line */
    if (line_len > 0) {
        line[line_len] = '\0';
        log_buf_push(a->buf, line);
    }

    fprintf(stderr, "[producer:%s] pipe closed, done\n", a->container_id);
    close(a->fd);
    log_buf_set_done(a->buf);
    free(a);
    return NULL;
}

/* ─────────────────────────────────────────────
 * Consumer thread: ring buffer → log file (Task 3)
 * ───────────────────────────────────────────── */
typedef struct {
    LogBuffer *buf;
    char       log_path[256];
    char       container_id[64];
} ConsumerArgs;

static void *consumer_thread(void *arg) {
    ConsumerArgs *a = (ConsumerArgs *)arg;
    char line[LOG_LINE_MAX];

    fprintf(stderr, "[consumer:%s] started, writing to %s\n",
            a->container_id, a->log_path);

    FILE *f = fopen(a->log_path, "a");
    if (!f) {
        perror("consumer: fopen");
        free(a);
        return NULL;
    }

    while (log_buf_pop(a->buf, line) == 0) {
        fprintf(stderr, "[consumer:%s] writing line\n", a->container_id);
        fputs(line, f);
        fflush(f);
    }

    fprintf(stderr, "[consumer:%s] buffer drained, exiting\n", a->container_id);
    fclose(f);
    free(a);
    return NULL;
}

/* ─────────────────────────────────────────────
 * Child process (runs inside container) (Task 1)
 * ───────────────────────────────────────────── */
static int container_child(void *arg) {
    ChildArgs *a = (ChildArgs *)arg;

    /* KEY FIX: redirect stdout and stderr into the pipe */
    if (dup2(a->pipe_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(a->pipe_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(a->pipe_write_fd);

    /* filesystem isolation */
    if (chroot(a->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/") < 0)        { perror("chdir");  return 1; }

    /* mount /proc so ps works inside container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");  /* non-fatal */

    execvp(a->command, a->argv);
    perror("execvp");
    return 127;
}

/* ─────────────────────────────────────────────
 * Launch a container (Task 1)
 * ───────────────────────────────────────────── */
static pid_t launch_container(Container *c, const char *rootfs,
                               const char *command, char **argv, int argc) {
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc stack"); return -1; }
    char *stack_top = stack + STACK_SIZE;

    ChildArgs *ca     = malloc(sizeof(ChildArgs));
    ca->rootfs        = (char *)rootfs;
    ca->command       = (char *)command;
    ca->argv          = argv;
    ca->argc          = argc;
    ca->pipe_write_fd = pipefd[1];   /* pass write end to child */

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_child, stack_top, flags, ca);

    /* parent always closes write end after clone */
    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        free(stack);
        free(ca);
        close(pipefd[0]);
        return -1;
    }

    /* set up logging pipeline */
    c->log_buf      = log_buf_create();
    c->pipe_read_fd = pipefd[0];

    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), LOG_DIR "/%s.log", c->id);

    /* producer thread */
    ProducerArgs *pa = malloc(sizeof(ProducerArgs));
    pa->fd  = pipefd[0];
    pa->buf = c->log_buf;
    strncpy(pa->container_id, c->id, sizeof(pa->container_id));
    pthread_create(&c->producer_tid, NULL, producer_thread, pa);

    /* consumer thread */
    ConsumerArgs *ca2 = malloc(sizeof(ConsumerArgs));
    ca2->buf = c->log_buf;
    strncpy(ca2->log_path, c->log_path, sizeof(ca2->log_path));
    strncpy(ca2->container_id, c->id, sizeof(ca2->container_id));
    pthread_create(&c->consumer_tid, NULL, consumer_thread, ca2);

    free(stack);
    return pid;
}

/* ─────────────────────────────────────────────
 * SIGCHLD handler (Task 2)
 * ───────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    char b = 1;
    write(sigchld_pipe[1], &b, 1);
}

static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        fprintf(stderr, "[supervisor] reaped pid=%d\n", pid);
        pthread_mutex_lock(&containers_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            Container *c = &containers[i];
            if (c->host_pid != pid) continue;
            pthread_mutex_lock(&c->lock);
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                && !c->stop_requested)
                c->state = STATE_HARD_LIMIT_KILLED;
            else if (c->stop_requested)
                c->state = STATE_STOPPED;
            else
                c->state = STATE_KILLED;
            c->exit_status = status;
            pthread_mutex_unlock(&c->lock);
            break;
        }
        pthread_mutex_unlock(&containers_lock);
    }
}

/* ─────────────────────────────────────────────
 * Command handlers (Task 2)
 * ───────────────────────────────────────────── */
static void cmd_start(int fd, char **tokens, int ntok) {
    if (ntok < 4) {
        dprintf(fd, "ERROR: usage: start <id> <rootfs> <command>\n");
        return;
    }
    const char *id      = tokens[1];
    const char *rootfs  = tokens[2];
    const char *command = tokens[3];

    int soft_mib = 40, hard_mib = 64, nice_val = 0;
    for (int i = 4; i < ntok - 1; i++) {
        if      (strcmp(tokens[i], "--soft-mib") == 0) soft_mib = atoi(tokens[++i]);
        else if (strcmp(tokens[i], "--hard-mib") == 0) hard_mib = atoi(tokens[++i]);
        else if (strcmp(tokens[i], "--nice")     == 0) nice_val = atoi(tokens[++i]);
    }

    pthread_mutex_lock(&containers_lock);
    if (find_container(id)) {
        pthread_mutex_unlock(&containers_lock);
        dprintf(fd, "ERROR: container '%s' already exists\n", id);
        return;
    }
    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        dprintf(fd, "ERROR: max containers reached\n");
        return;
    }

    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id) - 1);
    c->soft_mib   = soft_mib;
    c->hard_mib   = hard_mib;
    c->nice_val   = nice_val;
    c->state      = STATE_STARTING;
    c->start_time = time(NULL);
    pthread_mutex_init(&c->lock, NULL);

    /* build argv */
    char *argv[64];
    argv[0] = (char *)command;
    int argc = 1;
    for (int i = 4; i < ntok && argc < 63; i++) {
        if (strcmp(tokens[i], "--soft-mib") == 0 ||
            strcmp(tokens[i], "--hard-mib") == 0 ||
            strcmp(tokens[i], "--nice")     == 0) { i++; continue; }
        argv[argc++] = tokens[i];
    }
    argv[argc] = NULL;

    pid_t pid = launch_container(c, rootfs, command, argv, argc);
    if (pid < 0) {
        c->state = STATE_EMPTY;
        pthread_mutex_unlock(&containers_lock);
        dprintf(fd, "ERROR: failed to launch container\n");
        return;
    }
    c->host_pid = pid;
    c->state    = STATE_RUNNING;
    pthread_mutex_unlock(&containers_lock);

    dprintf(fd, "OK: started container '%s' pid=%d\n", id, pid);
}

static void cmd_ps(int fd) {
    pthread_mutex_lock(&containers_lock);
    dprintf(fd, "%-16s %-8s %-20s %-20s %-8s %-8s %s\n",
            "ID", "PID", "STARTED", "STATE", "SOFT_MIB", "HARD_MIB", "LOG");
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (c->state == STATE_EMPTY) continue;
        char tbuf[32];
        struct tm *tm = localtime(&c->start_time);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
        dprintf(fd, "%-16s %-8d %-20s %-20s %-8d %-8d %s\n",
                c->id, c->host_pid, tbuf, state_str(c->state),
                c->soft_mib, c->hard_mib, c->log_path);
    }
    pthread_mutex_unlock(&containers_lock);
}

static void cmd_logs(int fd, const char *id) {
    pthread_mutex_lock(&containers_lock);
    Container *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        dprintf(fd, "ERROR: no container '%s'\n", id);
        return;
    }
    char path[256];
    strncpy(path, c->log_path, sizeof(path));
    pthread_mutex_unlock(&containers_lock);

    FILE *f = fopen(path, "r");
    if (!f) {
        dprintf(fd, "ERROR: log file not found: %s\n", path);
        return;
    }
    char line[LOG_LINE_MAX];
    while (fgets(line, sizeof(line), f))
        dprintf(fd, "%s", line);
    fclose(f);
}

static void cmd_stop(int fd, const char *id) {
    pthread_mutex_lock(&containers_lock);
    Container *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        dprintf(fd, "ERROR: no container '%s'\n", id);
        return;
    }
    if (c->state != STATE_RUNNING) {
        pthread_mutex_unlock(&containers_lock);
        dprintf(fd, "ERROR: container '%s' is not running\n", id);
        return;
    }
    pthread_mutex_lock(&c->lock);
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&c->lock);
    pthread_mutex_unlock(&containers_lock);

    kill(pid, SIGTERM);
    dprintf(fd, "OK: sent SIGTERM to container '%s' (pid=%d)\n", id, pid);
}

/* ─────────────────────────────────────────────
 * Handle one CLI connection (Task 2)
 * ───────────────────────────────────────────── */
static void handle_client(int client_fd) {
    char buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    char *tokens[64];
    int ntok = 0;
    char *p = buf;
    while (*p && ntok < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        tokens[ntok++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    if (ntok == 0) return;

    fprintf(stderr, "[supervisor] received command: %s\n", tokens[0]);

    const char *verb = tokens[0];
    if (strcmp(verb, "start") == 0) {
        cmd_start(client_fd, tokens, ntok);
    } else if (strcmp(verb, "run") == 0) {
        cmd_start(client_fd, tokens, ntok);
        if (ntok >= 2) {
            const char *id = tokens[1];
            while (1) {
                usleep(200000);
                pthread_mutex_lock(&containers_lock);
                Container *c = find_container(id);
                if (!c || c->state == STATE_STOPPED ||
                    c->state == STATE_KILLED ||
                    c->state == STATE_HARD_LIMIT_KILLED) {
                    int es = c ? c->exit_status : -1;
                    pthread_mutex_unlock(&containers_lock);
                    dprintf(client_fd, "EXITED: status=%d\n", es);
                    break;
                }
                pthread_mutex_unlock(&containers_lock);
            }
        }
    } else if (strcmp(verb, "ps")   == 0) {
        cmd_ps(client_fd);
    } else if (strcmp(verb, "logs") == 0 && ntok >= 2) {
        cmd_logs(client_fd, tokens[1]);
    } else if (strcmp(verb, "stop") == 0 && ntok >= 2) {
        cmd_stop(client_fd, tokens[1]);
    } else {
        dprintf(client_fd, "ERROR: unknown command '%s'\n", verb);
    }
}

/* ─────────────────────────────────────────────
 * Accept thread (Task 2)
 * ───────────────────────────────────────────── */
static void *accept_thread(void *arg) {
    int server_fd = *(int *)arg;
    while (supervisor_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(client_fd);
        close(client_fd);
    }
    return NULL;
}

/* ─────────────────────────────────────────────
 * Shutdown (Task 2)
 * ───────────────────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;
    supervisor_running = 0;
}

static void shutdown_supervisor(void) {
    fprintf(stderr, "[supervisor] shutting down...\n");
    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (c->state == STATE_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&containers_lock);

    sleep(1);

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (c->state == STATE_EMPTY) continue;
        if (c->producer_tid) pthread_join(c->producer_tid, NULL);
        if (c->consumer_tid) pthread_join(c->consumer_tid, NULL);
    }
    pthread_mutex_unlock(&containers_lock);

    unlink(SOCKET_PATH);
    fprintf(stderr, "[supervisor] done.\n");
}

/* ─────────────────────────────────────────────
 * Supervisor main loop (Task 1 + 2)
 * ───────────────────────────────────────────── */
static void run_supervisor(const char *base_rootfs) {
    fprintf(stderr, "[supervisor] starting (rootfs=%s)\n", base_rootfs);

    if (pipe(sigchld_pipe) < 0) { perror("pipe sigchld"); exit(1); }
    fcntl(sigchld_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sigchld_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(server_fd,   (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind");   exit(1); }
    if (listen(server_fd, 8) < 0)                                       { perror("listen"); exit(1); }
    fprintf(stderr, "[supervisor] listening on %s\n", SOCKET_PATH);

    pthread_t atid;
    pthread_create(&atid, NULL, accept_thread, &server_fd);

    while (supervisor_running) {
        char tmp[64];
        ssize_t r = read(sigchld_pipe[0], tmp, sizeof(tmp));
        (void)r;
        reap_children();
        usleep(100000);
    }

    close(server_fd);
    pthread_join(atid, NULL);
    shutdown_supervisor();
}

/* ─────────────────────────────────────────────
 * CLI client (Task 2)
 * ───────────────────────────────────────────── */
static void run_cli(int argc, char **argv) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s\n"
                        "Is 'engine supervisor' running?\n", SOCKET_PATH);
        exit(1);
    }

    char buf[4096] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(buf, " ");
        strncat(buf, argv[i], sizeof(buf) - strlen(buf) - 2);
    }
    strcat(buf, "\n");

    write(fd, buf, strlen(buf));
    shutdown(fd, SHUT_WR);

    char resp[4096];
    ssize_t n;
    while ((n = read(fd, resp, sizeof(resp) - 1)) > 0) {
        resp[n] = '\0';
        printf("%s", resp);
    }
    close(fd);
}

/* ─────────────────────────────────────────────
 * Entry point
 * ───────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  engine ps\n"
            "  engine logs  <id>\n"
            "  engine stop  <id>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: engine supervisor <base-rootfs>\n");
            return 1;
        }
        run_supervisor(argv[2]);
    } else {
        run_cli(argc, argv);
    }
    return 0;
}
