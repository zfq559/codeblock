#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXEVENTNUM 5000
#define LISTENNUM 5000
#define BUFSIZE 64

int g_iServPort = 9000;

typedef struct {
  int sockfd;
  int len;
  char buf[BUFSIZE];
} stPtr;

void setNoblock(int sock) {
  int opt = 0;

  opt = fcntl(sock, F_GETFL, 0);
  if (opt < 0) {
    perror("fcntl GETFL");
    exit(1);
  }

  if (fcntl(sock, F_SETFL, opt | O_NONBLOCK) < 0) {
    perror("fcntl SETFL");
    exit(1);
  }
}

int main(int argc, char **argv) {
  int listenfd, connfd, epfd;

  if (argc == 2 && (g_iServPort = atoi(argv[1])) < 0) {
    fprintf(stderr, "Usage: %s {portnumber}\n", argv[0]);
    return 1;
  }

  struct sockaddr_in clientaddr, serveraddr;
  socklen_t clientlen = sizeof(struct sockaddr);

  if (-1 == (listenfd = socket(AF_INET, SOCK_STREAM, 0))) {
    perror("socket");
    exit(1);
  }
  int opt = 1;
  if (-1 == setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    perror("setsockopt");
    exit(1);
  }
  setNoblock(listenfd);

  bzero(&serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = (in_port_t)htons(g_iServPort);

  if (-1 ==
      bind(listenfd, (struct sockaddr *)&serveraddr, sizeof(struct sockaddr))) {
    perror("bind");
    exit(1);
  }

  struct epoll_event ev, events[5000];
  epfd = epoll_create(5000);
  if (epfd < 0) {
    perror("epoll_create");
    exit(1);
  }

  stPtr pdata;
  pdata.sockfd = listenfd;

  ev.data.ptr = &pdata;
  ev.events = EPOLLIN;
  if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev)) {
    perror("epoll_ctl add listenfd");
    exit(1);
  }

  listen(listenfd, 5000);

  int recvlen = 0;
  int sockfd = 0;
  char buf[BUFSIZE] = {0};
  while (1) {
    int nfds = epoll_wait(epfd, events, 5000, -1);
    if (nfds < 0) {
      perror("epoll_wait");
      exit(1);
    }

    for (int i = 0; i < nfds; i++) {
      if (((stPtr *)events[i].data.ptr)->sockfd == listenfd) {
        connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
        if (connfd < 0) {
          perror("accept");
          exit(1);
        }
        setNoblock(connfd);

        stPtr *p = (stPtr *)malloc(sizeof(stPtr));
        p->sockfd = connfd;
        ev.data.ptr = p;
        ev.events = EPOLLIN;

        if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev) == -1) {
          perror("epoll_ctl add connfd");
          exit(1);
        }
      } else if (events[i].events & EPOLLIN) {
        stPtr *p = (stPtr *)(events[i].data.ptr);
        bzero(p->buf, BUFSIZE);
        sockfd = p->sockfd;

        if ((p->len = recv(sockfd, p->buf, BUFSIZE, 0)) < 0) {
          if (errno == ECONNRESET) {
            if (-1 == epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, NULL)) {
              perror("epoll_ctl EPOLL_CTL_DEL");
              exit(1);
            }
            free(p);
            close(sockfd);
            continue;
          } else {
            perror("recv");
            exit(1);
          }
        } else if (0 == p->len) {
          if (-1 == epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, NULL)) {
            perror("epoll_ctl EPOLL_CTL_DEL");
            exit(1);
          }
          free(p);
          close(sockfd);
          continue;
        }

        ev.data.ptr = p;
        ev.events = EPOLLOUT;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev) == -1) {
          perror("epoll_ctl EPOLL_CTL_MOD to EPOLLOUT");
          exit(1);
        }
      } else if (events[i].events & EPOLLOUT) {
        printf("send\n");
        stPtr *p = (stPtr *)(events[i].data.ptr);
        sockfd = p->sockfd;

        if (send(sockfd, p->buf, p->len, MSG_DONTWAIT) < 0) {
          perror("send");
          exit(1);
        }

        ev.data.ptr = p;
        ev.events = EPOLLIN;
        if (-1 == epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev)) {
          perror("epoll_ctl EPOLL_CTL_MOD to EPOLLIN");
          exit(1);
        }
      }
    }
  }

  close(listenfd);
  close(epfd);

  return 0;
}
