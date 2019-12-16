#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXBUF 100

// 内存池初始大小200M
#define MAXPOOLNUM 10
#define POOLSIZE (200 * 1024 * 1024L)
#define CHUNKSIZE MAXBUF

// 默认端口 4789
static unsigned int myport = 4789;
static int listener;

// 内存池相关结构体定义
typedef union node {
  union node *next;
  char client_data[1];
} uChunk, *puChunk;

typedef struct {
  void *hdr[MAXPOOLNUM];
  int poolnum;
  puChunk start;
  puChunk end;
} stMemPool;

static stMemPool sMemPool;
static pthread_mutex_t malloc_lock;
static pthread_mutex_t free_lock;

// 模仿stl实现的内存池
void mem_init(void) {
  // 申请200M内存
  void *ret = malloc(POOLSIZE);
  if (NULL == ret) {
    printf("memory init error\n");
    exit(1);
  }

  int chunknum = POOLSIZE / CHUNKSIZE;
  puChunk cur = ret;
  puChunk next = ret;

  // 将内存块按照 CHUNKSIZE 一个分割，然后，各个小块用单链表连起来
  for (int i = 1;; i++) {
    cur = next;
    next = (puChunk)((char *)next + CHUNKSIZE);

    if (chunknum == i) {
      cur->next = NULL;
      break;
    } else {
      cur->next = next;
    }
  }

  sMemPool.start = (puChunk)ret;
  sMemPool.end = cur;
  sMemPool.poolnum = 1;
  sMemPool.hdr[0] = ret;

  pthread_mutex_init(&malloc_lock, NULL);
  pthread_mutex_init(&free_lock, NULL);
}

// 申请内存。从链表头部取出一块内存
void *mem_malloc(void) {
  pthread_mutex_lock(&malloc_lock);
  void *result = NULL;

  if (sMemPool.start != sMemPool.end) {
    result = sMemPool.start;
    sMemPool.start = sMemPool.start->next;
  } else //内存池已满，重新申请一大块内存
  {
    if (sMemPool.poolnum > MAXPOOLNUM) {
      printf("memory pool is too big");
      exit(1);
    }
    void *ret = malloc(POOLSIZE);
    if (NULL == ret) {
      perror("malloc");
      exit(1);
    }

    int chunknum = POOLSIZE / CHUNKSIZE;
    puChunk cur = ret;
    puChunk next = ret;

    for (int i = 1;; i++) {
      cur = next;
      next = (puChunk)((char *)next + CHUNKSIZE);

      if (chunknum == i) {
        cur->next = NULL;
        break;
      } else {
        cur->next = next;
      }
    }

    result = sMemPool.start;
    sMemPool.start = (puChunk)ret;
    sMemPool.end = cur;
    sMemPool.poolnum++;
    sMemPool.hdr[sMemPool.poolnum - 1] = ret;
  }
  pthread_mutex_unlock(&malloc_lock);
  return result;
}

// 释放内存。将内存放到链表底部
void mem_free(void *p) {
  pthread_mutex_lock(&free_lock);
  sMemPool.end->next = (puChunk)p;
  sMemPool.end = (puChunk)p;
  pthread_mutex_unlock(&free_lock);
}

void mem_destroy(void) {
  for (int i = 0; i < sMemPool.poolnum; i++) {
    free(sMemPool.hdr[i]);
  }
  pthread_mutex_destroy(&malloc_lock);
  pthread_mutex_destroy(&free_lock);
}

/*
 *线程池里所有运行和等待的任务都是一个CThread_worker
 *由于所有任务都在链表里，所以是一个链表结构
 */
typedef struct worker {
  /*回调函数，任务运行时会调用此函数，注意也可声明成其它形式*/
  void *(*process)(void *arg);
  void *arg; /*回调函数的参数*/
  struct worker *next;
} CThread_worker;

/*线程池结构*/
typedef struct {
  pthread_mutex_t queue_lock;
  pthread_cond_t queue_ready;

  /*链表结构，线程池中所有等待任务*/
  CThread_worker *queue_head;
  CThread_worker *queue_tail;

  /*是否销毁线程池*/
  int shutdown;
  pthread_t *threadid;
  /*线程池中允许的活动线程数目*/
  int max_thread_num;
  /*当前等待队列的任务数目*/

} CThread_pool;

typedef struct args {
  int fd;
  struct sockaddr *pclient_addr;
  int len;
  char *buf;
} * pArg, stArg;

int pool_add_worker(void *(*process)(void *arg), void *arg);
void *thread_routine(void *arg);

static CThread_pool *pool = NULL;
void pool_init(int max_thread_num) {
  pool = (CThread_pool *)malloc(sizeof(CThread_pool));

  pthread_mutex_init(&(pool->queue_lock), NULL);
  pthread_cond_init(&(pool->queue_ready), NULL);

  pool->queue_head = pool->queue_tail = NULL;

  pool->max_thread_num = max_thread_num;

  pool->shutdown = 0;

  pool->threadid = (pthread_t *)malloc(max_thread_num * sizeof(pthread_t));
  int i = 0;
  for (i = 0; i < max_thread_num; i++) {
    pthread_create(&(pool->threadid[i]), NULL, thread_routine, NULL);
  }
}

/*向线程池中加入任务*/
int pool_add_worker(void *(*process)(void *arg), void *arg) {
  /*构造一个新任务*/
  CThread_worker *newworker = (CThread_worker *)mem_malloc();
  newworker->process = process;
  newworker->arg = arg;
  newworker->next = NULL; /*别忘置空*/

  pthread_mutex_lock(&(pool->queue_lock));

  if (pool->queue_head != NULL) {
    pool->queue_tail->next = newworker;
    pool->queue_tail = newworker;
  } else {
    pool->queue_head = pool->queue_tail = newworker;
  }

  pthread_mutex_unlock(&(pool->queue_lock));
  pthread_cond_signal(&(pool->queue_ready));
  return 0;
}

/*销毁线程池，等待队列中的任务不会再被执行，但是正在运行的线程会一直
把任务运行完后再退出*/
int pool_destroy() {
  if (pool->shutdown)
    return -1; /*防止两次调用*/
  pool->shutdown = 1;

  /*唤醒所有等待线程，线程池要销毁了*/
  pthread_cond_broadcast(&(pool->queue_ready));

  /*阻塞等待线程退出，否则就成僵尸了*/
  int i;
  for (i = 0; i < pool->max_thread_num; i++)
    pthread_join(pool->threadid[i], NULL);
  free(pool->threadid);

  /*销毁等待队列*/
  CThread_worker *head = NULL;
  while (pool->queue_head != NULL) {
    head = pool->queue_head;
    pool->queue_head = pool->queue_head->next;
    mem_free(head);
  }
  /*条件变量和互斥量也别忘了销毁*/
  pthread_mutex_destroy(&(pool->queue_lock));
  pthread_cond_destroy(&(pool->queue_ready));

  free(pool);
  pool = NULL;

  return 0;
}

void *thread_routine(void *arg) {
  while (1) {
    pthread_mutex_lock(&(pool->queue_lock));
    /*如果等待队列为0并且不销毁线程池，则处于阻塞状态; 注意
    pthread_cond_wait是一个原子操作，等待前会解锁，唤醒后会加锁*/
    while (pool->queue_head == NULL && !pool->shutdown) {
      pthread_cond_wait(&(pool->queue_ready), &(pool->queue_lock));
    }

    /*线程池要销毁了*/
    if (pool->shutdown) {
      /*遇到break,continue,return等跳转语句，千万不要忘记先解锁*/
      pthread_mutex_unlock(&(pool->queue_lock));
      pthread_exit(NULL);
    }

    /*等待队列长度减去1，并取出链表中的头元素*/
    CThread_worker *worker = pool->queue_head;
    pool->queue_head = worker->next;
    pthread_mutex_unlock(&(pool->queue_lock));

    /*调用回调函数，执行任务*/
    (*(worker->process))(worker->arg);
    mem_free(worker);
    worker = NULL;
  }

  pthread_exit(NULL);
}

void *thread_send(void *parg) {
  pArg arg = (pArg)parg;
  sendto(arg->fd, arg->buf, arg->len, 0, (struct sockaddr *)arg->pclient_addr,
         sizeof(struct sockaddr));
  mem_free(arg->buf);
  mem_free(arg->pclient_addr);
  mem_free(arg);
}

int main(int argc, char **argv) {
  struct sockaddr_in my_addr, client_addr;
  int addr_size = sizeof(client_addr);

  if (argc > 1)
    myport = atoi(argv[1]);

  if ((listener = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
    perror("socket create failed");
    exit(1);
  }

  int opt = SO_REUSEADDR;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  bzero(&my_addr, sizeof(my_addr));
  my_addr.sin_family = PF_INET;
  my_addr.sin_port = htons(myport);
  my_addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(listener, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) ==
      -1) {
    perror("bind");
    exit(1);
  }

  mem_init();
  // 线程池初始线程个数为5
  pool_init(5);

  // while循环接受数据，用线程来发送数据
  while (1) {
    char *buf = mem_malloc();
    int ret = recvfrom(listener, buf, MAXBUF, 0,
                       (struct sockaddr *)&client_addr, &addr_size);

    pArg arg = (pArg)mem_malloc();
    arg->fd = listener;
    struct sockaddr *client = (struct sockaddr *)mem_malloc();
    memcpy(client, &client_addr, sizeof(struct sockaddr));
    arg->pclient_addr = client;
    arg->len = ret;
    arg->buf = buf;
    pool_add_worker(thread_send, arg);
  }

  pool_destroy();
  mem_destroy();

  close(listener);

  return 0;
}
