#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdarg.h>

#include "queue_fw.h"
////////////////////////////////////////////////
/* log color */
#define LOG_COL_NONE
#define LOG_COL_END         "\033[0m"
#define LOG_COL_BLK  	"\033[0;30m" /* black */
#define LOG_COL_RED  	"\033[0;31m" /* red */
#define LOG_COL_GRN  	"\033[0;32m" /* green */
#define LOG_COL_YLW  	"\033[0;33m" /* yellow */
#define LOG_COL_BLU  	"\033[0;34m" /* blue */
#define LOG_COL_PUR  	"\033[0;35m" /* purple */
#define LOG_COL_CYN  	"\033[0;36m" /* cyan */
#define LOG_COL_WHI  	"\033[0;37m"
#define LOG_COL_RED_BLK "\033[0;31;40m"
#define LOG_COL_RED_YLW "\033[0;31;43m"
#define LOG_COL_RED_WHI "\033[0;31;47m"
#define LOG_COL_GRN_BLK "\033[0;32;40m"
#define LOG_COL_YLW_BLK "\033[0;33;40m"
#define LOG_COL_YLW_GRN "\033[1;33;42m"
#define LOG_COL_YLW_PUR "\033[0;33;45m"
#define LOG_COL_WHI_GRN "\033[0;37;42m"

static void _LogOut_(enLogLevel Level, const char Format[], ...) __attribute__((format(printf,2,3)));

#define LOG_OUTPUT(_level_, _fmt_, ...) _LogOut_(_level_, _fmt_, ##__VA_ARGS__)

#define LOG_ERR(_fmt_, ...)		LOG_OUTPUT(LOG_LEVEL_ERR, _fmt_, ##__VA_ARGS__)
#define LOG_WARNING(_fmt_, ...)	LOG_OUTPUT(LOG_LEVEL_WARNING, _fmt_, ##__VA_ARGS__)
#define LOG_NOTIFY(_fmt_, ...)	LOG_OUTPUT(LOG_LEVEL_NOTIFY, _fmt_, ##__VA_ARGS__)
#define LOG_DEBUG(_fmt_, ...)	LOG_OUTPUT(LOG_LEVEL_DEBUG, _fmt_, ##__VA_ARGS__)

#define QUE_LOG_ERR(msg, ...)	    LOG_ERR(LOG_COL_RED_YLW "[FW_QUEUE_ERR]"msg LOG_COL_END"\n", ##__VA_ARGS__)
#define QUE_LOG_WARNING(msg, ...)	LOG_WARNING(LOG_COL_RED_WHI "[FW_QUEUE_WARNING]"msg LOG_COL_END"\n", ##__VA_ARGS__)
#define QUE_LOG_NOTIFY(msg, ...)	LOG_NOTIFY(LOG_COL_WHI_GRN "[FW_QUEUE_NOTIFY]"msg LOG_COL_END"\n", ##__VA_ARGS__)
#define QUE_LOG_DEBUG(msg, ...)		LOG_DEBUG(LOG_COL_NONE "[FW_QUEUE_DEBUG]"msg LOG_COL_NONE"\n", ##__VA_ARGS__)

////////////////////////////////////////////////
#define FW_QUEUE_MAX_CAPICITY	(1024)

typedef enum
{
	FW_RET_UNKNOW_ERROR 	= -6,  // unknow error
	FW_RET_TIMEOUT			= -5,  // timeout
	FW_RET_QUEUE_INVALID	= -4,  // queue is invalid 
	FW_RET_INVALID_PRMS 	= -3,  // invalid parameter
	FW_RET_QUEUE_EMPTY  	= -2,  // queue is empty
	FW_RET_QUEUE_FULL   	= -1,  // quque is full
	FW_RET_SUCCESS 			= 0,   // success
}enFwRetValue;

typedef enum {
	FW_QUE_AVAILABLE = 0,  // 可用
	FW_QUE_OCCUPIED,   	   // 被占用
}enFwQueueState;

typedef struct
{
	void *pAddr;
	struct stFwCellBuffNode *pNext;
}stFwCellBuffNode;

typedef struct
{
	stFwCellBuffNode *pFreeList;
	stFwCellBuffNode *pUsedList;
	stFwCellBuffNode *pFreeListTail; // the tail pointer of free list
	volatile int 	 freeCnt;
	pthread_mutex_t  mutex;
}stFwCellBuffListMng;

typedef struct
{
	volatile int readIdx;
	volatile int writeIdx;
	void** ppQueue;
	int maxCnt;
}stLoopQueue;

typedef struct{
	enFwQueueState 		state;
	stLoopQueue 		*pLoopQueue;
	void 				*pCellBuffPool;
	stFwCellBuffNode 	*pCellBuffNodePool;
	stFwCellBuffListMng cellBuffListMng;
	pthread_mutex_t 	mutex;
	pthread_cond_t  	pushCond;
	pthread_cond_t  	popCond;
	unsigned int 		queueCellSize;
}stFwQueueMng;

static stFwQueueMng s_fw_queue_mng[FW_QUEUE_MAX_NUM];
static pthread_mutex_t s_fw_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static enLogLevel s_log_out_level = LOG_LEVEL_ALL;

#define SAFETY_MUTEX_LOCK(mutex) \
do{ \
	pthread_cleanup_push(pthread_mutex_unlock, &(mutex)); \
	pthread_mutex_lock(&(mutex))
						
#define SAFETY_MUTEX_UNLOCK(mutex) \
	pthread_mutex_unlock(&mutex); \
	pthread_cleanup_pop(0); \
}while(0)

/* log out put */
static void _LogOut_(enLogLevel Level, const char Format[], ...)
{
	if(Level <= s_log_out_level) {
		char *pBuffer = NULL;
		char buffer[256] = {'\0'};
		pBuffer = &buffer[0];
		
		va_list  ArgPtr;
	    va_start(ArgPtr, Format);
		vsnprintf(pBuffer, 255, Format, ArgPtr);
		va_end(ArgPtr);

		printf(buffer);
	}
}

static void pthread_create_mutex(pthread_mutex_t *mutex)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr , PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(mutex, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void pthread_create_cond(pthread_cond_t *cond)
{
	pthread_condattr_t attrCond;
	pthread_condattr_init(&attrCond);
	pthread_condattr_setclock(&attrCond, CLOCK_MONOTONIC);
	pthread_cond_init(cond, &attrCond);	
}

static stLoopQueue* Fw_CreateLoopQueue(int queueMaxCnt)
{
	if(queueMaxCnt <= 0) return NULL;
	stLoopQueue *pLoopQueue = (stLoopQueue*)malloc(sizeof(stLoopQueue));
	if(pLoopQueue){
		pLoopQueue->readIdx = 0;
		pLoopQueue->writeIdx = 0;
		pLoopQueue->ppQueue = (void**)malloc(sizeof(void*) * (queueMaxCnt + 1));
		pLoopQueue->maxCnt = queueMaxCnt + 1;
	}
	return pLoopQueue;
}

static void Fw_DeleteLoopQueue(stLoopQueue *pLoopQueue)
{
	if(pLoopQueue){
		free(pLoopQueue->ppQueue);
		pLoopQueue->ppQueue = NULL;
		free(pLoopQueue);
		pLoopQueue = NULL;
	}
	return;
}

static void Fw_CalcLoopQueueNextIdx(volatile int *pIndex, int maxCnt)
{
	int newIndex = __sync_add_and_fetch(pIndex, 1);
	if (newIndex >= maxCnt){
		/*bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval, ...)
		   if *ptr == oldval, use newval to update *ptr value */
		__sync_bool_compare_and_swap(pIndex, newIndex, newIndex % maxCnt);
	}
}

static int Fw_CheckLoopQueueEmpty(stLoopQueue *pLoopQueue)
{
	return (pLoopQueue->readIdx == pLoopQueue->writeIdx) ? 1 : 0;
}

static int Fw_CheckLoopQueueFull(stLoopQueue *pLoopQueue)
{
	return ((pLoopQueue->writeIdx+1)%pLoopQueue->maxCnt== pLoopQueue->readIdx) ? 1 : 0;
}

static int Fw_GetLoopQueueNums(stLoopQueue *pLoopQueue)
{
	return (pLoopQueue->writeIdx - pLoopQueue->readIdx + pLoopQueue->maxCnt) % pLoopQueue->maxCnt;
}

static enFwRetValue Fw_WriteLoopQueue(stLoopQueue *pLoopQueue, void **ppIn)
{
	if(pLoopQueue == NULL) return FW_RET_QUEUE_INVALID;
	if(Fw_CheckLoopQueueFull(pLoopQueue)){
		QUE_LOG_DEBUG("loop queue have not enough space to write, readIdx=%d,writeIdx=%d",
			pLoopQueue->readIdx,pLoopQueue->writeIdx);
		return FW_RET_QUEUE_FULL;
	}

	pLoopQueue->ppQueue[pLoopQueue->writeIdx] = *ppIn;
	QUE_LOG_DEBUG("write[%d]: 0x%x", pLoopQueue->writeIdx, *ppIn);
	Fw_CalcLoopQueueNextIdx(&pLoopQueue->writeIdx, pLoopQueue->maxCnt);
	return FW_RET_SUCCESS;
}

static enFwRetValue Fw_ReadLoopQueue(stLoopQueue *pLoopQueue, void **ppOut)
{
	if(pLoopQueue == NULL) return FW_RET_QUEUE_INVALID;
	if(Fw_CheckLoopQueueEmpty(pLoopQueue)){
		QUE_LOG_DEBUG("Loop queue is empty!!!");
		return FW_RET_QUEUE_EMPTY;
	}
	*ppOut = pLoopQueue->ppQueue[pLoopQueue->readIdx];
	QUE_LOG_DEBUG("read[%d]: 0x%x", pLoopQueue->readIdx, *ppOut);
	Fw_CalcLoopQueueNextIdx(&pLoopQueue->readIdx, pLoopQueue->maxCnt);
	return FW_RET_SUCCESS;
}

static void fw_queue_error_string(int queId, enFwRetValue error)
{
	switch(error)
	{
		case FW_RET_UNKNOW_ERROR:
			QUE_LOG_ERR("Queue: %d, Unknow Error happened!", queId);
			break;
		case FW_RET_TIMEOUT:
			QUE_LOG_DEBUG("Queue: %d, Time out happened!", queId);
			break;
		case FW_RET_QUEUE_INVALID:
			QUE_LOG_ERR("Queue: %d, Queue is invalid!", queId);
			break;
		case FW_RET_INVALID_PRMS:
			QUE_LOG_ERR("Queue: %d, Invalid Parameter Error happened!", queId);
			break;
		case FW_RET_QUEUE_EMPTY:
			QUE_LOG_ERR("Queue: %d, Queue is Empty!", queId);
			break;
		case FW_RET_QUEUE_FULL:
			QUE_LOG_ERR("Queue: %d, Queue is Full!", queId);
			break;
		default:
			QUE_LOG_ERR("Queue: %d, No Error happened!", queId);
			break;
	}
}

static int fw_queue_cell_buff_list_create(stFwQueueMng *pFwQueueMng, unsigned int queCapacity, unsigned int queCellSize)
{
	unsigned int cell_buff_total_num = queCapacity + 2;
	if(pFwQueueMng->pCellBuffPool) return 0;
	pFwQueueMng->pCellBuffPool = (void*)malloc(cell_buff_total_num * queCellSize);
	if(!pFwQueueMng->pCellBuffPool) return -1;
	
	pFwQueueMng->pCellBuffNodePool = (stFwCellBuffNode*)malloc(sizeof(stFwCellBuffNode) * cell_buff_total_num);
	if(!pFwQueueMng->pCellBuffNodePool){
		free(pFwQueueMng->pCellBuffPool);
		pFwQueueMng->pCellBuffPool = NULL;
		return -1;
	}

	unsigned char *pAddr = (unsigned char*)pFwQueueMng->pCellBuffPool;
	stFwCellBuffNode *pNode = pFwQueueMng->pCellBuffNodePool;
	
	pFwQueueMng->cellBuffListMng.freeCnt = cell_buff_total_num;
	pFwQueueMng->cellBuffListMng.pFreeList = pNode;
	pFwQueueMng->cellBuffListMng.pFreeList->pAddr = pAddr;
	pFwQueueMng->cellBuffListMng.pFreeList->pNext = NULL;

	pAddr += queCellSize;
	pNode++;
	stFwCellBuffNode* pCur = pFwQueueMng->cellBuffListMng.pFreeList;
	int i = 0;
	for(; i < cell_buff_total_num - 1; i++){
		stFwCellBuffNode* pNew = pNode;
		pNew->pAddr = pAddr;
		pNew->pNext = NULL;
		pAddr += queCellSize;
		pNode++;
		pCur->pNext = pNew;
		pCur = pNew;
	}
	pFwQueueMng->cellBuffListMng.pUsedList = NULL;
	pFwQueueMng->cellBuffListMng.pFreeListTail = pCur;
	
	pthread_create_mutex(&pFwQueueMng->cellBuffListMng.mutex);

	QUE_LOG_NOTIFY("Queue Cell buffer list create success, create %d Cell buffers!", cell_buff_total_num);
	return 0;
}


static void fw_queue_cell_buff_list_destory(stFwQueueMng *pFwQueueMng)
{
	if(pFwQueueMng->pCellBuffPool){
		free(pFwQueueMng->pCellBuffPool);
		pFwQueueMng->pCellBuffPool = NULL;
	}

	if(pFwQueueMng->pCellBuffNodePool){
		free(pFwQueueMng->pCellBuffNodePool);
		pFwQueueMng->pCellBuffNodePool = NULL;
	}

	pFwQueueMng->cellBuffListMng.freeCnt = 0;
	pFwQueueMng->cellBuffListMng.pFreeList = NULL;
	pFwQueueMng->cellBuffListMng.pUsedList = NULL;
	pFwQueueMng->cellBuffListMng.pFreeListTail = NULL;
	return;
}

static int fw_queue_get_cell_buff(stFwCellBuffListMng *pCellBuffListMng, void **ppCellBuff)
{
	int ret = 0;
	SAFETY_MUTEX_LOCK(pCellBuffListMng->mutex);
	QUE_LOG_DEBUG("pCellBuffListMng->freeCnt = %d", pCellBuffListMng->freeCnt);
	if(pCellBuffListMng->freeCnt <= 0){
		QUE_LOG_WARNING("All the Cell buffer has been used!!!");
		ret = -1;
	}
	else{
		*ppCellBuff = pCellBuffListMng->pFreeList->pAddr;
		stFwCellBuffNode *pUsed = pCellBuffListMng->pFreeList;
		pCellBuffListMng->pFreeList = pCellBuffListMng->pFreeList->pNext;
		pCellBuffListMng->freeCnt--;
		if(!pCellBuffListMng->pUsedList){
			pUsed->pNext = NULL;
			pUsed->pAddr = NULL;
			pCellBuffListMng->pUsedList = pUsed;
		}
		else{
			pUsed->pAddr = NULL;
			pUsed->pNext = pCellBuffListMng->pUsedList->pNext;
			pCellBuffListMng->pUsedList->pNext = pUsed;
		}
	}
	SAFETY_MUTEX_UNLOCK(pCellBuffListMng->mutex);
	QUE_LOG_DEBUG("Get Cell Buffer : 0x%x", *ppCellBuff);
	return ret;
}

static void fw_queue_free_cell_buff(stFwCellBuffListMng *pCellBuffListMng, void *pCellBuff)
{
	SAFETY_MUTEX_LOCK(pCellBuffListMng->mutex);
	stFwCellBuffNode *pFree = pCellBuffListMng->pUsedList;
	pCellBuffListMng->pUsedList = pCellBuffListMng->pUsedList->pNext;
	pFree->pAddr = pCellBuff;
	pFree->pNext = NULL;
	if(!pCellBuffListMng->pFreeList){
		pCellBuffListMng->pFreeList = pFree;
		pCellBuffListMng->pFreeListTail = pCellBuffListMng->pFreeList;
		pCellBuffListMng->pFreeListTail->pNext = NULL;
	}
	else{
		#if 1
		/* add to the free list tail */
		pCellBuffListMng->pFreeListTail->pNext = pFree;
		pCellBuffListMng->pFreeListTail = pFree;
		#else
		/* add to the free list head */
		pFree->pNext = pCellBuffListMng->pFreeList->pNext;
		pCellBuffListMng->pFreeList->pNext = pFree;
		#endif
	}
	pCellBuffListMng->freeCnt++;
	SAFETY_MUTEX_UNLOCK(pCellBuffListMng->mutex);
	QUE_LOG_DEBUG("Free Cell Buffer : 0x%x", pCellBuff);
}

static enFwRetValue fw_queue_inner_push(int queId, void *pQueCell, int timeOutMs)
{
	enFwRetValue ret = FW_RET_SUCCESS;
	int os_result = 0;
	SAFETY_MUTEX_LOCK(s_fw_queue_mng[queId].mutex);
	if((queId < 0) || (queId >= FW_QUEUE_MAX_NUM)) {
		QUE_LOG_WARNING("queue ID must in the range: [%d -  %d], current ID: %d", 0, FW_QUEUE_MAX_NUM-1, queId);
		ret = FW_RET_INVALID_PRMS;
	}
	else if(s_fw_queue_mng[queId].state == FW_QUE_AVAILABLE) {
		ret = FW_RET_QUEUE_INVALID;		
	}
	else {
		while(Fw_WriteLoopQueue(s_fw_queue_mng[queId].pLoopQueue, &pQueCell) == FW_RET_QUEUE_FULL){
			/* if loop queue is full */
			if(timeOutMs < 0){
				os_result = pthread_cond_wait(&s_fw_queue_mng[queId].pushCond, &s_fw_queue_mng[queId].mutex);
			}
			else{
				struct timespec tv;
				clock_gettime(CLOCK_MONOTONIC, &tv);
				tv.tv_nsec += (timeOutMs%1000)*1000*1000;
				tv.tv_sec += tv.tv_nsec/(1000*1000*1000) + timeOutMs/1000;
				tv.tv_nsec = tv.tv_nsec%(1000*1000*1000);
				os_result = pthread_cond_timedwait(&s_fw_queue_mng[queId].pushCond, &s_fw_queue_mng[queId].mutex, &tv);
			}

			if(os_result == ETIMEDOUT){
				ret = FW_RET_TIMEOUT;
				break;
			}
		}
		pthread_cond_signal(&s_fw_queue_mng[queId].popCond);
	}
	SAFETY_MUTEX_UNLOCK(s_fw_queue_mng[queId].mutex);
		
	return ret;
}

static enFwRetValue fw_queue_inner_pop(int queId, void **ppQueCell, int timeOutMs)
{
	enFwRetValue ret = FW_RET_SUCCESS;
	int os_result = 0;
	void* pCellBuff = NULL;
	SAFETY_MUTEX_LOCK(s_fw_queue_mng[queId].mutex);
	if((queId < 0) || (queId >= FW_QUEUE_MAX_NUM)) {
		QUE_LOG_WARNING("queue ID must in the range: [%d -  %d], current ID: %d", 0, FW_QUEUE_MAX_NUM-1, queId);
		ret = FW_RET_INVALID_PRMS;
	}
	else if(s_fw_queue_mng[queId].state == FW_QUE_AVAILABLE) {
		ret = FW_RET_QUEUE_INVALID;	
	}
	else {
		while(Fw_ReadLoopQueue(s_fw_queue_mng[queId].pLoopQueue, ppQueCell) == FW_RET_QUEUE_EMPTY){
			/* if loop queue is empty */
			//MAILBOX_LOG_WARNING("Receive from Du FB Queue waiting.....");
			if(timeOutMs == -1){
				os_result = pthread_cond_wait(&s_fw_queue_mng[queId].popCond, &s_fw_queue_mng[queId].mutex);
			}
			else{
				struct timespec tv;
				clock_gettime(CLOCK_MONOTONIC, &tv);
				tv.tv_nsec += (timeOutMs%1000)*1000*1000;
				tv.tv_sec += tv.tv_nsec/(1000*1000*1000) + timeOutMs/1000;
				tv.tv_nsec = tv.tv_nsec%(1000*1000*1000);
				os_result = pthread_cond_timedwait(&s_fw_queue_mng[queId].popCond, &s_fw_queue_mng[queId].mutex, &tv);
			}
			//MAILBOX_LOG_WARNING("Receive from Du FB Queue wait Over.....");

			if(os_result == ETIMEDOUT){
				ret = FW_RET_TIMEOUT;
				break;
			}
		}
		
		pthread_cond_signal(&s_fw_queue_mng[queId].pushCond);
	}
	SAFETY_MUTEX_UNLOCK(s_fw_queue_mng[queId].mutex);
	return ret;
}

/*
**	功能：创建一个队列
**	参数：queCapacity    队列容量，容纳多少个元件(cell)
          queCellSize  队列存放每个元件的大小
**  返回：-1     失败
		  >=0 返回队列ID
*/
int fw_queue_create(unsigned int queCapacity, unsigned int queCellSize)
{
	int queueId = 0, ret = 0;
	SAFETY_MUTEX_LOCK(s_fw_queue_mutex);
	for(; queueId < FW_QUEUE_MAX_NUM; queueId++) {
		if(s_fw_queue_mng[queueId].state == FW_QUE_AVAILABLE) break;
	}
	
	if(queueId >= FW_QUEUE_MAX_NUM) {
		QUE_LOG_WARNING("Sorry all queue has been used, create queue FAIL!!!");
		ret = -1;
	}
	else{
		fw_queue_cell_buff_list_create(&s_fw_queue_mng[queueId], queCapacity, queCellSize);

		pthread_create_mutex(&s_fw_queue_mng[queueId].mutex);
		pthread_create_cond(&s_fw_queue_mng[queueId].pushCond);
		pthread_create_cond(&s_fw_queue_mng[queueId].popCond);
		queCapacity = (queCapacity > FW_QUEUE_MAX_CAPICITY) ? FW_QUEUE_MAX_CAPICITY : queCapacity;
		s_fw_queue_mng[queueId].pLoopQueue = Fw_CreateLoopQueue(queCapacity);
		s_fw_queue_mng[queueId].queueCellSize = queCellSize;
		s_fw_queue_mng[queueId].state = FW_QUE_OCCUPIED;
	}
	SAFETY_MUTEX_UNLOCK(s_fw_queue_mutex);
	return ((ret != -1) && s_fw_queue_mng[queueId].pLoopQueue) ? queueId : -1;
}

/*
**	功能：销毁一个队列
**	参数：queId    队列ID
**  返回：-1     失败
		  0   成功
*/
int fw_queue_destory(int queId)
{
	int ret = 0;
	SAFETY_MUTEX_LOCK(s_fw_queue_mutex);
	SAFETY_MUTEX_LOCK(s_fw_queue_mng[queId].mutex);
	if(queId < 0 || queId >= FW_QUEUE_MAX_NUM || 
		s_fw_queue_mng[queId].state != FW_QUE_OCCUPIED){
		ret = -1;
	}
	else{
		s_fw_queue_mng[queId].state = FW_QUE_AVAILABLE;
		pthread_cond_broadcast(&s_fw_queue_mng[queId].pushCond);
		pthread_cond_broadcast(&s_fw_queue_mng[queId].popCond);
		Fw_DeleteLoopQueue(s_fw_queue_mng[queId].pLoopQueue);
		s_fw_queue_mng[queId].pLoopQueue = NULL;
		pthread_cond_destroy(&s_fw_queue_mng[queId].pushCond);
		pthread_cond_destroy(&s_fw_queue_mng[queId].popCond);

		fw_queue_cell_buff_list_destory(&s_fw_queue_mng[queId]);
	}
	SAFETY_MUTEX_UNLOCK(s_fw_queue_mng[queId].mutex);
	SAFETY_MUTEX_UNLOCK(s_fw_queue_mutex);
	return ret;
}


/*
**	功能：向队列中推送元件
**	参数：queId         队列ID
	      pQueCell   元件对象指针
	      timeOutMs  超时等待时间：-1 waitforever; =0 no wait; >0 wait ms
**  返回：-1     失败/超时
		  0   成功
*/
int fw_queue_push(int queId, void *pQueCell, int timeOutMs)
{
	int ret = -1;
	void *pCellBuff = NULL;
	if(fw_queue_get_cell_buff(&s_fw_queue_mng[queId].cellBuffListMng, (void**)&pCellBuff) != -1) {
		memcpy(pCellBuff, pQueCell, s_fw_queue_mng[queId].queueCellSize);
		ret = fw_queue_inner_push(queId, (void*)pCellBuff, timeOutMs);
		if(ret < 0) {
			fw_queue_free_cell_buff(&s_fw_queue_mng[queId].cellBuffListMng, pCellBuff);
		}
	}
	return ret;
}

/*
**	功能：从队列中取得一个元件
**	参数：queId         队列ID
	      pQueCell   元件对象指针
	      timeOutMs  超时等待时间：-1 waitforever; =0 no wait; >0 wait ms
**  返回：-1     失败/超时
		  0   成功
*/
int fw_queue_pop(int queId, void *pQueCell, int timeOutMs)
{
	enFwRetValue ret = FW_RET_SUCCESS;
	void *pCellBuff = NULL;
	if(!pQueCell) { 
		QUE_LOG_ERR("pQueCell Must A Valid Pointer!");
		return -1;
	}

	ret = fw_queue_inner_pop(queId, (void**)&pCellBuff, timeOutMs);
	if(ret == FW_RET_SUCCESS) {
		if(pCellBuff) {
			memcpy(pQueCell, pCellBuff, s_fw_queue_mng[queId].queueCellSize);
			fw_queue_free_cell_buff(&s_fw_queue_mng[queId].cellBuffListMng, pCellBuff);
			return 0;
		}
	}
	else {
		fw_queue_error_string(queId, ret);
	}
	return -1;
}


/*
**	功能：将队列队首元件丢弃
**	参数：queId         队列ID
**  返回：-1     失败
		  0   成功
*/
int fw_queue_drop(int queId)
{
	enFwRetValue ret = FW_RET_SUCCESS;
	void *pCellBuff = NULL;
	ret = fw_queue_inner_pop(queId, (void**)&pCellBuff, 0);
	if(ret == FW_RET_SUCCESS) {
		if(pCellBuff) {
			fw_queue_free_cell_buff(&s_fw_queue_mng[queId].cellBuffListMng, pCellBuff);
			return 0;
		}
	}
	else {
		fw_queue_error_string(queId, ret);
	}
	return -1;
}

/*
**	功能：将队列所有元件清空
**	参数：queId         队列ID
**  返回：NULL
*/
void fw_queue_clean(int queId)
{
	void *pCellBuff = NULL;
	QUE_LOG_NOTIFY("Queue[%d] have %d cell objs!", queId, Fw_GetLoopQueueNums(s_fw_queue_mng[queId].pLoopQueue));
	SAFETY_MUTEX_LOCK(s_fw_queue_mng[queId].mutex);
	while(Fw_ReadLoopQueue(s_fw_queue_mng[queId].pLoopQueue, (void**)&pCellBuff) != FW_RET_QUEUE_EMPTY){
		fw_queue_free_cell_buff(&s_fw_queue_mng[queId].cellBuffListMng, pCellBuff);
	}
	SAFETY_MUTEX_UNLOCK(s_fw_queue_mng[queId].mutex);
}

/*
**	功能：设置log输出等级
**	参数：NULL
**  返回：NULL
*/
void fw_queue_log_level(enLogLevel logLevel)
{
	s_log_out_level = logLevel;
}
