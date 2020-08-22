/* job.c - Thread related functions
*
* Copyright(C) 2019 - 2020, sun shuo <sun.shuo@surparallel.org>
* All rights reserved.
*
* This program is free software : you can redistribute it and / or modify
* it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or(at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program.If not, see < https://www.gnu.org/licenses/>.
*/

#include "plateform.h"
#include <pthread.h>
#include "psds.h"
#include "pdict.h"
#include "pjob.h"
#include "pequeue.h"
#include "padlist.h"
#include "pcache.h"
#include "pinterface.h"
#include "pmanage.h"
#include "plocks.h"
#include "pelog.h"
#include "pdictexten.h"
#include "ptimesys.h"
#include "plibsys.h"
#include "plvm.h"
#include "pquicksort.h"
#include "pelagia.h"
#include "pjson.h"
#include "pfilesys.h"

/*
Thread model can be divided into two ways: asynchronous and synchronous
This is also true between multiple threads
For read operations, synchronization is used because the user virtual single thread environment needs to be maintained
For write operations, asynchronous queue mode is used for efficiency between different hardware environments
Disk writes are not crowded, so thread queues and threads are not implemented
Manage has separate threads because it manages multiple modules
File has a separate thread to write to a slow hard disk
*/
#define NORET
#define CheckUsingThread(r) if (plg_JobCheckUsingThread()) {elog(log_error, "Cannot run job interface in non job environment"); return r;}
#define MAXJOBID 1024
#define MAXORDERID 4194304

enum ScriptType {
	ST_LUA = 1,
	ST_LIB = 2,
	ST_PTR = 3
};

typedef struct _EventPorcess
{
	unsigned char scriptType;
	sds fileClass;
	sds function;
	RoutingFun functionPoint;
	unsigned int weight;
}*PEventPorcess, EventPorcess;

static void PtrFreeCallback(void *privdata, void *val) {
	DICT_NOTUSED(privdata);
	plg_CacheDestroyHandle(val);
}

static int sdsCompareCallback(void *privdata, const void *key1, const void *key2) {
	int l1, l2;
	DICT_NOTUSED(privdata);

	l1 = plg_sdsLen((sds)key1);
	l2 = plg_sdsLen((sds)key2);
	if (l1 != l2) return 0;
	return memcmp(key1, key2, l1) == 0;
}

static unsigned long long sdsHashCallback(const void *key) {
	return plg_dictGenHashFunction((unsigned char*)key, plg_sdsLen((char*)key));
}

static dictType PtrDictType = {
	sdsHashCallback,
	NULL,
	NULL,
	sdsCompareCallback,
	NULL,
	PtrFreeCallback
};

static void FreeCallback(void *privdata, void *val) {
	DICT_NOTUSED(privdata);
	free(val);
}

static dictType SdsDictType = {
	sdsHashCallback,
	NULL,
	NULL,
	sdsCompareCallback,
	NULL,
	FreeCallback
};

static void uintFreeCallback(void *privdata, void *val) {
	NOTUSED(privdata);
	unsigned int* ptr = (unsigned int*)val;
	free(ptr);
}

static unsigned long long hashCallback(const void *key) {
	return plg_dictGenHashFunction((unsigned char*)key, sizeof(unsigned int));
}

static int uintCompareCallback(void *privdata, const void *key1, const void *key2) {
	NOTUSED(privdata);
	if (*(unsigned int*)key1 != *(unsigned int*)key2)
		return 0;
	else
		return 1;
}

static dictType uintDictType = {
	hashCallback,
	NULL,
	NULL,
	uintCompareCallback,
	uintFreeCallback,
	NULL
};

typedef struct __Intervalometer {
	unsigned long long tim;
	sds Order;
	sds Value;
	unsigned int orderID;
}*PIntervalometer, Intervalometer;

/*
Threadtype: the type of the current thread
Pmanagequeue: event handle for management thread
Privatedata: the private data of the current thread, which is filehandle
EQueue: the message slot of the current thread
Order Ou queue: message slot corresponding to all events
Dictcache: all the caches of a job are used to release, create and check whether the tablename is writable
Order_Process: the processing process of the current thread event
Tablename? Cachehandle: Currently, all cachehandles corresponding to tablename are used to find and write data
Allweight: all processes have the same weight
Userevent: event not called remotely
Userprocess: handling of non remote call events
ExitThread: exit flag
Trancache: cache used by the current transaction
Tranflush: multiple caches preparing for flush
Transaction commit related flags
Flush_laststamp: time of last submission
Flush_interval: commit interval
Flush_Lastcount: number of submissions
Flush_count: total number of times
*/
typedef struct _JobHandle
{
	enum ThreadType threadType;
	void* pManageEqueue;
	void* privateData;
	void* eQueue;
	dict* order_equeue;
	dict* dictCache;
	dict* order_process;
	dict* tableName_cacheHandle;
	unsigned int allWeight;
	list* userEvent;
	list* userProcess;
	short exitThread;
	short donotFlush;
	short donotCommit;

	list* tranCache;
	list* tranFlush;

	//config
	unsigned long long flush_lastStamp;
	unsigned int flush_interval;
	unsigned int flush_lastCount;
	unsigned int flush_count;

	//vm
	void* luaHandle;

	//current order name from order_process;
	char* pOrderName;

	//intervalometer
	list* pListIntervalometer;
	PIntervalometer pMinPIntervalometer;

	//Statistics
	short isOpenStat;

	//order run time
	dict* order_runCount;
	unsigned long long statistics_frequency;
	unsigned int statistics_eventQueueLength;
	dict* order_msg;
	dict* order_byte;

	unsigned int maxQueue;

	//exit value
	sds m_value;

	//service id
	unsigned int jobID;
	unsigned int curretnOrderID;
	unsigned int orderID;
	dict* orderID_ptr;

} *PJobHandle, JobHandle;

static char* TString[] = {
	"TT_Byte",
	"TT_Double",
	"TT_String",
	"TT_Set",
};

char* plg_TT2String(unsigned short tt) {

	if (tt <= TT_Set) {
		return TString[tt];
	} else {
		return "unknown type";
	}
}

void* plg_JobCreateFunPtr(RoutingFun funPtr) {

	PEventPorcess pEventPorcess = malloc(sizeof(EventPorcess));
	pEventPorcess->scriptType = ST_PTR;
	pEventPorcess->functionPoint = funPtr;
	pEventPorcess->weight = 1;
	return pEventPorcess;
}

void* plg_JobCreateLua(char* fileClass, short fileClassLen, char* fun, short funLen) {

	PEventPorcess pEventPorcess = malloc(sizeof(EventPorcess));
	pEventPorcess->scriptType = ST_LUA;
	pEventPorcess->fileClass = plg_sdsNewLen(fileClass, fileClassLen);
	pEventPorcess->function = plg_sdsNewLen(fun, funLen);
	pEventPorcess->weight = 1;
	return pEventPorcess;
}

void* plg_JobCreateLib(char* fileClass, short fileClassLen, char* fun, short funLen) {

	PEventPorcess pEventPorcess = malloc(sizeof(EventPorcess));
	pEventPorcess->scriptType = ST_LIB;
	pEventPorcess->fileClass = plg_sdsNewLen(fileClass, fileClassLen);
	pEventPorcess->function = plg_sdsNewLen(fun, funLen);
	pEventPorcess->weight = 1;
	return pEventPorcess;
}

void plg_JobSetWeight(void* pvEventPorcess, unsigned int weight) {
	PEventPorcess pEventPorcess = pvEventPorcess;
	pEventPorcess->weight = weight;
}

void plg_JobProcessDestory(void* pvEventPorcess) {

	PEventPorcess pEventPorcess = pvEventPorcess;
	if (pEventPorcess->scriptType != ST_PTR) {
		plg_sdsFree(pEventPorcess->fileClass);
		plg_sdsFree(pEventPorcess->function);
	}
	free(pEventPorcess);
}

static void listSdsFree(void *ptr) {
	plg_sdsFree(ptr);
}

static void listIntervalometerFree(void *ptr) {
	PIntervalometer pPIntervalometer = (PIntervalometer)ptr;
	plg_sdsFree(pPIntervalometer->Order);
	plg_sdsFree(pPIntervalometer->Value);
	free(ptr);
}

static void listProcessFree(void *ptr) {
	plg_JobProcessDestory(ptr);
}

void* job_Handle() {

	CheckUsingThread(0);
	void* p = plg_LocksGetSpecific();
	if (!p) {
		plg_assert(0);
		elog(log_error, "job_Handle ");
	}

	return p;
}

unsigned int job_MaxQueue() {

	CheckUsingThread(0);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}
	return pJobHandle->maxQueue;
}

void* job_ManageEqueue() {

	CheckUsingThread(0);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}
	return pJobHandle->pManageEqueue;
}

void plg_JobSetExitThread(char value) {

	CheckUsingThread(NORET);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}
	pJobHandle->exitThread = value;
}

void plg_JobSetDonotFlush() {

	CheckUsingThread(NORET);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}
	pJobHandle->donotFlush = 1;
}

void plg_JobSetDonotCommit() {

	CheckUsingThread(NORET);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}
	pJobHandle->donotCommit = 1;
}

void job_Flush(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	listIter* iter = plg_listGetIterator(pJobHandle->tranFlush, AL_START_HEAD);
	listNode* node;
	while ((node = plg_listNext(iter)) != NULL) {
		plg_CacheFlush(listNodeValue(node));
	}
	plg_listReleaseIterator(iter);
	plg_listEmpty(pJobHandle->tranFlush);
}

void job_Commit(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	listIter* iter = plg_listGetIterator(pJobHandle->tranCache, AL_START_HEAD);
	listNode* node;
	while ((node = plg_listNext(iter)) != NULL) {
		plg_CacheCommit(listNodeValue(node));

		plg_listAddNodeHead(pJobHandle->tranFlush, listNodeValue(node));
	}
	plg_listReleaseIterator(iter);
	plg_listEmpty(pJobHandle->tranCache);
}

void job_Rollback(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	listIter* iter = plg_listGetIterator(pJobHandle->tranCache, AL_START_HEAD);
	listNode* node;
	while ((node = plg_listNext(iter)) != NULL) {
		plg_CacheRollBack(listNodeValue(node));
	}
	plg_listReleaseIterator(iter);
	plg_listEmpty(pJobHandle->tranCache);
}

static int OrderDestroy(char* value, short valueLen) {
	elog(log_fun, "job.OrderDestroy");
	PJobHandle pJobHandle = job_Handle();
	pJobHandle->m_value = plg_sdsNewLen(value, valueLen);
	plg_JobSetExitThread(3);
	return 1;
}

static int OrderDestroyJob(char* value, short valueLen) {
	NOTUSED(value);
	NOTUSED(valueLen);
	elog(log_fun, "job.OrderDestroyJob");
	plg_JobSetExitThread(1);
	return 1;
}

static int OrderJobFinish(char* value, short valueLen) {
	NOTUSED(value);
	NOTUSED(valueLen);
	PJobHandle pJobHandle = job_Handle();

	if (!pJobHandle->donotCommit) {
		job_Commit(pJobHandle);		
	} else {
		pJobHandle->donotCommit = 0;
	}
	
	if (!pJobHandle->donotFlush) {
		unsigned long long stamp = plg_GetCurrentSec();
		if (pJobHandle->flush_count > pJobHandle->flush_lastCount++) {
			pJobHandle->flush_lastCount = 0;
			job_Flush(pJobHandle);
		} else if (pJobHandle->flush_lastStamp - stamp > pJobHandle->flush_interval) {
			pJobHandle->flush_lastStamp = stamp;
			job_Flush(pJobHandle);
		}	
	} else {
		pJobHandle->donotFlush = 0;
	}
	return 1;
}

void plg_JobForceCommit() {
	PJobHandle pJobHandle = job_Handle();
	job_Commit(pJobHandle);
	job_Flush(pJobHandle);
}

static void InitProcessCommend(void* pvJobHandle) {

	//event process
	PJobHandle pJobHandle = pvJobHandle;
	plg_JobAddAdmOrderProcess(pJobHandle, "destroy", plg_JobCreateFunPtr(OrderDestroy));
	plg_JobAddAdmOrderProcess(pJobHandle, "destroyjob", plg_JobCreateFunPtr(OrderDestroyJob));
	plg_JobAddAdmOrderProcess(pJobHandle, "finish", plg_JobCreateFunPtr(OrderJobFinish));
}

void plg_JobSetPrivate(void* pvJobHandle, void* privateData) {
	PJobHandle pJobHandle = pvJobHandle;
	pJobHandle->privateData = privateData;
}

void* plg_JobGetPrivate() {

	CheckUsingThread(0);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}
	return pJobHandle->privateData;
}

SDS_TYPE
void* plg_JobCreateHandle(void* pManageEqueue, enum ThreadType threadType, char* luaLIBPath, short luaHot, unsigned int jobID) {

	PJobHandle pJobHandle = malloc(sizeof(JobHandle));
	pJobHandle->eQueue = plg_eqCreate();
	pJobHandle->order_equeue = plg_dictCreate(plg_DefaultSdsDictPtr(), NULL, DICT_MIDDLE);
	pJobHandle->order_process = plg_dictCreate(plg_DefaultSdsDictPtr(), NULL, DICT_MIDDLE);
	pJobHandle->tableName_cacheHandle = plg_dictCreate(plg_DefaultSdsDictPtr(), NULL, DICT_MIDDLE);
	pJobHandle->dictCache = plg_dictCreate(&PtrDictType, NULL, DICT_MIDDLE);
	pJobHandle->order_runCount = plg_dictCreate(&SdsDictType, NULL, DICT_MIDDLE);
	pJobHandle->order_msg = plg_dictCreate(&SdsDictType, NULL, DICT_MIDDLE);
	pJobHandle->order_byte = plg_dictCreate(&SdsDictType, NULL, DICT_MIDDLE);

	pJobHandle->tranCache = plg_listCreate(LIST_MIDDLE);
	pJobHandle->tranFlush = plg_listCreate(LIST_MIDDLE);

	pJobHandle->userEvent = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pJobHandle->userEvent, listSdsFree);

	pJobHandle->userProcess = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pJobHandle->userProcess, listProcessFree);

	pJobHandle->threadType = threadType;
	pJobHandle->pManageEqueue = pManageEqueue;
	pJobHandle->allWeight = 0;
	pJobHandle->exitThread = 0;
	pJobHandle->donotFlush = 0;
	pJobHandle->donotCommit = 0;
	pJobHandle->privateData = 0;
	pJobHandle->pMinPIntervalometer = 0;
	
	pJobHandle->flush_lastStamp = plg_GetCurrentSec();
	pJobHandle->flush_interval = 5*60;
	pJobHandle->flush_count = 1;
	pJobHandle->flush_lastCount = 0;
	pJobHandle->maxQueue = 0;

	if (luaLIBPath && plg_sdsLen(luaLIBPath)) {
		pJobHandle->luaHandle = plg_LvmLoad(luaLIBPath, luaHot);
	} else {
		pJobHandle->luaHandle = 0;
	}

	SDS_CHECK(pJobHandle->allWeight, pJobHandle->luaHandle);
	pJobHandle->pListIntervalometer = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pJobHandle->pListIntervalometer, listIntervalometerFree);

	if (pJobHandle->threadType == TT_PROCESS) {
		InitProcessCommend(pJobHandle);
	}

	pJobHandle->isOpenStat = 0;
	pJobHandle->statistics_frequency = 5000;
	pJobHandle->statistics_eventQueueLength = 0;

	pJobHandle->jobID = jobID;
	pJobHandle->orderID = 0;
	pJobHandle->orderID_ptr = plg_dictCreate(&uintDictType, NULL, DICT_MIDDLE);

	elog(log_fun, "plg_JobCreateHandle:%U", pJobHandle);
	return pJobHandle;
}

static void OrderFree(void* ptr) {

	POrderPacket pOrderPacket = ptr;
	plg_sdsFree(pOrderPacket->order);
	plg_sdsFree(pOrderPacket->value);
	free(pOrderPacket);
}

void plg_JobDestoryHandle(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	elog(log_fun, "plg_JobDestoryHandle:%U", pJobHandle);
	plg_eqDestory(pJobHandle->eQueue, OrderFree);
	plg_dictRelease(pJobHandle->order_equeue);
	plg_dictRelease(pJobHandle->dictCache);
	plg_listRelease(pJobHandle->tranCache);
	plg_listRelease(pJobHandle->tranFlush);
	plg_dictRelease(pJobHandle->order_process);
	plg_dictRelease(pJobHandle->tableName_cacheHandle);
	plg_listRelease(pJobHandle->userEvent);
	plg_listRelease(pJobHandle->userProcess);
	plg_listRelease(pJobHandle->pListIntervalometer);
	plg_dictRelease(pJobHandle->order_runCount);
	plg_dictRelease(pJobHandle->order_msg);
	plg_dictRelease(pJobHandle->order_byte);
	plg_dictRelease(pJobHandle->orderID_ptr);

	if (pJobHandle->luaHandle) {
		plg_LvmDestory(pJobHandle->luaHandle);
	}

	free(pJobHandle);
}

unsigned char plg_JobFindTableName(void* pvJobHandle, sds tableName) {

	PJobHandle pJobHandle = pvJobHandle;
	dictEntry * entry = plg_dictFind(pJobHandle->dictCache, tableName);
	if (entry != 0) {
		return 1;
	} else {
		return 0;
	}
}

void plg_JobAddEventEqueue(void* pvJobHandle, sds nevent, void* equeue) {
	PJobHandle pJobHandle = pvJobHandle;
	plg_dictAdd(pJobHandle->order_equeue, nevent, equeue);
}

void plg_JobAddEventProcess(void* pvJobHandle, sds nevent, void* pvProcess) {

	PEventPorcess process = pvProcess;
	PJobHandle pJobHandle = pvJobHandle;
	plg_dictAdd(pJobHandle->order_process, nevent, process);
	pJobHandle->allWeight += process->weight;
}

/*
In this case, the cache should be triggered to obtain the disk related handle or perform table initialization when the re query is empty
*/
void* plg_JobNewTableCache(void* pvJobHandle, char* table, void* pDiskHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, table);
	if (valueEntry == 0) {
		void* pCacheHandle = plg_CacheCreateHandle(pDiskHandle);
		plg_CacheSetStat(pCacheHandle, pJobHandle->isOpenStat);
		plg_dictAdd(pJobHandle->dictCache, table, pCacheHandle);
		return pCacheHandle;
	} else {
		return dictGetVal(valueEntry);
	}
}

void plg_JobAddTableCache(void* pvJobHandle, char* table, void* pCacheHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, table);
	if (valueEntry == 0) {
		plg_dictAdd(pJobHandle->tableName_cacheHandle, table, pCacheHandle);
	}
}

void* plg_JobEqueueHandleIsCore(void* pvJobHandle, unsigned int core) {
	PJobHandle pJobHandle = pvJobHandle;
	if (pJobHandle->jobID == core) {
		return pJobHandle->eQueue;
	} else {
		return 0;
	}
}

void* plg_JobEqueueHandle(void* pvJobHandle) {
	PJobHandle pJobHandle = pvJobHandle;
	return pJobHandle->eQueue;
}

unsigned int  plg_JobAllWeight(void* pvJobHandle) {
	PJobHandle pJobHandle = pvJobHandle;
	return pJobHandle->allWeight;
}

unsigned int  plg_JobIsEmpty(void* pvJobHandle) {
	PJobHandle pJobHandle = pvJobHandle;
	return dictSize(pJobHandle->order_process);
}

/*
User VM use
*/
int plg_JobRemoteCallWithOrderID(void* order, short orderLen, void* value, short valueLen, unsigned int orderID) {

	CheckUsingThread(0);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		
		return 0;
	}

	POrderPacket pOrderPacket = malloc(sizeof(OrderPacket));
	pOrderPacket->order = plg_sdsNewLen(order, orderLen);
	pOrderPacket->value = plg_sdsNewLen(value, valueLen);
	pOrderPacket->orderID = 0;
	
	dictEntry* entryOrder = plg_dictFind(pJobHandle->order_equeue, pOrderPacket->order);
	if (entryOrder) {

		if (orderID != 0) {
			elog(log_error, "plg_JobRemoteCallWithOrderID::Use OrderID %i to call an order with shared data", orderID);
		}
		if (0 == plg_eqIfNoPush(dictGetVal(entryOrder), pOrderPacket, pJobHandle->maxQueue)) {
			plg_sdsFree(pOrderPacket->order);
			plg_sdsFree(pOrderPacket->value);
			free(pOrderPacket);
			
			elog(log_error, "plg_MngRemoteCall Queue limit exceeded for %i", pJobHandle->maxQueue);
		}
		if (pJobHandle->isOpenStat) {
			dictAddValueWithUint(pJobHandle->order_msg, dictGetKey(entryOrder), 1);
			dictAddValueWithUint(pJobHandle->order_byte, dictGetKey(entryOrder), valueLen);
		}
		return 1;
	} else {
		void* pManage = pJobHandle->privateData;
		char* retOrder;
		pOrderPacket->orderID = orderID;
		int r = plg_MngRemoteCallPacket(pManage, pOrderPacket, &retOrder, orderID);
		if (pJobHandle->isOpenStat) {
			dictAddValueWithUint(pJobHandle->order_msg, dictGetKey(entryOrder), 1);
			dictAddValueWithUint(pJobHandle->order_byte, dictGetKey(entryOrder), valueLen);
		}
		return r;
	}
}

int plg_JobRemoteCall(void* order, short orderLen, void* value, short valueLen) {
	return plg_JobRemoteCallWithOrderID(order, orderLen, value, valueLen, 0);
}

int plg_JobRemoteCallWithMaxCore(void* order, short orderLen, void* value, short valueLen) {
	CheckUsingThread(0);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsOrder = plg_sdsNewLen(order, orderLen);

	dictEntry* entryOrder = plg_dictFind(pJobHandle->order_equeue, sdsOrder);
	plg_sdsFree(sdsOrder);
	if (entryOrder) {
		elog(log_error, "plg_JobRemoteCallWithMaxCore::to call an order with shared data");
		return 0;
	} else {
		void* pManage = pJobHandle->privateData;
		int r = plg_MngRemoteCallWithMaxCore(pManage, order, orderLen, value, valueLen);

		if (pJobHandle->isOpenStat) {
			dictAddValueWithUint(pJobHandle->order_msg, dictGetKey(entryOrder), r);
			dictAddValueWithUint(pJobHandle->order_byte, dictGetKey(entryOrder), valueLen * r);
		}
		return r;
	}
}


static char job_IsCacheAllowWrite(void* pvJobHandle, char* PtrCache) {

	PJobHandle pJobHandle = pvJobHandle;
	dictEntry* entry = plg_dictFind(pJobHandle->dictCache, PtrCache);
	if (entry != 0)
		return 1;
	else
		return 0;
}

static int job_IsTableAllowWrite(void* pvJobHandle, char* sdsTable) {

	PJobHandle pJobHandle = pvJobHandle;
	return plg_MngTableIsInOrder(pJobHandle->privateData, pJobHandle->pOrderName, plg_sdsLen(pJobHandle->pOrderName), sdsTable, plg_sdsLen(sdsTable));
}

static unsigned long long plg_JogActIntervalometer(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	if (!listLength(pJobHandle->pListIntervalometer)) {
		return 0;
	}
	
	unsigned long long milli = plg_GetCurrentMilli();
	listIter* iter = plg_listGetIterator(pJobHandle->pListIntervalometer, AL_START_HEAD);
	listNode* node;
	while ((node = plg_listNext(iter)) != NULL) {

		PIntervalometer pPIntervalometer = (PIntervalometer)node->value;
		if (pPIntervalometer->tim <= milli) {
			plg_JobRemoteCallWithOrderID(pPIntervalometer->Order, plg_sdsLen(pPIntervalometer->Order), pPIntervalometer->Value, plg_sdsLen(pPIntervalometer->Value), pPIntervalometer->orderID);
			plg_listDelNode(pJobHandle->pListIntervalometer, node);
		} else if (!pJobHandle->pMinPIntervalometer) {
			pJobHandle->pMinPIntervalometer = pPIntervalometer;
		} else if (pPIntervalometer->tim < pJobHandle->pMinPIntervalometer->tim) {
			pJobHandle->pMinPIntervalometer = pPIntervalometer;
		}
		else {
			break;
		}
	}
	plg_listReleaseIterator(iter);

	if (pJobHandle->pMinPIntervalometer) {
		return pJobHandle->pMinPIntervalometer->tim;
	} else {
		return 0;
	}
}

static unsigned long long plg_JogMinIntervalometer(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	if (!listLength(pJobHandle->pListIntervalometer)) {
		return 0;
	}

	if (pJobHandle->pMinPIntervalometer) {
		return pJobHandle->pMinPIntervalometer->tim;
	} else {
		return 0;
	}
}

void plg_JobSetStat(void* pvJobHandle, short stat, unsigned long long checkTime) {
	PJobHandle pJobHandle = pvJobHandle;
	pJobHandle->isOpenStat = stat;
	pJobHandle->statistics_frequency = checkTime;
}

void plg_JobSetMaxQueue(void* pvJobHandle, unsigned int maxQueue){
	PJobHandle pJobHandle = pvJobHandle;
	pJobHandle->maxQueue = maxQueue;
}

static void plg_LogStat(void* pvJobHandle, unsigned long long passTime) {

	pJSON* root = pJson_CreateObject();
	PJobHandle pJobHandle = pvJobHandle;

	pJSON* orderJson = pJson_CreateObject();
	pJson_AddItemToObject(root, "order", orderJson);
	pJson_AddNumberToObject(orderJson, "time", passTime);
	pJson_AddNumberToObject(orderJson, "queue", pJobHandle->statistics_eventQueueLength);
	pJobHandle->statistics_eventQueueLength = 0;

	if (dictSize(pJobHandle->order_runCount)){
		pJSON* runJson = pJson_CreateObject();
		pJson_AddItemToObject(orderJson, "run", runJson);
		dictIterator* iter_runCount = plg_dictGetSafeIterator(pJobHandle->order_runCount);
		dictEntry* node_runCount;
		while ((node_runCount = plg_dictNext(iter_runCount)) != NULL) {
			char* key = dictGetKey(node_runCount);
			unsigned int* count = dictGetVal(node_runCount);
			pJson_AddNumberToObject(runJson, key, *count);
		}
		plg_dictReleaseIterator(iter_runCount);
	}

	if (dictSize(pJobHandle->order_msg)){
		pJSON* callJson = pJson_CreateObject();
		pJson_AddItemToObject(orderJson, "call", callJson);
		dictIterator* iter_msg = plg_dictGetSafeIterator(pJobHandle->order_msg);
		dictEntry* node_msg;
		while ((node_msg = plg_dictNext(iter_msg)) != NULL) {
			char* key = dictGetKey(node_msg);
			unsigned int* count = dictGetVal(node_msg);

			unsigned int byte = 0;
			dictEntry*node_byte = plg_dictFind(pJobHandle->order_byte, key);
			if (node_byte) {
				byte = *(unsigned int*)dictGetVal(node_byte);
			}

			pJSON* callItemJson = pJson_CreateObject();
			pJson_AddItemToObject(callJson, key, callItemJson);
			pJson_AddNumberToObject(callItemJson, "count", *count);
			pJson_AddNumberToObject(callItemJson, "byte", byte);
		}
		plg_dictReleaseIterator(iter_msg);
	}

	unsigned long long allCacheCount = 0;
	unsigned long long allFreeCacheCount = 0;
	
	dictIterator* iter_cache = plg_dictGetSafeIterator(pJobHandle->dictCache);
	dictEntry* node_cache;
	while ((node_cache = plg_dictNext(iter_cache)) != NULL) {
		unsigned long long cacheCount;
		unsigned long long freeCacheCount;
		plg_CachePageAllCount(dictGetVal(node_cache), &cacheCount, &freeCacheCount);
		allCacheCount += cacheCount;
		allFreeCacheCount += freeCacheCount;
	}
	plg_dictReleaseIterator(iter_cache);

	pJSON* cacheJson = pJson_CreateObject();
	pJson_AddItemToObject(root, "cache", cacheJson);
	pJson_AddNumberToObject(cacheJson, "cache", allCacheCount);
	pJson_AddNumberToObject(cacheJson, "free", allFreeCacheCount);

	if (dictSize(pJobHandle->dictCache)) {
		iter_cache = plg_dictGetSafeIterator(pJobHandle->dictCache);
		while ((node_cache = plg_dictNext(iter_cache)) != NULL) {
			plg_CachePageCountPrint(dictGetVal(node_cache), cacheJson);
		}
		plg_dictReleaseIterator(iter_cache);
	}

	char* outPut = pJson_PrintUnformatted(root);
	pJson_Delete(root);
	elog(log_stat, "%s", outPut);
	free(outPut);
}

static void* plg_JobThreadRouting(void* pvJobHandle) {

	PJobHandle pJobHandle = pvJobHandle;
	plg_LocksSetSpecific(pJobHandle);
	
	elog(log_fun, "plg_JobThreadRouting");

	//init
	void* sdsKey = plg_sdsNew("init");
	dictEntry* entry = plg_dictFind(pJobHandle->order_process, sdsKey);
	if (entry) {
		PEventPorcess pEventPorcess = (PEventPorcess)dictGetVal(entry);
		if (pEventPorcess->scriptType == ST_PTR) {
			pEventPorcess->functionPoint(NULL, 0);
		}
	}
	plg_sdsFree(sdsKey);

	//start
	sdsKey = plg_sdsNew("start");
	PEventPorcess pStartPorcess = 0;
	entry = plg_dictFind(pJobHandle->order_process, sdsKey);
	if (entry) {
		pStartPorcess = (PEventPorcess)dictGetVal(entry);
	}
	plg_sdsFree(sdsKey);

	//finish
	sdsKey = plg_sdsNew("finish");
	PEventPorcess pFinishPorcess = 0;
	entry = plg_dictFind(pJobHandle->order_process, sdsKey);
	if (entry) {
		pFinishPorcess = (PEventPorcess)dictGetVal(entry);
	}
	plg_sdsFree(sdsKey);

	unsigned long long timer = 0;
	unsigned long long checkTime = plg_GetCurrentMilli();

	do {
		if (timer == 0) {
			plg_eqWait(pJobHandle->eQueue);
		} else {

			long long secs = timer / 1000;
			long long msecs = (timer % 1000) * (1000 * 1000);
			if (-1 == plg_eqTimeWait(pJobHandle->eQueue, secs, msecs)) {
				timer = plg_JogActIntervalometer(pJobHandle);
			}
		}

		do {
			unsigned int nowEventQueueLength;
			POrderPacket pOrderPacket = (POrderPacket)plg_eqPopWithLen(pJobHandle->eQueue, &nowEventQueueLength);
			if (pOrderPacket != 0) {
				if (pJobHandle->statistics_eventQueueLength < nowEventQueueLength) {
					pJobHandle->statistics_eventQueueLength = nowEventQueueLength;
				}

				elog(log_details, "ThreadType:%i.plg_JobThreadRouting.order:%s jobid:%i", pJobHandle->threadType, pJobHandle->jobID, (char*)pOrderPacket->order);
				pJobHandle->pOrderName = pOrderPacket->order;

				//start
				if (pStartPorcess && pStartPorcess->scriptType == ST_PTR) {
					pStartPorcess->functionPoint(NULL, 0);
				}

				PEventPorcess pEventPorcess = 0;
				entry = plg_dictFind(pJobHandle->order_process, pOrderPacket->order);
				if (entry) {
					pEventPorcess = (PEventPorcess)dictGetVal(entry);
					pJobHandle->pOrderName = dictGetKey(entry);
				} else {
					void* pManage = pJobHandle->privateData;
					pEventPorcess = plg_MngGetProcess(pManage, pOrderPacket->order, &pJobHandle->pOrderName);
				} 
				
				if (!pEventPorcess) {
					elog(log_error, "not proocess for order %s", pOrderPacket->order);
					continue;
				}

				if (pEventPorcess) {

					if (pOrderPacket->orderID) {
						pJobHandle->curretnOrderID = JobJobOrderID(pOrderPacket->orderID);
					} else {
						pJobHandle->curretnOrderID = 0;
					}

					if (pEventPorcess->scriptType == ST_PTR) {

						if (0 == pEventPorcess->functionPoint(pOrderPacket->value, plg_sdsLen(pOrderPacket->value))) {
							job_Rollback(pJobHandle);
						}
					} else if (pEventPorcess->scriptType == ST_LIB) {

						void* pManage = pJobHandle->privateData;
						void* libHandle = plg_MngFindLibFun(pManage, pEventPorcess->function);
						if (libHandle) {
							RoutingFun fun = plg_SysLibSym(libHandle, pEventPorcess->function);
							if (fun) {
								if (0 == fun(pOrderPacket->value, plg_sdsLen(pOrderPacket->value))) {
									job_Rollback(pJobHandle);
								}
							} else {
								elog(log_error, "Lib instruction %s received, but no Lib function found for %s!", (char*)pOrderPacket->order, pEventPorcess->function);
							}
						} else {
							elog(log_error, "Lib instruction %s received, but no Lib extern found for %s!", (char*)pOrderPacket->order, pEventPorcess->function);
						}

					} else if (pEventPorcess->scriptType == ST_LUA){
						if (pJobHandle->luaHandle)  {

							if (0 == plg_LvmCallFile(pJobHandle->luaHandle, pEventPorcess->fileClass, pEventPorcess->function, pOrderPacket->value, plg_sdsLen(pOrderPacket->value))) {
								job_Rollback(pJobHandle);
							}

						} else {
							elog(log_error, "Lua instruction %s received, but no Lua virtual machine found!", (char*)pOrderPacket->order);
						}
					}
				}

				//finish
				if (pFinishPorcess && pFinishPorcess->scriptType == ST_PTR) {
					pFinishPorcess->functionPoint(NULL, 0);
				}

				if (pJobHandle->isOpenStat) {
					dictAddValueWithUint(pJobHandle->order_runCount, pJobHandle->pOrderName, 1);
					unsigned long long milli = plg_GetCurrentMilli();
					if ((milli - checkTime) > pJobHandle->statistics_frequency) {	
						plg_LogStat(pJobHandle, milli - checkTime);
						checkTime = milli;
						plg_dictEmpty(pJobHandle->order_runCount, 0);
						plg_dictEmpty(pJobHandle->order_msg, 0);
					}
				}

				pJobHandle->pOrderName = 0;
				plg_sdsFree(pOrderPacket->order);
				plg_sdsFree(pOrderPacket->value);
				free(pOrderPacket);

				elog(log_details, "plg_JobThreadRouting.finish!");
			} else {
				break; 
			}

			if (pJobHandle->exitThread != 0) {
				break;
			}
		} while (1);

		timer = plg_JogMinIntervalometer(pJobHandle);
		plg_assert(listLength(pJobHandle->pListIntervalometer)?timer:1);

		if (pJobHandle->exitThread == 1) {

			elog(log_details, "ThreadType:%i.plg_JobThreadRouting.exitThread:%i", pJobHandle->threadType, pJobHandle->exitThread);
			break;
		} else if (pJobHandle->exitThread == 2) {

			elog(log_details, "ThreadType:%i.plg_JobThreadRouting.exitThread:%i", pJobHandle->threadType, pJobHandle->exitThread);
			void* pManage = pJobHandle->privateData;
			pthread_detach(pthread_self());
			plg_MngSendExit(pManage);
			plg_MutexThreadDestroy();
			return 0;
		} else if (pJobHandle->exitThread == 3) {

			elog(log_details, "ThreadType:%i.plg_JobThreadRouting.exitThread:%i", pJobHandle->threadType, pJobHandle->exitThread);
			plg_JobSendOrder(job_ManageEqueue(), "destroycount", pJobHandle->m_value, plg_sdsLen(pJobHandle->m_value));
			plg_sdsFree(pJobHandle->m_value);
			break;
		}
	} while (1);

	plg_MutexThreadDestroy();
	pthread_detach(pthread_self());

	return 0;
}

int plg_JobStartRouting(void* pvJobHandle) {
	pthread_t pid;
	int r = pthread_create(&pid, NULL, plg_JobThreadRouting, pvJobHandle);

	elog(log_fun, "plg_JobStartRouting %U", pvJobHandle);
	return r;
}

/*
Internal message pipeline usage of manage and file
*/
void plg_JobSendOrder(void* eQueue, char* order, char* value, short valueLen) {

	POrderPacket POrderPacket = malloc(sizeof(OrderPacket));
	POrderPacket->order = plg_sdsNew(order);
	POrderPacket->value = plg_sdsNewLen(value, valueLen);

	plg_eqPush(eQueue, POrderPacket);
}

void plg_JobAddAdmOrderProcess(void* pvJobHandle, char* nameOrder, void* pvProcess) {

	PEventPorcess process = pvProcess;
	PJobHandle pJobHandle = pvJobHandle;
	sds sdsOrder= plg_sdsNew(nameOrder);
	dictEntry * entry = plg_dictFind(pJobHandle->order_process, sdsOrder);
	if (entry == 0) {
		plg_listAddNodeHead(pJobHandle->userEvent, sdsOrder);
		plg_listAddNodeHead(pJobHandle->userProcess, process);
		plg_dictAdd(pJobHandle->order_process, sdsOrder, process);
	} else {
		plg_sdsFree(sdsOrder);
		free(process);
	}
}

/*
Check the current running thread type to prevent users
Improper use of API in improper thread environment
*/
char plg_JobCheckIsType(enum ThreadType threadType) {

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	if (pJobHandle == 0) {
		return 0;
	} else if (threadType == pJobHandle->threadType) {
		return 1;
	} else {
		return 0;
	}
}

char plg_JobCheckUsingThread() {

	if (1 == plg_JobCheckIsType(TT_OTHER)) {
		return 1;
	}
	return 0;
}

void plg_JobPrintStatus(void* pvJobHandle, void* vJson) {

	pJSON* root = vJson;
	PJobHandle pJobHandle = pvJobHandle;
	pJson_AddNumberToObject(root, "order_equeue", dictSize(pJobHandle->order_equeue));
	pJson_AddNumberToObject(root, "dictCache", dictSize(pJobHandle->dictCache));
	pJson_AddNumberToObject(root, "order_process", dictSize(pJobHandle->order_process));
	pJson_AddNumberToObject(root, "tableName_cacheHandle", dictSize(pJobHandle->tableName_cacheHandle));
	pJson_AddNumberToObject(root, "allWeight", pJobHandle->allWeight);
	pJson_AddNumberToObject(root, "userEvent", listLength(pJobHandle->userEvent));
	pJson_AddNumberToObject(root, "userProcess", listLength(pJobHandle->userProcess));
}

void plg_JobPrintDetails(void* pvJobHandle, void* vJson) {
	
	pJSON* root = vJson;
	PJobHandle pJobHandle = pvJobHandle;
	pJSON* job = pJson_CreateObject();
	sds eq = plg_sdsCatPrintf(plg_sdsEmpty(), "%p", pJobHandle->eQueue);
	pJson_AddItemToObject(root, eq, job);
	plg_sdsFree(eq);

	pJSON* tableName_cacheHandle = pJson_CreateObject();
	pJson_AddItemToObject(job, "tableName_cacheHandle", tableName_cacheHandle);

	dictIterator* dictIter = plg_dictGetSafeIterator(pJobHandle->tableName_cacheHandle);
	dictEntry* dictNode;
	while ((dictNode = plg_dictNext(dictIter)) != NULL) {
		sds eq = plg_sdsCatPrintf(plg_sdsEmpty(), "%p", dictGetVal(dictNode));
		pJson_AddStringToObject(tableName_cacheHandle, (char*)dictGetKey(dictNode), eq);
		plg_sdsFree(eq);
	}
	plg_dictReleaseIterator(dictIter);

	pJSON* dictCache = pJson_CreateObject();
	pJson_AddItemToObject(job, "dictCache", dictCache);
	dictIter = plg_dictGetSafeIterator(pJobHandle->dictCache);
	while ((dictNode = plg_dictNext(dictIter)) != NULL) {
		sds eq = plg_sdsCatPrintf(plg_sdsEmpty(), "%p", dictGetVal(dictNode));
		pJson_AddStringToObject(dictCache, (char*)dictGetKey(dictNode), eq);
		plg_sdsFree(eq);
	}
	plg_dictReleaseIterator(dictIter);

	pJSON* order_equeue = pJson_CreateObject();
	pJson_AddItemToObject(job, "order_equeue", order_equeue);
	dictIter = plg_dictGetSafeIterator(pJobHandle->order_equeue);
	while ((dictNode = plg_dictNext(dictIter)) != NULL) {
		sds eq = plg_sdsCatPrintf(plg_sdsEmpty(), "%p", dictGetVal(dictNode));
		pJson_AddStringToObject(order_equeue, (char*)dictGetKey(dictNode), eq);
		plg_sdsFree(eq);
	}
	plg_dictReleaseIterator(dictIter);

}

void plg_JobPrintOrder(void* pvJobHandle, void* vJson) {

	pJSON* root = vJson;
	PJobHandle pJobHandle = pvJobHandle;

	pJSON* job = pJson_CreateArray();
	sds eq = plg_sdsCatPrintf(plg_sdsEmpty(), "%p", pJobHandle->eQueue);
	pJson_AddItemToObject(root, eq, job);
	plg_sdsFree(eq);

	dictIterator* dictIter = plg_dictGetSafeIterator(pJobHandle->order_equeue);
	dictEntry* dictNode;
	while ((dictNode = plg_dictNext(dictIter)) != NULL) {
		if (dictGetVal(dictNode) == pJobHandle->eQueue) {
			pJson_AddItemToArray(job, pJson_CreateString((char*)dictGetKey(dictNode)));
		}
	}
	plg_dictReleaseIterator(dictIter);
}

unsigned short plg_JobGetTableType(void* table, short tableLen) {

	CheckUsingThread(0);
	unsigned short r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		r = plg_CacheGetTableType(dictGetVal(valueEntry), sdsTable, job_IsTableAllowWrite(pJobHandle, sdsTable));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobGetTableType. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}


unsigned short plg_JobSetTableType(void* table, short tableLen, unsigned short tableType) {

	CheckUsingThread(0);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheSetTableType(dictGetVal(valueEntry), sdsTable, tableType);
			if (r == tableType) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSetTableType.No permission in <%s> to table <%s>!", order, sdsTable);
		}

	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSetTableType. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

unsigned short plg_JobSetTableTypeIfByte(void* table, short tableLen, unsigned short tableType) {

	CheckUsingThread(0);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheSetTableTypeIfByte(dictGetVal(valueEntry), sdsTable, tableType);
			if (r == tableType) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSetTableTypeIfByte.No permission in <%s> to table <%s>!", order, sdsTable);
		}

	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSetTableTypeIfByte. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

/*
First check the running cache
*/
unsigned int plg_JobSet(void* table, short tableLen, void* key, short keyLen, void* value, unsigned int valueLen) {
	CheckUsingThread(0);
	elog(log_fun, "plg_JobSet %s %s", table, key);
	
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheTableAdd(dictGetVal(valueEntry), sdsTable, key, keyLen, value, valueLen);
			if (r) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSet.No permission in <%s> to table <%s>!", order, sdsTable);
		}

	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSet. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

/*
Get set type will fail
*/
void* plg_JobGet(void* table, short tableLen, void* key, short keyLen, unsigned int* valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobGet %s %s", table, key);

	void* ptr = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		void* pDictExten = plg_DictExtenCreate();
		if (0 <= plg_CacheTableFind(dictGetVal(valueEntry), sdsTable, key, keyLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)))) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				void* valuePtr = plg_DictExtenValue(entry, valueLen);
				if (*valueLen) {
					ptr = malloc(*valueLen);
					memcpy(ptr, valuePtr, *valueLen);
				}
			}
			
		} else {
			elog(log_error, "plg_JobGet.Serious error in search operation!");
		}
		plg_DictExtenDestroy(pDictExten);
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobGet. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return ptr;
}

unsigned int plg_JobDel(void* table, short tableLen, void* key, short keyLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobDel %s %s", table, key);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheTableDel(dictGetVal(valueEntry), sdsTable, key, keyLen);
			if (r) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobDel.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobDel. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

unsigned int plg_JobLength(void* table, short tableLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobLength %s", table);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	unsigned int len = 0;
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		len = plg_CacheTableLength(dictGetVal(valueEntry), sdsTable, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobLength. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return len;
}

unsigned int plg_JobSetIfNoExit(void* table, short tableLen, void* key, short keyLen, void* value, unsigned int valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSetIfNoExit %s %s", table, key);

	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheTableAddIfNoExist(dictGetVal(valueEntry), sdsTable, key, keyLen, value, valueLen);
			if (r) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSetIfNoExit.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSetIfNoExit. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

unsigned int plg_JobIsKeyExist(void* table, short tableLen, void* key, short keyLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobIsKeyExist %s %s", table, key);

	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		r = plg_CacheTableIsKeyExist(dictGetVal(valueEntry), sdsTable, key, keyLen, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobIsKeyExist. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

unsigned int plg_JobRename(void* table, short tableLen, void* key, short keyLen, void* newKey, short newKeyLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobRename %s %s %s", table, key, newKey);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheTableRename(dictGetVal(valueEntry), sdsTable, key, keyLen, newKey, newKeyLen);
			if (r) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobRename.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobRename. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
	return r;
}

void plg_JobLimite(void* table, short tableLen, void* key, short keyLen, unsigned int left, unsigned int right, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobLimite %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableLimite(dictGetVal(valueEntry), sdsTable, key, keyLen, left, right, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobLimite. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobOrder(void* table, short tableLen, short order, unsigned int limite, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobLimite %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableOrder(dictGetVal(valueEntry), sdsTable, order, limite, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobOrder. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobRang(void* table, short tableLen, void* beginKey, short beginKeyLen, void* endKey, short endKeyLen, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobRang %s %s %s", table, beginKey, endKey);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableRang(dictGetVal(valueEntry), sdsTable, beginKey, beginKeyLen, endKey, endKeyLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobRang. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

}

void plg_JobPoint(void* table, short tableLen, void* beginKey, short beginKeyLen, unsigned int direction, unsigned int offset, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobPoint %s %s", table, beginKey);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTablePoint(dictGetVal(valueEntry), sdsTable, beginKey, beginKeyLen, direction, offset, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobPoint. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

}

void plg_JobPattern(void* table, short tableLen, void* beginKey, short beginKeyLen, void* endKey, short endKeyLen, void* pattern, short patternLen, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobPattern %s %s %s", table, beginKey, endKey);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTablePattern(dictGetVal(valueEntry), sdsTable, beginKey, beginKeyLen, endKey, endKeyLen, pattern, patternLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobPattern. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobMembers(void* table, short tableLen, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobMembers %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();

	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableMembers(dictGetVal(valueEntry), sdsTable, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobMembers. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

/*
First check the running cache
*/
unsigned int plg_JobMultiSet(void* table, short tableLen, void* pDictExten) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobMultiSet %s", table);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	unsigned int r = 0;
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheTableMultiAdd(dictGetVal(valueEntry), sdsTable, pDictExten);
			if (r) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobMultiSet.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobMultiSet. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
	return r;
}

void plg_JobMultiGet(void* table, short tableLen, void* pKeyDictExten, void* pValueDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobMultiGet %s", table);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableMultiFind(dictGetVal(valueEntry), sdsTable, pKeyDictExten, pValueDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobMultiGet. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void* plg_JobRand(void* table, short tableLen, unsigned int* valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobRand %s", table);

	void* ptr = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {

		void* pDictExten = plg_DictExtenCreate();
		if (1 <= plg_CacheTableRand(dictGetVal(valueEntry), sdsTable, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)))) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				void* valuePtr = plg_DictExtenValue(entry, valueLen);
				if (*valueLen) {
					ptr = malloc(*valueLen);
					memcpy(ptr, valuePtr, *valueLen);
				}
			}

		} else {
			elog(log_error, "plg_JobRand.Serious error in search operation!");
		}
		plg_DictExtenDestroy(pDictExten);
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobRand. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return ptr;
}

void plg_JobTableClear(void* table, short tableLen) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobTableClear %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			plg_CacheTableClear(dictGetVal(valueEntry), sdsTable);
			plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobTableClear.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobTableClear. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

unsigned int plg_JobSAdd(void* table, short tableLen, void* key, short keyLen, void* value, short valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSAdd %s %s", table, key);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);	
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			r = plg_CacheTableSetAdd(dictGetVal(valueEntry), sdsTable, key, keyLen, value, valueLen);
			if (r) {
				plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
			}
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSAdd.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSAdd. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

void plg_JobSRang(void* table, short tableLen, void* key, short keyLen, void* beginValue, short beginValueLen, void* endValue, short endValueLen, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSRang %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetRang(dictGetVal(valueEntry), sdsTable, key, keyLen, beginValue, beginValueLen, endValue, endValueLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSRang. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSPoint(void* table, short tableLen, void* key, short keyLen, void* beginValue, short beginValueLen, unsigned int direction, unsigned int offset, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSPoint %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetPoint(dictGetVal(valueEntry), sdsTable, key, keyLen, beginValue, beginValueLen, direction, offset, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSPoint. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSLimite(void* table, short tableLen, void* key, short keyLen, void* value, short valueLen, unsigned int left, unsigned int right, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSLimite %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetLimite(dictGetVal(valueEntry), sdsTable, key, keyLen, value, valueLen, left, right, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSLimite. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

unsigned int plg_JobSLength(void* table, short tableLen, void* key, short keyLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSLength %s %s", table, key);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	unsigned int len = 0;
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		len = plg_CacheTableSetLength(dictGetVal(valueEntry), sdsTable, key, keyLen, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSLength. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return len;
}

unsigned int plg_JobSIsKeyExist(void* table, short tableLen, void* key, short keyLen, void* value, short valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSIsKeyExist %s %s", table, key);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		r = plg_CacheTableSetIsKeyExist(dictGetVal(valueEntry), sdsTable, key, keyLen, value, valueLen, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSIsKeyExist. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

void plg_JobSMembers(void* table, short tableLen, void* key, short keyLen, void* pDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSMembers %s %s", table, key);

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetMembers(dictGetVal(valueEntry), sdsTable, key, keyLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSMembers. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void* plg_JobSRand(void* table, short tableLen, void* key, short keyLen, unsigned int* valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSRand %s %s", table, key);
	void* ptr = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {

		void* pDictExten = plg_DictExtenCreate();
		if (1 <= plg_CacheTableSetRand(dictGetVal(valueEntry), sdsTable, key, keyLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)))) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				void* keyPtr = plg_DictExtenKey(entry, valueLen);
				if (*valueLen) {
					ptr = malloc(*valueLen);
					memcpy(ptr, keyPtr, *valueLen);
				}
			}

		} else {
			elog(log_error, "plg_JobSRand.Serious error in search operation!");
		}
		plg_DictExtenDestroy(pDictExten);
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSRand. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return ptr;
}

void plg_JobSDel(void* table, short tableLen, void* key, short keyLen, void* pValueDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSDel %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			plg_CacheTableSetDel(dictGetVal(valueEntry), sdsTable, key, keyLen, pValueDictExten);
			plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSDel.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSDel. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void* plg_JobSPop(void* table, short tableLen, void* key, short keyLen, unsigned int* valueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSPop %s %s", table, key);
	void* ptr = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {

		void* pDictExten = plg_DictExtenCreate();
		if (1 <= plg_CacheTableSetPop(dictGetVal(valueEntry), sdsTable, key, keyLen, pDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)))) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				void* valuePtr = plg_DictExtenKey(entry, valueLen);
				if (*valueLen) {
					ptr = malloc(*valueLen);
					memcpy(ptr, valuePtr, *valueLen);
				}
			}

		} else {
			elog(log_error, "plg_JobSPop.Serious error in search operation!");
		}
		plg_DictExtenDestroy(pDictExten);
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSPop. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return ptr;

}

unsigned int plg_JobSRangCount(void* table, short tableLen, void* key, short keyLen, void* beginValue, short beginValueLen, void* endValue, short endValueLen) {

	CheckUsingThread(0);
	elog(log_fun, "plg_JobSRangCount %s %s", table, key);
	unsigned int r = 0;
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		r = plg_CacheTableSetRangCount(dictGetVal(valueEntry), sdsTable, key, keyLen, beginValue, beginValueLen, endValue, endValueLen, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSRangCount. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);

	return r;
}

void plg_JobSUion(void* table, short tableLen, void* pSetDictExten, void* pKeyDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSUion %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetUion(dictGetVal(valueEntry), sdsTable, pSetDictExten, pKeyDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSUion. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSUionStore(void* table, short tableLen, void* pSetDictExten, void* key, short keyLen) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSUionStore %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			plg_CacheTableSetUionStore(dictGetVal(valueEntry), sdsTable, pSetDictExten, key, keyLen);
			plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSUionStore.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSUionStore. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSInter(void* table, short tableLen, void* pSetDictExten, void* pKeyDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSInter %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetInter(dictGetVal(valueEntry), sdsTable, pSetDictExten, pKeyDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSInter. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSInterStore(void* table, short tableLen, void* pSetDictExten, void* key, short keyLen) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSInterStore %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			plg_CacheTableSetInterStore(dictGetVal(valueEntry), sdsTable, pSetDictExten, key, keyLen);
			plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSInterStore.No permission in <%s> to table <%s>!", order, sdsTable);
		}	
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSInterStore. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSDiff(void* table, short tableLen, void* pSetDictExten, void* pKeyDictExten) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSDiff %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableSetDiff(dictGetVal(valueEntry), sdsTable, pSetDictExten, pKeyDictExten, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSDiff. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSDiffStore(void* table, short tableLen, void* pSetDictExten, void* key, short keyLen) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSDiff %s %s", table, key);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			plg_CacheTableSetDiffStore(dictGetVal(valueEntry), sdsTable, pSetDictExten, key, keyLen);
			plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSDiffStore.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSDiffStore. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

void plg_JobSMove(void* table, short tableLen, void* srcKey, short srcKeyLen, void* desKey, short desKeyLen, void* value, short valueLen) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobSMove %s %s %s", table, srcKey, desKey);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		if (job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)) && job_IsTableAllowWrite(pJobHandle, sdsTable)) {
			plg_CacheTableSetMove(dictGetVal(valueEntry), sdsTable, srcKey, srcKeyLen, desKey, desKeyLen, value, valueLen);
			plg_listAddNodeHead(pJobHandle->tranCache, dictGetVal(valueEntry));
		} else {
			short orderLen;
			char* order = plg_JobCurrentOrder(&orderLen);
			elog(log_error, "plg_JobSMove.No permission in <%s> to table <%s>!", order, sdsTable);
		}
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSMove. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}


void plg_JobTableMembersWithJson(void* table, short tableLen, void* jsonRoot) {

	CheckUsingThread(NORET);
	elog(log_fun, "plg_JobTableMembersWithJson %s", table);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);

	dictEntry* valueEntry = plg_dictFind(pJobHandle->tableName_cacheHandle, sdsTable);
	if (valueEntry != 0) {
		plg_CacheTableMembersWithJson(dictGetVal(valueEntry), sdsTable, jsonRoot, job_IsCacheAllowWrite(pJobHandle, dictGetKey(valueEntry)));
	} else {
		short orderLen;
		char* order = plg_JobCurrentOrder(&orderLen);
		elog(log_error, "in order <%s>.plg_JobSDiff. Cannot access table <%s>!", order, sdsTable);
	}
	plg_sdsFree(sdsTable);
}

char* plg_JobCurrentOrder(short* orderLen) {
	CheckUsingThread(0);
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return 0;
	}

	*orderLen = plg_sdsLen(pJobHandle->pOrderName);
	return pJobHandle->pOrderName;
}

void plg_JobAddTimerWithOrderID(double timer, void* order, short orderLen, void* value, short valueLen, unsigned int orderID) {
	CheckUsingThread(NORET);

	unsigned long long milli = plg_GetCurrentMilli();
	PJobHandle pJobHandle = plg_LocksGetSpecific();
	
	if (!pJobHandle) {
		elog(log_error, "plg_LocksGetSpecific:pJobHandle ");
		return;
	}

	PIntervalometer pPIntervalometer = malloc(sizeof(Intervalometer));
	pPIntervalometer->Order = plg_sdsNewLen(order, orderLen);
	pPIntervalometer->Value = plg_sdsNewLen(value, valueLen);
	pPIntervalometer->orderID = orderID;
	pPIntervalometer->tim = milli + timer * 1000;
	plg_listAddNodeHead(pJobHandle->pListIntervalometer, pPIntervalometer);
	
	if (pJobHandle->pMinPIntervalometer && pJobHandle->pMinPIntervalometer->tim < pPIntervalometer->tim) {
		pJobHandle->pMinPIntervalometer = pPIntervalometer;
	} else {
		pJobHandle->pMinPIntervalometer = pPIntervalometer;
	}
}

void plg_JobAddTimer(double timer, void* order, short orderLen, void* value, short valueLen) {
	plg_JobAddTimerWithOrderID(timer, order, orderLen, value, valueLen, 0);
}
char* plg_JobTableNameWithJson() {
	CheckUsingThread(0);

	void* pManage = plg_JobGetPrivate();
	if (!pManage) {
		return 0;
	}

	short orderLen = 0;
	void* orderName = plg_JobCurrentOrder(&orderLen);
	if (!orderLen) {
		return 0;
	}

	return plg_MngOrderAllTableWithJson(pManage, orderName, orderLen);
}

char** plg_JobTableName(short* tableLen) {
	CheckUsingThread(0);

	void* pManage = plg_JobGetPrivate();
	if (!pManage) {
		return 0;
	}

	short orderLen = 0;
	void* orderName = plg_JobCurrentOrder(&orderLen);
	if (!orderLen) {
		return 0;
	}

	return plg_MngOrderAllTable(pManage, orderName, orderLen, tableLen);
}

void plg_MemoryFree(void* ptr) {
	free(ptr);
}

unsigned int plg_JobCreateOrderID(void* ptr) {

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	if (pJobHandle->jobID > MAXJOBID) {
		elog(log_error, "jobid exceeds the limit of MAXJOBID");
	}

	if (pJobHandle->orderID > MAXORDERID) {
		elog(log_error, "orderID exceeds the limit of MAXORDERID");
	}

	if (pJobHandle->orderID + 1 > MAXORDERID) {

		if (dictSize(pJobHandle->orderID_ptr) != 0) {
			elog(log_error, "The reset of OrderID may cause the existing service data to be overwritten, The number of existing services is %i", dictSize(pJobHandle->orderID_ptr));
		}
		pJobHandle->orderID = 0;
	}

	unsigned int orderID = pJobHandle->jobID;
	orderID = orderID << 22;
	orderID = orderID | ++pJobHandle->orderID;

	pJobHandle->curretnOrderID = orderID;

	unsigned int * key = malloc(sizeof(unsigned int));
	*key = orderID;
	plg_dictAdd(pJobHandle->orderID_ptr, key, ptr);

	return orderID;
}

void plg_jobRemoveOrderID() {
	PJobHandle pJobHandle = plg_LocksGetSpecific();

	unsigned int orderID = plg_JobGetOrderID();
	if (orderID == 0) {
		elog(log_error, "current order id is empty");
		return;
	}
	dictEntry* entry = plg_dictFind(pJobHandle->orderID_ptr, &orderID);
	if (entry) {
		plg_dictDelete(pJobHandle->orderID_ptr, &orderID);
	} else {
		elog(log_error, "OrderID that doesn't exist in plg_jobRemoveOrderID");
	}
}

void* plg_JobGetOrderIDPtr() {

	PJobHandle pJobHandle = plg_LocksGetSpecific();

	unsigned int orderID = plg_JobGetOrderID();
	if (orderID == 0) {
		elog(log_error, "current order id is empty");
		return 0;
	}
	dictEntry* entry = plg_dictFind(pJobHandle->orderID_ptr, &orderID);
	if (entry) {
		return dictGetVal(entry);
	} else {
		elog(log_error, "OrderID that doesn't exist in plg_JobGetOrderIDPtr");
		return 0;
	}
}

void plg_JobSetOrderIDPtr(void* ptr) {

	PJobHandle pJobHandle = plg_LocksGetSpecific();

	unsigned int orderID = plg_JobGetOrderID();
	if (orderID == 0) {
		elog(log_error, "current order id is empty");
		return;
	}
	dictEntry* entry = plg_dictFind(pJobHandle->orderID_ptr, &orderID);
	if (entry) {
		dictSetVal(pJobHandle->orderID_ptr, entry, ptr);
	} else {
		elog(log_error, "OrderID that doesn't exist in plg_JobSetOrderIDPtr");
	}
}

unsigned int plg_JobGetOrderID() {

	PJobHandle pJobHandle = plg_LocksGetSpecific();
	if (pJobHandle->jobID > MAXJOBID) {
		elog(log_error, "jobid exceeds the limit of MAXJOBID");
	}

	if (pJobHandle->curretnOrderID > MAXORDERID) {
		elog(log_error, "orderID exceeds the limit of MAXORDERID");
	}

	if (pJobHandle->curretnOrderID == 0) {
		return 0;
	} else {
		unsigned int orderID = pJobHandle->jobID;
		orderID = orderID << 22;
		orderID = orderID | pJobHandle->curretnOrderID;

		return orderID;
	}
}

unsigned int JobJobID(unsigned int orderID) {

	unsigned int jobID = orderID >> 22;
	unsigned int jobOrderID = orderID << 10;
	jobOrderID = jobOrderID >> 10;
	if (jobID > MAXJOBID) {
		elog(log_error, "jobid exceeds the limit of MAXJOBID");
	}

	if (jobOrderID > MAXORDERID) {
		elog(log_error, "orderID exceeds the limit of MAXORDERID");
	}

	return jobID;
}

unsigned int JobJobOrderID(unsigned int orderID) {

	unsigned int jobID = orderID >> 22;
	unsigned int jobOrderID = orderID << 10;
	jobOrderID = jobOrderID >> 10;
	if (jobID > MAXJOBID) {
		elog(log_error, "jobid exceeds the limit of MAXJOBID");
	}

	if (jobOrderID > MAXORDERID) {
		elog(log_error, "serivceID exceeds the limit of MAXORDERID");
	}

	return jobOrderID;
}

unsigned int JobGetOrderIDFromJobID(unsigned int jobID) {

	if (jobID > MAXJOBID) {
		elog(log_error, "jobid exceeds the limit of MAXJOBID");
	}
	
	unsigned int orderID = jobID << 22;
	return orderID;
}

#undef NORET
#undef CheckUsingThread