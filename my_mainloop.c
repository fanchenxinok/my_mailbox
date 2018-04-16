#define WRAPER_SYSAPI (0)
#if WRAPER_SYSAPI
#define _GNU_SOURCE
#include <dlfcn.h>
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <stdarg.h>
#include <errno.h>

typedef enum{
	FALSE,
	TRUE
}enBool;

typedef enum{
	STATUS_INIT,
	STATUS_READY,
	STATUS_POLL,
	STATUS_QUIT
}enStatus;

typedef enum
{
	AVAIL_STATUS_FREE,
	AVAIL_STATUS_USED
}enAvailableStatus;

typedef enum{
	EVENT_READ = 0x01,
	EVENT_WRITE = 0x02,
	EVENT_PRI = 0x04, /*表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来*/
	EVENT_ERR = 0x08,
	EVENT_HUP = 0x0A, /*表示对应的文件描述符被挂断；*/
	EVENT_ET = 0x10, /*将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)来说的。*/
	EVENT_ONESHOT = 0x20 /*只监听一次事件*/
}enPollEvent;

typedef struct{
	enStatus status;   /* main loop status */
	struct epoll_event *pEvents; /* epoll events array pointer */
	int epoll_id;      /* epoll id */
	int fire_events;   /* event number of shot */
	int time_out;      /* epoll_wait time out */
	void *pUserData;
}stMyMainLoop;

typedef int (*pollFunc)(int fd, int events, void *pUserData);

typedef struct{
	int fd;
	pollFunc pPollFunc;
	enAvailableStatus used;  /* 0: unused, 1: used */
}stPollFucMap;

#define LOOP_BUFF_MAX_IDX (64)
#define LOOP_BUFF_ITEAM_MAX_LEN (256)
typedef struct{
	int readIdx;
	int writeIdx;
	char buffer[LOOP_BUFF_MAX_IDX][LOOP_BUFF_ITEAM_MAX_LEN];
}stLoopBuff;

#define MAX_EVENTS_NUM (128)
static struct epoll_event events[MAX_EVENTS_NUM] = {0};
static stPollFucMap pollFuncs[MAX_EVENTS_NUM] = {0};
static stLoopBuff loopBuff = {0};

typedef enum
{
	READ_PIPE,
	WRITE_PIPE,
	MAX_PIPE
}enPipeType;

typedef enum
{
	MAIL_BOX_ID1,
	MAIL_BOX_ID2,
	MAIL_BOX_ID3,
	MAIL_BOX_NUM
}enMailBoxId;

typedef struct
{
	stMyMainLoop *pMainLoop;
	int pipe_fd[MAX_PIPE];
	enAvailableStatus state;
}stMyMailBox;

typedef struct
{
	int eventType;
	char eventData[256];
}stMyMsg;

typedef int (*handleMsg)(stMyMsg *pMsg);

#define MAX_MAIL_BOX_NUM (10)
static stMyMailBox s_my_mailbox[MAIL_BOX_NUM] = {0};
static pthread_mutex_t s_mailbox_mutex;

#define CHECK_POINTER_NULL(pointer, ret) \
    do{\
        if(pointer == NULL){ \
            printf("[main loop] check '%s' is NULL at:%u\n", #pointer, __LINE__);\
            return ret; \
        }\
    }while(0)

#define CHECK_CONDITION(condition) \
	do{\
		if(!condition){\
			printf("[main loop] check condition [%s] fail at:%d\n", #condition, __LINE__);\
			return; \
		}\
	}while(0)

#define CHECK_CONDITION_RET(condition, ret) \
	do{\
		if(condition){\
			printf("[main loop] check condition [%s] return, at:%d\n", #condition, __LINE__);\
			return ret; \
		}\
	}while(0)

#define SAFETY_MUTEX_LOCK(mutex) \
do{ \
	pthread_cleanup_push(pthread_mutex_unlock, &(mutex)); \
	pthread_mutex_lock(&(mutex))
		
#define SAFETY_MUTEX_UNLOCK(mutex) \
	pthread_mutex_unlock(&mutex); \
	pthread_cleanup_pop(0); \
}while(0)


#if WRAPER_SYSAPI
#define MAX_DEPTH (20)
void my_backtrace(void)
{
	void *buffer[MAX_DEPTH];
	int nptrs = backtrace(buffer, MAX_DEPTH);
	char **stack = backtrace_symbols(buffer, nptrs);
	int i;
	if (stack){
		for (i = 0; i < nptrs; ++i){
			printf("[BT][%d] bt %s\n", i, stack[i]);
		}
		free(stack);
	}
	return;
}

int open(const char *path, int oflags, mode_t mode)
{
	static int (*real_open)(const char *, int, mode_t) = NULL;
	void *ptr = 0;
	if(real_open == NULL){
		real_open = dlsym(RTLD_NEXT, "open");
	}

	int fd = real_open(path, oflags, mode);

	printf("[O]open!![%d], path=%s\n", fd, path);
	my_backtrace();
	return fd;
}

int close(int fd)
{
	static int (*real_close)(int) = NULL;
	void *ptr = 0;
	if(real_close == NULL){
		real_close = dlsym(RTLD_NEXT, "close");
	}

	printf("[C]close!![%d]\n", fd);
	my_backtrace();

	return real_close(fd);
}
#endif


static void epollAddEvent(int epollFd, int addFd, int state)
{
	struct epoll_event ev;
	ev.events = state;
	ev.data.fd = addFd;
	epoll_ctl(epollFd, EPOLL_CTL_ADD, addFd, &ev);
	return;
}

static void epollDeleteEvent(int epollFd, int deleteFd, int state)
{
	struct epoll_event ev;
	ev.events = state;
	ev.data.fd = deleteFd;
	epoll_ctl(epollFd, EPOLL_CTL_DEL, deleteFd, &ev);
	return;
}

static void epollModifyEvent(int epollFd, int modifyFd, int state)
{
	struct epoll_event ev;
	ev.events = state;
	ev.data.fd = modifyFd;
	epoll_ctl(epollFd, EPOLL_CTL_MOD, modifyFd, &ev);
	return;
}

static int getEpollState(int pollState)
{
	return (((pollState & EVENT_READ) ? EPOLLIN : 0) |
		((pollState & EVENT_WRITE) ? EPOLLOUT : 0) |
		((pollState & EVENT_PRI) ? EPOLLPRI : 0) |
		((pollState & EVENT_ERR) ? EPOLLERR : 0) |
		((pollState & EVENT_HUP) ? EPOLLHUP : 0) |
		((pollState & EVENT_ET) ? EPOLLET : 0) |
		((pollState & EVENT_ONESHOT) ? EPOLLONESHOT : 0));
}

static int registUserPollFunc(int userFd, pollFunc func)
{
	int i= 0;
	for(; i < MAX_EVENTS_NUM; i++){
		if(pollFuncs[i].used == AVAIL_STATUS_FREE){
			pollFuncs[i].used = AVAIL_STATUS_USED;
			pollFuncs[i].fd = userFd;
			pollFuncs[i].pPollFunc = func;
			break;
		}
	}

	if(i >= MAX_EVENTS_NUM){
		return -1;
	}
	return 0;
}

static int unRegistUserPollFunc(int userFd)
{
	int i= 0;
	for(; i < MAX_EVENTS_NUM; i++){
		if(pollFuncs[i].fd == userFd){
			pollFuncs[i].used = AVAIL_STATUS_FREE;
			pollFuncs[i].fd = 0;
			pollFuncs[i].pPollFunc = NULL;
			break;
		}
	}
	return 0;
}

const pollFunc getUserPollFunc(int userFd)
{
	int i = 0;
	for(; i < MAX_EVENTS_NUM; i++){
		//printf("userFd=%d, pollFuncs[i].fd=%d\n", userFd, pollFuncs[i].fd);
		if(userFd == pollFuncs[i].fd){
			return pollFuncs[i].pPollFunc;
		}
	}
	return NULL;
}

static int defaultPollFunc(int fd, int events, void *pUserData)
{
	if(events & EPOLLIN){
		stMyMsg msg = {0};
		read(fd, &msg, sizeof(stMyMsg)); 
		handleMsg pHandleMsgFunc = (handleMsg)pUserData;
		if(pHandleMsgFunc != NULL){
			pHandleMsgFunc(&msg);
		}
	}
	return 0;
}

static void initLoopBuff()
{
	loopBuff.readIdx = 0;
	loopBuff.writeIdx = 0;
	memset(loopBuff.buffer, '\0', sizeof(char)*LOOP_BUFF_MAX_IDX*LOOP_BUFF_ITEAM_MAX_LEN);
	return;
}

static enBool checkLoopBuffEmpty()
{
	return (loopBuff.readIdx == loopBuff.writeIdx) ? TRUE : FALSE;
}

static enBool checkLoopBuffFull()
{
	return ((loopBuff.writeIdx+1)%LOOP_BUFF_MAX_IDX == loopBuff.readIdx) ? TRUE : FALSE;
}

static void calLoopBuffNextIdx(int *index)
{
	*index = (*index + 1) % LOOP_BUFF_MAX_IDX;
	return;
}

enBool readLoopBuff(char *pOut)
{
	CHECK_POINTER_NULL(pOut, FALSE);
	if(checkLoopBuffEmpty()){
		printf("loop buff have not data to read, readIdx=%d,writeIdx=%d\n",
			loopBuff.readIdx,loopBuff.writeIdx);
		return FALSE;
	}
	memcpy(pOut, loopBuff.buffer[loopBuff.readIdx], sizeof(char)*LOOP_BUFF_ITEAM_MAX_LEN);
	calLoopBuffNextIdx(&loopBuff.readIdx);
	return TRUE;
}

enBool writeLoopBuff(char *pIn, int len)
{
	CHECK_POINTER_NULL(pIn, FALSE);
	if(checkLoopBuffFull()){
		printf("loop buff have not space to write, readIdx=%d,writeIdx=%d\n",
			loopBuff.readIdx,loopBuff.writeIdx);
		return FALSE;
	}
	if(len > LOOP_BUFF_ITEAM_MAX_LEN)
		len = LOOP_BUFF_ITEAM_MAX_LEN;
	memcpy(loopBuff.buffer[loopBuff.readIdx], pIn, sizeof(char)*len);
	calLoopBuffNextIdx(&loopBuff.writeIdx);
	return TRUE;
}

stMyMainLoop* my_mainloop_new()
{
	stMyMainLoop *pMainLoop = (stMyMainLoop*)malloc(sizeof(stMyMainLoop));
	CHECK_POINTER_NULL(pMainLoop, NULL);
	pMainLoop->status = STATUS_INIT;
	pMainLoop->fire_events = 0;
	pMainLoop->pEvents = events;
	pMainLoop->epoll_id = epoll_create(MAX_EVENTS_NUM);
	pMainLoop->time_out = -1;
	pMainLoop->pUserData = NULL;
	//memset(pollFuncs, 0, sizeof(stPollFucMap) * MAX_EVENTS_NUM);
	return pMainLoop;
}

int my_mainloop_free(stMyMainLoop *pMainLoop)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	pMainLoop->status = STATUS_QUIT;
	close(pMainLoop->epoll_id);
	pMainLoop->epoll_id = -1;
	free(pMainLoop);
	pMainLoop = NULL;
	return 0;
}

int my_mainloop_prepare(stMyMainLoop *pMainLoop)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	CHECK_CONDITION_RET(pMainLoop->status != STATUS_INIT, -1);
	printf("my_mainloop_prepare ....!\n");
	memset(pMainLoop->pEvents, 0, sizeof(struct epoll_event) * MAX_EVENTS_NUM);
	pMainLoop->status = STATUS_READY;
	return 0;
}

int my_mainloop_poll(stMyMainLoop *pMainLoop)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	CHECK_CONDITION_RET(pMainLoop->status != STATUS_READY, -1);
	CHECK_CONDITION_RET(pMainLoop->epoll_id == -1, -1);
	pMainLoop->fire_events = epoll_wait(pMainLoop->epoll_id, pMainLoop->pEvents, MAX_EVENTS_NUM, pMainLoop->time_out);
	if(pMainLoop->fire_events < 0){
		perror("epoll_wait: ");
		printf("pMainLoop->epoll_id=%d\n", pMainLoop->epoll_id);
	}
	if(pMainLoop->status == STATUS_QUIT){
		return -1;
	}
	pMainLoop->status = STATUS_POLL;
	return 0;
}

int my_mainloop_dispatch(stMyMainLoop *pMainLoop)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	CHECK_CONDITION_RET(pMainLoop->status != STATUS_POLL, -1);
	pollFunc pFunc = NULL;
	int i = 0;
	printf("firing events' num = %d, errno = %d\n", pMainLoop->fire_events, errno);
	for(; i < pMainLoop->fire_events; i++){
		printf("event fd = %d\n", pMainLoop->pEvents[i].data.fd);
		pFunc = getUserPollFunc(pMainLoop->pEvents[i].data.fd);
		if(pFunc != NULL){
			(*pFunc)(pMainLoop->pEvents[i].data.fd, pMainLoop->pEvents[i].events, pMainLoop->pUserData);
		}
		else{
			defaultPollFunc(pMainLoop->pEvents[i].data.fd, pMainLoop->pEvents[i].events, pMainLoop->pUserData);
			printf("pFunc is NULL\n");
		}
	}
	pMainLoop->status = STATUS_INIT;
	return 0;
}

int my_mainloop_run(stMyMainLoop *pMainLoop)
{
	int ret = -1;
	CHECK_POINTER_NULL(pMainLoop, -1);
	while(pMainLoop->status != STATUS_QUIT){
		printf("pMainLoop->status=%d\n", pMainLoop->status);
		ret = my_mainloop_prepare(pMainLoop);
		if(ret < 0){
			printf("my_mainloop_prepare return %d\n", ret);
			break;
		}

		ret = my_mainloop_poll(pMainLoop);
		if(ret < 0){
			printf("my_mainloop_poll return %d\n", ret);
			break;
		}

		ret = my_mainloop_dispatch(pMainLoop);
		if(ret < 0){
			printf("my_mainloop_dispatch return %d\n", ret);
			break;
		}
	}
	printf("pMainLoop->status=%d\n", pMainLoop->status);
	return ret;
}

int my_mainloop_wakeup(stMyMainLoop *pMainLoop)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	char c = 'w';
	//write(pMainLoop->pipe_fd[WRITE_PIPE], &c, sizeof(char));
	return;
}

int my_mainloop_stop(stMyMainLoop *pMainLoop)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	pMainLoop->status = STATUS_QUIT;
	//my_mainloop_wakeup(pMainLoop);
	return 0;
}

int my_mainloop_add_event(stMyMainLoop *pMainLoop, int userFd, int pollState)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	int state = getEpollState(pollState);
	if(state < 0){
		printf("getEpollState == -1\n");
		return -1;
	}
	epollAddEvent(pMainLoop->epoll_id, userFd, state); 
	return 0;
}

int my_mainloop_delete_event(stMyMainLoop *pMainLoop, int userFd, int pollState)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	int state = getEpollState(pollState);
	if(state < 0){
		printf("getEpollState == -1\n");
		return -1;
	}
	epollDeleteEvent(pMainLoop->epoll_id, userFd, state); 
	return 0;
}

int my_mainloop_modify_event(stMyMainLoop *pMainLoop, int userFd, int pollState)
{
	CHECK_POINTER_NULL(pMainLoop, -1);
	int state = getEpollState(pollState);
	if(state < 0){
		printf("getEpollState == -1\n");
		return -1;
	}
	epollModifyEvent(pMainLoop->epoll_id, userFd, state); 
	return 0;
}

int my_mainloop_add_pollfunc(int userFd, pollFunc func)
{
	CHECK_CONDITION_RET(userFd < 0, -1);
	CHECK_POINTER_NULL(func, -1);
	int ret = registUserPollFunc(userFd, func);
	return ret;
}

int my_mainloop_delete_pollfunc(int userFd)
{
	CHECK_CONDITION_RET(userFd < 0, -1);
	unRegistUserPollFunc(userFd);
	return 0;
}

static void* thread_func(void* userData)
{
	int fd = *(int*)userData;
	printf("thread:fd = %d\n",fd);
	while(1){
		write(fd, "hello\n", 6);
		sleep(1);
		//lseek(fd, SEEK_SET);
	}
	return (void*)0;
}

static void* thread_func_pipe(void* userData)
{
	int fd = *(int*)userData;
	printf("thread:fd = %d\n",fd);
	while(1){
		write(fd, "hello, i am writing data into pipe\n", 50);
		sleep(1);
		//lseek(fd, SEEK_SET);
	}
	return (void*)0;
}

void my_msg_packer(char* msgData, ...)
{
	if(msgData == NULL)
		return;
	va_list vaList;
	void* para;
	char* funParam = msgData;
	va_start(vaList, msgData);
	int len = va_arg(vaList, int);
	while((len != 0) && (len != -1)){
		para = va_arg(vaList, void*);
		memcpy(funParam, para, len);
		funParam += len;
		len = va_arg(vaList, int);
	}
	va_end(vaList);
	return;
}

void my_msg_unpacker(char* msgData, ...)
{
	if(msgData == NULL)
		return;
	va_list vaList;
	void* para;
	char* funParam = msgData;
	va_start(vaList, msgData);
	int len = va_arg(vaList, int);
	while((len != 0) && (len != -1)){
		para = va_arg(vaList, void*);
		memcpy(para, funParam, len);
		funParam += len;
		len = va_arg(vaList, int);
	}
	va_end(vaList);
	return;
}

static int readMsgInner(int fd, int events, void *pUserData)
{
	if(events & EPOLLIN){
		stMyMsg msg = {0};
		read(fd, &msg, sizeof(stMyMsg)); 
		handleMsg pHandleMsgFunc = (handleMsg)pUserData;
		if(pHandleMsgFunc != NULL){
			pHandleMsgFunc(&msg);
		}
	}

	if(events & EPOLLERR){
		printf("readMsgInner event poll error!!!\n");
	}
	return 0;
}

void my_mailbox_init()
{
	int i = 0;
	for(; i < MAIL_BOX_NUM; i++){
		s_my_mailbox[i].state = AVAIL_STATUS_FREE;
		s_my_mailbox[i].pMainLoop = NULL;
		s_my_mailbox[i].pipe_fd[READ_PIPE] = -1;
		s_my_mailbox[i].pipe_fd[WRITE_PIPE] = -1;
	}

	pthread_mutex_init(&s_mailbox_mutex, NULL);
	return;
}

int my_create_mailbox(enMailBoxId mailBoxId)
{
	int ret = 0;
	if((mailBoxId >= MAX_MAIL_BOX_NUM) ||
		(s_my_mailbox[mailBoxId].state != AVAIL_STATUS_FREE)){
		printf("my_create_mailbox %d fail! (state=%d)\n", 
			mailBoxId, s_my_mailbox[mailBoxId].state);
		return -1;
	}

	SAFETY_MUTEX_LOCK(s_mailbox_mutex);

	s_my_mailbox[mailBoxId].pMainLoop = my_mainloop_new();
	pipe(s_my_mailbox[mailBoxId].pipe_fd);

	printf("read fd=%d, write_fd=%d\n", s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE],
		s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE]);

	if(my_mainloop_add_event(s_my_mailbox[mailBoxId].pMainLoop,
							s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE],
							EVENT_READ | EVENT_ERR) == 0){
		if(my_mainloop_add_pollfunc(s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE], readMsgInner) < 0){
			printf("my_mainloop_add_pollfunc fail\n");
			ret = -1;
		}
	}
	else{
		printf("my_mainloop_add_event fail\n");
		ret = -1;
	}
	
	if(ret < 0){
		my_mainloop_free(s_my_mailbox[mailBoxId].pMainLoop);
		close(s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE]);
		s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE] = -1;
		close(s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE]);
		s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE] = -1;
		s_my_mailbox[mailBoxId].pMainLoop = NULL;
	}

	s_my_mailbox[mailBoxId].state = AVAIL_STATUS_USED;
	SAFETY_MUTEX_UNLOCK(s_mailbox_mutex);
	printf("#######my_create_mailbox %d success!#######\n", mailBoxId);
	return ret;
}

void my_delete_mailbox(enMailBoxId mailBoxId)
{
	if((mailBoxId >= MAX_MAIL_BOX_NUM) ||
		(s_my_mailbox[mailBoxId].state == AVAIL_STATUS_FREE)){
		printf("my_delete_mailbox %d fail! (state=%d)\n",
			mailBoxId, s_my_mailbox[mailBoxId].state);
		return;
	}

	SAFETY_MUTEX_LOCK(s_mailbox_mutex);
	if(s_my_mailbox[mailBoxId].pMainLoop){
		s_my_mailbox[mailBoxId].pMainLoop->status = STATUS_QUIT;
		s_my_mailbox[mailBoxId].state = AVAIL_STATUS_FREE;
		stMyMsg msg = {-1};
		write(s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE], &msg, sizeof(stMyMsg));
		usleep(100);
		
		my_mainloop_delete_pollfunc(s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE]);
		my_mainloop_delete_event(s_my_mailbox[mailBoxId].pMainLoop, s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE], EVENT_READ);
		if(close(s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE]) < 0){
			printf("my_delete_mailbox-> close read fd fail!!!!\n");
		}
		s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE] = -1;
		if(close(s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE]) < 0){
			printf("my_delete_mailbox-> close write fd fail!!!!\n");
		}
		s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE] = -1;
		my_mainloop_free(s_my_mailbox[mailBoxId].pMainLoop);
		s_my_mailbox[mailBoxId].pMainLoop = NULL;
	}
	SAFETY_MUTEX_UNLOCK(s_mailbox_mutex);
	printf("--------my_delete_mailbox %d done!!\n", mailBoxId);
	return;
}

void my_sendto_mailbox(enMailBoxId mailBoxId, stMyMsg *pMsg)
{
	if((mailBoxId >= MAX_MAIL_BOX_NUM) ||
		(s_my_mailbox[mailBoxId].state != AVAIL_STATUS_USED) ||
		(s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE] == -1)){
		printf("my_sendto_mailbox %d fail! (pipe_fd=%d, state=%d)\n", 
			mailBoxId, s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE],
			s_my_mailbox[mailBoxId].state);
		return;
	}
	
	SAFETY_MUTEX_LOCK(s_mailbox_mutex);
	write(s_my_mailbox[mailBoxId].pipe_fd[WRITE_PIPE], pMsg, sizeof(stMyMsg));
	SAFETY_MUTEX_UNLOCK(s_mailbox_mutex);
	return;
}

/* timeOut: -1: wait forever else positive value (ms) */
void my_getfrom_mailbox(enMailBoxId mailBoxId, handleMsg pHandleMsgFunc, int timeOut)
{
	int ret = -1;
	if((mailBoxId >= MAX_MAIL_BOX_NUM) ||
		(s_my_mailbox[mailBoxId].state != AVAIL_STATUS_USED) ||
		(s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE] == -1)){
		printf("my_getfrom_mailbox_loop fail!\n");
		return;
	}

	SAFETY_MUTEX_LOCK(s_mailbox_mutex);
	s_my_mailbox[mailBoxId].pMainLoop->pUserData = (void*)pHandleMsgFunc;
	s_my_mailbox[mailBoxId].pMainLoop->time_out = timeOut;
	SAFETY_MUTEX_UNLOCK(s_mailbox_mutex);

	if(s_my_mailbox[mailBoxId].pMainLoop->status != STATUS_QUIT){
		printf("pMainLoop->status=%d\n", s_my_mailbox[mailBoxId].pMainLoop->status);
		ret = my_mainloop_prepare(s_my_mailbox[mailBoxId].pMainLoop);
		if(ret < 0){
			printf("my_mainloop_prepare return %d\n", ret);
			return;
		}

		ret = my_mainloop_poll(s_my_mailbox[mailBoxId].pMainLoop);
		if(ret < 0){
			printf("my_mainloop_poll return %d\n", ret);
			return;
		}

		ret = my_mainloop_dispatch(s_my_mailbox[mailBoxId].pMainLoop);
		if(ret < 0){
			printf("my_mainloop_dispatch return %d\n", ret);
			return;
		}
	}
	return;

}

/* timeOut: -1: wait forever else positive value (ms) */
void my_getfrom_mailbox_loop(enMailBoxId mailBoxId, handleMsg pHandleMsgFunc, int timeOut)
{
	if((mailBoxId >= MAX_MAIL_BOX_NUM) ||
		(s_my_mailbox[mailBoxId].state != AVAIL_STATUS_USED) ||
		(s_my_mailbox[mailBoxId].pipe_fd[READ_PIPE] == -1)){
		printf("my_getfrom_mailbox_loop fail!\n");
		return;
	}

	SAFETY_MUTEX_LOCK(s_mailbox_mutex);
	s_my_mailbox[mailBoxId].pMainLoop->pUserData = (void*)pHandleMsgFunc;
	s_my_mailbox[mailBoxId].pMainLoop->time_out = timeOut;
	SAFETY_MUTEX_UNLOCK(s_mailbox_mutex);
	my_mainloop_run(s_my_mailbox[mailBoxId].pMainLoop);
	return;

}

static void* sendMsgTest1(void* userData)
{
	int cnt = 0;
	stMyMsg msg = {0};
	while(1){
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID1, &msg);
		
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX1]hello, test 2 message send!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID1, &msg);
		//usleep(1000);

		msg.eventType = 1;
		int a = 100, b = 200;
		my_msg_packer(msg.eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		my_sendto_mailbox(MAIL_BOX_ID1, &msg);
		usleep(1000);
		//cnt++;
		//if(cnt > 10){
		//	my_delete_mailbox(MAIL_BOX_ID1);
		//	cnt = 0;
		//}
	}
	return (void*)0;
}

static void* sendMsgTest2(void* userData)
{
	static int cnt = 0;
	stMyMsg msg = {0};
	while(1){
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX2]hello, test 1 message send!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID2, &msg);
		
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX2]hello, test 2 message send!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID2, &msg);
		usleep(10000);

		msg.eventType = 1;
		int a = 1000, b = 2000;
		my_msg_packer(msg.eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		my_sendto_mailbox(MAIL_BOX_ID2, &msg);
		usleep(10000);
		cnt++;
		if(cnt > 10){
			my_delete_mailbox(MAIL_BOX_ID2);
			break;
		}
	}
	return (void*)0;
}

static void* sendMsgTest3(void* userData)
{
	stMyMsg msg = {0};
	while(1){
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX3]hello, test 1 message send!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID3, &msg);
		usleep(1000000);
	}
	return (void*)0;
}

static void* sendMsgTest4(void* userData)
{
	stMyMsg msg = {0};
	int cnt = 0;
	while(1){
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX1]###############!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID1, &msg);
		usleep(1000);
		if(++cnt > 30000){
			my_delete_mailbox(MAIL_BOX_ID3);
			break;
		}
	}
	return (void*)0;
}

static int handleMsgTest(stMyMsg *pMsg)
{
	if(pMsg->eventType == 0){
		printf("msgData: %s\n", pMsg->eventData);
	}
	else{
		int a = 0, b = 0;
		my_msg_unpacker(pMsg->eventData, sizeof(int), &a, sizeof(int), &b, -1);
		printf("a = %d, b = %d\n", a, b);
	}
	return 0;	
}

static void* loopGetMsg1(void* userData)
{
	int cnt = 20;
	while(cnt-- > 0){
		my_create_mailbox(MAIL_BOX_ID1);
		my_getfrom_mailbox_loop(MAIL_BOX_ID1, handleMsgTest, -1);
	}
	return (void*)0;
}

static void* loopGetMsg2(void* userData)
{
	my_getfrom_mailbox_loop(MAIL_BOX_ID2, handleMsgTest, 1000);
	return (void*)0;
}

static int handleMsgSelf(stMyMsg *pMsg)
{
	if(pMsg->eventType == 0){
		printf("msgData: %s\n", pMsg->eventData);
		/* 测试自己给自己发消息 */
		stMyMsg msg = {0};
		msg.eventType = 0;
		sprintf(msg.eventData, "%s", "[MAILBOX1]hello, test 1 message send by myself!!!\n");
		my_sendto_mailbox(MAIL_BOX_ID1, &msg);
		usleep(10000);
		printf("send message to MAIL_BOX_ID1 by myself.\n");
	}
	return 0;	
}


int main()
{
	#if 0
	stMyMainLoop *pMainLoop = NULL;
	pMainLoop = my_mainloop_new();

	int pipe_fd[2];
	pipe(pipe_fd);
	int threadID = -1;
	pthread_create(&threadID, NULL, thread_func_pipe, (void*)&pipe_fd[1]); 
	printf("pipe read fd = %d, write fd = %d\n", pipe_fd[0], pipe_fd[1]);

	int events = EVENT_READ;
	my_mainloop_add_event(pMainLoop, pipe_fd[0], events);
	my_mainloop_add_pollfunc(pipe_fd[0], defaultPollFunc);
	
	my_mainloop_run(pMainLoop);
	my_mainloop_free(pMainLoop);
	close(pipe_fd[0]);
	close(pipe_fd[1]);
	#endif
	
	//stdout = freopen("./fan_log.txt", "wt", stdout);
	/* Test mailBox */
	my_mailbox_init();

	/* 测试case 1: 普通测试 */
	#if 0
	my_create_mailbox(MAIL_BOX_ID2);
	my_create_mailbox(MAIL_BOX_ID3);
	
	int threadID = -1;
	pthread_create(&threadID, NULL, sendMsgTest1, NULL); 
	pthread_create(&threadID, NULL, sendMsgTest2, NULL); 
	pthread_create(&threadID, NULL, sendMsgTest3, NULL);
	pthread_create(&threadID, NULL, sendMsgTest4, NULL);
	pthread_create(&threadID, NULL, loopGetMsg1, NULL);
	pthread_create(&threadID, NULL, loopGetMsg2, NULL);

	my_getfrom_mailbox_loop(MAIL_BOX_ID3, handleMsgTest, 500);
	#endif

	/* 测试case 2: 测试自己给自己发消息 */
	my_create_mailbox(MAIL_BOX_ID1);

	/* 开启另一个线程也给mailbox1发消息 */
	/*
	如果存在一个线程自己给自己发消息，其他
	线程也在频繁的发消息，则会发生管道满了
	往管道写数据就阻塞了。
	*/
	#if 0
	int threadID = -1;
	pthread_create(&threadID, NULL, sendMsgTest1, NULL);
	stMyMsg msg = {0};
	msg.eventType = 0;
	sprintf(msg.eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
	my_sendto_mailbox(MAIL_BOX_ID1, &msg);
	my_getfrom_mailbox_loop(MAIL_BOX_ID1, handleMsgSelf, 500);
	#endif
	/* 测试case 3: 一次性发送多条，一次性读取多条*/
	#if 0
	stMyMsg msg = {0};
	msg.eventType = 0;
	sprintf(msg.eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
	int cnt = 0;
	while(1){
		/* 经测试pipe 管道可以连续写入的数据为240 * sizeof(stMyMsg) = 240 * 260 bytes */
		cnt++;
		my_sendto_mailbox(MAIL_BOX_ID1, &msg);
		printf("cnt = %d, size = %d\n", cnt, sizeof(stMyMsg));
		if(cnt >= 10){
			break;
		}
	}
	
	my_getfrom_mailbox_loop(MAIL_BOX_ID1, handleMsgTest, -1);
	#endif

	/* 测试case 4: 子进程发消息，子进程收消息 */
	#if 1
	my_create_mailbox(MAIL_BOX_ID2);
	
	int threadID = -1;
	pthread_create(&threadID, NULL, sendMsgTest2, NULL); 
	pthread_create(&threadID, NULL, loopGetMsg2, NULL);
	while(1){
		usleep(1000000);
	}
	#endif
	return 0;
}
