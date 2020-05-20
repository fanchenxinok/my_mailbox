#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "queue_fw.h"

typedef struct
{
	int a;
	int b;
}stCellType1;

typedef struct
{
	char name[64];
	int age;
}stCellType2;

void* que1_send1(void *user)
{
	int queId = *(int*)user;
	stCellType1 cellData;
	cellData.a = 0;
	cellData.b = 0;
	while(1) {
		cellData.a += 2;
		cellData.b += 2;
		fw_queue_push(queId, &cellData, 1000);
		usleep(10000);
	}
}

void* que1_send2(void *user)
{
	int queId = *(int*)user;
	stCellType1 cellData;
	cellData.a = 100;
	cellData.b = 100;
	while(1) {
		cellData.a += 2;
		cellData.b += 2;
		fw_queue_push(queId, &cellData, -1);
		usleep(20000);

		cellData.a += 3;
		cellData.b += 3;
		fw_queue_push(queId, &cellData, 1000);
		usleep(15000);
	}
}

void* que1_recev(void *user)
{
	int queId = *(int*)user;
	stCellType1 cellData;
	while(1) {
		if(fw_queue_pop(queId, &cellData, -1) != -1) {
			printf("Q1 Recv: a = %d, b = %d\n", cellData.a, cellData.b);
		}
	}
}

void* que2_send1(void *user)
{
	int queId = *(int*)user;
	stCellType2 cellData;
	while(1) {
		sprintf(cellData.name, "%s", "Xiaoming");
		cellData.age = 10;
		fw_queue_push(queId, &cellData, 1000);
		usleep(25000);
	}
}

void* que2_send2(void *user)
{
	int queId = *(int*)user;
	stCellType2 cellData;
	while(1) {
		sprintf(cellData.name, "%s", "XiaoLi");
		cellData.age = 20;
		fw_queue_push(queId, &cellData, -1);
		usleep(100000);

		sprintf(cellData.name, "%s", "XiaoHong");
		cellData.age = 24;
		fw_queue_push(queId, &cellData, 1000);
		usleep(40000);
	}
}

void* que2_recev(void *user)
{
	int queId = *(int*)user;
	stCellType2 cellData;
	while(1) {
		if(fw_queue_pop(queId, &cellData, -1) != -1) {
			printf("Q2 Recv: name = %s, age = %d\n", cellData.name, cellData.age);
		}
	}
}


void main(int argc, char* argv[])
{
	int ret = 0;
	if(argc == 2) {
		enLogLevel log_level = atoi(argv[1]);
		fw_queue_log_level(log_level);
	}

	int que1 = fw_queue_create(20, sizeof(stCellType1));
	if(que1 < 0) {
		printf("create queue 1 fail!\n");
	}

	int que2 = fw_queue_create(30, sizeof(stCellType2));
	if(que1 < 0) {
		printf("create queue 1 fail!\n");
	}

	pthread_t thread_id;
	pthread_create(&thread_id, NULL, &que1_recev, (void*)&que1);
	pthread_create(&thread_id, NULL, &que1_send1, (void*)&que1);
	pthread_create(&thread_id, NULL, &que1_send2, (void*)&que1);
	
	pthread_create(&thread_id, NULL, &que2_recev, (void*)&que2);
	pthread_create(&thread_id, NULL, &que2_send1, (void*)&que2);
	pthread_create(&thread_id, NULL, &que2_send2, (void*)&que2);

	int cnt = 0;
	while(cnt < 5) {
		usleep(1000000);
		cnt++;
	}

	fw_queue_destory(que2);
	fw_queue_destory(que1);
}

