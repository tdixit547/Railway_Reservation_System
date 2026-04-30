/*
 * Railway Reservation System - Client
 * OS Lab Mini Project
 *
 * Connects to the server via TCP socket.
 * Provides a menu-driven interface.
 * The available options depend on the user's role
 * (received from server after login).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9090
#define BUF_SIZE 4096
#define SERVER_IP "127.0.0.1"

/* role constants */
#define ROLE_USER  0
#define ROLE_AGENT 1
#define ROLE_ADMIN 2


/* receive full response from server (reads until "END\n" or "BYE\n" or error) */
int recv_response(int sockfd, char *buf, int bufsize) {
    int total = 0;
    while (total < bufsize - 1) {
        int n = recv(sockfd, buf + total, bufsize - 1 - total, 0);
        if (n <= 0) return n;
        total += n;
        buf[total] = '\0';

        /* check if we got a complete response */
        if (strstr(buf, "END\n") || strstr(buf, "BYE\n") ||
            strstr(buf, "AUTH_OK") || strstr(buf, "AUTH_FAIL") ||
            strstr(buf, "WELCOME"))
            break;
    }
    return total;
}


/* display menu based on role */
void show_menu(int role) {
    printf("\n========================================\n");
    printf("   Railway Reservation System - Menu\n");
    printf("========================================\n");
    printf("  1. View Available Trains\n");
    printf("  2. Book Ticket\n");
    printf("  3. View Bookings\n");
    printf("  4. Cancel Booking\n");

    if (role == ROLE_ADMIN) {
        printf("  5. Add New Train (Admin)\n");
        printf("  6. Register New User (Admin)\n");
    }

    printf("  0. Logout\n");
    printf("========================================\n");
    printf("Enter choice: ");
}


int main(void) {
    int sockfd;
    struct sockaddr_in serv_addr;
    char buf[BUF_SIZE];

    printf("=== Railway Reservation System ===\n\n");

    /* create socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr);

    /* connect to server */
    printf("Connecting to server at %s:%d...\n", SERVER_IP, PORT);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        printf("Make sure the server is running first.\n");
        exit(1);
    }

    printf("Connected!\n\n");

    /* receive welcome message */
    int n = recv_response(sockfd, buf, BUF_SIZE);
    if (n <= 0) {
        printf("Server disconnected.\n");
        close(sockfd);
        return 1;
    }
    printf("%s", buf);

    /* get login credentials */
    char username[64], password[64];
    printf("Username: ");
    scanf("%63s", username);
    printf("Password: ");
    scanf("%63s", password);

    /* send LOGIN command */
    char login_cmd[256];
    snprintf(login_cmd, sizeof(login_cmd), "LOGIN %s %s\n", username, password);
    send(sockfd, login_cmd, strlen(login_cmd), 0);

    /* receive auth response */
    memset(buf, 0, sizeof(buf));
    n = recv_response(sockfd, buf, BUF_SIZE);
    if (n <= 0) {
        printf("Server disconnected.\n");
        close(sockfd);
        return 1;
    }

    /* check if login succeeded */
    if (strstr(buf, "AUTH_FAIL")) {
        printf("Login failed! Invalid username or password.\n");
        close(sockfd);
        return 1;
    }

    /* parse role from response: "AUTH_OK <role>" */
    int role = ROLE_USER;
    char role_str[16];
    if (sscanf(buf, "AUTH_OK %15s", role_str) == 1) {
        if (strcmp(role_str, "admin") == 0) role = ROLE_ADMIN;
        else if (strcmp(role_str, "agent") == 0) role = ROLE_AGENT;
        else role = ROLE_USER;
    }

    printf("\nLogin successful! Welcome, %s (role: %s)\n", username, role_str);

    /* main menu loop */
    int choice;
    while (1) {
        show_menu(role);
        if (scanf("%d", &choice) != 1) {
            /* clear invalid input */
            while (getchar() != '\n');
            printf("Invalid input. Try again.\n");
            continue;
        }

        memset(buf, 0, sizeof(buf));

        switch (choice) {
            case 1: {
                /* View trains */
                send(sockfd, "VIEW_TRAINS\n", 12, 0);
                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                break;
            }

            case 2: {
                /* Book ticket */
                int train_id, seats;
                printf("Enter Train ID: ");
                scanf("%d", &train_id);
                printf("Enter number of seats: ");
                scanf("%d", &seats);

                char cmd[128];
                snprintf(cmd, sizeof(cmd), "BOOK %d %d\n", train_id, seats);
                send(sockfd, cmd, strlen(cmd), 0);

                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                break;
            }

            case 3: {
                /* View bookings */
                send(sockfd, "VIEW_BOOKINGS\n", 14, 0);
                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                break;
            }

            case 4: {
                /* Cancel booking */
                int bid;
                printf("Enter Booking ID to cancel: ");
                scanf("%d", &bid);

                char cmd[64];
                snprintf(cmd, sizeof(cmd), "CANCEL %d\n", bid);
                send(sockfd, cmd, strlen(cmd), 0);

                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                break;
            }

            case 5: {
                /* Add train - admin only */
                if (role != ROLE_ADMIN) {
                    printf("Access denied. Admin only.\n");
                    break;
                }
                char name[50], from[30], to[30];
                int seats, fare;
                printf("Train name (no spaces): ");
                scanf("%49s", name);
                printf("From station: ");
                scanf("%29s", from);
                printf("To station: ");
                scanf("%29s", to);
                printf("Total seats: ");
                scanf("%d", &seats);
                printf("Fare per seat: ");
                scanf("%d", &fare);

                char cmd[256];
                snprintf(cmd, sizeof(cmd), "ADD_TRAIN %s %s %s %d %d\n",
                         name, from, to, seats, fare);
                send(sockfd, cmd, strlen(cmd), 0);

                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                break;
            }

            case 6: {
                /* Register user - admin only */
                if (role != ROLE_ADMIN) {
                    printf("Access denied. Admin only.\n");
                    break;
                }
                char uname[64], pass[64], urole[16];
                printf("New username: ");
                scanf("%63s", uname);
                printf("Password: ");
                scanf("%63s", pass);
                printf("Role (admin/agent/user): ");
                scanf("%15s", urole);

                char cmd[256];
                snprintf(cmd, sizeof(cmd), "REGISTER %s %s %s\n",
                         uname, pass, urole);
                send(sockfd, cmd, strlen(cmd), 0);

                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                break;
            }

            case 0: {
                /* Logout */
                send(sockfd, "LOGOUT\n", 7, 0);
                n = recv_response(sockfd, buf, BUF_SIZE);
                if (n > 0) printf("%s", buf);
                printf("Logged out. Goodbye!\n");
                close(sockfd);
                return 0;
            }

            default:
                printf("Invalid choice. Try again.\n");
                break;
        }
    }

    close(sockfd);
    return 0;
}
