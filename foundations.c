#include "l8_common.h"

#define SPELL_TYPES 3
const char* spell_names[SPELL_TYPES] = {"Divination", "Summon Elemental", "Fireball"};
#define BOARD_SIZE 8
#define BACKLOG 16

#define MAX_QUEUE 10
#define THREAD_COUNT 3
#define FAMILIAR_DELAY 100

#define MAX_CLIENTS 2
#define MAX_NAME_LENGTH 14
/*
typedef struct __attribute__((__packed__)) packed
{
    char c1;
    int i1;
    char c2;
    int i2;
};
typedef struct not_packed
{
    char c1;
    int i1;
    char c2;
    int i2;
};*/

void usage(char* name)
{
    printf("%s <in_port>\n", name);
    printf("  in_port - port that accepts messages\n");
    exit(EXIT_FAILURE);
}

void doServer(int fd)
{
    char buff[1001];
    struct sockaddr_in addr;
    while (1) {
        socklen_t addr_len = sizeof(addr);
        int recv_len = recvfrom(fd, buff, 1000, 0, (struct sockaddr *)&addr, &addr_len);
        if (recv_len < 0) { perror("recvfrom"); continue; }
        printf("[Length]: %d\n", recv_len);
        if (recv_len > 0) {
            printf("[Type]: '%c' (0x%02x)\n", isprint((unsigned char)buff[0]) ? buff[0] : '?', (unsigned char)buff[0]);
        }
        buff[recv_len] = '\0';  
        printf("[Preview]: %s\n", buff+1);
}
}

int main(int argc, char** argv)
{
    if (argc != 2) {
    usage(argv[0]);
  }
  uint16_t port = atoi(argv[1]);
  int fd = bind_inet_socket(port, SOCK_DGRAM, 10);
  doServer(fd);
  if(close(fd)<0){ERR("close");}
  return 0;
}
