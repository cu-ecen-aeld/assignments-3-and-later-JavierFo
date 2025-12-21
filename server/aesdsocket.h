#define MAX_PACKET_SIZE 65535 


struct clientInfo{
    int clientFD;
    struct sockaddr_in* clientAddr;
};

int server_init();
void processClientData(struct clientInfo* clientInfo );
