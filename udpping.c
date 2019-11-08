#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#define TABLEBIT 16
#define DELAYTABLE (1<<TABLEBIT)
#define TABLEINDEX(idx) (idx-((idx>>TABLEBIT)<<TABLEBIT))

#define ERR_EXIT(m) \
do \
{ \
    perror(m); \
    exit(EXIT_FAILURE); \
} while(0)

static char* s_pDstIp;
static int s_iDstPort;
static int s_iInterval;
static int s_iDataLen;
static int s_iSock;
static unsigned long s_luCount;
struct sockaddr_in s_servaddr;
static unsigned long s_luSeq;
static unsigned long s_luSentCount;
static struct itimerval sst_usec_delay;

// 记录发出去的UDP包序列
struct
{
    unsigned long seq;
} stDelayTable[DELAYTABLE];

// 安装信号
void (*Signal(int signo, void (*func)(int)))(int)
{
    struct sigaction act, oact;

    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0; /* So if set SA_RESETHAND is cleared */

    if (sigaction(signo, &act, &oact) == -1) 
        return SIG_ERR;
    return (oact.sa_handler);
}

// 解析参数
void parseArg(int argc, char** argv)
{
    if (argc < 5)
    {
        printf("Usage: %s {dstip} {dstport} {interval(microsecond)} {datalen(24-100)} {count} \n", argv[0]);
        exit(0);
    }

    s_pDstIp = argv[1];
    s_iDstPort = atoi(argv[2]);
    s_iInterval = atoi(argv[3]);
    s_iDataLen = atoi(argv[4]);
    if (s_iDataLen < 24 || s_iDataLen > 100)
    {
        printf("datalen is wrong.\n");
        exit(0);
    }
    s_luCount = atol(argv[5]);
}

// 初始化socket客户端、设定发包间隔
void init(void)
{
    if ((s_iSock = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
    {
        ERR_EXIT("socket");
    }

    memset(&s_servaddr, 0, sizeof(s_servaddr));
    s_servaddr.sin_family = AF_INET;
    s_servaddr.sin_port = htons(s_iDstPort);
    s_servaddr.sin_addr.s_addr = inet_addr(s_pDstIp);

    sst_usec_delay.it_value.tv_sec = s_iInterval/1000000;
    sst_usec_delay.it_value.tv_usec = s_iInterval%1000000;
    sst_usec_delay.it_interval.tv_sec = 0;
    sst_usec_delay.it_interval.tv_usec = 0;
}

void Exit(int signal_id)
{
    exit(0);
}

// 已经发包的数据，放在这个hash表，供查询计算rtt
// void addDelayTable(void)
// {
//     unsigned int idx = TABLEINDEX(s_luSentCount);

//     struct timeval tv;
//     gettimeofday(&tv, NULL);

//     stDelayTable[idx].seq = s_luSentCount;
//     stDelayTable[idx].sec = tv.tv_sec;
//     stDelayTable[idx].usec = tv.tv_usec;
// }

void sendData(int signal_id)
{
    int errno_save = errno;
    char buf[100] = {0};

    Signal(SIGALRM, sendData);

    // 第一个8个字节存序列号，后面16字节存时间戳
    memcpy(buf, &s_luSentCount, sizeof(s_luSentCount));
    struct timeval tv;
    gettimeofday(&tv, NULL);
    memcpy(buf+sizeof(s_luSentCount), &tv, sizeof(struct timeval));

    srand(time(NULL));
    for (int len=sizeof(s_luSentCount)+sizeof(struct timeval); len<s_iDataLen; len++)
    {
        buf[len] = rand()%57+65;
    }
    
    // 记录发出的包的序列号到hash表
    stDelayTable[TABLEINDEX(s_luSentCount)].seq = s_luSentCount;
    if (-1 == sendto(s_iSock, buf, s_iDataLen, 0, (struct sockaddr *)&s_servaddr, sizeof(s_servaddr)))
    {
        ERR_EXIT("sendto");
    }

    // 达到指定的发包个数，停止发包。退出前，设置1s缓冲时间，用来收包
    s_luSentCount++;
    if (s_luSentCount >= s_luCount)
    {
        Signal(SIGALRM, Exit);
        alarm(1);
    }
    else
    {
        setitimer(ITIMER_REAL, &sst_usec_delay, NULL);
    }

    errno = errno_save;
}

void recvData(void)
{
    char recvbuf[100] = {0};

    if (-1 == recvfrom(s_iSock, recvbuf, sizeof(recvbuf), 0, NULL, NULL))
    {
        if (errno == EINTR)
        {
            return;
        }
        ERR_EXIT("recvfrom");
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);

    unsigned long seq;
    memcpy(&seq, recvbuf, sizeof(seq));
    int idx = TABLEINDEX(seq);
    // 接受的包，在hash表里没找到，认为是重复的包
    if (seq != stDelayTable[idx].seq)
    {
        printf("DUP! ");
    }
    
    struct timeval start;
    memcpy(&start, recvbuf+sizeof(seq), sizeof(struct timeval));
    printf("cur=%ld.%ld, seq=%lu, time=%.2lf ms\n", tv.tv_sec, tv.tv_usec, seq, (tv.tv_sec-start.tv_sec)*1000+(double)(tv.tv_usec-start.tv_usec)/1000);
}

int main(int argc, char** argv)
{
    parseArg(argc, argv);
    init();

    Signal(SIGALRM, sendData);
    kill(getpid(), SIGALRM);

    while (1)
    {
        recvData();
    }

    close(s_iSock);
    
    return 0;
}
