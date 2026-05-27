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
#define DIVINATION_REPLY_INTS 25

#define START_PEBBLES 10

#define CMD_CAST 0
#define CMD_STOP 1

// Board cells: 0 empty, 1 player0 elemental, 2 player1 elemental
#define CELL_EMPTY 0
#define CELL_P1 1
#define CELL_P2 2

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
  char name[MAX_NAME_LENGTH + 1];
} login_message_t;

typedef struct {
  int cmd_type;
  cast_message_t cast; // host-order spell/X/Y for CMD_CAST
  int player_id;
} queue_item_t;

typedef struct {
  queue_item_t buff[MAX_QUEUE];
  pthread_mutex_t mutex;
  sem_t sem;
  pthread_cond_t empty_cond;
  int head;
  int tail;
  int count;
  int inflight;
  int stop_requested;
} queue_t;

typedef struct {
  struct sockaddr_in add;
  char name[MAX_NAME_LENGTH + 1];
  uint16_t pebbles;
} player_t;

typedef struct {
  pthread_mutex_t mutex;
  player_t players[MAX_CLIENTS];
  int players_count;
  int game_started;
  int game_over;
  int winner; // -1 until set
  int fd;
  int board[BOARD_SIZE][BOARD_SIZE];
} gamestate_t;

typedef struct {
  queue_t *queue;
  gamestate_t *game;
} worker_ctx_t;

typedef struct {
  gamestate_t *game;
} judge_ctx_t;

void usage(char *name) {
  printf("%s <in_port>\n", name);
  printf("  in_port - port that accepts messages\n");
  exit(EXIT_FAILURE);
}

static int spell_cost(uint16_t spell) {
  if (spell == 0) return 1;
  if (spell == 1) return 3;
  if (spell == 2) return 4;
  return 0;
}

static int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b) {
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int find_player(gamestate_t *game, const struct sockaddr_in *addr) {
  for (int i = 0; i < game->players_count; i++) {
    if (addr_equal(&game->players[i].add, addr)) return i;
  }
  return -1;
}

static int elemental_count(gamestate_t *game, int pid) {
  int target = (pid == 0) ? CELL_P1 : CELL_P2;
  int cnt = 0;
  for (int y = 0; y < BOARD_SIZE; y++) {
    for (int x = 0; x < BOARD_SIZE; x++) {
      if (game->board[y][x] == target) cnt++;
    }
  }
  return cnt;
}

static void queue_init(queue_t *queue) {
  pthread_mutex_init(&queue->mutex, NULL);
  pthread_cond_init(&queue->empty_cond, NULL);
  sem_init(&queue->sem, 0, 0);
  queue->head = 0;
  queue->tail = 0;
  queue->count = 0;
  queue->inflight = 0;
  queue->stop_requested = 0;
}

static void queue_destroy(queue_t *queue) {
  sem_destroy(&queue->sem);
  pthread_cond_destroy(&queue->empty_cond);
  pthread_mutex_destroy(&queue->mutex);
}

static void queue_push(queue_t *queue, const queue_item_t *item) {
  pthread_mutex_lock(&queue->mutex);
  if (queue->count < MAX_QUEUE) {
    queue->buff[queue->tail] = *item;
    queue->tail = (queue->tail + 1) % MAX_QUEUE;
    queue->count++;
    sem_post(&queue->sem);
  } else if (item->cmd_type == CMD_CAST) {
    printf("Queue full, discarding command\n");
  }
  pthread_mutex_unlock(&queue->mutex);
}

static queue_item_t queue_pop(queue_t *queue) {
  sem_wait(&queue->sem);
  pthread_mutex_lock(&queue->mutex);
  if (queue->stop_requested && queue->count == 0) {
    pthread_mutex_unlock(&queue->mutex);
    queue_item_t stop_item;
    memset(&stop_item, 0, sizeof(stop_item));
    stop_item.cmd_type = CMD_STOP;
    return stop_item;
  }
  queue_item_t item = queue->buff[queue->head];
  queue->head = (queue->head + 1) % MAX_QUEUE;
  queue->count--;
  queue->inflight++;
  pthread_mutex_unlock(&queue->mutex);
  return item;
}

static void queue_complete(queue_t *queue) {
  pthread_mutex_lock(&queue->mutex);
  queue->inflight--;
  if (queue->count == 0 && queue->inflight == 0) {
    pthread_cond_signal(&queue->empty_cond);
  }
  pthread_mutex_unlock(&queue->mutex);
}

static void send_pebbles(gamestate_t *game, int pid) {
  uint16_t net = htons(game->players[pid].pebbles);
  (void)sendto(game->fd, &net, sizeof(net), 0,
               (const struct sockaddr *)&game->players[pid].add,
               sizeof(struct sockaddr_in));
}

static void send_divination(gamestate_t *game, int pid, uint16_t x, uint16_t y) {
  // 25 values (5x5), each uint16_t network-order.
  uint16_t payload[DIVINATION_REPLY_INTS];
  int idx = 0;
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      int xx = (int)x + dx;
      int yy = (int)y + dy;
      uint16_t code;
      if (xx < 0 || xx >= BOARD_SIZE || yy < 0 || yy >= BOARD_SIZE) {
        code = 3; // outside board
      } else {
        int cell = game->board[yy][xx];
        if (cell == CELL_EMPTY) {
          code = 0;
        } else if ((pid == 0 && cell == CELL_P1) || (pid == 1 && cell == CELL_P2)) {
          code = 1; // own elemental
        } else {
          code = 2; // opponent elemental
        }
      }
      payload[idx++] = htons(code);
    }
  }

  (void)sendto(game->fd, payload, sizeof(payload), 0,
               (const struct sockaddr *)&game->players[pid].add,
               sizeof(struct sockaddr_in));
}

static void apply_spell_effect(gamestate_t *game, int pid, const cast_message_t *cast) {
  uint16_t x = cast->X;
  uint16_t y = cast->Y;
  if (cast->spell == 0) {
    send_divination(game, pid, x, y);
  } else if (cast->spell == 1) {
    // Summon elemental only on empty tile.
    if (game->board[y][x] == CELL_EMPTY) {
      game->board[y][x] = (pid == 0) ? CELL_P1 : CELL_P2;
    }
  } else if (cast->spell == 2) {
    // Fireball destroys all elementals in 3x3 around target.
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        int xx = (int)x + dx;
        int yy = (int)y + dy;
        if (xx >= 0 && xx < BOARD_SIZE && yy >= 0 && yy < BOARD_SIZE) {
          game->board[yy][xx] = CELL_EMPTY;
        }
      }
    }
  }
}

static void maybe_finish_by_depletion(gamestate_t *game) {
  if (!game->game_started || game->game_over) return;
  int e0 = elemental_count(game, 0);
  int e1 = elemental_count(game, 1);
  int lose0 = (game->players[0].pebbles == 0 && e0 == 0);
  int lose1 = (game->players[1].pebbles == 0 && e1 == 0);
  if (!lose0 && !lose1) return;

  if (lose0 && lose1) {
    // Spec: if simultaneous, first-logged-in player wins.
    game->winner = 0;
  } else if (lose0) {
    game->winner = 1;
  } else {
    game->winner = 0;
  }
  game->game_over = 1;

  int loser = (game->winner == 0) ? 1 : 0;
  printf("-= Congratulations, %s, you win! =-\n", game->players[game->winner].name);
  char w = 'w';
  char l = 'l';
  (void)sendto(game->fd, &w, 1, 0,
               (const struct sockaddr *)&game->players[game->winner].add,
               sizeof(struct sockaddr_in));
  (void)sendto(game->fd, &l, 1, 0,
               (const struct sockaddr *)&game->players[loser].add,
               sizeof(struct sockaddr_in));
}

static void print_board_status(gamestate_t *game) {
  printf("=== Judge tick ===\n");
  for (int y = 0; y < BOARD_SIZE; y++) {
    for (int x = 0; x < BOARD_SIZE; x++) {
      char c = '.';
      if (game->board[y][x] == CELL_P1) c = 'A';
      else if (game->board[y][x] == CELL_P2) c = 'B';
      printf("%c", c);
    }
    printf("\n");
  }
  printf("Legend: A=%s, B=%s\n", game->players[0].name, game->players[1].name);
  printf("Pouches: %s=%hu, %s=%hu\n",
         game->players[0].name, game->players[0].pebbles,
         game->players[1].name, game->players[1].pebbles);
}

static void *judge(void *args) {
  judge_ctx_t *ctx = (judge_ctx_t *)args;
  gamestate_t *game = ctx->game;

  while (1) {
    ms_sleep(1000);

    pthread_mutex_lock(&game->mutex);
    if (!game->game_started) {
      pthread_mutex_unlock(&game->mutex);
      continue;
    }
    if (game->game_over) {
      pthread_mutex_unlock(&game->mutex);
      break;
    }

    print_board_status(game);

    // Stage 4: update pouches every second based on own elemental counts.
    int e0 = elemental_count(game, 0);
    int e1 = elemental_count(game, 1);
    game->players[0].pebbles = (uint16_t)(game->players[0].pebbles + e0);
    game->players[1].pebbles = (uint16_t)(game->players[1].pebbles + e1);

    send_pebbles(game, 0);
    send_pebbles(game, 1);

    maybe_finish_by_depletion(game);
    pthread_mutex_unlock(&game->mutex);
  }

  return NULL;
}

static void *worker(void *args) {
  worker_ctx_t *ctx = (worker_ctx_t *)args;
  queue_t *queue = ctx->queue;
  gamestate_t *game = ctx->game;

  while (1) {
    queue_item_t item = queue_pop(queue);
    if (item.cmd_type == CMD_STOP) break;

    pthread_mutex_lock(&game->mutex);
    int over = game->game_over;
    pthread_mutex_unlock(&game->mutex);
    if (over) {
      queue_complete(queue);
      continue;
    }

    int cost = spell_cost(item.cast.spell);
    int enough = 0;

    pthread_mutex_lock(&game->mutex);
    if (game->players[item.player_id].pebbles >= (uint16_t)cost) {
      game->players[item.player_id].pebbles =
          (uint16_t)(game->players[item.player_id].pebbles - (uint16_t)cost);
      enough = 1;
    }
    pthread_mutex_unlock(&game->mutex);

    if (!enough) {
      pthread_mutex_lock(&game->mutex);
      if (!game->game_over) {
        printf("[tee hee] Not enough pebbles, %s!\n", game->players[item.player_id].name);
      }
      pthread_mutex_unlock(&game->mutex);
      ms_sleep(FAMILIAR_DELAY);
      queue_complete(queue);
      continue;
    }

    ms_sleep(FAMILIAR_DELAY);

    pthread_mutex_lock(&game->mutex);
    if (!game->game_over) {
      apply_spell_effect(game, item.player_id, &item.cast);
      printf("[Cast] %s casts %s onto %hu,%hu\n",
             game->players[item.player_id].name,
             spell_names[item.cast.spell],
             item.cast.X, item.cast.Y);

      // After a successful cast, always send current pebbles.
      send_pebbles(game, item.player_id);
      maybe_finish_by_depletion(game);
    }
    pthread_mutex_unlock(&game->mutex);

    queue_complete(queue);
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
  game.winner = -1;

  pthread_t worker_threads[THREAD_COUNT];
  worker_ctx_t wctx = {.queue = &queue, .game = &game};
  for (int i = 0; i < THREAD_COUNT; i++) {
    if (pthread_create(&worker_threads[i], NULL, worker, &wctx) != 0) {
      ERR("pthread_create worker");
    }
  }

  pthread_t judge_thread;
  judge_ctx_t jctx = {.game = &game};
  if (pthread_create(&judge_thread, NULL, judge, &jctx) != 0) {
    ERR("pthread_create judge");
  }

  char buff[MAX_BUFF_LEN + 1];
  memset(buff, 0, sizeof(buff));
  struct sockaddr_in addr;

  while (1) {
    pthread_mutex_lock(&game.mutex);
    int over = game.game_over;
    pthread_mutex_unlock(&game.mutex);
    if (over) break;

    socklen_t addr_len = sizeof(addr);
    int recv_len = recvfrom(fd, buff, MAX_BUFF_LEN, 0,
                            (struct sockaddr *)&addr, &addr_len);
    if (recv_len < 0) {
      if (errno == EINTR) continue;
      perror("recvfrom");
      continue;
    }

    if (recv_len != MAX_BUFF_LEN) {
      printf("Message of incorrect length\n");
      continue;
    }

    if (buff[0] == 'l') {
      pthread_mutex_lock(&game.mutex);
      if (game.game_started || game.players_count >= MAX_CLIENTS || find_player(&game, &addr) >= 0) {
        pthread_mutex_unlock(&game.mutex);
        continue;
      }

      login_message_t *msg = (login_message_t *)buff;
      msg->name[MAX_NAME_LENGTH] = '\0';
      player_t *p = &game.players[game.players_count];
      p->add = addr;
      strncpy(p->name, msg->name, MAX_NAME_LENGTH);
      p->name[MAX_NAME_LENGTH] = '\0';
      p->pebbles = START_PEBBLES;
      game.players_count++;
      printf("[Login] Welcome, %s\n", p->name);
      if (game.players_count == MAX_CLIENTS) {
        game.game_started = 1;
      }
      pthread_mutex_unlock(&game.mutex);
      continue;
    }

    if (buff[0] == 'c') {
      pthread_mutex_lock(&game.mutex);
      if (!game.game_started) {
        pthread_mutex_unlock(&game.mutex);
        continue;
      }
      int pid = find_player(&game, &addr);
      if (pid < 0) {
        pthread_mutex_unlock(&game.mutex);
        continue;
      }
      pthread_mutex_unlock(&game.mutex);

      cast_message_t *msg = (cast_message_t *)buff;
      msg->spell = ntohs(msg->spell);
      msg->X = ntohs(msg->X);
      msg->Y = ntohs(msg->Y);
      if (msg->spell >= SPELL_TYPES || msg->X >= BOARD_SIZE || msg->Y >= BOARD_SIZE) {
        printf("Value OUT OF RANGE\n");
        continue;
      }

      queue_item_t item;
      memset(&item, 0, sizeof(item));
      item.cmd_type = CMD_CAST;
      item.cast = *msg;
      item.player_id = pid;
      queue_push(&queue, &item);
      continue;
    }

    if (buff[0] == 'q') {
      pthread_mutex_lock(&game.mutex);
      if (!game.game_started) {
        pthread_mutex_unlock(&game.mutex);
        continue;
      }
      int pid = find_player(&game, &addr);
      if (pid < 0) {
        pthread_mutex_unlock(&game.mutex);
        continue;
      }
      int winner = (pid == 0) ? 1 : 0;
      printf("[Quit] %s quit. Goodbye!\n", game.players[pid].name);
      printf("-= Congratulations, %s, you win! =-\n", game.players[winner].name);
      game.winner = winner;
      game.game_over = 1;
      char w = 'w';
      char l = 'l';
      (void)sendto(game.fd, &w, 1, 0,
                   (const struct sockaddr *)&game.players[winner].add,
                   sizeof(struct sockaddr_in));
      (void)sendto(game.fd, &l, 1, 0,
                   (const struct sockaddr *)&game.players[pid].add,
                   sizeof(struct sockaddr_in));
      pthread_mutex_unlock(&game.mutex);
      break;
    }

    printf("Incorrect message type\n");
  }

  // Stop workers.
  pthread_mutex_lock(&queue.mutex);
  queue.stop_requested = 1;
  // Discard queued commands when game ends.
  queue.head = 0;
  queue.tail = 0;
  queue.count = 0;
  pthread_mutex_unlock(&queue.mutex);
  for (int i = 0; i < THREAD_COUNT; i++) {
    sem_post(&queue.sem);
  }

  for (int i = 0; i < THREAD_COUNT; i++) {
    pthread_join(worker_threads[i], NULL);
  }
  pthread_join(judge_thread, NULL);

  pthread_mutex_destroy(&game.mutex);
  queue_destroy(&queue);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    usage(argv[0]);
  }
  uint16_t port = (uint16_t)atoi(argv[1]);
  int fd = bind_inet_socket(port, SOCK_DGRAM, BACKLOG);
  doServer(fd);
  if (close(fd) < 0) {
    ERR("close");
  }
  return 0;
}
