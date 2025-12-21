#include<stdio.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<syslog.h>
#include<unistd.h>
#include<fcntl.h>
#include<string.h>
#include<errno.h>
#include<stdlib.h>
#include<arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include "aesdsocket.h"
#include <signal.h>
#include <syslog.h>


volatile sig_atomic_t keepRunning = 1;

void sig_handler(int sig){
    keepRunning = 0;
}

int main( int argc, char *argv[] ) {

    struct sockaddr_in clientAddr;
    socklen_t clientAddr_len = sizeof(clientAddr);
    //char buffer[1024];

    struct sigaction sa;
    int option;
    int daemonize = 0;

    while ((option = getopt(argc, argv, "d")) != -1) 
    {
        switch (option) 
        {
            case 'd':
                daemonize = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
                break;
        }    
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT | SIGTERM, &sa, NULL);   

    printf("Server Initialize\n");
    int sockFD = server_init();
    if(sockFD < 0)
    {
        syslog(LOG_ERR , "Error creating socket");
        return -1;
    }

    if(daemonize == 1)
    {
        pid_t pid = fork();
        if(pid < 0)
        {
            syslog(LOG_ERR , "Error forking process");
            return -1;
        }
        if (pid > 0)
        {
            printf("Parent process exiting\n");
            exit(0);
        }
        setsid();
        umask(0);
        chdir("/");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        int fd = open("/dev/null", O_RDWR);
        if(fd < 0)
        {
            syslog(LOG_ERR , "Error opening /dev/null");
            return -1;
        }
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    printf("Server listening on %d\n", sockFD);
    if(listen(sockFD, 5) < 0){
        syslog(LOG_ERR , "Error listening on socket");
        close(sockFD);
        return -1;
    }

    // ensure old data file is removed
    remove( "/var/tmp/aesdsocketdata.txt" );

    int clientFD;
    struct clientInfo* clientInfo;
    while (keepRunning) 
    {
        

        clientFD =  accept(sockFD, (struct sockaddr*)&clientAddr,  &clientAddr_len);
        if (clientFD < 0) {
            syslog(LOG_ERR ,"Error accepting connection");
            perror ("Error accepting connection");
            close(sockFD);
            return -1;
        }
        clientInfo = malloc(sizeof(struct clientInfo));
        clientInfo->clientFD = clientFD;
        clientInfo->clientAddr = &clientAddr;
        
        syslog(LOG_INFO , "Accepted connection from %s", inet_ntoa(clientAddr.sin_addr));
        printf("accepting connection from %s", inet_ntoa(clientAddr.sin_addr));

        pthread_t clinthread;
        int* clientPtr = malloc(sizeof(int));
        *clientPtr = clientFD;

        
        if(pthread_create(&clinthread , NULL , (void*)processClientData , clientInfo ) < 0){
            syslog(LOG_ERR , "Error creating thread");
            free( clientPtr);
            return -1;
        }
        
        pthread_detach(clinthread);
        printf("Handler assigned to thread. Waiting for next connection.\\n");
    }

    close(sockFD);
    close(clientFD);
    syslog(LOG_INFO , "Server closed");
    close(sockFD);
    remove("/var/tmp/aesdsocketdata.txt");
    

    return 0;
}

int server_init(){
    
    struct sockaddr_in serverAddr;
    int port = 9000;
    int sockFD = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFD < 0) {
        printf("Error creating socket\n");
        return -1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY ;

    if (bind(sockFD, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        syslog(LOG_ERR , "Error binding socket");
        return -1;
    }
    
    return sockFD;
}

void processClientData( struct clientInfo* clientInfo )
{
    char buffer[1024];
    char *ptr = buffer;
    char *packet_buffer = malloc(MAX_PACKET_SIZE);
    int fd = open("/var/tmp/aesdsocketdata.txt" , O_CREAT | O_RDWR  , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    if(fd < 0){
        perror("Error opening file");
        syslog(LOG_ERR , "Error opening file");
    }
    printf("Connection accepted\n");
    while((keepRunning == 1))
    {
        int bytes = recv(clientInfo->clientFD, buffer, sizeof(buffer), 0);
        if(bytes < 0){
            syslog(LOG_ERR , "Error receiving data");
            break;
        }
        if(bytes == 0){

            printf("Connection closed\n");
            syslog(LOG_INFO , "Closed connection from %s", inet_ntoa(clientInfo->clientAddr->sin_addr)); 
            break;
        }
        ptr = buffer;
        printf("Received %d bytes: %s\n", bytes, buffer);

        int bytes_written = 0;
        while ((bytes_written < bytes) && (keepRunning == 1)) {
            // SEARCH FOR NEW LINE IN THE RECIEVED DATA
            char *newline_pos = strchr(ptr, '\n');
            printf("buffer pointer: %p \n", (void *)(ptr));
            printf("newline pointer: %p \n", (void *)(newline_pos));
            if (newline_pos != NULL) {
                // COPY THE DATA UNTIL THE NEWLINE TO THE PACKET BUFFER
                int packet_size = newline_pos - ptr;
                memcpy(packet_buffer, ptr, (packet_size + 1) );
                // WRITE THE PACKET BUFFER TO THE FILE
                printf("Received %d bytes: %s\n", packet_size+1, packet_buffer);
                //Seek to the end of the file
                lseek(fd, 0, SEEK_END);
                bytes_written +=  write(fd, packet_buffer, (packet_size+ 1));
                printf("Written %d bytes\n", packet_size+1);
                // bytes_written += packet_size ;
                // MOVE THE BUFFER POINTER TO THE NEXT LINE
                ptr = newline_pos + 1;

                printf("sending data\n");   
                // REWIND THE FILE TO THE BEGINING OF THE DATA
                lseek(fd, 0, SEEK_SET);

                // send the full content of  /var/tmp/aesdsocketdata
                char *buffer_rFile = malloc(MAX_PACKET_SIZE);
                int bytes_read = read(fd, buffer_rFile, MAX_PACKET_SIZE);
                if (bytes_read < 0) {
                    syslog(LOG_ERR , "Error reading file");
                    printf("Error reading file\n");
                }
                send(clientInfo->clientFD, buffer_rFile, bytes_read, 0);
                free(buffer_rFile);
            }
            else {
                // COPY THE REMAINING DATA TO THE PACKET BUFFER
                int packet_size = bytes - bytes_written;
                memcpy(packet_buffer, ptr, packet_size );
                lseek(fd, 0, SEEK_END);
                // WRITE THE PACKET BUFFER TO THE FILE
                bytes_written += write(fd, packet_buffer, packet_size  );
                // bytes_written += packet_size ;
                // MOVE THE BUFFER POINTER TO THE END OF THE DATA
                ptr += packet_size;
            }
        }

    }
      
    close(fd);
    free(packet_buffer);
    close (clientInfo->clientFD);
    free((void*)(clientInfo));
    printf("Connection closed\n");
    
}

/*#include <stdio.h>
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

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define BUFFER_SIZE 1024

// Global variables for signal handling cleanup
int server_socket_fd = -1;
int client_socket_fd = -1;
bool signal_caught = false;

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

// Helper to clean up file and sockets on exit
void cleanup_and_exit(int exit_code) {
    if (client_socket_fd != -1) {
        close(client_socket_fd);
    }
    if (server_socket_fd != -1) {
        close(server_socket_fd);
    }
    
    // Delete the file
    remove(DATA_FILE);
    
    closelog();
    exit(exit_code);
}

int main(int argc, char *argv[]) {
    struct addrinfo hints, *res;
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size;
    int status;
    char client_ip[INET6_ADDRSTRLEN];
    bool daemon_mode = false;

    // Check for daemon argument
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }
    
    // Open syslog
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Register signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error registering signal handlers: %s", strerror(errno));
        return -1;
    }

    // Initialize socket structure
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;     // Fill in my IP for me

    if ((status = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return -1;
    }

    // Create socket
    server_socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_socket_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    // Set socket options to reuse address (avoids "Address already in use" error on rapid restarts)
    int yes = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        freeaddrinfo(res);
        close(server_socket_fd);
        return -1;
    }

    // Bind socket
    if (bind(server_socket_fd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        freeaddrinfo(res);
        close(server_socket_fd);
        return -1;
    }

    freeaddrinfo(res);
    
    // Daemonization Logic
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Fork failed");
            return -1;
        }
        if (pid > 0) {
            // Parent process exits
            exit(0);
        }

        // Child process continues
        if (setsid() < 0) return -1; // Create new session
        if (chdir("/") < 0) return -1; // Change to root directory

        // Redirect standard file descriptors to /dev/null
        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null != -1) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }

    // Listen
    if (listen(server_socket_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_socket_fd);
        return -1;
    }

    // Main loop
    while (!signal_caught) {
        client_addr_size = sizeof client_addr;
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_size);
        
        if (signal_caught) break; // Check immediately after unblocking

        if (client_socket_fd == -1) {
            // If accept was interrupted by a signal, loop back to check flag
            if (errno == EINTR) continue;
            
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue; 
        }

        // Get Client IP
        void *addr;
        if (((struct sockaddr *)&client_addr)->sa_family == AF_INET) {
            addr = &(((struct sockaddr_in *)&client_addr)->sin_addr);
        } else {
            addr = &(((struct sockaddr_in6 *)&client_addr)->sin6_addr);
        }
        inet_ntop(client_addr.ss_family, addr, client_ip, sizeof client_ip);
        
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Receive Data Logic
        char *packet_buffer = NULL;
        size_t current_packet_size = 0;
        char recv_buf[BUFFER_SIZE];
        ssize_t bytes_received;
        bool packet_complete = false;

        // Open file for appending
        int file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (file_fd == -1) {
            syslog(LOG_ERR, "Could not open data file: %s", strerror(errno));
            close(client_socket_fd);
            continue;
        }

        // Read loop until newline or connection closed
        while ((bytes_received = recv(client_socket_fd, recv_buf, BUFFER_SIZE, 0)) > 0) {
            
            // Reallocate buffer to hold new data
            char *temp = realloc(packet_buffer, current_packet_size + bytes_received);
            if (temp == NULL) {
                syslog(LOG_ERR, "Malloc failed");
                if (packet_buffer) free(packet_buffer);
                packet_buffer = NULL;
                break; // Discard packet
            }
            packet_buffer = temp;
            
            memcpy(packet_buffer + current_packet_size, recv_buf, bytes_received);
            current_packet_size += bytes_received;

            // Check for newline
            if (memchr(recv_buf, '\n', bytes_received) != NULL) {
                packet_complete = true;
                break;
            }
        }

        if (packet_complete && packet_buffer != NULL) {
            // Write to file
            if (write(file_fd, packet_buffer, current_packet_size) == -1) {
                syslog(LOG_ERR, "File write failed: %s", strerror(errno));
            }
        }
        
        if (packet_buffer) free(packet_buffer);
        close(file_fd);

        // Send full file content back to client
        // Re-open in read-only mode to stream back
        file_fd = open(DATA_FILE, O_RDONLY);
        if (file_fd != -1) {
            char send_buf[BUFFER_SIZE];
            ssize_t bytes_read;
            while ((bytes_read = read(file_fd, send_buf, BUFFER_SIZE)) > 0) {
                if (send(client_socket_fd, send_buf, bytes_read, 0) == -1) {
                    syslog(LOG_ERR, "Send failed: %s", strerror(errno));
                    break;
                }
            }
            close(file_fd);
        }

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(client_socket_fd);
        client_socket_fd = -1;
    }

    cleanup_and_exit(0);
    return 0;
}*/
