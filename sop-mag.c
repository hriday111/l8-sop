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

#define START_PEBBLES 10

// Queue command types for clean worker shutdown.
#define CMD_CAST 0
#define CMD_STOP 1
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


typedef struct {
  int cmd_type; // CMD_CAST or CMD_STOP
  cast_message_t cast; // host-order spell/X/Y for CMD_CAST
  int player_id;       // index into game_state.players for CMD_CAST
} queue_item_t;

typedef struct{
  queue_item_t buff[MAX_QUEUE];
  pthread_mutex_t mutex;
  sem_t sem;
  pthread_cond_t empty_cond;
  int head;
  int tail;
  int count;
  int inflight;        // dequeued-but-not-finished commands
  int stop_requested;
} queue_t;

typedef struct{
  struct sockaddr_in add;
  char name[MAX_NAME_LENGTH+1];
  uint16_t pebbles;
} player_t;

typedef struct {
  pthread_mutex_t mutex;
  player_t players[MAX_CLIENTS];
  int players_count; // logged-in players
  int game_started;  // true after two distinct logins
  int game_over;     // set on surrender
  int fd;             // UDP socket for pebbles updates
} gamestate_t;
void usage(char *name) {
  printf("%s <in_port>\n", name);
  printf("  in_port - port that accepts messages\n");
  exit(EXIT_FAILURE);
}

void queue_init(queue_t *queue){
  pthread_mutex_init(&queue->mutex, NULL);
  sem_init(&queue->sem, 0,0);
  pthread_cond_init(&queue->empty_cond, NULL);
  queue->head=0;
  queue->tail =0;
  queue->count=0;
  queue->inflight = 0;
  queue->stop_requested = 0;

}
void queue_destroy(queue_t* queue){
  sem_destroy(&queue->sem);
  pthread_cond_destroy(&queue->empty_cond);
  pthread_mutex_destroy(&queue->mutex);
}
queue_item_t queue_pop(queue_t* queue){
  sem_wait(&queue->sem);
  pthread_mutex_lock(&queue->mutex);
  if(queue->stop_requested && queue->count == 0){
    pthread_mutex_unlock(&queue->mutex);
    queue_item_t item;
    memset(&item, 0, sizeof(item));
    item.cmd_type = CMD_STOP;
    return item;
  }

  queue_item_t item = queue->buff[queue->head];
  queue->head=(queue->head+1)%MAX_QUEUE;
  queue->count--;
  queue->inflight++;
  pthread_mutex_unlock(&queue->mutex);
  return item;

}
void queue_push(queue_t* queue, const queue_item_t* item){
  pthread_mutex_lock(&queue->mutex);
  if(queue->count<MAX_QUEUE) {
    queue->buff[queue->tail] = *item;
    queue->tail=(queue->tail+1)%MAX_QUEUE;
    queue->count++;
    sem_post(&queue->sem);
  } else {
    if(item->cmd_type == CMD_CAST){
      printf("Queue full, discarding command\n");
    }
  }
  pthread_mutex_unlock(&queue->mutex);
}
typedef struct {
  queue_t *queue;
  gamestate_t *game;
} worker_ctx_t;

static int spell_cost(uint16_t spell){
  // Stage 3 spell costs:
  // 0 (Divination) -> 1 pebble
  // 1 (Summon Elemental) -> 3 pebbles
  // 2 (Fireball) -> 4 pebbles
  if(spell == 0) return 1;
  if(spell == 1) return 3;
  if(spell == 2) return 4;
  return 0;
}

static int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b){
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int find_player(gamestate_t *game, const struct sockaddr_in *addr){
  for(int i = 0; i < game->players_count; i++){
    if(addr_equal(&game->players[i].add, addr)) return i;
  }
  return -1;
}

void* worker(void* args)
{
  worker_ctx_t *ctx = (worker_ctx_t*)args;
  queue_t *queue = ctx->queue;
  gamestate_t *game = ctx->game;

  while(1)
  {
    queue_item_t item = queue_pop(queue);
    if(item.cmd_type == CMD_STOP){
      break;
    }

    if(game->game_over){
      pthread_mutex_lock(&queue->mutex);
      queue->inflight--;
      if(queue->count == 0 && queue->inflight == 0){
        pthread_cond_signal(&queue->empty_cond);
      }
      pthread_mutex_unlock(&queue->mutex);
      continue;
    }

    int cost = spell_cost(item.cast.spell);
    int enough = 0;
    uint16_t new_pebbles = 0;

    pthread_mutex_lock(&game->mutex);
    uint16_t cur = game->players[item.player_id].pebbles;
    if(cur >= (uint16_t)cost){
      game->players[item.player_id].pebbles = cur - (uint16_t)cost;
      new_pebbles = game->players[item.player_id].pebbles;
      enough = 1;
    }
    pthread_mutex_unlock(&game->mutex);

    if(!enough){
      if(!game->game_over){
        printf("[tee hee] Not enough pebbles, %s!\n", game->players[item.player_id].name);
      }
      ms_sleep(FAMILIAR_DELAY);
    } else {
      ms_sleep(FAMILIAR_DELAY);
      if(!game->game_over){
        printf("[Cast] %s casts %s onto %hu,%hu .\n",
               game->players[item.player_id].name,
               spell_names[item.cast.spell],
               item.cast.X, item.cast.Y);

        uint16_t net_pebbles = htons(new_pebbles);
        (void)sendto(game->fd, &net_pebbles, sizeof(net_pebbles), 0,
                      (const struct sockaddr *)&game->players[item.player_id].add,
                      sizeof(struct sockaddr_in));
      }
    }

    pthread_mutex_lock(&queue->mutex);
    queue->inflight--;
    if(queue->count == 0 && queue->inflight == 0){
      pthread_cond_signal(&queue->empty_cond);
    }
    pthread_mutex_unlock(&queue->mutex);
  }

  return NULL;
}
void doServer(int fd) {

  queue_t queue;
  queue_init(&queue);

  gamestate_t game;
  memset(&game, 0, sizeof(game));
  pthread_mutex_init(&game.mutex, NULL);
  game.fd = fd;
  game.players_count = 0;
  game.game_started = 0;
  game.game_over = 0;

  pthread_t threads[THREAD_COUNT];
  worker_ctx_t ctx = {.queue = &queue, .game = &game};
  for(int i=0; i<THREAD_COUNT; i++)
  {
    if(pthread_create(&threads[i], NULL, worker, &ctx)<0){ERR("pthread_create");}
  }

  char buff[MAX_BUFF_LEN + 1];
  struct sockaddr_in addr;
  memset(buff, 0, sizeof(buff));

  int handled_messages = 0;
  while (!game.game_over && handled_messages < 4) {
    socklen_t addr_len = sizeof(addr);
    int recv_len = recvfrom(fd, buff, MAX_BUFF_LEN, 0,
                             (struct sockaddr *)&addr, &addr_len);
    if (recv_len < 0) {
      perror("recvfrom");
      continue;
    }

    // Stage 1: fixed-size protocol (exactly 16 bytes).
    if (recv_len != MAX_BUFF_LEN) {
      printf("Message of incorrect length\n");
      continue;
    }

    if (buff[0] == 'l') {
      // Stage 3: before game starts accept only login.
      if(game.game_started) {
        continue;
      }
      if (game.players_count >= MAX_CLIENTS) {
        continue;
      }

      login_message_t *message = (login_message_t *)buff;
      message->name[MAX_NAME_LENGTH] = '\0';

      if(find_player(&game, &addr) >= 0){
        continue;
      }

      player_t *p = &game.players[game.players_count];
      p->add = addr;
      strncpy(p->name, message->name, MAX_NAME_LENGTH);
      p->name[MAX_NAME_LENGTH] = '\0';
      p->pebbles = START_PEBBLES;
      game.players_count++;

      printf("[Login] Welcome, %s\n", p->name);
      handled_messages++;

      if(game.players_count == MAX_CLIENTS){
        game.game_started = 1;
      }
    } else if (buff[0] == 'q') {
      if(!game.game_started){
        continue;
      }

      int pid = find_player(&game, &addr);
      if(pid < 0){
        continue;
      }

      int winner = (pid == 0) ? 1 : 0;
      printf("[Quit] %s quit. Goodbye!\n", game.players[pid].name);
      printf("-= Congratulations, %s, you win! =-\n", game.players[winner].name);
      game.game_over = 1;
      break;
    } else if (buff[0] == 'c') {
      if(!game.game_started){
        continue;
      }

      int pid = find_player(&game, &addr);
      if(pid < 0){
        continue;
      }

      cast_message_t *message = (cast_message_t *)&buff;
      message->spell = ntohs(message->spell);
      message->X = ntohs(message->X);
      message->Y = ntohs(message->Y);

      if (message->spell >= SPELL_TYPES || message->X >= BOARD_SIZE ||
          message->Y >= BOARD_SIZE) {
        printf("Value OUT OF RANGE\n");
        continue;
      }

      queue_item_t item;
      memset(&item, 0, sizeof(item));
      item.cmd_type = CMD_CAST;
      item.cast = *message; // host-order spell/X/Y
      item.player_id = pid;
      queue_push(&queue, &item);
      handled_messages++;
    } else {
      printf("Incorrect message type\n");
    }
  }

  // Shutdown workers.
  pthread_mutex_lock(&queue.mutex);
  if(game.game_over){
    // Discard pending queued work; in-flight workers will observe game_over and stop printing.
    queue.head = 0;
    queue.tail = 0;
    queue.count = 0;
  } else {
    // Stage 1/2: end after receiving and handing 4 messages.
    while(queue.count > 0 || queue.inflight > 0){
      pthread_cond_wait(&queue.empty_cond, &queue.mutex);
    }
  }
  queue.stop_requested = 1;
  pthread_mutex_unlock(&queue.mutex);

  for(int i=0; i<THREAD_COUNT; i++){
    sem_post(&queue.sem);
  }

  for(int i=0; i<THREAD_COUNT; i++)
  {
    pthread_join(threads[i], NULL);
  }

  pthread_mutex_destroy(&game.mutex);
  queue_destroy(&queue);
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
