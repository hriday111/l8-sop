#include "l8_common.h"

int bind_udp_client_socket(const char *local_ip, uint16_t local_port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) ERR("socket");

  struct sockaddr_in local_addr;
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons(local_port);

  if (local_ip == NULL || strcmp(local_ip, "0.0.0.0") == 0 || strcmp(local_ip, "*") == 0) {
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    if (inet_pton(AF_INET, local_ip, &local_addr.sin_addr) != 1) {
      close(fd);
      ERR("inet_pton");
    }
  }

  if (bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
    close(fd);
    ERR("bind");
  }

  return fd;
}

void usage(char *name) {
  fprintf(stderr, "Usage: %s <server_ip> <server_port> <message>\n", name);
  exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
  if (argc != 2) usage(argv[0]);

  const char *server_ip = "127.0.0.1";//argv[1];
  uint16_t server_port = 2000;//(uint16_t)atoi(argv[2]);
  const char *msg = argv[1];

  // 1) Create + bind client socket (ephemeral local port)
  int fd = bind_udp_client_socket("0.0.0.0", 0);

  // 2) Prepare server address
  struct sockaddr_in srv;
  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_port = htons(server_port);
  if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1) ERR("inet_pton server");

  // 3) Send datagram
  if (sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
    ERR("sendto");
  }

  if (close(fd) < 0) ERR("close");
  return 0;
}