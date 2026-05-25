#include "l8_common.h"

#define SPELL_TYPES 3
const char *spell_names[SPELL_TYPES] = {"Divination", "Summon Elemental",
                                        "Fireball"};
#define BOARD_SIZE 8
#define BACKLOG 16

#define MAX_QUEUE 10
#define THREAD_COUNT 3
#define FAMILIAR_DELAY 100

#define MAX_CLIENTS 2
#define MAX_NAME_LENGTH 14
#define MAX_BUFF_LEN 16
typedef struct {
  char buff[16];
} message_t;

typedef struct __attribute__((__packed__)) {
  char type;
  char padding;
  uint16_t spell;
  uint16_t X;
  uint16_t Y;
} cast_message_t;

typedef struct __attribute__((__packed__)) {
  char type;
} quit_message_t;
typedef struct __attribute__((__packed__)) {
  char type;
  char padding;
  char name[MAX_NAME_LENGTH+1];
} login_message_t;


typedef struct{
  cast_message_t buff[MAX_QUEUE];
  pthread_mutex_t mutex;
  sem_t sem;
  int head;
  int tail;
  int count;
}queue_t;
void usage(char *name) {
  printf("%s <in_port>\n", name);
  printf("  in_port - port that accepts messages\n");
  exit(EXIT_FAILURE);
}

void queue_init(queue_t *queue){
  pthread_mutex_init(&queue->mutex, NULL);
  sem_init(&queue->sem, 0,0);
  queue->head=0;
  queue->tail =0;
  queue->count=0;

}
void queue_destroy(queue_t* queue){
  sem_destroy(&queue->sem);
  pthread_mutex_destroy(&queue->mutex);
}
cast_message_t queue_pop(queue_t* queue){
  sem_wait(&queue->sem);
  cast_message_t message;

  pthread_mutex_lock(&queue->mutex);

  message=queue->buff[queue->head];
  queue->head=(queue->head+1)%MAX_QUEUE;
  queue->count--;
  pthread_mutex_unlock(&queue->mutex);
  return message;

}
void queue_push(queue_t* queue, cast_message_t* message){
  pthread_mutex_lock(&queue->mutex);
  if(queue->count<MAX_QUEUE) {
    memcpy(&queue->buff[queue->tail], message, sizeof(cast_message_t));
    queue->tail=(queue->tail+1)%MAX_QUEUE;
    queue->count++;
    sem_post(&queue->sem);


  }
  pthread_mutex_unlock(&queue->mutex);
}
void* worker(void* args)
{
  queue_t *queue =  args;
  while(1)
  {
    cast_message_t message=queue_pop(queue);
    printf("[Cast] Someone casts %s onto %hu,%hu\n",
             spell_names[message.spell], message.X, message.Y);
  }
}
void doServer(int fd) {

  queue_t queue;

  queue_init(&queue);

  pthread_t threads[THREAD_COUNT];
  for(int i=0; i<THREAD_COUNT; i++)
  {

    if(pthread_create(&threads[i], NULL, worker, &queue)<0){ERR("pthread_create");}
  }
  char buff[MAX_BUFF_LEN + 1];
  struct sockaddr_in addr;
  socklen_t addr_len;
  memset(buff, 0, 17);
  int count_message =0;
  while (count_message<=4) {
    int recv_len = 0;
    if ((recv_len = recvfrom(fd, buff, MAX_BUFF_LEN, 0,
                             (struct sockaddr *)&addr, &addr_len)) < 0) {
      perror("recvfrom");
      continue;
    }

    if (recv_len < MAX_BUFF_LEN) {
      printf("Message of incorrect length\n");
      continue;
    }

    if (buff[0] == 'l') {
      login_message_t *message;
      message = (login_message_t *)buff;
      message->name[MAX_NAME_LENGTH] = '\0';
      printf("[Login] Welcome, %s\n", message->name);
      count_message++;
    } else if (buff[0] == 'q') {
      count_message++;
    } else if (buff[0] == 'c') {
      cast_message_t *message;
      message = (cast_message_t *)&buff;
      message->spell = ntohs(message->spell);
      message->X = ntohs(message->X);
      message->Y = ntohs(message->Y);

      if (message->spell >= SPELL_TYPES || message->X >= BOARD_SIZE ||
          message->Y >= BOARD_SIZE) {
        printf("Value OUT OF RANGE\n");
        continue;
      }
      //printf("[Cast] Someone casts %s onto %hu,%hu\n",
      //       spell_names[message->spell], message->X, message->Y);
      count_message++;

      queue_push(&queue,message);
    } else {
      printf("Incorrect message type\n");
    }
  }
  for(int i=0; i<THREAD_COUNT; i++)
  {

    pthread_join(threads[i], NULL);
  }
  
}
int main(int argc, char **argv) {
  // printf("sizeof(struct packed) == %d\n", sizeof(struct packed));
  // printf("sizeof(struct not_packed) == %d\n", sizeof(struct not_packed));
  if (argc != 2) {
    usage(argv[0]);
  }
  uint16_t port = atoi(argv[1]);
  int fd = bind_inet_socket(port, SOCK_DGRAM, 10);

  doServer(fd);

  if (close(fd) < 0) {
    ERR("close");
  }
  return 0;
}
