# Railway Reservation System

A multi-threaded, socket-based Railway Reservation System implemented in C. Developed as an Operating Systems Lab mini-project to demonstrate core OS concepts through a practical client-server application.

---

## OS Concepts Demonstrated

| Concept | Implementation |
|---|---|
| Socket Programming | TCP client-server architecture on port `9090` |
| Concurrency Control | `pthread_mutex` protects shared data; `semaphore` limits max concurrent clients |
| File Locking | `fcntl` advisory locks (`F_RDLCK` / `F_WRLCK`) for safe concurrent file access |
| Inter-Process Communication | Pipe between parent (server) and forked child (logger process) |
| Role-Based Authorization | Three roles — Admin, Agent, User — with different privilege levels |
| Multi-threading | Each client connection is handled in a separate `pthread` |
| Process Management | `fork()` to spawn a dedicated logger child process |

---

## Architecture

```
+-----------+       TCP Socket        +----------------------------+
|  Client   | <---------------------> |          Server            |
|  (client) |    port 9090            |                            |
+-----------+                         |  +---- Thread Pool ------+ |
                                      |  | pthread per client    | |
+-----------+                         |  | mutex + semaphore     | |
|  Client   | <---------------------> |  +-----------------------+ |
+-----------+                         |           | pipe           |
                                      |  +--------v-------------+  |
                                      |  |  Logger (child)       |  |
                                      |  |  writes server.log    |  |
                                      |  +-----------------------+  |
                                      +----------------------------+

Data Files: users.txt | trains.txt | bookings.txt
```

---

## Role-Based Access Control

| Action | User | Agent | Admin |
|---|:---:|:---:|:---:|
| View Trains | Yes | Yes | Yes |
| Book Tickets | Yes | Yes | Yes |
| View Own Bookings | Yes | Yes | Yes |
| View All Bookings | No | Yes | Yes |
| Cancel Own Booking | Yes | Yes | Yes |
| Cancel Any Booking | No | Yes | Yes |
| Add Trains | No | No | Yes |
| Register Users | No | No | Yes |

---

## Getting Started

### Prerequisites

- Linux environment (uses POSIX APIs: `fork`, `pipe`, `fcntl`, `pthreads`)
- GCC compiler
- Make build tool

### Build

```bash
make
```

### Run

**Terminal 1 — Start the server:**
```bash
./server
```

**Terminal 2 — Connect a client:**
```bash
./client
```

Multiple client terminals can be opened simultaneously to test concurrency.

### Clean Up

```bash
make clean
```

---

## Default Credentials

| Username | Password | Role |
|---|---|---|
| `admin` | `admin123` | Admin |
| `agent1` | `agent123` | Agent |
| `rahul` | `pass123` | User |
| `priya` | `pass456` | User |

---

## Project Structure

```
.
├── server.c        # Server — multi-threaded, handles all business logic
├── client.c        # Client — menu-driven TCP client
├── Makefile         # Build configuration
├── users.txt        # User credentials and roles
├── trains.txt       # Train data (id, name, route, seats, fare)
├── bookings.txt     # Booking records (generated at runtime)
├── Report.pdf       # Project report
└── README.md
```

---

## How It Works

1. **Server starts** — initializes mutex, semaphore (max 5 clients), creates a pipe, and forks a logger child process.
2. **Logger child** reads from the pipe and appends timestamped entries to `server.log`.
3. **Client connects** — server spawns a new thread (if a semaphore slot is available; otherwise the connection is rejected).
4. **Authentication** — credentials are checked against `users.txt` with an `fcntl` read lock.
5. **Commands** — each operation (`BOOK`, `CANCEL`, etc.) acquires the mutex and uses file locks for data consistency.
6. **Logout** — client sends `LOGOUT`, and the thread releases the semaphore slot.

---

## Testing Concurrency

Open three or more client terminals simultaneously and attempt to book the same train's last few seats. This verifies that the mutex and file locking mechanisms prevent race conditions and double-booking.

---

## License

This project was developed for academic purposes as part of the OS Lab coursework.
