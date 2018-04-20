#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <stdarg.h>
#include <errno.h>

/*************************************************/
/*
   pMsgBuffer  ->      [stMyMsg      |  stMyMsg       |       stMyMsg   |        stMyMsg      | ... ]
					      \                    \
   pMsgNodeBuffer -> [(pAddr, pNext) | (pAddr, pNext) .... ]
*/

#define MAILBOX1_QUEUE_NUM (10)
#define MAILBOX2_QUEUE_NUM (5)
#define MAILBOX3_QUEUE_NUM (6)

enum
{
	MAIL_BOX_ID1,
	MAIL_BOX_ID2,
	MAIL_BOX_ID3,
	MAILBOX_MAX_CNT
};

typedef struct
{
	int eventType;
	char eventData[128];
}stMyMsg;

/*定义消息内存的大小*/
#define MSG_EACH_SIZE (sizeof(stMyMsg))
#define MSG_TOTAL_CNT (MAILBOX1_QUEUE_NUM + MAILBOX2_QUEUE_NUM + MAILBOX3_QUEUE_NUM)

#define CHECK_POINTER_NULL(pointer, ret) \
    do{\
        if(pointer == NULL){ \
            printf("Check '%s' is NULL at:%u\n", #pointer, __LINE__);\
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


typedef enum{
	FALSE,
	TRUE
}enBool;

typedef struct
{
	void *pAddr;
	struct stMsgNode *pNext;
}stMsgNode;

typedef struct
{
	stMsgNode *pFreeList;
	stMsgNode *pUsedList;
	volatile int      freeCnt;
	pthread_mutex_t   mutex;
}stMsgBufferList;

static void *pMsgBuffer = NULL;
static stMsgNode *pMsgNodeBuffer = NULL;
static stMsgBufferList s_msg_buffer_list;
enBool Init_MessageBuffer()
{
	if(pMsgBuffer) return TRUE;
	pMsgBuffer = (void*)malloc(MSG_EACH_SIZE * MSG_TOTAL_CNT);
	if(!pMsgBuffer) return FALSE;
	pMsgNodeBuffer = (stMsgNode*)malloc(sizeof(stMsgNode) * MSG_TOTAL_CNT);
	if(!pMsgNodeBuffer){
		free(pMsgBuffer);
		pMsgBuffer = NULL;
		return FALSE;
	}
	unsigned char *pAddr = (unsigned char*)pMsgBuffer;
	stMsgNode *pNode = pMsgNodeBuffer;
	
	s_msg_buffer_list.freeCnt = MSG_TOTAL_CNT;
	s_msg_buffer_list.pFreeList = pNode;
	s_msg_buffer_list.pFreeList->pAddr = pAddr;
	s_msg_buffer_list.pFreeList->pNext = NULL;
	pAddr += MSG_EACH_SIZE;
	pNode++;
	stMsgNode* pCur = s_msg_buffer_list.pFreeList;
	for(int i = 0; i < MSG_TOTAL_CNT - 1; i++){
		stMsgNode* pNew = pNode;
		pNew->pAddr = pAddr;
		pNew->pNext = NULL;
		pAddr += MSG_EACH_SIZE;
		pNode++;
		pCur->pNext = pNew;
		pCur = pNew;
	}
	s_msg_buffer_list.pUsedList = NULL;

	pthread_mutex_init(&s_msg_buffer_list.mutex, NULL);
	return TRUE;
}


void DeInit_MessageBuffer()
{
	if(pMsgBuffer){
		free(pMsgBuffer);
		pMsgBuffer = NULL;
	}

	if(pMsgNodeBuffer){
		free(pMsgNodeBuffer);
		pMsgNodeBuffer = NULL;
	}
	return;
}

enBool Get_MessageBuffer(stMyMsg **ppMsgBuff)
{
	CHECK_POINTER_NULL(ppMsgBuff, FALSE);
	printf("s_msg_buffer_list.freeCnt = %d\n", s_msg_buffer_list.freeCnt);
	if(s_msg_buffer_list.freeCnt <= 0){
		printf("All the msg buffer has been used!!!\n");
		return FALSE;
	}

	SAFETY_MUTEX_LOCK(s_msg_buffer_list.mutex);
	*ppMsgBuff = (stMyMsg*)s_msg_buffer_list.pFreeList->pAddr;
	stMsgNode *pUsed = s_msg_buffer_list.pFreeList;
	s_msg_buffer_list.pFreeList = s_msg_buffer_list.pFreeList->pNext;
	s_msg_buffer_list.freeCnt--;
	pUsed->pAddr = NULL;
	if(!s_msg_buffer_list.pUsedList){
		pUsed->pNext = NULL;
		s_msg_buffer_list.pUsedList = pUsed;
	}
	else{
		pUsed->pNext = s_msg_buffer_list.pUsedList->pNext;
		s_msg_buffer_list.pUsedList->pNext = pUsed;
	}
	SAFETY_MUTEX_UNLOCK(s_msg_buffer_list.mutex);
	return TRUE;
}

enBool Free_MessageBuffer(stMyMsg *pMsgBuff)
{
	CHECK_POINTER_NULL(pMsgBuff, FALSE);
	SAFETY_MUTEX_LOCK(s_msg_buffer_list.mutex);
	stMsgNode *pFree = s_msg_buffer_list.pUsedList;
	s_msg_buffer_list.pUsedList = s_msg_buffer_list.pUsedList->pNext;
	pFree->pAddr = (void*)pMsgBuff;
	pFree->pNext = NULL;
	if(!s_msg_buffer_list.pFreeList){
		s_msg_buffer_list.pFreeList = pFree;
	}
	else{
		pFree->pNext = s_msg_buffer_list.pFreeList->pNext;
		s_msg_buffer_list.pFreeList->pNext = pFree;
	}
	s_msg_buffer_list.freeCnt++;
	SAFETY_MUTEX_UNLOCK(s_msg_buffer_list.mutex);
	return TRUE;
}

typedef struct{
	volatile int readIdx;
	volatile int writeIdx;
	void** ppQueue; /* 存放stMyMsg 的地址(stMyMsg*) */
	int maxCnt;
}stLoopQueue;

stLoopQueue* Create_LoopQueue(int queueMaxCnt)
{
	if(queueMaxCnt <= 0) return NULL;
	stLoopQueue *pLoopQueue = (stLoopQueue*)malloc(sizeof(stLoopQueue));
	if(pLoopQueue){
		pLoopQueue->readIdx = 0;
		pLoopQueue->writeIdx = 0;
		pLoopQueue->ppQueue = (void**)malloc(sizeof(void*) * (queueMaxCnt + 1));
		pLoopQueue->maxCnt = queueMaxCnt + 1; /* 用循环队列机制需多出一个空间 */
	}
	return pLoopQueue;
}

void Delete_LoopQueue(stLoopQueue *pLoopQueue)
{
	if(pLoopQueue){
		free(pLoopQueue->ppQueue);
		free(pLoopQueue);
		pLoopQueue = NULL;
	}
	return;
}

static void calcLoopQueueNextIdx(int *pIndex, int maxCnt)
{
	int newIndex = __sync_add_and_fetch(pIndex, 1);
    if (newIndex >= maxCnt){
		/*bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval, ...)
		   if *ptr == oldval, use newval to update *ptr value */
        __sync_bool_compare_and_swap(pIndex, newIndex, newIndex % maxCnt);
    }
	return;
}

static enBool checkLoopQueueEmpty(stLoopQueue *pLoopQueue)
{
	return (pLoopQueue->readIdx == pLoopQueue->writeIdx) ? TRUE : FALSE;
}

static enBool checkLoopQueueFull(stLoopQueue *pLoopQueue)
{
	return ((pLoopQueue->writeIdx+1)%pLoopQueue->maxCnt== pLoopQueue->readIdx) ? TRUE : FALSE;
}

enBool Write_LoopQueue(stLoopQueue *pLoopQueue, void **ppIn)
{
	CHECK_POINTER_NULL(pLoopQueue, FALSE);
	CHECK_POINTER_NULL(ppIn, FALSE);
	if(checkLoopQueueFull(pLoopQueue)){
		printf("loop queue have not enough space to write, readIdx=%d,writeIdx=%d\n",
			pLoopQueue->readIdx,pLoopQueue->writeIdx);
		return FALSE;
	}

	pLoopQueue->ppQueue[pLoopQueue->writeIdx] = *ppIn;
	printf("write[%d]: 0x%x\n", pLoopQueue->writeIdx, *ppIn);
	calcLoopQueueNextIdx(&pLoopQueue->writeIdx, pLoopQueue->maxCnt);
	return TRUE;
}

enBool Read_LoopQueue(stLoopQueue *pLoopQueue, void **ppOut)
{
	CHECK_POINTER_NULL(pLoopQueue, FALSE);
	CHECK_POINTER_NULL(ppOut, FALSE);
	if(checkLoopQueueEmpty(pLoopQueue)){
		return FALSE;
	}
	*ppOut = pLoopQueue->ppQueue[pLoopQueue->readIdx];
	printf("read[%d]: 0x%x\n", pLoopQueue->readIdx, *ppOut);
	calcLoopQueueNextIdx(&pLoopQueue->readIdx, pLoopQueue->maxCnt);
	return TRUE;
}

typedef enum
{
	AVAIL_STATUS_FREE,
	AVAIL_STATUS_USED
}enAvailableStatus;

typedef struct{
	enAvailableStatus state;
	stLoopQueue *pLoopQueue;
	pthread_mutex_t mutex;
	pthread_cond_t  pushCond;
	pthread_cond_t  popCond;
}stMailBox;

static stMailBox s_mail_box[MAILBOX_MAX_CNT];
static pthread_mutex_t s_mail_box_mutex = PTHREAD_MUTEX_INITIALIZER;

void Init_MailBox()
{
	for(int i = 0; i < MAILBOX_MAX_CNT; i++){
		s_mail_box[i].state = AVAIL_STATUS_FREE;
		s_mail_box[i].pLoopQueue = NULL;
	}
	return;
}

enBool Create_MailBox(int mailBoxID, int queueMaxCnt)
{
	pthread_condattr_t attrCond;
	if(mailBoxID < 0 || mailBoxID >= MAILBOX_MAX_CNT || 
		s_mail_box[mailBoxID].state != AVAIL_STATUS_FREE){
		return FALSE;
	}
	SAFETY_MUTEX_LOCK(s_mail_box_mutex);
	s_mail_box[mailBoxID].state = AVAIL_STATUS_USED;
	pthread_mutex_init(&s_mail_box[mailBoxID].mutex, NULL);
	pthread_condattr_init(&attrCond);
	pthread_condattr_setclock(&attrCond, CLOCK_MONOTONIC);
	pthread_cond_init(&s_mail_box[mailBoxID].pushCond, &attrCond);
	pthread_cond_init(&s_mail_box[mailBoxID].popCond, &attrCond);
	s_mail_box[mailBoxID].pLoopQueue = Create_LoopQueue(queueMaxCnt);
	SAFETY_MUTEX_UNLOCK(s_mail_box_mutex);
	return s_mail_box[mailBoxID].pLoopQueue ? TRUE : FALSE;
}

enBool Delete_MailBox(int mailBoxID)
{
	if(mailBoxID < 0 || mailBoxID >= MAILBOX_MAX_CNT || 
		s_mail_box[mailBoxID].state != AVAIL_STATUS_USED){
		return FALSE;
	}
	SAFETY_MUTEX_LOCK(s_mail_box_mutex);
	s_mail_box[mailBoxID].state = AVAIL_STATUS_FREE;
	Delete_LoopQueue(s_mail_box[mailBoxID].pLoopQueue);
	pthread_cond_destroy(&s_mail_box[mailBoxID].pushCond);
	pthread_cond_destroy(&s_mail_box[mailBoxID].popCond);
	SAFETY_MUTEX_UNLOCK(s_mail_box_mutex);
	return TRUE;
}

/* timeOutMs :   -1  waitforever
			     >0  wait timeOutMs ms
*/
enBool SendTo_MailBox(int mailBoxID, void *pMsg, int timeOutMs)
{
	CHECK_POINTER_NULL(pMsg, FALSE);
	if(mailBoxID < 0 || mailBoxID >= MAILBOX_MAX_CNT || 
		s_mail_box[mailBoxID].state != AVAIL_STATUS_USED ||
		timeOutMs < -1){
		return FALSE;
	}

	enBool ret = TRUE;
	int os_result = 0;
	SAFETY_MUTEX_LOCK(s_mail_box[mailBoxID].mutex);
	while(Write_LoopQueue(s_mail_box[mailBoxID].pLoopQueue, &pMsg) == FALSE){
		/* 队列满*/
		if(timeOutMs == -1){
			os_result = pthread_cond_wait(&s_mail_box[mailBoxID].pushCond, &s_mail_box[mailBoxID].mutex);
		}
		else{
			struct timespec tv;
			clock_gettime(CLOCK_MONOTONIC, &tv);
			tv.tv_nsec += (timeOutMs%1000)*1000*1000;
			tv.tv_sec += tv.tv_nsec/(1000*1000*1000) + timeOutMs/1000;
			tv.tv_nsec = tv.tv_nsec%(1000*1000*1000);
			os_result = pthread_cond_timedwait(&s_mail_box[mailBoxID].pushCond, &s_mail_box[mailBoxID].mutex, &tv);
		}

		if(os_result == ETIMEDOUT){
			printf("SendTo_MailBox time out!\n");
			ret = FALSE;
			break;
		}
	}
	SAFETY_MUTEX_UNLOCK(s_mail_box[mailBoxID].mutex);
	pthread_cond_signal(&s_mail_box[mailBoxID].popCond);
	return ret;
}

/* timeOutMs :   -1  waitforever
			     >0  wait timeOutMs ms
*/
enBool ReceiveFrom_MailBox(int mailBoxID, void **ppMsg, int timeOutMs)
{
	CHECK_POINTER_NULL(ppMsg, FALSE);
	if(mailBoxID < 0 || mailBoxID >= MAILBOX_MAX_CNT || 
		s_mail_box[mailBoxID].state != AVAIL_STATUS_USED ||
		timeOutMs < -1){
		return FALSE;
	}

	enBool ret = TRUE;
	int os_result = 0;
	SAFETY_MUTEX_LOCK(s_mail_box[mailBoxID].mutex);
	while(Read_LoopQueue(s_mail_box[mailBoxID].pLoopQueue, ppMsg) == FALSE){
		/* 队列空 */
		if(timeOutMs == -1){
			os_result = pthread_cond_wait(&s_mail_box[mailBoxID].popCond, &s_mail_box[mailBoxID].mutex);
		}
		else{
			struct timespec tv;
			clock_gettime(CLOCK_MONOTONIC, &tv);
			tv.tv_nsec += (timeOutMs%1000)*1000*1000;
			tv.tv_sec += tv.tv_nsec/(1000*1000*1000) + timeOutMs/1000;
			tv.tv_nsec = tv.tv_nsec%(1000*1000*1000);
			os_result = pthread_cond_timedwait(&s_mail_box[mailBoxID].popCond, &s_mail_box[mailBoxID].mutex, &tv);
		}

		if(os_result == ETIMEDOUT){
			printf("Receive_MailBox time out!\n");
			ret = FALSE;
			break;
		}
	}
	SAFETY_MUTEX_UNLOCK(s_mail_box[mailBoxID].mutex);
	pthread_cond_signal(&s_mail_box[mailBoxID].pushCond);
	return ret;
}

/*************************************************/

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

static void* sendMsgTest1(void* userData)
{
	int cnt = 0;
	stMyMsg *pMsg = NULL;
	while(1){
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 2 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		usleep(1000);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 1;
		int a = 100, b = 200;
		my_msg_packer(pMsg->eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		usleep(200000);
	}
	return (void*)0;
}

static void* sendMsgTest2(void* userData)
{
	static int cnt = 0;
	stMyMsg *pMsg = NULL;
	while(1){
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX2]hello, test 1 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID2, pMsg, -1);


		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX2]hello, test 2 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID2, pMsg, -1);
		usleep(10000);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 1;
		int a = 1000, b = 2000;
		my_msg_packer(pMsg->eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		SendTo_MailBox(MAIL_BOX_ID2, pMsg, -1);
		usleep(100000);
		#if 0
		cnt++;
		if(cnt > 10){
			Delete_MailBox(MAIL_BOX_ID2);
			break;
		}
		#endif
	}
	return (void*)0;
}

static void* sendMsgTest3(void* userData)
{
	int cnt = 0;
	stMyMsg *pMsg = NULL;
	while(1){
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		usleep(100);
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 2 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		usleep(10000);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 1;
		int a = 100, b = 200;
		my_msg_packer(pMsg->eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		usleep(300000);
	}
	return (void*)0;
}

static void* sendMsgTest4(void* userData)
{
	int cnt = 0;
	stMyMsg *pMsg = NULL;
	while(1){
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX3]hello, test 1 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID3, pMsg, -1);
		usleep(100);
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX3]hello, test 2 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID3, pMsg, -1);
		usleep(10000);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 1;
		int a = 10000, b = 20000;
		my_msg_packer(pMsg->eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		SendTo_MailBox(MAIL_BOX_ID3, pMsg, -1);
		usleep(300000);
	}
	return (void*)0;
}


static int handleMsgTest(stMyMsg *pMsg)
{
	CHECK_POINTER_NULL(pMsg, -1);
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
	while(1){
		stMyMsg *pMsg = NULL;
		if(ReceiveFrom_MailBox(MAIL_BOX_ID1, &pMsg, -1)){
			handleMsgTest(pMsg);
			Free_MessageBuffer(pMsg);
		}
	}
	return (void*)0;
}

static void* loopGetMsg2(void* userData)
{
	while(1){
		stMyMsg *pMsg = NULL;
		if(ReceiveFrom_MailBox(MAIL_BOX_ID2, &pMsg, -1)){
			handleMsgTest(pMsg);
			Free_MessageBuffer(pMsg);
		}
	}
	return (void*)0;
}

static void* loopGetMsg3(void* userData)
{
	while(1){
		stMyMsg *pMsg = NULL;
		if(ReceiveFrom_MailBox(MAIL_BOX_ID3, &pMsg, -1)){
			handleMsgTest(pMsg);
			Free_MessageBuffer(pMsg);
		}
	}
	return (void*)0;
}

static void* loopGetMsg4(void* userData)
{
	while(1){
		stMyMsg *pMsg = NULL;
		if(ReceiveFrom_MailBox(MAIL_BOX_ID1, &pMsg, -1)){
			handleMsgTest(pMsg);
			Free_MessageBuffer(pMsg);

			/* 又自己给自己发消息 */
			Get_MessageBuffer(&pMsg);
			pMsg->eventType = 0;
			sprintf(pMsg->eventData, "%s", "[MAILBOX1]message send by myself!!!\n");
			SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		}
	}
	return (void*)0;
}

static void* sendReceiveSelf(void* userData)
{
	stMyMsg *pMsg = NULL;
	Get_MessageBuffer(&pMsg);
	pMsg->eventType = 0;
	sprintf(pMsg->eventData, "%s", "[MAILBOX1]message send by myself!!!\n");
	SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
	while(1){
		if(ReceiveFrom_MailBox(MAIL_BOX_ID1, &pMsg, -1)){
			handleMsgTest(pMsg);
			Free_MessageBuffer(pMsg);

			/* 又自己给自己发消息 */
			Get_MessageBuffer(&pMsg);
			pMsg->eventType = 0;
			sprintf(pMsg->eventData, "%s", "[MAILBOX1]message send by myself!!!\n");
			SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
		}
	}
	return (void*)0;
}


int main()
{
	int pID[2] = {-1, -1};
	pipe(pID);
	int a = 10;
	write(pID[1], &a, sizeof(int));
	int b = -1;
	read(pID[0], &b, sizeof(int));
	printf("b = %d\n", b);

	int *p = &a;
	printf("p = 0x%x, *p = %d\n", p, *p);
	write(pID[1], &p, sizeof(void**));
	//int *q = NULL;
	//read(pID[0], &q, sizeof(void**));
	//printf("q = 0x%x, *q = %d\n", q, *q);

	void test(int pid , void** pp){
		int *q = NULL;
		read(pid, &q, sizeof(void**));
		printf("q = 0x%x, *q = %d\n", q, *q);
		*pp = q;
		return;
	}
	int *k = NULL;
	int **kk = (void**)&k;
	test(pID[0], kk);
	printf("k = 0x%x, *k = %d\n", k, *k);

	Init_MessageBuffer();
	stMyMsg *pMsgBuff = NULL;
	Get_MessageBuffer(&pMsgBuff);
	sprintf(pMsgBuff->eventData, "Hello world, I am %s!\n", "Fan chenxin");
	printf("%s\n", pMsgBuff->eventData);
	Free_MessageBuffer(pMsgBuff);

	for(int i = 0; i < MSG_TOTAL_CNT + 1; i++){
		Get_MessageBuffer(&pMsgBuff);
		printf("pMsgBuff = 0x%x\n", pMsgBuff);
	}
	
	DeInit_MessageBuffer();

	Init_MessageBuffer();
	Init_MailBox();

	Create_MailBox(MAIL_BOX_ID1, MAILBOX1_QUEUE_NUM);
	Create_MailBox(MAIL_BOX_ID2, MAILBOX2_QUEUE_NUM);
	Create_MailBox(MAIL_BOX_ID3, MAILBOX3_QUEUE_NUM);

	/* 测试case 1: 普通测试 */
	#if 0
	int threadID = -1;
	pthread_create(&threadID, NULL, sendMsgTest1, NULL); 
	pthread_create(&threadID, NULL, sendMsgTest2, NULL);
	pthread_create(&threadID, NULL, sendMsgTest3, NULL);
	pthread_create(&threadID, NULL, sendMsgTest4, NULL);
	pthread_create(&threadID, NULL, loopGetMsg1, NULL);
	pthread_create(&threadID, NULL, loopGetMsg2, NULL);
	pthread_create(&threadID, NULL, loopGetMsg3, NULL);

	while(1){
		usleep(500000);
	}
	#endif

	/* 测试case 2: 测试自己给自己发消息 */

	/* 开启另一个线程也给mailbox1发消息 */
	/*
	如果存在一个线程自己给自己发消息，其他
	线程也在频繁的发消息, 则会发生阻塞
	*/
	#if 0
	int threadID = -1;
	pthread_create(&threadID, NULL, sendMsgTest1, NULL);
	stMyMsg *pMsg = NULL;
	Get_MessageBuffer(&pMsg);
	pMsg->eventType = 0;
	sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
	SendTo_MailBox(MAIL_BOX_ID1, pMsg, -1);
	pthread_create(&threadID, NULL, loopGetMsg4, NULL);
	while(1){
		usleep(500000);
	}
	#endif
	/* 测试case 3: 延迟接收 */
	#if 0
	int threadID = -1;
	pthread_create(&threadID, NULL, sendMsgTest1, NULL);
	pthread_create(&threadID, NULL, sendMsgTest2, NULL);
	usleep(1000000);
	pthread_create(&threadID, NULL, loopGetMsg1, NULL);
	pthread_create(&threadID, NULL, loopGetMsg2, NULL);
	while(1){
		usleep(500000);
	}
	#endif

	/* 测试case 4: 测试timeout */
	#if 0
	int cnt = 0;
	int threadID = -1;
	stMyMsg *pMsg = NULL;
	while(1){
		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 1 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, 100);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 0;
		sprintf(pMsg->eventData, "%s", "[MAILBOX1]hello, test 2 message send!!!\n");
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, 200);
		usleep(1000);

		Get_MessageBuffer(&pMsg);
		pMsg->eventType = 1;
		int a = 100, b = 200;
		my_msg_packer(pMsg->eventData,
					 	sizeof(int), &a,
					 	sizeof(int), &b, -1);
		SendTo_MailBox(MAIL_BOX_ID1, pMsg, 300);
		usleep(10000);
		if(cnt++ > 50){
			break;
		}
	}
	pthread_create(&threadID, NULL, loopGetMsg1, NULL);
	while(1){
		usleep(500000);
	}
	#endif

	/* 测试case 5: 单线程收发 */
	#if 1
	int threadID = -1;
	pthread_create(&threadID, NULL, sendReceiveSelf, NULL);
	while(1){
		usleep(500000);
	}
	#endif

	Delete_MailBox(MAIL_BOX_ID1);
	Delete_MailBox(MAIL_BOX_ID2);
	Delete_MailBox(MAIL_BOX_ID3);
	DeInit_MessageBuffer();
	return 0;
}
