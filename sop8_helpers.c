#include "l8_common.h"



#define SOP8_MAX_IP_STR 64

typedef struct {
  int used;
  struct sockaddr_in addr;
  int32_t last_seq;
} sop8_peer_slot_t;

/*UDP socket/address helpers*/

int sop8_make_udp_socket(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    ERR("socket");
  }
  return fd;
}

int sop8_bind_udp_server(uint16_t port) {
  return bind_inet_socket(port, SOCK_DGRAM, 16);
}

struct sockaddr_in sop8_make_inet_addr(const char *ip, uint16_t port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    ERR("inet_pton");
  }
  return addr;
}

int sop8_addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b) {
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

void sop8_addr_to_string(const struct sockaddr_in *addr, char *out, size_t out_size) {
  char ip[SOP8_MAX_IP_STR];
  const char *res = inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
  if (!res) {
    snprintf(out, out_size, "<invalid-addr>");
    return;
  }
  snprintf(out, out_size, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

/*Datagram send/recv helpers*/

ssize_t sop8_recv_datagram(int fd, void *buf, size_t buf_len, struct sockaddr_in *src) {
  socklen_t src_len = sizeof(*src);
  return TEMP_FAILURE_RETRY(recvfrom(fd, buf, buf_len, 0, (struct sockaddr *)src, &src_len));
}

ssize_t sop8_send_datagram(int fd, const void *buf, size_t len, const struct sockaddr_in *dst) {
  return TEMP_FAILURE_RETRY(sendto(fd, buf, len, 0, (const struct sockaddr *)dst, sizeof(*dst)));
}

int sop8_set_recv_timeout_ms(int fd, int ms) {
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int sop8_set_send_timeout_ms(int fd, int ms) {
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/*Byte-order/int field helpers*/

uint16_t sop8_read_u16_be(const void *p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return ntohs(v);
}

void sop8_write_u16_be(void *p, uint16_t v) {
  uint16_t be = htons(v);
  memcpy(p, &be, sizeof(be));
}

int32_t sop8_read_i32_be(const void *p) {
  int32_t v;
  memcpy(&v, p, sizeof(v));
  return (int32_t)ntohl((uint32_t)v);
}

void sop8_write_i32_be(void *p, int32_t v) {
  uint32_t be = htonl((uint32_t)v);
  memcpy(p, &be, sizeof(be));
}



/*
 * Convenience ACK helper used in many UDP reliability tasks:
 * send back exactly what was received.
 */
int sop8_send_ack_echo(int fd, const void *pkt, size_t pkt_len, const struct sockaddr_in *dst) {
  return sop8_send_datagram(fd, pkt, pkt_len, dst) == (ssize_t)pkt_len ? 0 : -1;
}

/*Peer table helpers (dedup/reordering tasks)*/

void sop8_peer_table_init(sop8_peer_slot_t *slots, size_t n) {
  for (size_t i = 0; i < n; i++) {
    slots[i].used = 0;
    slots[i].last_seq = 0;
    memset(&slots[i].addr, 0, sizeof(slots[i].addr));
  }
}

/*
 * Find existing peer by src addr, or create a new slot.
 * Returns index on success, -1 if no slot available.
 */
int sop8_peer_find_or_add(sop8_peer_slot_t *slots, size_t n, const struct sockaddr_in *src) {
  int free_idx = -1;
  for (size_t i = 0; i < n; i++) {
    if (slots[i].used) {
      if (sop8_addr_equal(&slots[i].addr, src)) return (int)i;
    } else if (free_idx < 0) {
      free_idx = (int)i;
    }
  }
  if (free_idx >= 0) {
    slots[free_idx].used = 1;
    slots[free_idx].addr = *src;
    slots[free_idx].last_seq = 0;
  }
  return free_idx;
}

/*
 * Sequence classification for per-peer reliable UDP protocols.
 * Returns:
 *  -1 => old/duplicate (seq <= last_seq)
 *   0 => next expected (seq == last_seq + 1)
 *   1 => future/out-of-order (seq > last_seq + 1)
 */
int sop8_classify_seq(int32_t last_seq, int32_t seq) {
  if (seq <= last_seq) return -1;
  if (seq == last_seq + 1) return 0;
  return 1;
}

/* Update last_seq once a packet is accepted as next expected. */
void sop8_accept_seq(sop8_peer_slot_t *slot, int32_t seq) {
  slot->last_seq = seq;
}
