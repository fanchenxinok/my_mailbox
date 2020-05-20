#ifndef __MY_QUEUE_FW_H__
#define __MY_QUEUE_FW_H__

/****************************************************
 This is Queue universal Framework
 Author: Fanchenxin
 Email:  531266381@qq.com
 ****************************************************/
#define FW_QUEUE_MAX_NUM	(64)

/* log level */
typedef enum{
	LOG_LEVEL_NONE,
	LOG_LEVEL_ERR,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_NOTIFY,
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_ALL
}enLogLevel;

/*
**	功能：创建一个队列
**	参数：queCapacity    队列容量，容纳多少个元件(cell)
          queCellSize  队列存放每个元件的大小
**  返回：-1     失败
		  >=0 返回队列ID
*/
int fw_queue_create(unsigned int queCapacity, unsigned int queCellSize);

/*
**	功能：销毁一个队列
**	参数：queId    队列ID
**  返回：-1     失败
		  0   成功
*/
int fw_queue_destory(int queId);

/*
**	功能：向队列中推送元件
**	参数：queId         队列ID
	      pQueCell   元件对象指针
	      timeOutMs  超时等待时间：-1 waitforever; =0 no wait; >0 wait ms
**  返回：-1     失败/超时
		  0   成功
*/
int fw_queue_push(int queId, void *pQueCell, int timeOutMs);

/*
**	功能：从队列中取得一个元件
**	参数：queId         队列ID
	      pQueCell   元件对象指针
	      timeOutMs  超时等待时间：-1 waitforever; =0 no wait; >0 wait ms
**  返回：-1     失败/超时
		  0   成功
*/
int fw_queue_pop(int queId, void *pQueCell, int timeOutMs);

/*
**	功能：将队列队首元件丢弃
**	参数：queId         队列ID
**  返回：-1     失败
		  0   成功
*/
int fw_queue_drop(int queId);

/*
**	功能：将队列所有元件清空
**	参数：queId         队列ID
**  返回：NULL
*/
void fw_queue_clean(int queId);

/*
**	功能：设置log输出等级, default: LOG_LEVEL_ALL
**	参数：NULL
**  返回：NULL
*/
void fw_queue_log_level(enLogLevel logLevel);

#endif