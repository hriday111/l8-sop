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
#define MAX_BUFF_LENGTH 16
typedef struct __attribute__((__packed__)) 
{
    char type;
    char padding;
    uint16_t spell;
    uint16_t X;
    uint16_t Y;
} cast_message_t;


typedef struct __attribute__((__packed__)) 
{
    char type;
    char padding;
    char name[MAX_NAME_LENGTH];
} login_message_t;


typedef struct __attribute__((__packed__)) 
{
    char type;
} quit_message_t;

void usage(char* name)
{
    printf("%s <in_port>\n", name);
    printf("  in_port - port that accepts messages\n");
    exit(EXIT_FAILURE);
}

void doServer(int fd)
{
    char buff[MAX_BUFF_LENGTH+1];
    struct sockaddr_in addr;
    memset(buff, 0, MAX_BUFF_LENGTH+1);
    int count_msgs=0;
    while (count_msgs < 4) {
        socklen_t addr_len = sizeof(addr);
        int recv_len = recvfrom(fd, buff, MAX_BUFF_LENGTH, 0, (struct sockaddr *)&addr, &addr_len);
        if (recv_len < 0) { perror("recvfrom"); continue; }
        printf("[Length]: %d\n", recv_len);
        if (recv_len != MAX_BUFF_LENGTH) {
            printf("Message of incorrect length\n");
            continue;
        }
        if(buff[0]=='l')
        {
            login_message_t *message;
            message=(login_message_t*)buff;
            message->name[MAX_NAME_LENGTH-1] = '\0';
            printf("[Login] Welcome, %s\n", message->name);
            count_msgs++;
        }
        else if(buff[0]=='c')
        {
            cast_message_t *message;
            message=(cast_message_t*)buff;
            message->spell = ntohs(message->spell);
            message->X = ntohs(message->X);
            message->Y = ntohs(message->Y);
            if (message->spell >= SPELL_TYPES || message->X >= BOARD_SIZE || message->Y >= BOARD_SIZE) {
                printf("Value OUT OF RANGE \n");
                    continue;
            }
            count_msgs++;
            printf("[Cast] Someone casts %s onto %hu,%hu\n",
            spell_names[message->spell], message->X, message->Y);    
        }
        else if(buff[0]=='q'){
            continue;
        }
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
