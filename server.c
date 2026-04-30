/*
 * Railway Reservation System - Server
 * OS Lab Mini Project
 *
 * Concepts demonstrated:
 *  - Socket Programming (TCP client-server)
 *  - Role-Based Authorization (admin, agent, user)
 *  - File Locking (fcntl read/write locks)
 *  - Concurrency Control (pthread mutex + semaphore)
 *  - Data Consistency (mutex + file locks prevent race conditions)
 *  - IPC (pipe to logger child process)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#define PORT 9090
#define BUF_SIZE 4096
#define MAX_LINE 256
#define MAX_TRAINS 50
#define MAX_BOOKINGS 200
#define MAX_CLIENTS 5

/* Response buffer large enough for all bookings: MAX_BOOKINGS * ~100 bytes + header */
#define RESP_BUF_SIZE 32768

/* role constants */
#define ROLE_USER  0
#define ROLE_AGENT 1
#define ROLE_ADMIN 2

/* ---- Concurrency Control ---- */
/* mutex protects shared file access between threads */
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

/* semaphore limits how many clients can be served at once */
sem_t client_sem;

/* ---- IPC ---- */
/* pipe to send log messages to the logger child process */
int log_pipe[2];

/* global booking counter */
int booking_counter = 0;

/* server socket (global so we can clean up) */
int server_fd = -1;

/* struct for per-client info passed to thread */
typedef struct {
    int sockfd;
    char username[64];
    int role;
} ClientSession;

/* struct to hold a train record */
typedef struct {
    int id;
    char name[50];
    char from[30];
    char to[30];
    int seats;
    int fare;
} Train;

/* struct to hold a booking record */
typedef struct {
    int id;
    char user[64];
    int train_id;
    int seats;
    int amount;
    char status[16];
} Booking;


/* ======================================================
 * FILE LOCKING - uses fcntl for advisory locking
 * This ensures safe access when multiple threads/processes
 * try to read or write the same file.
 * ====================================================== */
void lock_file(int fd, int type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;     /* F_RDLCK, F_WRLCK, F_UNLCK */
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;        /* 0 means lock entire file */

    if (fcntl(fd, F_SETLKW, &fl) == -1) {
        perror("fcntl lock");
    }
}

void unlock_file(int fd) {
    lock_file(fd, F_UNLCK);
}


/* ======================================================
 * IPC - send a log message through pipe to logger process
 * ====================================================== */
void send_log(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[512];
    snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, msg);
    write(log_pipe[1], buf, strlen(buf));
}


/* ======================================================
 * LOGGER CHILD PROCESS
 * Reads from the pipe and writes to server.log
 * This is the IPC demonstration - parent communicates
 * with child via pipe.
 * ====================================================== */
void logger_process(void) {
    close(log_pipe[1]); /* child only reads */

    FILE *logfp = fopen("server.log", "a");
    if (!logfp) {
        perror("open server.log");
        exit(1);
    }

    char buf[512];
    ssize_t n;

    while ((n = read(log_pipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fprintf(logfp, "%s", buf);
        fflush(logfp);
    }

    fclose(logfp);
    close(log_pipe[0]);
    exit(0);
}


/* ======================================================
 * ROLE-BASED AUTHORIZATION
 * Reads users.txt, checks credentials, returns role.
 * Roles control what operations a client can perform.
 * ====================================================== */
int authenticate_user(const char *username, const char *password) {
    int fd = open("users.txt", O_RDONLY);
    if (fd < 0) {
        perror("open users.txt");
        return -1;
    }

    lock_file(fd, F_RDLCK);

    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return -1; }

    char line[MAX_LINE], u[64], p[64], r[16];
    int role = -1;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%63s %63s %15s", u, p, r) == 3) {
            if (strcmp(u, username) == 0 && strcmp(p, password) == 0) {
                if (strcmp(r, "admin") == 0) role = ROLE_ADMIN;
                else if (strcmp(r, "agent") == 0) role = ROLE_AGENT;
                else role = ROLE_USER;
                break;
            }
        }
    }

    unlock_file(fd);
    fclose(fp);   /* also closes fd */
    return role;
}


/* ======================================================
 * READ TRAINS from file into array
 * ====================================================== */
int read_trains(Train trains[], int max) {
    int fd = open("trains.txt", O_RDONLY);
    if (fd < 0) return 0;

    lock_file(fd, F_RDLCK);
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return 0; }

    char line[MAX_LINE];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%d %49s %29s %29s %d %d",
                   &trains[count].id, trains[count].name,
                   trains[count].from, trains[count].to,
                   &trains[count].seats, &trains[count].fare) == 6) {
            count++;
        }
    }

    unlock_file(fd);
    fclose(fp);
    return count;
}


/* ======================================================
 * WRITE TRAINS array back to file
 * ====================================================== */
void write_trains(Train trains[], int count) {
    int fd = open("trains.txt", O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd < 0) { perror("write trains.txt"); return; }

    lock_file(fd, F_WRLCK);
    FILE *fp = fdopen(fd, "w");

    fprintf(fp, "# id name from to seats fare\n");
    for (int i = 0; i < count; i++) {
        fprintf(fp, "%d %s %s %s %d %d\n",
                trains[i].id, trains[i].name,
                trains[i].from, trains[i].to,
                trains[i].seats, trains[i].fare);
    }

    fflush(fp);
    unlock_file(fd);
    fclose(fp);
}


/* ======================================================
 * READ BOOKINGS from file into array
 * ====================================================== */
int read_bookings(Booking bookings[], int max) {
    int fd = open("bookings.txt", O_RDONLY);
    if (fd < 0) return 0;

    lock_file(fd, F_RDLCK);
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return 0; }

    char line[MAX_LINE];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%d %63s %d %d %d %15s",
                   &bookings[count].id, bookings[count].user,
                   &bookings[count].train_id, &bookings[count].seats,
                   &bookings[count].amount, bookings[count].status) == 6) {
            count++;
        }
    }

    unlock_file(fd);
    fclose(fp);
    return count;
}


/* ======================================================
 * WRITE BOOKINGS array back to file
 * ====================================================== */
void write_bookings(Booking bookings[], int count) {
    int fd = open("bookings.txt", O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd < 0) { perror("write bookings.txt"); return; }

    lock_file(fd, F_WRLCK);
    FILE *fp = fdopen(fd, "w");

    fprintf(fp, "# booking_id username train_id seats amount status\n");
    for (int i = 0; i < count; i++) {
        fprintf(fp, "%d %s %d %d %d %s\n",
                bookings[i].id, bookings[i].user,
                bookings[i].train_id, bookings[i].seats,
                bookings[i].amount, bookings[i].status);
    }

    fflush(fp);
    unlock_file(fd);
    fclose(fp);
}


/* ======================================================
 * Initialize the booking counter by reading existing bookings
 * ====================================================== */
void init_booking_counter(void) {
    Booking bookings[MAX_BOOKINGS];
    int count = read_bookings(bookings, MAX_BOOKINGS);
    booking_counter = 0;
    for (int i = 0; i < count; i++) {
        if (bookings[i].id > booking_counter)
            booking_counter = bookings[i].id;
    }
}


/* ======================================================
 * Helper: send a string to client socket
 * ====================================================== */
void send_msg(int sockfd, const char *msg) {
    send(sockfd, msg, strlen(msg), 0);
}


/* ======================================================
 * VIEW TRAINS handler
 * ====================================================== */
void handle_view_trains(int sockfd) {
    /* lock mutex - prevents other threads from modifying file at the same time */
    pthread_mutex_lock(&data_mutex);

    Train trains[MAX_TRAINS];
    int count = read_trains(trains, MAX_TRAINS);

    char response[RESP_BUF_SIZE] = "";
    strcat(response, "\n--- Available Trains ---\n");
    strcat(response, "ID   Name               From         To           Seats  Fare\n");
    strcat(response, "---- ------------------ ------------ ------------ ------ -----\n");

    for (int i = 0; i < count; i++) {
        char row[MAX_LINE];
        snprintf(row, sizeof(row), "%-4d %-18s %-12s %-12s %-6d %d\n",
                 trains[i].id, trains[i].name, trains[i].from,
                 trains[i].to, trains[i].seats, trains[i].fare);
        strcat(response, row);
    }

    if (count == 0)
        strcat(response, "  (no trains found)\n");

    strcat(response, "END\n");
    send_msg(sockfd, response);

    pthread_mutex_unlock(&data_mutex);
}


/* ======================================================
 * BOOK TICKET handler
 * Uses mutex + file lock to ensure data consistency.
 * Only one thread can modify seats at a time, preventing
 * race conditions and lost updates.
 * ====================================================== */
void handle_book_ticket(int sockfd, const char *username, int train_id, int num_seats) {
    pthread_mutex_lock(&data_mutex);

    Train trains[MAX_TRAINS];
    int tcount = read_trains(trains, MAX_TRAINS);

    /* find the requested train */
    int idx = -1;
    for (int i = 0; i < tcount; i++) {
        if (trains[i].id == train_id) { idx = i; break; }
    }

    if (idx == -1) {
        send_msg(sockfd, "ERROR: Train not found\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    if (num_seats <= 0) {
        send_msg(sockfd, "ERROR: Invalid number of seats\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    if (trains[idx].seats < num_seats) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ERROR: Only %d seats available\nEND\n",
                 trains[idx].seats);
        send_msg(sockfd, msg);
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    /* update seat count */
    trains[idx].seats -= num_seats;
    write_trains(trains, tcount);

    /* create booking record */
    booking_counter++;
    int bid = booking_counter;
    int amount = num_seats * trains[idx].fare;

    /* append to bookings file with file locking */
    int fd = open("bookings.txt", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        lock_file(fd, F_WRLCK);
        FILE *fp = fdopen(fd, "a");
        fprintf(fp, "%d %s %d %d %d confirmed\n",
                bid, username, train_id, num_seats, amount);
        fflush(fp);
        unlock_file(fd);
        fclose(fp);
    }

    /* IPC: send log through pipe to logger child process */
    char logmsg[256];
    snprintf(logmsg, sizeof(logmsg),
             "BOOKING: id=%d user=%s train=%d seats=%d amount=%d",
             bid, username, train_id, num_seats, amount);
    send_log(logmsg);

    char response[256];
    snprintf(response, sizeof(response),
             "SUCCESS: Booked! Booking ID: %d, Amount: Rs.%d\nEND\n",
             bid, amount);
    send_msg(sockfd, response);

    pthread_mutex_unlock(&data_mutex);
}


/* ======================================================
 * VIEW BOOKINGS handler
 * Users see their own bookings only. Agents/Admins see all.
 * This is part of role-based authorization.
 * ====================================================== */
void handle_view_bookings(int sockfd, const char *username, int role) {
    pthread_mutex_lock(&data_mutex);

    Booking bookings[MAX_BOOKINGS];
    int count = read_bookings(bookings, MAX_BOOKINGS);

    char response[RESP_BUF_SIZE] = "";
    strcat(response, "\n--- Bookings ---\n");
    strcat(response, "BID  User           TrainID  Seats  Amount  Status\n");
    strcat(response, "---- -------------- -------  -----  ------  ---------\n");

    int found = 0;
    for (int i = 0; i < count; i++) {
        /* role-based: users see only their own bookings */
        if (role == ROLE_USER && strcmp(bookings[i].user, username) != 0)
            continue;

        char row[MAX_LINE];
        snprintf(row, sizeof(row), "%-4d %-14s %-7d  %-5d  %-6d  %s\n",
                 bookings[i].id, bookings[i].user, bookings[i].train_id,
                 bookings[i].seats, bookings[i].amount, bookings[i].status);
        strcat(response, row);
        found++;
    }

    if (found == 0)
        strcat(response, "  (no bookings found)\n");

    strcat(response, "END\n");
    send_msg(sockfd, response);

    pthread_mutex_unlock(&data_mutex);
}


/* ======================================================
 * CANCEL BOOKING handler
 * Users can cancel their own. Agents/admins can cancel any.
 * Demonstrates data consistency - seats are restored atomically.
 * ====================================================== */
void handle_cancel_booking(int sockfd, const char *username, int role, int booking_id) {
    pthread_mutex_lock(&data_mutex);

    Booking bookings[MAX_BOOKINGS];
    int bcount = read_bookings(bookings, MAX_BOOKINGS);

    /* find booking */
    int idx = -1;
    for (int i = 0; i < bcount; i++) {
        if (bookings[i].id == booking_id) { idx = i; break; }
    }

    if (idx == -1) {
        send_msg(sockfd, "ERROR: Booking not found\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    /* role check: users can only cancel their own bookings */
    if (role == ROLE_USER && strcmp(bookings[idx].user, username) != 0) {
        send_msg(sockfd, "ERROR: You can only cancel your own bookings\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    if (strcmp(bookings[idx].status, "cancelled") == 0) {
        send_msg(sockfd, "ERROR: Booking is already cancelled\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    /* restore seats to the train */
    Train trains[MAX_TRAINS];
    int tcount = read_trains(trains, MAX_TRAINS);
    for (int i = 0; i < tcount; i++) {
        if (trains[i].id == bookings[idx].train_id) {
            trains[i].seats += bookings[idx].seats;
            break;
        }
    }
    write_trains(trains, tcount);

    /* mark as cancelled */
    strcpy(bookings[idx].status, "cancelled");
    write_bookings(bookings, bcount);

    /* IPC log */
    char logmsg[256];
    snprintf(logmsg, sizeof(logmsg),
             "CANCEL: booking_id=%d by=%s", booking_id, username);
    send_log(logmsg);

    send_msg(sockfd, "SUCCESS: Booking cancelled and seats restored\nEND\n");
    pthread_mutex_unlock(&data_mutex);
}


/* ======================================================
 * ADD TRAIN handler (admin only)
 * Role-based: only admins can add trains.
 * ====================================================== */
void handle_add_train(int sockfd, int role, const char *username, char *args) {
    if (role != ROLE_ADMIN) {
        send_msg(sockfd, "ERROR: Only admins can add trains\nEND\n");
        return;
    }

    char name[50], from[30], to[30];
    int seats, fare;

    if (sscanf(args, "%49s %29s %29s %d %d", name, from, to, &seats, &fare) != 5) {
        send_msg(sockfd, "ERROR: Usage: ADD_TRAIN name from to seats fare\nEND\n");
        return;
    }

    pthread_mutex_lock(&data_mutex);

    Train trains[MAX_TRAINS];
    int count = read_trains(trains, MAX_TRAINS);

    if (count >= MAX_TRAINS) {
        send_msg(sockfd, "ERROR: Maximum trains reached\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    /* assign next train id */
    int max_id = 0;
    for (int i = 0; i < count; i++) {
        if (trains[i].id > max_id) max_id = trains[i].id;
    }

    trains[count].id = max_id + 1;
    strcpy(trains[count].name, name);
    strcpy(trains[count].from, from);
    strcpy(trains[count].to, to);
    trains[count].seats = seats;
    trains[count].fare = fare;
    count++;

    write_trains(trains, count);

    char logmsg[256];
    snprintf(logmsg, sizeof(logmsg), "ADD_TRAIN: id=%d name=%s by=%s",
             max_id + 1, name, username);
    send_log(logmsg);

    char response[128];
    snprintf(response, sizeof(response),
             "SUCCESS: Train added with ID %d\nEND\n", max_id + 1);
    send_msg(sockfd, response);

    pthread_mutex_unlock(&data_mutex);
}


/* ======================================================
 * REGISTER USER handler (admin only)
 * ====================================================== */
void handle_register_user(int sockfd, int role, char *args) {
    if (role != ROLE_ADMIN) {
        send_msg(sockfd, "ERROR: Only admins can register users\nEND\n");
        return;
    }

    char uname[64], pass[64], urole[16];
    if (sscanf(args, "%63s %63s %15s", uname, pass, urole) != 3) {
        send_msg(sockfd, "ERROR: Usage: REGISTER username password role(admin/agent/user)\nEND\n");
        return;
    }

    /* validate role string */
    if (strcmp(urole, "admin") != 0 && strcmp(urole, "agent") != 0 &&
        strcmp(urole, "user") != 0) {
        send_msg(sockfd, "ERROR: Role must be admin, agent, or user\nEND\n");
        return;
    }

    pthread_mutex_lock(&data_mutex);

    /* check for duplicate username before appending */
    {
        int rfd = open("users.txt", O_RDONLY);
        if (rfd >= 0) {
            lock_file(rfd, F_RDLCK);
            FILE *rfp = fdopen(rfd, "r");
            if (rfp) {
                char line[MAX_LINE], eu[64], ep[64], er[16];
                while (fgets(line, sizeof(line), rfp)) {
                    if (line[0] == '#' || line[0] == '\n') continue;
                    if (sscanf(line, "%63s %63s %15s", eu, ep, er) == 3) {
                        if (strcmp(eu, uname) == 0) {
                            unlock_file(rfd);
                            fclose(rfp);
                            send_msg(sockfd, "ERROR: Username already exists\nEND\n");
                            pthread_mutex_unlock(&data_mutex);
                            return;
                        }
                    }
                }
                unlock_file(rfd);
                fclose(rfp);
            } else {
                close(rfd);
            }
        }
    }

    int fd = open("users.txt", O_WRONLY | O_APPEND);
    if (fd < 0) {
        send_msg(sockfd, "ERROR: Cannot open users file\nEND\n");
        pthread_mutex_unlock(&data_mutex);
        return;
    }

    lock_file(fd, F_WRLCK);
    FILE *fp = fdopen(fd, "a");
    fprintf(fp, "%s %s %s\n", uname, pass, urole);
    fflush(fp);
    unlock_file(fd);
    fclose(fp);

    char logmsg[128];
    snprintf(logmsg, sizeof(logmsg), "REGISTER: user=%s role=%s", uname, urole);
    send_log(logmsg);

    send_msg(sockfd, "SUCCESS: User registered\nEND\n");
    pthread_mutex_unlock(&data_mutex);
}


/* ======================================================
 * CLIENT HANDLER THREAD
 * Each client gets its own thread. The semaphore limits
 * how many threads run at once (concurrency control).
 * ====================================================== */
void *handle_client(void *arg) {
    ClientSession *session = (ClientSession *)arg;
    int sockfd = session->sockfd;
    char buf[BUF_SIZE];
    ssize_t n;

    /* first thing: receive LOGIN command */
    send_msg(sockfd, "WELCOME: Railway Reservation System\nPlease login: LOGIN <username> <password>\n");

    n = recv(sockfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) goto cleanup;
    buf[n] = '\0';

    /* remove trailing newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    nl = strchr(buf, '\r');
    if (nl) *nl = '\0';

    /* parse LOGIN command */
    char cmd[32], username[64], password[64];
    if (sscanf(buf, "%31s %63s %63s", cmd, username, password) != 3 ||
        strcmp(cmd, "LOGIN") != 0) {
        send_msg(sockfd, "ERROR: Expected LOGIN <username> <password>\n");
        goto cleanup;
    }

    /* authenticate */
    int role = authenticate_user(username, password);
    if (role < 0) {
        send_msg(sockfd, "AUTH_FAIL: Invalid credentials\n");
        char logmsg[128];
        snprintf(logmsg, sizeof(logmsg), "LOGIN_FAIL: user=%s", username);
        send_log(logmsg);
        goto cleanup;
    }

    /* send auth success with role info */
    const char *role_str = (role == ROLE_ADMIN) ? "admin" :
                           (role == ROLE_AGENT) ? "agent" : "user";
    char auth_msg[128];
    snprintf(auth_msg, sizeof(auth_msg), "AUTH_OK %s\n", role_str);
    send_msg(sockfd, auth_msg);

    strcpy(session->username, username);
    session->role = role;

    char logmsg[128];
    snprintf(logmsg, sizeof(logmsg), "LOGIN: user=%s role=%s", username, role_str);
    send_log(logmsg);

    /* main command loop */
    while (1) {
        n = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        /* strip newline */
        nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        nl = strchr(buf, '\r');
        if (nl) *nl = '\0';

        if (strlen(buf) == 0) continue;

        /* parse command */
        char command[32];
        sscanf(buf, "%31s", command);

        if (strcmp(command, "VIEW_TRAINS") == 0) {
            handle_view_trains(sockfd);
        }
        else if (strcmp(command, "BOOK") == 0) {
            /* BOOK <train_id> <seats> */
            int tid, seats;
            if (sscanf(buf, "%*s %d %d", &tid, &seats) == 2) {
                handle_book_ticket(sockfd, username, tid, seats);
            } else {
                send_msg(sockfd, "ERROR: Usage: BOOK <train_id> <seats>\nEND\n");
            }
        }
        else if (strcmp(command, "VIEW_BOOKINGS") == 0) {
            handle_view_bookings(sockfd, username, role);
        }
        else if (strcmp(command, "CANCEL") == 0) {
            /* CANCEL <booking_id> */
            int bid;
            if (sscanf(buf, "%*s %d", &bid) == 1) {
                handle_cancel_booking(sockfd, username, role, bid);
            } else {
                send_msg(sockfd, "ERROR: Usage: CANCEL <booking_id>\nEND\n");
            }
        }
        else if (strcmp(command, "ADD_TRAIN") == 0) {
            /* ADD_TRAIN <name> <from> <to> <seats> <fare> */
            char *args = buf + strlen("ADD_TRAIN");
            while (*args == ' ') args++;
            handle_add_train(sockfd, role, username, args);
        }
        else if (strcmp(command, "REGISTER") == 0) {
            /* REGISTER <username> <password> <role> */
            char *args = buf + strlen("REGISTER");
            while (*args == ' ') args++;
            handle_register_user(sockfd, role, args);
        }
        else if (strcmp(command, "LOGOUT") == 0) {
            send_msg(sockfd, "BYE\n");
            break;
        }
        else {
            send_msg(sockfd, "ERROR: Unknown command\nEND\n");
        }
    }

cleanup:
    close(sockfd);
    printf("Client disconnected: %s\n", session->username[0] ? session->username : "(unknown)");

    /* release semaphore slot so another client can connect */
    sem_post(&client_sem);

    free(session);
    return NULL;
}


/* signal handler for clean shutdown */
void handle_sigint(int sig) {
    (void)sig;
    printf("\nShutting down server...\n");
    if (server_fd >= 0) close(server_fd);
    close(log_pipe[1]);
    exit(0);
}


/* ======================================================
 * MAIN - sets up socket, pipe, logger process, accepts clients
 * ====================================================== */
int main(void) {
    struct sockaddr_in addr;
    int opt = 1;

    printf("=== Railway Reservation System Server ===\n");

    /* initialize semaphore for limiting concurrent clients */
    sem_init(&client_sem, 0, MAX_CLIENTS);

    /* initialize booking counter from existing data */
    init_booking_counter();
    printf("Booking counter initialized: %d\n", booking_counter);

    /* create pipe for IPC with logger process */
    if (pipe(log_pipe) == -1) {
        perror("pipe");
        exit(1);
    }

    /* fork logger child process */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* child process - runs the logger */
        logger_process();
        /* never returns */
    }

    /* parent continues as server */
    close(log_pipe[0]); /* parent only writes to pipe */

    signal(SIGINT, handle_sigint);
    signal(SIGCHLD, SIG_IGN); /* prevent zombie process */

    send_log("Server started");

    /* create TCP socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Server listening on port %d\n", PORT);
    printf("Max concurrent clients: %d\n", MAX_CLIENTS);
    printf("Waiting for connections...\n\n");

    /* accept loop */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("New connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        /* Try to acquire a semaphore slot BEFORE handing off the connection.
         * If all MAX_CLIENTS slots are taken, reject the new client immediately
         * so we don't hold its socket open indefinitely. */
        if (sem_trywait(&client_sem) != 0) {
            /* server is at capacity */
            const char *full_msg = "ERROR: Server is full. Please try again later.\n";
            send(client_fd, full_msg, strlen(full_msg), 0);
            close(client_fd);
            printf("Connection rejected (server at capacity)\n");
            continue;
        }

        ClientSession *session = malloc(sizeof(ClientSession));
        memset(session, 0, sizeof(ClientSession));
        session->sockfd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, session) != 0) {
            perror("pthread_create");
            close(client_fd);
            sem_post(&client_sem);
            free(session);
            continue;
        }

        pthread_detach(tid);
    }

    close(server_fd);
    close(log_pipe[1]);
    sem_destroy(&client_sem);
    return 0;
}