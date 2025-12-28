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
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER; 

// Structure to pass arguments to the connection thread
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

// --- TIMESTAMP THREAD FUNCTION ---
// This thread runs in the background and writes the time every 10 seconds.
void *timestamp_thread_func(void *arg) {
    while (!signal_caught) {
        // Sleep for 10 seconds
        // Note: For faster exit on signal, one might use nanosleep in a loop 
        // or pthread_cond_timedwait, but sleep(10) is sufficient for this requirement.
        sleep(10);

        if (signal_caught) break;

        time_t rawtime;
        struct tm *info;
        char time_str[100];
        char output_str[200];

        // Get current time
        time(&rawtime);
        info = localtime(&rawtime);

        // Format time per RFC 2822: "Day, DD Mon YYYY HH:MM:SS Zone"
        // Example: "Thu, 27 Dec 2025 04:26:51 -0600"
        strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S %z", info);
        
        // Prepare final string: "timestamp:time\n"
        snprintf(output_str, sizeof(output_str), "timestamp:%s\n", time_str);

        // --- CRITICAL SECTION START ---
        // Lock mutex to ensure we don't write while a connection thread is writing/reading
        if (pthread_mutex_lock(&file_mutex) != 0) {
            syslog(LOG_ERR, "Timestamp thread: Mutex lock failed");
        } else {
            // Open file for appending
            int file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (file_fd != -1) {
                if (write(file_fd, output_str, strlen(output_str)) == -1) {
                    syslog(LOG_ERR, "Timestamp thread: File write failed: %s", strerror(errno));
                }
                close(file_fd);
            } else {
                syslog(LOG_ERR, "Timestamp thread: Could not open file: %s", strerror(errno));
            }
            
            pthread_mutex_unlock(&file_mutex);
        }
        // --- CRITICAL SECTION END ---
    }
    return NULL;
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
    while ((bytes_received = recv(data->client_fd, recv_buf, BUFFER_SIZE, 0)) > 0) {
        char *temp = realloc(packet_buffer, current_packet_size + bytes_received);
        if (temp == NULL) {
            syslog(LOG_ERR, "Malloc failed");
            if (packet_buffer) free(packet_buffer);
            packet_buffer = NULL;
            break; 
        }
        packet_buffer = temp;
        
        memcpy(packet_buffer + current_packet_size, recv_buf, bytes_received);
        current_packet_size += bytes_received;

        if (memchr(recv_buf, '\n', bytes_received) != NULL) {
            packet_complete = true;
            break;
        }
    }

    if (packet_complete && packet_buffer != NULL) {
        // --- CRITICAL SECTION START ---
        if (pthread_mutex_lock(data->mutex) != 0) {
            syslog(LOG_ERR, "Mutex lock failed");
        } else {
            int file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (file_fd == -1) {
                syslog(LOG_ERR, "Could not open data file: %s", strerror(errno));
            } else {
                if (write(file_fd, packet_buffer, current_packet_size) == -1) {
                    syslog(LOG_ERR, "File write failed: %s", strerror(errno));
                }
                close(file_fd);
            }
            
            // --- READ AND SEND BACK ---
            file_fd = open(DATA_FILE, O_RDONLY);
            if (file_fd != -1) {
                char send_buf[BUFFER_SIZE];
                ssize_t bytes_read;
                while ((bytes_read = read(file_fd, send_buf, BUFFER_SIZE)) > 0) {
                    if (send(data->client_fd, send_buf, bytes_read, 0) == -1) {
                        syslog(LOG_ERR, "Send failed: %s", strerror(errno));
                        break;
                    }
                }
                close(file_fd);
            }
            
            pthread_mutex_unlock(data->mutex);
            // --- CRITICAL SECTION END ---
        }
    }

    if (packet_buffer) free(packet_buffer);
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", data->client_ip);
    
    data->thread_complete = true;
    return NULL;
}

int main(int argc, char *argv[]) {
    struct addrinfo hints, *res;
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size;
    int status;
    bool daemon_mode = false;
    pthread_t timestamp_thread_id; 

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }
    
    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error registering signal handlers: %s", strerror(errno));
        return -1;
    }

    SLIST_INIT(&head);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((status = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return -1;
    }

    server_socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_socket_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    int yes = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        freeaddrinfo(res);
        close(server_socket_fd);
        return -1;
    }

    if (bind(server_socket_fd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        freeaddrinfo(res);
        close(server_socket_fd);
        return -1;
    }

    freeaddrinfo(res);
    
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Fork failed");
            return -1;
        }
        if (pid > 0) exit(0);

        if (setsid() < 0) return -1;
        if (chdir("/") < 0) return -1;

        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null != -1) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }

    if (listen(server_socket_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_socket_fd);
        return -1;
    }

    // --- START TIMESTAMP THREAD ---
    // We start this after daemonization so it runs in the background process.
    if (pthread_create(&timestamp_thread_id, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread");
        // We continue even if this fails, though typically we might want to exit.
    }

    // Main Accept Loop
    while (!signal_caught) {
        client_addr_size = sizeof client_addr;
        int client_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_size);
        
        if (signal_caught) break;

        if (client_fd == -1) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue; 
        }

        struct thread_data_t *new_thread_params = malloc(sizeof(struct thread_data_t));
        if (new_thread_params == NULL) {
            syslog(LOG_ERR, "Malloc for thread params failed");
            close(client_fd);
            continue;
        }

        void *addr;
        if (((struct sockaddr *)&client_addr)->sa_family == AF_INET) {
            addr = &(((struct sockaddr_in *)&client_addr)->sin_addr);
        } else {
            addr = &(((struct sockaddr_in6 *)&client_addr)->sin6_addr);
        }
        inet_ntop(client_addr.ss_family, addr, new_thread_params->client_ip, sizeof(new_thread_params->client_ip));
        
        syslog(LOG_INFO, "Accepted connection from %s", new_thread_params->client_ip);

        new_thread_params->client_fd = client_fd;
        new_thread_params->mutex = &file_mutex;
        new_thread_params->thread_complete = false;

        struct slist_data_s *new_node = malloc(sizeof(struct slist_data_s));
        if (new_node == NULL) {
             syslog(LOG_ERR, "Malloc for list node failed");
             free(new_thread_params);
             close(client_fd);
             continue;
        }
        
        new_node->thread_params = new_thread_params;

        if (pthread_create(&new_node->thread_id, NULL, thread_func, (void *)new_thread_params) != 0) {
            syslog(LOG_ERR, "Thread creation failed");
            free(new_thread_params);
            free(new_node);
            close(client_fd);
            continue;
        }

        SLIST_INSERT_HEAD(&head, new_node, entries);

        // Cleanup completed threads
        struct slist_data_s *cursor = SLIST_FIRST(&head);
        struct slist_data_s *next_node = NULL;

        while (cursor != NULL) {
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
    if (server_socket_fd != -1) {
        close(server_socket_fd);
    }
    
    // Join timestamp thread
    // Note: This may wait up to 10 seconds for the sleep() to finish.
    pthread_join(timestamp_thread_id, NULL);

    // Join connection threads
    while (!SLIST_EMPTY(&head)) {
        struct slist_data_s *cursor = SLIST_FIRST(&head);
        pthread_join(cursor->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, entries);
        free(cursor->thread_params);
        free(cursor);
    }

    pthread_mutex_destroy(&file_mutex);
    remove(DATA_FILE);
    closelog();
    
    return 0;
}

