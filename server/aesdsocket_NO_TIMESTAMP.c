#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/stat.h>
#include <signal.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define BUFFER_SIZE 1024

// Global variables for synchronization and cleanup
int server_socket_fd = -1;
bool signal_caught = false;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex to protect file writes

// Structure to pass arguments to the thread
struct thread_data_t {
    int client_fd;
    char client_ip[INET6_ADDRSTRLEN];
    bool thread_complete;
    pthread_mutex_t *mutex;
};

// Structure for the linked list node containing thread info
struct slist_data_s {
    pthread_t thread_id;
    struct thread_data_t *thread_params;
    SLIST_ENTRY(slist_data_s) entries;
};

// Define the head of the singly linked list
SLIST_HEAD(slisthead, slist_data_s) head;

// Signal handler for SIGINT and SIGTERM
void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        signal_caught = true;
        
        // Shutdown server socket to break out of blocking accept() call
        if (server_socket_fd != -1) {
            shutdown(server_socket_fd, SHUT_RDWR);
        }
    }
}

// Thread function to handle client connection
void *thread_func(void *thread_param) {
    struct thread_data_t *data = (struct thread_data_t *)thread_param;
    char recv_buf[BUFFER_SIZE];
    ssize_t bytes_received;
    char *packet_buffer = NULL;
    size_t current_packet_size = 0;
    bool packet_complete = false;

    // --- RECEIVE DATA ---
    // Loop to read data until a newline is found or connection closes
    while ((bytes_received = recv(data->client_fd, recv_buf, BUFFER_SIZE, 0)) > 0) {
        
        // Dynamically reallocate buffer to hold incoming chunks
        char *temp = realloc(packet_buffer, current_packet_size + bytes_received);
        if (temp == NULL) {
            syslog(LOG_ERR, "Malloc failed");
            if (packet_buffer) free(packet_buffer);
            packet_buffer = NULL;
            break; 
        }
        packet_buffer = temp;
        
        // Copy new data into the buffer
        memcpy(packet_buffer + current_packet_size, recv_buf, bytes_received);
        current_packet_size += bytes_received;

        // Check if the received chunk contains a newline character
        if (memchr(recv_buf, '\n', bytes_received) != NULL) {
            packet_complete = true;
            break;
        }
    }

    if (packet_complete && packet_buffer != NULL) {
        // --- CRITICAL SECTION START ---
        // Lock the mutex before writing to the shared file to prevent interleaving
        if (pthread_mutex_lock(data->mutex) != 0) {
            syslog(LOG_ERR, "Mutex lock failed");
        } else {
            // Open file for appending (create if it doesn't exist)
            int file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (file_fd == -1) {
                syslog(LOG_ERR, "Could not open data file: %s", strerror(errno));
            } else {
                // Write the accumulated packet to the file
                if (write(file_fd, packet_buffer, current_packet_size) == -1) {
                    syslog(LOG_ERR, "File write failed: %s", strerror(errno));
                }
                close(file_fd);
            }
            
            // --- READ AND SEND BACK ---
            // While we still hold the lock, read the entire file and send it back.
            // This ensures the client receives the file state exactly as it was after their write.
            
            file_fd = open(DATA_FILE, O_RDONLY);
            if (file_fd != -1) {
                char send_buf[BUFFER_SIZE];
                ssize_t bytes_read;
                // Read file in chunks and send to client
                while ((bytes_read = read(file_fd, send_buf, BUFFER_SIZE)) > 0) {
                    if (send(data->client_fd, send_buf, bytes_read, 0) == -1) {
                        syslog(LOG_ERR, "Send failed: %s", strerror(errno));
                        break;
                    }
                }
                close(file_fd);
            }
            
            // Unlock the mutex
            pthread_mutex_unlock(data->mutex);
            // --- CRITICAL SECTION END ---
        }
    }

    // Cleanup resources for this thread
    if (packet_buffer) free(packet_buffer);
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", data->client_ip);
    
    // Mark thread as complete so main loop can join it
    data->thread_complete = true;
    
    return NULL;
}

int main(int argc, char *argv[]) {
    struct addrinfo hints, *res;
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size;
    int status;
    bool daemon_mode = false;

    // Check for daemon argument (-d)
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }
    
    // Initialize system logging
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Register signal handlers for graceful exit
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error registering signal handlers: %s", strerror(errno));
        return -1;
    }

    // Initialize list head
    SLIST_INIT(&head);

    // Setup socket address structure
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;     // Use my IP

    if ((status = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return -1;
    }

    // Create the socket
    server_socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_socket_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    // Allow address reuse to avoid bind errors on restart
    int yes = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        freeaddrinfo(res);
        close(server_socket_fd);
        return -1;
    }

    // Bind the socket to the port
    if (bind(server_socket_fd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        freeaddrinfo(res);
        close(server_socket_fd);
        return -1;
    }

    freeaddrinfo(res);
    
    // Handle Daemonization if requested
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Fork failed");
            return -1;
        }
        if (pid > 0) {
            // Parent exits
            exit(0);
        }

        // Child process continues
        if (setsid() < 0) return -1;   // Create new session
        if (chdir("/") < 0) return -1; // Change to root

        // Redirect stdin/out/err to /dev/null
        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null != -1) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }

    // Start listening for connections
    if (listen(server_socket_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_socket_fd);
        return -1;
    }

    // Main Accept Loop
    while (!signal_caught) {
        client_addr_size = sizeof client_addr;
        
        // Accept new connection (blocking call)
        int client_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_size);
        
        if (signal_caught) break; // Exit loop if signal received

        if (client_fd == -1) {
            if (errno == EINTR) continue; // Interrupted by signal, retry
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue; 
        }

        // --- CONNECTION ACCEPTED ---
        
        // Allocate memory for thread parameters
        struct thread_data_t *new_thread_params = malloc(sizeof(struct thread_data_t));
        if (new_thread_params == NULL) {
            syslog(LOG_ERR, "Malloc for thread params failed");
            close(client_fd);
            continue;
        }

        // Get Client IP string for logging
        void *addr;
        if (((struct sockaddr *)&client_addr)->sa_family == AF_INET) {
            addr = &(((struct sockaddr_in *)&client_addr)->sin_addr);
        } else {
            addr = &(((struct sockaddr_in6 *)&client_addr)->sin6_addr);
        }
        inet_ntop(client_addr.ss_family, addr, new_thread_params->client_ip, sizeof(new_thread_params->client_ip));
        
        syslog(LOG_INFO, "Accepted connection from %s", new_thread_params->client_ip);

        // Populate thread parameters
        new_thread_params->client_fd = client_fd;
        new_thread_params->mutex = &file_mutex;
        new_thread_params->thread_complete = false;

        // Allocate memory for the list node
        struct slist_data_s *new_node = malloc(sizeof(struct slist_data_s));
        if (new_node == NULL) {
             syslog(LOG_ERR, "Malloc for list node failed");
             free(new_thread_params);
             close(client_fd);
             continue;
        }
        
        new_node->thread_params = new_thread_params;

        // Spawn the thread
        if (pthread_create(&new_node->thread_id, NULL, thread_func, (void *)new_thread_params) != 0) {
            syslog(LOG_ERR, "Thread creation failed");
            free(new_thread_params);
            free(new_node);
            close(client_fd);
            continue;
        }

        // Insert the new thread node into the linked list
        SLIST_INSERT_HEAD(&head, new_node, entries);

        // --- CLEANUP COMPLETED THREADS (FIXED) ---
        // Manually iterate to allow safe removal without SLIST_FOREACH_SAFE
        struct slist_data_s *cursor = SLIST_FIRST(&head);
        struct slist_data_s *next_node = NULL;

        while (cursor != NULL) {
            // Save next node before we potentially delete the current one
            next_node = SLIST_NEXT(cursor, entries);
            
            if (cursor->thread_params->thread_complete) {
                pthread_join(cursor->thread_id, NULL);
                SLIST_REMOVE(&head, cursor, slist_data_s, entries);
                free(cursor->thread_params);
                free(cursor);
            }
            
            cursor = next_node;
        }
    }

    // --- SHUTDOWN & CLEANUP ---
    
    // Close the server socket
    if (server_socket_fd != -1) {
        close(server_socket_fd);
    }
    
    // Wait for all remaining threads to complete
    struct slist_data_s *cursor;
    while (!SLIST_EMPTY(&head)) {
        cursor = SLIST_FIRST(&head);
        
        // Join the thread
        pthread_join(cursor->thread_id, NULL);
        
        // Remove from list
        SLIST_REMOVE_HEAD(&head, entries);
        
        // Free memory
        free(cursor->thread_params);
        free(cursor);
    }

    // Destroy the mutex
    pthread_mutex_destroy(&file_mutex);

    // Delete the data file
    remove(DATA_FILE);
    
    // Close syslog and exit
    closelog();
    
    return 0;
}
