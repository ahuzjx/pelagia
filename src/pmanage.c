/* manage.c - Global manager
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
#include "pequeue.h"
#include "psds.h"
#include "pdict.h"
#include "padlist.h"
#include "pdictset.h"
#include "pelog.h"
#include "pjob.h"
#include "pfile.h"
#include "pdisk.h"
#include "pinterface.h"
#include "pmanage.h"
#include "plocks.h"
#include "pfilesys.h"
#include "ptimesys.h"
#include "pelagia.h"
#include "pjson.h"
#include "pjob.h"
#include "pbase64.h"
#include "pstart.h"
#include "plibsys.h"

#define NORET
#define CheckUsingThread(r) if (plg_MngCheckUsingThread()) {elog(log_error, "Cannot run management interface in non user environment");return r;}

/*
The structure of global filer is protected by readlock lock,
Only the thread obtaining the lock can read and write data in this structure
Global management is equivalent to a router
All objects with locks are registered here
Direct transfer of pointers between threads is prohibited
Because the lock itself is a special memory
Once assigned, it cannot be destroyed
It can only be destroyed when the system exits
The locks to be recorded include
1. Cache data of each thread
2. Data lock of each file
3. Handle of each message queue
The manned process of tablename
1. Read the JSON to get the corresponding relationship between the job and tablename and generate the tablename "job by classification
2. Classify jobs according to table name
----------------------------------
Readlock: multithreaded read lock
Pjobhandle: one way unequal, message processing sent to the management queue does not pass the order queue, and management needs a management thread to process management instructions,
Status: run 1, no run 0
Dictdisk: handle to all disks, one disk for each file
Listjob: handle to all jobs
Listorder: internal event
Listuserevent: external event
Order_Process: main table, all events correspond to the handle of processing method plg_Mngaddorder creation
Order_queue: all message correspondence tables, externally sent to internal functions for use
Event ABCD dicttablename: main table, all events and the list of their corresponding tablenames PLG ABCD mngaddtable creation
Tablename "diskhandle: handle of hard disk corresponding to all tablenames
*/
typedef struct _Manage
{
	void* mutexHandle;
	void* pJobHandle;
	int runStatus;

	//Data to be destroyed by management
	list* listDisk;
	list* listJob;
	list* listOrder;
	list* listProcess;
	dict* dictTableName;

	dict* order_process;
	dict* order_equeue;
	PDictSet order_tableName;
	dict* tableName_diskHandle;
	sds	dbPath;
	sds objName;
	
	unsigned short fileCount;
	unsigned int jobDestroyCount;
	unsigned int fileDestroyCount;
	unsigned int maxTableWeight;

	//lvm
	sds luaLIBPath;
	short luaHot;
	dict* libFun;

	//event for exit;
	void* pEvent;

	//noSave
	short noSave;

	//stat
	short isOpenStat;
	unsigned long long checkTime;

	//
	unsigned int maxQueue;
} *PManage, Manage;

static void listSdsFree(void *ptr) {
	plg_sdsFree(ptr);
}

static void listFileFree(void *ptr) {
	plg_DiskFileCloseHandle(ptr);
}

static void listJobFree(void *ptr) {
	plg_JobDestoryHandle(ptr);
}

static void listProcessFree(void *ptr) {
	plg_JobProcessDestory(ptr);
}

static int sdsCompareCallback(void *privdata, const void *key1, const void *key2) {
	int l1, l2;
	DICT_NOTUSED(privdata);

	l1 = plg_sdsLen((sds)key1);
	l2 = plg_sdsLen((sds)key2);
	if (l1 != l2) return 0;
	return memcmp(key1, key2, l1) == 0;
}

static void sdsFreeCallback(void *privdata, void *val) {
	DICT_NOTUSED(privdata);
	plg_sdsFree(val);
}

static void dictTableNameFree(void *privdata, void *val) {
	NOTUSED(privdata);
	PTableName pTableName = val;
	plg_sdsFree(pTableName->sdsParent);
	free(val);
}

static unsigned long long sdsHashCallback(const void *key) {
	return plg_dictGenHashFunction((unsigned char*)key, plg_sdsLen((char*)key));
}

static dictType SdsDictType = {
	sdsHashCallback,
	NULL,
	NULL,
	sdsCompareCallback,
	sdsFreeCallback,
	dictTableNameFree
};

static void dictLibFree(void *privdata, void *val) {
	NOTUSED(privdata);
	plg_SysLibUnload(val);
	free(val);
}

static dictType LibDictType = {
	sdsHashCallback,
	NULL,
	NULL,
	sdsCompareCallback,
	sdsFreeCallback,
	dictLibFree
};

typedef struct _ManageDestroy
{
	PManage pManage;
	AfterDestroyFun fun;
	void* ptr;
	char type;
}*PManageDestroy, ManageDestroy;

char* plg_MngGetDBPath(void* pvManage) {
	PManage pManage = pvManage;
	return pManage->dbPath;
}

/*
load file from p1,p2,p3,p4... to init listDisk
*/
static void manage_InitLoadFile(void* pvManage) {

	PManage pManage = pvManage;
	pManage->fileCount = 0;
	unsigned short loop = 0;
	do {

		sds fullPath = plg_sdsCatFmt(plg_sdsEmpty(), "%sp%i", pManage->dbPath, loop++);
		void* pDiskHandle;

		if (access_t(fullPath, 0) != 0){
			plg_sdsFree(fullPath);
			break;
		}

		if (1 == plg_DiskFileOpen(plg_JobEqueueHandle(pManage->pJobHandle), fullPath, &pDiskHandle, 0, pManage->noSave)) {
			plg_listAddNodeHead(pManage->listDisk, pDiskHandle);
		} else {
			plg_sdsFree(fullPath);
			break;
		}	
	} while (1);
}

static void manage_DestroyDisk(void* pvManage) {

	PManage pManage = pvManage;
	plg_dictEmpty(pManage->tableName_diskHandle, NULL);
	plg_listEmpty(pManage->listDisk);
}

static void manage_AddTableToDisk(void* pvManage, PTableName pTableName, sds tableName) {

	PManage pManage = pvManage;
	unsigned int count = UINT_MAX;
	void* countLost = 0;

	unsigned int noSaveCount = UINT_MAX;
	void* noSaveCountLost = 0;

	listIter* iter = plg_listGetIterator(pManage->listDisk, AL_START_HEAD);
	listNode* node;
	while ((node = plg_listNext(iter)) != NULL) {

		//Already exists in the file
		if (plg_DiskTableFind(listNodeValue(node), tableName, NULL)) {
			plg_dictAdd(pManage->tableName_diskHandle, tableName, listNodeValue(node));
			plg_listReleaseIterator(iter);
			return;
		}

		//have parent
		if (pTableName->sdsParent && plg_DiskTableFind(listNodeValue(node), pTableName->sdsParent, 0)) {
			plg_DiskAddTableWeight(listNodeValue(node), 1);
			plg_dictAdd(pManage->tableName_diskHandle, tableName, listNodeValue(node));
			plg_listReleaseIterator(iter);
			return;
		}

		//no save file
		if (plg_DiskIsNoSave(listNodeValue(node)) && plg_DiskGetTableAllWeight(listNodeValue(node)) < noSaveCount) {
			noSaveCountLost = listNodeValue(node);
			noSaveCount = pTableName->weight;
		}

		//Count the number of files that have been added find the least added files
		if (plg_DiskGetTableAllWeight(listNodeValue(node)) < count) {
			countLost = listNodeValue(node);
			count = pTableName->weight;
		}
	}
	plg_listReleaseIterator(iter);

	if (pTableName->noSave) {
		if (noSaveCount > pManage->maxTableWeight) {
			sds fullPath = plg_sdsCatFmt(plg_sdsEmpty(), "%spnosave", pManage->dbPath);
			void* pDiskHandle;
			if (1 == plg_DiskFileOpen(plg_JobEqueueHandle(pManage->pJobHandle), fullPath, &pDiskHandle, 1, pTableName->noSave)) {
				plg_listAddNodeHead(pManage->listDisk, pDiskHandle);
				plg_DiskAddTableWeight(pDiskHandle, pTableName->weight);
				plg_dictAdd(pManage->tableName_diskHandle, tableName, pDiskHandle);
			}
		} else {
			plg_DiskAddTableWeight(noSaveCountLost, pTableName->weight);
			plg_dictAdd(pManage->tableName_diskHandle, tableName, noSaveCountLost);
		}
	} else {
		if (count > pManage->maxTableWeight) {

			plg_MkDirs(pManage->dbPath);
			sds fullPath = plg_sdsCatFmt(plg_sdsEmpty(), "%sp%i", pManage->dbPath, listLength(pManage->listDisk));
			void* pDiskHandle;
			if (1 == plg_DiskFileOpen(plg_JobEqueueHandle(pManage->pJobHandle), fullPath, &pDiskHandle, 1, pTableName->noSave)) {
				plg_listAddNodeHead(pManage->listDisk, pDiskHandle);
				plg_DiskAddTableWeight(pDiskHandle, pTableName->weight);
				plg_dictAdd(pManage->tableName_diskHandle, tableName, pDiskHandle);
			}
		} else {
			plg_DiskAddTableWeight(countLost, pTableName->weight);
			plg_dictAdd(pManage->tableName_diskHandle, tableName, countLost);
		}
	}

}

//Set the table in the file to usable state
static void pFillTableNameCB(void* pDiskHandle, void* ptr, char* tableName) {

	PManage pManage = (PManage)ptr;

	PTableName pTableName = malloc(sizeof(TableName));
	pTableName->sdsParent = 0;
	pTableName->weight = 1;
	pTableName->noShare = 0;
	pTableName->noSave = pManage->noSave;
	plg_dictAdd(pManage->dictTableName, tableName, pTableName);
	plg_dictAdd(pManage->tableName_diskHandle, tableName, pDiskHandle);
}

//Used to output the specified data file to JSON
static void manage_CreateDiskWithFileName(void* pvManage,char* FileName) {

	PManage pManage = pvManage;
	pManage->fileCount = 0;

	sds fullPath = plg_sdsCatFmt(plg_sdsEmpty(), "%s%s", pManage->dbPath, FileName);
	void* pDiskHandle;

	if (access_t(fullPath, 0) != 0){
		elog(log_error, "manage_CreateDiskWithFileName.plg_SysFileExits:%s no exit!", fullPath);
		plg_sdsFree(fullPath);
		return;
	}

	if (1 == plg_DiskFileOpen(plg_JobEqueueHandle(pManage->pJobHandle), fullPath, &pDiskHandle, 0, pManage->noSave)) {
		plg_listAddNodeHead(pManage->listDisk, pDiskHandle);
	} else {
		elog(log_error, "manage_CreateDiskWithFileName.plg_DiskFileOpen:%s", fullPath);
		plg_sdsFree(fullPath);
		return;
	}

	plg_DiskFillTableName(pDiskHandle, pManage, pFillTableNameCB);
}

static void manage_CreateDisk(void* pvManage) {

	PManage pManage = pvManage;
	manage_InitLoadFile(pManage);
	//dictTableName
	dictIterator* tableNameIter = plg_dictGetIterator(pManage->dictTableName);
	dictEntry* tableNameNode;
	while ((tableNameNode = plg_dictNext(tableNameIter)) != NULL) {

		PTableName pTableName = dictGetVal(tableNameNode);
		manage_AddTableToDisk(pManage, pTableName, dictGetKey(tableNameNode));
	}

	plg_dictReleaseIterator(tableNameIter);
}

int plg_MngFreeJob(void* pvManage) {

	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Releasing resources is not allowed while the system is running");
		return 0;
	}

	CheckUsingThread(0);

	//listjob
	plg_listEmpty(pManage->listJob);
	plg_dictEmpty(pManage->order_equeue, NULL);
	plg_DictSetEmpty(pManage->order_tableName);

	return 1;
}

static void manage_AddEqueueToJob(void* pvManage, sds order, void* equeue) {

	//Manage external function usage
	PManage pManage = pvManage;
	plg_dictAdd(pManage->order_equeue, order, equeue);

	//listjob
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {
		plg_JobAddEventEqueue(listNodeValue(jobNode), order, equeue);
	}
	plg_listReleaseIterator(jobIter);
}

/*
Add table to job
*/
static void manage_AddTableToJob(void* pvManage, void* pJobHandle, dict * table) {

	PManage pManage = pvManage;
	dictIterator* tableIter = plg_dictGetSafeIterator(table);
	dictEntry* tableNode;
	while ((tableNode = plg_dictNext(tableIter)) != NULL) {
		dictEntry* diskEntry = plg_dictFind(pManage->tableName_diskHandle, dictGetKey(tableNode));
		if (diskEntry == 0) {
			continue;
		}

		void* pCacheHandle = plg_JobNewTableCache(pJobHandle, dictGetKey(diskEntry), dictGetVal(diskEntry));
		dictEntry * tableEntry = plg_dictFind(pManage->dictTableName, dictGetKey(diskEntry));
		if (tableEntry == 0) {
			continue;
		}

		PTableName pTableName = dictGetVal(tableEntry);
		//only add to current job
		if (pTableName->noShare) {
			plg_JobAddTableCache(pJobHandle, dictGetKey(tableNode), pCacheHandle);
		} else {
			//listjob
			listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
			listNode* jobNode;
			while ((jobNode = plg_listNext(jobIter)) != NULL) {
				plg_JobAddTableCache(listNodeValue(jobNode), dictGetKey(tableNode), pCacheHandle);
			}
			plg_listReleaseIterator(jobIter);
		}
	}
	plg_dictReleaseIterator(tableIter);
}

/*
loop event_dictTableName
core: number core
*/
int plg_MngInterAllocJob(void* pvManage, unsigned int core, char* fileName) {

	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Reallocation of resources is not allowed while the system is running");
		return 0;
	}

	//plg_MngFreeJob(pManage);
	manage_DestroyDisk(pManage);

	if (fileName) {
		manage_CreateDiskWithFileName(pManage, fileName);
	} else {
		manage_CreateDisk(pManage);
	}
	
	CheckUsingThread(0);
	//Create n jobs
	for (unsigned int l = 0; l < core; l++) {
		void* pJobHandle = plg_JobCreateHandle(plg_JobEqueueHandle(pManage->pJobHandle), TT_PROCESS, pManage->luaLIBPath, pManage->luaHot, l + 1);
		plg_JobSetStat(pJobHandle, pManage->isOpenStat, pManage->checkTime);
		plg_JobSetPrivate(pJobHandle, pvManage);
		plg_JobSetMaxQueue(pJobHandle, pManage->maxQueue);
		plg_listAddNodeHead(pManage->listJob, pJobHandle);
	}

	//listOrder
	int breakDup = 0;
	listIter* eventIterDup = 0;
	listIter* eventIter = plg_listGetIterator(pManage->listOrder, AL_START_HEAD);
	listNode* eventNode;

	do {
		while ((eventNode = plg_listNext(eventIter)) != NULL) {

			//process
			dictEntry * EventProcessEntry = plg_dictFind(pManage->order_process, listNodeValue(eventNode));
			if (EventProcessEntry == 0) {
				continue;
			}

			//List job intersection classification
			char nextContinue = 0;
			listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
			listNode* jobNode;
			while ((jobNode = plg_listNext(jobIter)) != NULL) {

				//Judge whether there is intersection
				dict * table = plg_DictSetValue(pManage->order_tableName, listNodeValue(eventNode));
				if (!table) {
					continue;
				}
				dictIterator* tableIter = plg_dictGetSafeIterator(table);
				dictEntry* tableNode;
				while ((tableNode = plg_dictNext(tableIter)) != NULL) {

					if (plg_JobFindTableName(listNodeValue(jobNode), dictGetKey(tableNode))) {

						//table
						manage_AddTableToJob(pManage, listNodeValue(jobNode), table);

						//process
						plg_JobAddEventProcess(listNodeValue(jobNode), dictGetKey(EventProcessEntry), dictGetVal(EventProcessEntry));

						//equeue
						manage_AddEqueueToJob(pManage, listNodeValue(eventNode), plg_JobEqueueHandle(listNodeValue(jobNode)));
						nextContinue = 1;
						break;
					}
				}
				plg_dictReleaseIterator(tableIter);
				if (nextContinue) {
					break;
				}
			}
			plg_listReleaseIterator(jobIter);
			if (nextContinue) {
				continue;
			}

			//other
			if (!breakDup) {
				breakDup = 1;
				eventIterDup = plg_listIteratorDup(eventIter);
				do {			
					//table
					dict * table = plg_DictSetValue(pManage->order_tableName, listNodeValue(eventNode));
					if (!table) {
						continue;
					}

					void* minJob = 0;
					unsigned int Weight = UINT_MAX;
					listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
					listNode* jobNode;
					while ((jobNode = plg_listNext(jobIter)) != NULL) {
				
						//min
						if (plg_JobAllWeight(listNodeValue(jobNode)) < Weight) {
							Weight = plg_JobAllWeight(listNodeValue(jobNode));
							minJob = listNodeValue(jobNode);
						}
					}
					plg_listReleaseIterator(jobIter);

					//table
					manage_AddTableToJob(pManage, minJob, table);

					//process
					plg_JobAddEventProcess(minJob, dictGetKey(EventProcessEntry), dictGetVal(EventProcessEntry));

					//equeue
					manage_AddEqueueToJob(pManage, listNodeValue(eventNode), plg_JobEqueueHandle(minJob));
				} while (0);
			}
		}
		
		plg_listReleaseIterator(eventIter);
		if (breakDup) {
			breakDup = 0;	
			eventIter = eventIterDup;
			continue;
		} else {
			break;
		}
	} while (1);
	return 1;
}

void* plg_MngRandJobEqueue(void* pvManage) {
	PManage pManage = pvManage;

	void* jobEqueue = 0;
	if (listLength(pManage->listJob)) {
		int r = rand() % listLength(pManage->listJob);
		listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
		listNode* jobNode;
		while ((jobNode = plg_listNext(jobIter)) != NULL) {

			jobEqueue = plg_JobEqueueHandle(listNodeValue(jobNode));
			if (--r < 0) {
				break;
			}
		}
		plg_listReleaseIterator(jobIter);
	}

	return jobEqueue;
}

void* plg_MngJobEqueueWithCore(void* pvManage, unsigned int core) {
	PManage pManage = pvManage;

	void* jobEqueue = 0;
	if (listLength(pManage->listJob)) {

		listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
		listNode* jobNode;
		while ((jobNode = plg_listNext(jobIter)) != NULL) {

			jobEqueue = plg_JobEqueueHandleIsCore(listNodeValue(jobNode), core);
			if (jobEqueue != 0) {
				break;
			}
		}
		plg_listReleaseIterator(jobIter);
	}

	return jobEqueue;
}
 
void* plg_MngGetProcess(void* pvManage, char* sdsOrder, char** retSdsOrder) {
	PManage pManage = pvManage;

	dictEntry * EventProcessEntry = plg_dictFind(pManage->order_process, sdsOrder);
	if (EventProcessEntry != 0) {
		*retSdsOrder = dictGetKey(EventProcessEntry);
		return dictGetVal(EventProcessEntry);
	}

	return 0;
}

int plg_MngAllocJob(void* pvManage, unsigned int core) {
	PManage pManage = pvManage;
	return plg_MngInterAllocJob(pManage, core, 0);
}

/*
Add an event and then manage &amp; createjob to multiple threads
Single: 1 exclusive thread
*/
int plg_MngAddOrder(void* pvManage, char* nameOrder, short nameOrderLen, void* ptrProcess) {

	CheckUsingThread(0);
	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Changes are not allowed during system runing!");
		return 0;
	}

	sds sdsnameOrder = plg_sdsNewLen(nameOrder, nameOrderLen);
	dictEntry * entry = plg_dictFind(pManage->order_process, sdsnameOrder);
	if (entry == 0) {
		plg_listAddNodeHead(pManage->listOrder, sdsnameOrder);
		plg_listAddNodeHead(pManage->listProcess, ptrProcess);
		plg_dictAdd(pManage->order_process, sdsnameOrder, ptrProcess);
		return 1;
	} else {
		plg_sdsFree(sdsnameOrder);
		free(ptrProcess);
		return 0;
	}
}

/*
Add and assign table to file
Parent: the owning key. The child key and the parent key must be in the same file, which takes precedence over single
Single: 1 exclusive file
*/
int plg_MngAddTable(void* pvManage, char* nameOrder, short nameOrderLen, char* nameTable, short nameTableLen) {

	CheckUsingThread(0);
	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Changes are not allowed during system runing!");
		return 0;
	}

	sds sdsnameOrder = plg_sdsNewLen(nameOrder, nameOrderLen);
	dictEntry * entry = plg_dictFind(pManage->order_process, sdsnameOrder);
	if (entry != 0) {
		plg_sdsFree(sdsnameOrder);
		sdsnameOrder = dictGetKey(entry);
	} else {
		plg_sdsFree(sdsnameOrder);
		elog(log_error, "not find order!");
		return 0;
	}

	sds sdsTableName = plg_sdsNewLen(nameTable, nameTableLen);
	dictEntry * tableEntry = plg_dictFind(pManage->dictTableName, sdsTableName);
	if (tableEntry == 0) {
		PTableName pTableName = malloc(sizeof(TableName));
		pTableName->sdsParent = 0;
		pTableName->weight = 1;
		pTableName->noShare = 0;
		pTableName->noSave = pManage->noSave;
		plg_dictAdd(pManage->dictTableName, sdsTableName, pTableName);

		if (!plg_DictSetIn(pManage->order_tableName, sdsnameOrder, sdsTableName)) {
			//add to list wait for manage_CreateJob
			plg_DictSetAdd(pManage->order_tableName, sdsnameOrder, sdsTableName);
		}
	} else {
		if (!plg_DictSetIn(pManage->order_tableName, sdsnameOrder, dictGetKey(tableEntry))) {
			//add to list wait for manage_CreateJob
			plg_DictSetAdd(pManage->order_tableName, sdsnameOrder, dictGetKey(tableEntry));
		}
		plg_sdsFree(sdsTableName);
	}
	return 1;
}

int plg_MngSetTableParent(void* pvManage, char* nameTable, short nameTableLen, char* parent, short parentLen) {

	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Changes are not allowed during system runing!");
		return 0;
	}

	int ret = 0;
	sds sdsNameTable = plg_sdsNewLen(nameTable, nameTableLen);
	dictEntry* tableNameNode = plg_dictFind(pManage->dictTableName, sdsNameTable);
	if (tableNameNode != NULL) {
		PTableName pTableName = dictGetVal(tableNameNode);
		if (pTableName != 0) {
			plg_sdsFree(pTableName->sdsParent);
		}
		pTableName->sdsParent = plg_sdsNewLen(parent, parentLen);
		ret = 1;
	}
	plg_sdsFree(sdsNameTable);
	return ret;
}

int plg_MngSetWeight(void* pvManage, char* nameTable, short nameTableLen, unsigned int weight) {

	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Changes are not allowed during system runing!");
		return 0;
	}

	int ret = 0;
	sds sdsNameTable = plg_sdsNewLen(nameTable, nameTableLen);
	dictEntry* tableNameNode = plg_dictFind(pManage->dictTableName, sdsNameTable);
	if (tableNameNode != NULL) {
		PTableName pTableName = dictGetVal(tableNameNode);
		pTableName->weight = weight;
		ret = 1;
	}
	plg_sdsFree(sdsNameTable);
	return ret;
}

int plg_MngSetNoSave(void* pvManage, char* nameTable, short nameTableLen, unsigned char noSave) {

	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Changes are not allowed during system runing!");
		return 0;
	}

	int ret = 0;
	sds sdsNameTable = plg_sdsNewLen(nameTable, nameTableLen);
	dictEntry* tableNameNode = plg_dictFind(pManage->dictTableName, sdsNameTable);
	if (tableNameNode != NULL) {
		PTableName pTableName = dictGetVal(tableNameNode);
		pTableName->noSave = noSave;
		ret = 1;
	}
	plg_sdsFree(sdsNameTable);
	return ret;
}

int plg_MngSetNoShare(void* pvManage, char* nameTable, short nameTableLen, unsigned char noShare) {

	PManage pManage = pvManage;
	if (pManage->runStatus) {
		elog(log_error, "Changes are not allowed during system runing!");
		return 0;
	}

	int ret = 0;
	sds sdsNameTable = plg_sdsNewLen(nameTable, nameTableLen);
	dictEntry* tableNameNode = plg_dictFind(pManage->dictTableName, sdsNameTable);
	if (tableNameNode != NULL) {
		PTableName pTableName = dictGetVal(tableNameNode);
		pTableName->noShare = noShare;
		ret = 1;
	}
	plg_sdsFree(sdsNameTable);
	return ret;
}

/*
Because of the check mode, create and star are separated
Users can adjust the number of cores according to the results. If they are not satisfied, they can
Recreate EJB after destroyjob
Because there is a read-only order with random access, it should be reserved for empty jobs
*/
int plg_MngStarJob(void* pvManage) {

	CheckUsingThread(0);
	PManage pManage = pvManage;
	if (pManage->runStatus == 1) {
		return 0;
	}

	//file job before workjob start
	listIter* diskIter = plg_listGetIterator(pManage->listDisk, AL_START_HEAD);
	listNode* diskNode;
	while ((diskNode = plg_listNext(diskIter)) != NULL) {
		void* fileHandle = plg_DiskFileHandle(listNodeValue(diskNode));
		if (fileHandle) {
			if (plg_JobStartRouting(plg_FileJobHandle(fileHandle)) != 0)
				elog(log_error, "can't create thread");
		}
	}
	plg_listReleaseIterator(diskIter);

	//listJob
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {
		if (plg_JobStartRouting(listNodeValue(jobNode)) != 0)
			elog(log_error, "can't create thread");
	}
	plg_listReleaseIterator(jobIter);

	//manage
	if (plg_JobStartRouting(pManage->pJobHandle) != 0)
		elog(log_error, "can't create thread");

	pManage->runStatus = 1;
	pManage->jobDestroyCount = 0;
	pManage->fileDestroyCount = 0;

	return 1;
}

/*
Exit thread through message system, cannot force exit
*/
static void manage_DestroyJob(void* pvManage, AfterDestroyFun fun, void* ptr) {

	elog(log_fun, "manage_DestroyJob");
	CheckUsingThread(NORET);
	//listjob
	PManage pManage = pvManage;
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {

		ManageDestroy pManageDestroy;
		pManageDestroy.fun = fun;
		pManageDestroy.ptr = ptr;
		pManageDestroy.type = 1;
		pManageDestroy.pManage = pManage;
		plg_JobSendOrder(plg_JobEqueueHandle(listNodeValue(jobNode)), "destroy", (char*)&pManageDestroy, sizeof(ManageDestroy));
	}
	plg_listReleaseIterator(jobIter);
}

/*
Exit thread through message system, cannot force exit
*/
void plg_MngStopJob(void* pvManage) {

	CheckUsingThread(NORET);
	//listjob
	PManage pManage = pvManage;
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {
		plg_JobSendOrder(plg_JobEqueueHandle(listNodeValue(jobNode)), "destroyjob", 0, 0);
	}
	plg_listReleaseIterator(jobIter);
	pManage->runStatus = 0;
}


/*
Unequal communication means that users send and receive data in different ways
Do not use send data to wait for communication users,
User receives data using event
*/
int plg_MngRemoteCallWithOrderID(void* pvManage, char* order, short orderLen, char* value, short valueLen, unsigned int orderID) {

	int r = 0;
	CheckUsingThread(0);

	PManage pManage = pvManage;
	POrderPacket pOrderPacket = malloc(sizeof(OrderPacket));
	pOrderPacket->order = plg_sdsNewLen(order, orderLen);
	pOrderPacket->value = plg_sdsNewLen(value, valueLen);
	pOrderPacket->orderID = 0;

	dictEntry* entry = plg_dictFind(pManage->order_equeue, pOrderPacket->order);
	if (entry) {

		if (orderID != 0) {
			elog(log_error, "plg_MngRemoteCallWithOrderID::Use OrderID %i to call an order with shared data", orderID);
		}
		r = plg_eqIfNoPush(dictGetVal(entry), pOrderPacket, pManage->maxQueue);
		if (r == 0) {
			plg_sdsFree(pOrderPacket->order);
			plg_sdsFree(pOrderPacket->value);
			free(pOrderPacket);

			elog(log_error, "plg_MngRemoteCall Queue limit exceeded for %i", pManage->maxQueue);
		}
	} else {

		dictEntry* entryPrcess = plg_dictFind(pManage->order_process, pOrderPacket->order);
		if (entryPrcess) {

			void* equeue = 0;
			if (orderID) {
				equeue = plg_MngJobEqueueWithCore(pvManage, JobJobID(orderID));
			} else {
				equeue = plg_MngRandJobEqueue(pvManage);
			}
			
			if (JobJobOrderID(orderID) == 0) {
				pOrderPacket->orderID = 0;
			} else {
				pOrderPacket->orderID = orderID;
			}
			
			r = plg_eqIfNoPush(equeue, pOrderPacket, pManage->maxQueue);
			if (r == 0) {
				plg_sdsFree(pOrderPacket->order);
				plg_sdsFree(pOrderPacket->value);
				free(pOrderPacket);

				elog(log_error, "plg_MngRemoteCall Queue limit exceeded for %i", pManage->maxQueue);
			}
		} else {
			elog(log_error, "plg_MngRemoteCall.Order:%s not found", order);

			plg_sdsFree(pOrderPacket->order);
			plg_sdsFree(pOrderPacket->value);
			free(pOrderPacket);
		}
	}

	return r;
}

int plg_MngRemoteCall(void* pvManage, char* order, short orderLen, char* value, short valueLen) {
	return plg_MngRemoteCallWithOrderID(pvManage, order, orderLen, value, valueLen, 0);
}

int plg_MngRemoteCallPacket(void* pvManage, void* pvOrderPacket, char** order, unsigned int orderID) {

	int r = 0;
	PManage pManage = pvManage;
	POrderPacket pOrderPacket = pvOrderPacket;
	dictEntry* entryPrcess = plg_dictFind(pManage->order_process, pOrderPacket->order);
	if (entryPrcess) {
		void* equeue = 0;
		if (orderID) {
			equeue = plg_MngJobEqueueWithCore(pvManage, JobJobID(orderID));
		} else {
			equeue = plg_MngRandJobEqueue(pvManage);
		}
		
		*order = dictGetKey(entryPrcess);
		plg_eqPush(equeue, pOrderPacket);
		r = 1;
	} else {
		elog(log_error, "plg_MngRemoteCallPacket.Order:%s not found", pOrderPacket->order);

		plg_sdsFree(pOrderPacket->order);
		plg_sdsFree(pOrderPacket->value);
		free(pOrderPacket);
	}

	return r;
}

int plg_MngRemoteCallWithMaxCore(void* pvManage, char* order, short orderLen, char* value, short valueLen) {

	PManage pManage = pvManage;
	int limite = listLength(pManage->listJob);
	for (int c = 1; c <= limite; c++) {	
		plg_MngRemoteCallWithOrderID(pvManage, order, orderLen, value, valueLen, JobGetOrderIDFromJobID(c));
	}
	return limite;
}

int plg_MngRemoteCallWithArg2(void* pvManage, char* order, short orderLen, void* eventHandle, int argc, const char** argv, unsigned int orderID) {

	int ret = 0;
	pJSON* root = pJson_CreateObject();

	pJson_AddNumberToObject(root, "argc", argc);
	char* bEventHandle = plg_B64Encode((unsigned char*)&eventHandle, sizeof(void*));
	pJson_AddStringToObject(root, "event", bEventHandle);

	if (argc) {
		pJSON* argvJson = pJson_CreateStringArray(argv, argc);
		pJson_AddItemToObject(root, "argv", argvJson);
	}

	char* cValue = pJson_Print(root);
	ret = plg_MngRemoteCallWithOrderID(pvManage, order, orderLen, cValue, strlen(cValue), orderID);

	free(bEventHandle);
	pJson_Delete(root);

	return ret;
}

int plg_MngRemoteCallWithArg(void* pvManage, char* order, short orderLen, void* eventHandle, int argc, const char** argv) {

	return plg_MngRemoteCallWithArg2(pvManage, order, orderLen, eventHandle, argc, argv, 0);
}

int plg_MngRemoteCallWithJson2(void* pvManage, char* order, short orderLen, void* eventHandle, char* json, short jsonLen, unsigned int orderID) {

	NOTUSED(jsonLen);
	int ret = 0;
	pJSON* root = pJson_Parse(json);
	if (!root) {
		elog(log_error, "plg_MngRemoteCallWithJson: parse json");
		return ret;
	}

	char* bEventHandle = plg_B64Encode((unsigned char*)&eventHandle, sizeof(void*));
	pJson_AddStringToObject(root, "event", bEventHandle);

	char* cValue = pJson_Print(root);
	ret = plg_MngRemoteCallWithOrderID(pvManage, order, orderLen, cValue, strlen(cValue), orderID);

	free(bEventHandle);
	pJson_Delete(root);
	return ret;
}

int plg_MngRemoteCallWithJson(void* pvManage, char* order, short orderLen, void* eventHandle, char* json, short jsonLen) {

	return plg_MngRemoteCallWithJson2(pvManage, order, orderLen, eventHandle, json, jsonLen, 0);
}

void* plg_MngJobHandle(void* pvManage) {
	PManage pManage = pvManage;
	return pManage->pJobHandle;
}

/*
Consider carefully when the lock is nested in a loop, where adding a lock may result in a release failure or deadlock
*/
static int OrderDestroyCount(char* value, short valueLen) {
	unsigned int run = 0;
	PManageDestroy pManageDestroy = (PManageDestroy)value;
	MutexLock(pManageDestroy->pManage->mutexHandle, pManageDestroy->pManage->objName);
	if (valueLen == sizeof(ManageDestroy)) {
		if (pManageDestroy->type == 1) {
			pManageDestroy->pManage->jobDestroyCount += 1;
			if (pManageDestroy->pManage->jobDestroyCount == listLength(pManageDestroy->pManage->listJob)) {
				run = 1;
			}			
		}else if (pManageDestroy->type == 2) {
			pManageDestroy->pManage->fileDestroyCount += 1;
			if (pManageDestroy->pManage->fileDestroyCount == listLength(pManageDestroy->pManage->listDisk)) {
				run = 2;
			}
		}
	}
	MutexUnlock(pManageDestroy->pManage->mutexHandle, pManageDestroy->pManage->objName);

	if (run == 1) {
		sleep(0);
		pManageDestroy->fun(pManageDestroy->ptr);
	} else if (run == 2) {
		sleep(0);
		pManageDestroy->fun(pManageDestroy->ptr);
	}

	return 1;
}

static void manage_InternalDestoryHandle(void* pvManage) {

	PManage pManage = pvManage;
	plg_MutexDestroyHandle(pManage->mutexHandle);
	plg_listRelease(pManage->listDisk);
	plg_listRelease(pManage->listJob);
	plg_listRelease(pManage->listOrder);
	plg_listRelease(pManage->listProcess);
	plg_dictRelease(pManage->dictTableName);

	plg_dictRelease(pManage->order_process);
	plg_dictRelease(pManage->order_equeue);
	plg_DictSetDestroy(pManage->order_tableName);
	plg_dictRelease(pManage->tableName_diskHandle);
	
	plg_sdsFree(pManage->dbPath);
	plg_sdsFree(pManage->objName);
	plg_sdsFree(pManage->luaLIBPath);
	plg_dictRelease(pManage->libFun);
	
	free(pManage);
}

/*
All threads have been stopped and locks are no longer needed
*/
static void CompleteDestroyFile(void* value) {

	PManageDestroy pManageDestroy = (PManageDestroy)value;
	pManageDestroy->pManage->runStatus = 0; 
	free(value);
	plg_JobSetExitThread(2);
}

//to Destroy file
static void CallBackDestroyFile(void* value) {

	elog(log_fun, "CallBackDestroyFile");
	unsigned int run = 0;
	PManageDestroy pManageDestroy = (PManageDestroy)value;
	MutexLock(pManageDestroy->pManage->mutexHandle, pManageDestroy->pManage->objName);
	if (listLength(pManageDestroy->pManage->listDisk) == 0) {
		run = 1;
	} else {
		
		//listjob
		listIter* diskIter = plg_listGetIterator(pManageDestroy->pManage->listDisk, AL_START_HEAD);
		listNode* diskNode;
		while ((diskNode = plg_listNext(diskIter)) != NULL) {

			ManageDestroy pManageDestroyForFile;
			pManageDestroyForFile.fun = CompleteDestroyFile;
			pManageDestroyForFile.ptr = pManageDestroy;
			pManageDestroyForFile.pManage = pManageDestroy->pManage;
			pManageDestroyForFile.type = 2;		
			if (plg_DiskFileHandle(listNodeValue(diskNode))) {
				void* eQueueHandle = plg_JobEqueueHandle(plg_FileJobHandle(plg_DiskFileHandle(listNodeValue(diskNode))));
				plg_JobSendOrder(eQueueHandle, "destroy", (char*)&pManageDestroyForFile, sizeof(ManageDestroy));
			} else {
				plg_JobSendOrder(plg_JobEqueueHandle(pManageDestroy->pManage->pJobHandle), "destroycount", (char*)&pManageDestroyForFile, sizeof(ManageDestroy));
			}
		}
		plg_listReleaseIterator(diskIter);
	}
	MutexUnlock(pManageDestroy->pManage->mutexHandle, pManageDestroy->pManage->objName);

	if (run == 1) {
		CompleteDestroyFile(pManageDestroy);
	}
}

void plg_MngSetStat(void* pvManage, short stat) {
	PManage pManage = pvManage;
	pManage->isOpenStat = stat;
}

void plg_MngSetMaxQueue(void* pvManage, unsigned int maxQueue) {
	PManage pManage = pvManage;
	pManage->maxQueue = maxQueue;
}

void plg_MngSetStatCheckTime(void* pvManage, short checkTime) {
	PManage pManage = pvManage;
	pManage->checkTime = checkTime;
}

/*
Create a handle to manage multiple files
Multithreading is not safe and is read-only during multithreading startup.
*/
void* plg_MngCreateHandle(char* dbPath, short dbPahtLen) {

	CheckUsingThread(0);

	plg_LocksCreate();
	plg_LogInit();

	PManage pManage = malloc(sizeof(Manage));
	pManage->mutexHandle = plg_MutexCreateHandle(LockLevel_1);

	pManage->listDisk = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pManage->listDisk, listFileFree);
	
	pManage->listJob = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pManage->listJob, listJobFree);

	pManage->listOrder = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pManage->listOrder, listSdsFree);

	pManage->listProcess = plg_listCreate(LIST_MIDDLE);
	listSetFreeMethod(pManage->listProcess, listProcessFree);

	pManage->dictTableName = plg_dictCreate(&SdsDictType, NULL, DICT_MIDDLE);
	
	pManage->maxQueue = 0;
	pManage->isOpenStat = 0;
	pManage->checkTime = 5000;
	pManage->order_tableName = plg_DictSetCreate(plg_DefaultSdsDictPtr(), DICT_MIDDLE, plg_DefaultSdsDictPtr(), DICT_MIDDLE);
	pManage->tableName_diskHandle = plg_dictCreate(plg_DefaultSdsDictPtr(), NULL, DICT_MIDDLE);
	pManage->order_process = plg_dictCreate(plg_DefaultSdsDictPtr(), NULL, DICT_MIDDLE);
	pManage->order_equeue = plg_dictCreate(plg_DefaultSdsDictPtr(), NULL, DICT_MIDDLE);
	pManage->dbPath = plg_sdsNewLen(dbPath, dbPahtLen);
	pManage->objName = plg_sdsNew("manage");
	pManage->pJobHandle = plg_JobCreateHandle(0, TT_MANAGE, NULL, 0, 1);
	plg_JobSetPrivate(pManage->pJobHandle, pManage);

	pManage->fileCount = 0;
	pManage->jobDestroyCount = 0;
	pManage->fileDestroyCount = 0;
	pManage->runStatus = 0;
	pManage->maxTableWeight = 1000;
	pManage->luaLIBPath = plg_sdsEmpty();
	pManage->luaHot = 0;
	pManage->noSave = 0;

	pManage->libFun = plg_dictCreate(&LibDictType, NULL, DICT_MIDDLE);

	//event process
	plg_JobAddAdmOrderProcess(pManage->pJobHandle, "destroycount", plg_JobCreateFunPtr(OrderDestroyCount));

	return pManage;
}

void plg_MngAddLibFun(void* pvManage, char* libPath, char* fun) {
	PManage pManage = pvManage;
	dictEntry * entry = plg_dictFind(pManage->libFun, fun);
	if (!entry) {

		sds sdsPath = plg_sdsNew(libPath);
		if (0 != access_t(sdsPath, 0)) {
			sdsPath = plg_sdsCat(sdsPath, LIB_EXT);
		}

		void* p = plg_SysLibLoad(sdsPath, 1);
		plg_sdsFree(sdsPath);
		if (p == NULL) {
			elog(log_error, "plg_MngAddLibFun.plg_SysLibLoad:%s", sdsPath);
			return;
		}

		plg_dictAdd(pManage->libFun, plg_sdsNew(fun), p);
	} else {
		elog(log_warn, "plg_MngAddLibFun. function %s Repetitive definition at %s.", libPath, fun);
	}
}

void* plg_MngFindLibFun(void* pvManage, char* Fun) {
	PManage pManage = pvManage;
	dictEntry * entry = plg_dictFind(pManage->libFun, Fun);
	if (entry != 0) {
		return dictGetVal(entry);
	} else {
		return 0;
	}
}

void plg_MngSetLuaLibPath(void* pvManage, char* newLuaLibPath) {
	PManage pManage = pvManage;
	plg_sdsFree(pManage->luaLIBPath);
	pManage->luaLIBPath = plg_sdsNew(newLuaLibPath);
}

void plg_MngSetLuaHot(void* pvManage, short luaHot) {
	PManage pManage = pvManage;
	pManage->luaHot = luaHot;
}

void plg_MngSetAllNoSave(void* pvManage, short noSave) {
	PManage pManage = pvManage;
	pManage->noSave = noSave;
}

void plg_MngSendExit(void* pvManage){
	PManage pManage = pvManage;
	plg_EventSend(pManage->pEvent, NULL, 0);
}

/*
Actually stop all threads from executing in multithreading safety
*/
void plg_MngDestoryHandle(void* pvManage) {
	
	CheckUsingThread(NORET);
	PManage pManage = pvManage;
	if (pManage->runStatus == 0) {
		plg_JobDestoryHandle(pManage->pJobHandle);
	} else {
		void* pEvent = plg_EventCreateHandle();
		pManage->pEvent = pEvent;
		MutexLock(pManage->mutexHandle, pManage->objName);
		PManageDestroy pManageDestroy = malloc(sizeof(ManageDestroy));
		pManageDestroy->pManage = pManage;
		pManageDestroy->type = 3;
		manage_DestroyJob(pManage, CallBackDestroyFile, pManageDestroy);
		MutexUnlock(pManage->mutexHandle, pManage->objName);

		plg_EventWait(pEvent);
		plg_EventDestroyHandle(pEvent);
		plg_JobDestoryHandle(pManage->pJobHandle);
	}
	manage_InternalDestoryHandle(pManage);
	plg_LogDestroy();
	plg_LocksDestroy();
}

char plg_MngCheckUsingThread() {
	if (1 != plg_JobCheckIsType(TT_OTHER)) {
		return 0;
	}
	return 1;
}

void plg_MngSetMaxTableWeight(void* pvManage, unsigned int maxTableWeight) {
	PManage pManage = pvManage;
	pManage->maxTableWeight = maxTableWeight;
}

char* plg_MngPrintAllJobStatusJson(void* pvManage) {

	pJSON* root = pJson_CreateObject();
	PManage pManage = pvManage;
	pJson_AddNumberToObject(root, "size", listLength(pManage->listJob));
	//listjob
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {
		plg_JobPrintStatus(listNodeValue(jobNode), root);
	}
	plg_listReleaseIterator(jobIter);

	char* ps = pJson_Print(root);
	pJson_Delete(root);
	return ps;
}

void plg_MngPrintAllJobStatus(void* pvManage) {

	char* ps = plg_MngPrintAllJobStatusJson(pvManage);
	puts(ps);
	puts("\n");
	free(ps);

}

char* plg_MngPrintAllJobDetailsJson(void* pvManage) {

	pJSON* root = pJson_CreateObject();
	PManage pManage = pvManage;
	pJson_AddNumberToObject(root, "size", listLength(pManage->listJob));
	//listjob
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {
		plg_JobPrintDetails(listNodeValue(jobNode), root);
	}
	plg_listReleaseIterator(jobIter);

	char* ps = pJson_Print(root);
	pJson_Delete(root);
	return ps;
}

void plg_MngPrintAllJobDetails(void* pvManage) {

	char* ps = plg_MngPrintAllJobDetailsJson(pvManage);
	puts(ps);
	puts("\n");
	free(ps);

}

char* plg_MngPrintAllJobOrderJson(void* pvManage) {

	pJSON* root = pJson_CreateObject();
	PManage pManage = pvManage;
	pJson_AddNumberToObject(root, "size", listLength(pManage->listJob));
	//listjob
	listIter* jobIter = plg_listGetIterator(pManage->listJob, AL_START_HEAD);
	listNode* jobNode;
	while ((jobNode = plg_listNext(jobIter)) != NULL) {
		plg_JobPrintOrder(listNodeValue(jobNode), root);
	}
	plg_listReleaseIterator(jobIter);

	char* ps = pJson_Print(root);
	pJson_Delete(root);
	return ps;
}

void plg_MngPrintAllJobOrder(void* pvManage) {

	char* ps = plg_MngPrintAllJobOrderJson(pvManage);
	puts(ps);
	puts("\n");
	free(ps);
}

char* plg_MngPrintAllStatusJson(void* pvManage) {

	pJSON* root = pJson_CreateObject();
	PManage pManage = pvManage;
	pJson_AddNumberToObject(root, "listDisk", listLength(pManage->listDisk));
	pJson_AddNumberToObject(root, "listJob", listLength(pManage->listJob));
	pJson_AddNumberToObject(root, "listOrder", listLength(pManage->listOrder));
	pJson_AddNumberToObject(root, "listProcess", listLength(pManage->listProcess));
	pJson_AddNumberToObject(root, "dictTableName", dictSize(pManage->dictTableName));
	pJson_AddNumberToObject(root, "order_process", dictSize(pManage->order_process));
	pJson_AddNumberToObject(root, "order_equeue", dictSize(pManage->order_equeue));
	pJson_AddNumberToObject(root, "order_tableName", plg_DictSetSize(pManage->order_tableName));
	pJson_AddNumberToObject(root, "tableName_diskHandle", dictSize(pManage->tableName_diskHandle));
	pJson_AddNumberToObject(root, "fileCount", pManage->fileCount);
	pJson_AddNumberToObject(root, "jobDestroyCount", pManage->jobDestroyCount);
	pJson_AddNumberToObject(root, "fileDestroyCount", pManage->fileDestroyCount);

	char* ps = pJson_Print(root);
	pJson_Delete(root);
	return ps;
}

void plg_MngPrintAllStatus(void* pvManage) {

	char* ps = plg_MngPrintAllStatusJson(pvManage);
	puts(ps);
	puts("\n");
	free(ps);
}

char* plg_MngPrintAllDetailsJson(void* pvManage) {

	pJSON* root = pJson_CreateObject();
	PManage pManage = pvManage;
	dict * table = plg_DictSetDict(pManage->order_tableName);

	dictIterator* tableIter = plg_dictGetSafeIterator(table);
	dictEntry* tableNode;
	while ((tableNode = plg_dictNext(tableIter)) != NULL) {

		pJSON* table = pJson_CreateArray();
		pJson_AddItemToObject(root, (char*)dictGetKey(tableNode), table);

		dict* value = dictGetVal(tableNode);
		dictIterator* valueIter = plg_dictGetSafeIterator(value);
		dictEntry* valueNode;
		while ((valueNode = plg_dictNext(valueIter)) != NULL) {

			pJSON* tableName = pJson_CreateString((char*)dictGetKey(valueNode));
			pJson_AddItemToArray(table, tableName);
		}
		plg_dictReleaseIterator(valueIter);
	}
	plg_dictReleaseIterator(tableIter);

	char* ps = pJson_Print(root);
	pJson_Delete(root);
	return ps;
}

void plg_MngPrintAllDetails(void* pvManage) {

	char* ps = plg_MngPrintAllDetailsJson(pvManage);
	puts(ps);
	puts("\n");
	free(ps);
}

/*
loop event_dictTableName
core: number core
*/
char* plg_MngPrintPossibleAllocJson(void* pvManage) {

	pJSON* root = pJson_CreateObject();
	PManage pManage = pvManage;
	void* pDictTableName = plg_DictSetCreate(plg_DefaultUintPtr(), DICT_MIDDLE, plg_DefaultSdsDictPtr(), DICT_MIDDLE);
	void* pDictOrder = plg_DictSetCreate(plg_DefaultUintPtr(), DICT_MIDDLE, plg_DefaultSdsDictPtr(), DICT_MIDDLE);
	//listOrder
	int breakDup = 0;
	listIter* eventIterDup = 0;
	listIter* eventIter = plg_listGetIterator(pManage->listOrder, AL_START_HEAD);
	listNode* eventNode;

	do {
		//listOrder
		while ((eventNode = plg_listNext(eventIter)) != NULL) {

			//process
			dictEntry * EventProcessEntry = plg_dictFind(pManage->order_process, listNodeValue(eventNode));
			if (EventProcessEntry == 0) {
				continue;
			}

			//loop dictTableName
			char nextContinue = 0;
			dictIterator* jobIter = plg_dictGetIterator((dict*)plg_DictSetDict(pDictTableName));
			dictEntry* jobNode;
			while ((jobNode = plg_dictNext(jobIter)) != NULL) {

				//Judge whether there is intersection
				dict * table = plg_DictSetValue(pManage->order_tableName, listNodeValue(eventNode));
				if (!table) {
					continue;
				}
				dictIterator* tableIter = plg_dictGetSafeIterator(table);
				dictEntry* tableNode;
				while ((tableNode = plg_dictNext(tableIter)) != NULL) {

					//Find the intersection in dicttablename, and merge together if there is any
					if (plg_DictSetIn(pDictTableName, dictGetKey(jobNode), dictGetKey(tableNode))) {
						dictIterator* tableLoopIter = plg_dictGetSafeIterator(table);
						dictEntry* tableLoopNode;
						while ((tableLoopNode = plg_dictNext(tableLoopIter)) != NULL) {
							plg_DictSetAdd(pDictTableName, dictGetKey(jobNode), dictGetKey(tableLoopNode));
						}
						plg_dictReleaseIterator(tableLoopIter);

						plg_DictSetAdd(pDictOrder, dictGetKey(jobNode), dictGetKey(EventProcessEntry));
						nextContinue = 1;
						break;
					}
				}
				plg_dictReleaseIterator(tableIter);
				if (nextContinue) {
					break;
				}
			}
			plg_dictReleaseIterator(jobIter);
			if (nextContinue) {
				continue;
			}

			if (!breakDup) {
				breakDup = 1;
				eventIterDup = plg_listIteratorDup(eventIter);
				//Create new and merge not found
				do {
					unsigned int* key = malloc(sizeof(unsigned int));
					*key = dictSize((dict*)plg_DictSetDict(pDictTableName));
					unsigned int* key2 = malloc(sizeof(unsigned int));
					*key2 = *key;

					dict * table = plg_DictSetValue(pManage->order_tableName, listNodeValue(eventNode));
					if (table) {
						dictIterator* tableLoopIter = plg_dictGetSafeIterator(table);
						dictEntry* tableLoopNode;
						while ((tableLoopNode = plg_dictNext(tableLoopIter)) != NULL) {
							plg_DictSetAdd(pDictTableName, key, dictGetKey(tableLoopNode));
						}
						plg_dictReleaseIterator(tableLoopIter);

						plg_DictSetAdd(pDictOrder, key2, dictGetKey(EventProcessEntry));
					}

				} while (0);
			}
		}
		plg_listReleaseIterator(eventIter);
		if (breakDup) {
			breakDup = 0;
			eventIter = eventIterDup;
			continue;
		} else {
			break;
		}
	} while (1);

	//Print all results
	pJson_AddNumberToObject(root, "size", dictSize((dict*)plg_DictSetDict(pDictTableName)));
	pJSON* allGroup = pJson_CreateArray();
	pJson_AddItemToObject(root, "group", allGroup);

	dictIterator* jobIter = plg_dictGetIterator((dict*)plg_DictSetDict(pDictTableName));
	dictEntry* jobNode;
	while ((jobNode = plg_dictNext(jobIter)) != NULL) {

		pJSON* group = pJson_CreateObject();
		pJson_AddItemToArray(allGroup, group);

		pJSON* order = pJson_CreateArray();
		pJson_AddItemToObject(group, "order", order);

		dictIterator* orderIter = plg_dictGetIterator((dict*)plg_DictSetValue(pDictOrder, dictGetKey(jobNode)));
		dictEntry* orderNode;
		while ((orderNode = plg_dictNext(orderIter)) != NULL) {
			pJson_AddItemToArray(order, pJson_CreateString((char*)dictGetKey(orderNode)));
		}
		plg_dictReleaseIterator(orderIter);

		pJSON* table = pJson_CreateArray();
		pJson_AddItemToObject(group, "table", table);

		dictIterator* tableIter = plg_dictGetIterator((dict*)plg_DictSetValue(pDictTableName, dictGetKey(jobNode)));
		dictEntry* tableNode;
		while ((tableNode = plg_dictNext(tableIter)) != NULL) {
			pJson_AddItemToArray(table, pJson_CreateString((char*)dictGetKey(tableNode)));
		}
		plg_dictReleaseIterator(tableIter);
	}
	plg_dictReleaseIterator(jobIter);
	//free
	plg_DictSetDestroy(pDictTableName);
	plg_DictSetDestroy(pDictOrder);
	
	char* ps = pJson_Print(root);
	pJson_Delete(root);
	return ps;
}

void plg_MngPrintPossibleAlloc(void* pvManage) {

	char* ps = plg_MngPrintPossibleAllocJson(pvManage);
	puts(ps);
	puts("\n");
	free(ps);
}

typedef struct _Param
{
	sds outJson;
	void* pEvent;
	void* pManage;
	pJSON* fromJson;
	short endFlg;
	short tbaleType;
}*PParam, Param;

static int OutJsonRouting(char* value, short valueLen) {

	//routing
	NOTUSED(valueLen);
	PParam pParam = (PParam)value;
	PManage pManage = pParam->pManage;

	plg_MkDirs(pParam->outJson);
	
	dictIterator* tableNameIter = plg_dictGetIterator(pManage->dictTableName);
	dictEntry* tableNameNode;
	while ((tableNameNode = plg_dictNext(tableNameIter)) != NULL) {
		pJSON* root = pJson_CreateObject();

		pJson_AddNumberToObject(root, "tableType", plg_JobGetTableType(dictGetKey(tableNameNode), plg_sdsLen(dictGetKey(tableNameNode))));
		pJSON* tableObj = pJson_CreateObject();
		pJson_AddItemToObject(root, dictGetKey(tableNameNode), tableObj);
		plg_JobTableMembersWithJson(dictGetKey(tableNameNode), plg_sdsLen(dictGetKey(tableNameNode)), tableObj);

		sds fileName = plg_sdsCatFmt(plg_sdsEmpty(), "%s/%s.json", pParam->outJson, dictGetKey(tableNameNode));
		//open file
		FILE *outputFile;
		outputFile = fopen_t(fileName, "wb");
		if (outputFile) {
			char* ptr = pJson_Print(root);
			fwrite(ptr, 1, strlen(ptr), outputFile);
			free(ptr);
		} else {
			elog(log_warn, "OutJsonRouting.fopen_t.wb!");
		}

		plg_sdsFree(fileName);
		fclose(outputFile); 
		pJson_Delete(root);

	}
	plg_dictReleaseIterator(tableNameIter);
	
	plg_sdsFree(pParam->outJson);
	//all pass
	plg_EventSend(pParam->pEvent, NULL, 0);
	printf("OutJsonRouting all pass!\n");
	return 1;
}

void plg_MngOutJson(char* fileName, char* outJson) {

	void* pManage = plg_MngCreateHandle(0, 0);
	
	void* pEvent = plg_EventCreateHandle();

	plg_MngFreeJob(pManage);

	char order[10] = { 0 };
	strcpy(order, "order");
	plg_MngAddOrder(pManage, order, strlen(order), plg_JobCreateFunPtr(OutJsonRouting));

	plg_MngInterAllocJob(pManage, 1, fileName);
	
	plg_MngStarJob(pManage);

	Param param;
	param.outJson = plg_sdsNew(outJson);
	param.pEvent = pEvent;
	param.pManage = pManage;
	plg_MngRemoteCall(pManage, order, strlen(order), (char*)&param, sizeof(Param));

	//Because it is not a thread created by ptw32, ptw32 new cannot release memory leak
	plg_EventWait(pEvent);

	unsigned int eventLen;
	void * ptr = plg_EventRecvAlloc(pEvent, &eventLen);
	plg_EventFreePtr(ptr);

	plg_EventDestroyHandle(pEvent);
	plg_MngDestoryHandle(pManage);
}

static int FromJsonRouting(char* value, short valueLen) {

	//routing
	NOTUSED(valueLen);
	PParam pParam = (PParam)value;
	short tableType = pParam->tbaleType;
	long long count = 0;
	
	for (int i = 0; i < pJson_GetArraySize(pParam->fromJson); i++)
	{
		pJSON * item = pJson_GetArrayItem(pParam->fromJson, i);
		if (pJson_String == item->type && tableType == TT_Byte) {
			unsigned int outLen;
			unsigned char* pValue = plg_B64DecodeEx(item->valuestring, strlen(item->valuestring), &outLen);
			plg_JobSet(pParam->fromJson->string, strlen(pParam->fromJson->string), item->string, strlen(item->string), pValue, outLen);
		} else if (pJson_String == item->type && (tableType == TT_String || tableType == -1)) {
			plg_JobSet(pParam->fromJson->string, strlen(pParam->fromJson->string), item->string, strlen(item->string), item->valuestring, strlen(item->valuestring) + 1);
			tableType = TT_String;
		} else if (pJson_Number == item->type && (tableType == TT_Double || tableType == -1)) {			
			plg_JobSet(pParam->fromJson->string, strlen(pParam->fromJson->string), item->string, strlen(item->string), &item->valuedouble, sizeof(double));
			tableType = TT_Double;
		} else if (pJson_Object == item->type) {
			
			for (int i = 0; i < pJson_GetArraySize(item); i++)
			{
				pJSON * setItem = pJson_GetArrayItem(item, i);
				for (int i = 0; i < pJson_GetArraySize(item); i++)
				{
					plg_JobSAdd(pParam->fromJson->string, strlen(pParam->fromJson->string), item->string, strlen(item->string), setItem->string, strlen(setItem->string));
				}
			}
		}

		//commit pre 100000
		if (++count % 100000 == 0) {
			plg_JobForceCommit();
		}
	}

	if (pParam->endFlg) {
		//all pass
		plg_EventSend(pParam->pEvent, NULL, 0);
		printf("FromJsonRouting all pass!\n");
	}

	return 1;
}

void plg_MngFromJson(char* fromJson) {

	void* pManage = plg_MngCreateHandle(0, 0);
	void* pEvent = plg_EventCreateHandle();

	plg_MngFreeJob(pManage);

	char order[10] = { 0 };
	strcpy(order, "order");
	plg_MngAddOrder(pManage, order, strlen(order), plg_JobCreateFunPtr(FromJsonRouting));

	//open file
	FILE *inputFile;
	char* rootJson = 0;
	inputFile = fopen_t(fromJson, "rb");
	if (inputFile) {
		fseek_t(inputFile, 0, SEEK_END);
		unsigned int inputFileLength = ftell_t(inputFile);

		fseek_t(inputFile, 0, SEEK_SET);
		rootJson = malloc(inputFileLength);
		unsigned long long retRead = fread(rootJson, 1, inputFileLength, inputFile);
		if (retRead != inputFileLength) {
			elog(log_error, "plg_MngFromJson.fread.rootJson!");
			return;
		}
		fclose(inputFile);
	} else {
		elog(log_warn, "plg_MngFromJson.fopen_t.rb!");
	}

	pJSON * root = pJson_Parse(rootJson);
	free(rootJson);
	if (!root) {
		elog(log_error, "plg_MngFromJson:json Error before: [%s]\n", pJson_GetErrorPtr());
		return;
	}

	short tbaleType = -1;
	pJSON * item = pJson_GetObjectItem(root, "tableType");
	if (item) {
		tbaleType  = (short)item->valuedouble;
	}

	for (int i = 0; i < pJson_GetArraySize(root); i++)
	{
		pJSON * item = pJson_GetArrayItem(root, i);
		if (pJson_Object == item->type) {
			plg_MngAddTable(pManage, order, strlen(order), item->string, strlen(item->string));
		}
	}

	plg_MngAllocJob(pManage, 1);
	plg_MngStarJob(pManage);

	for (int i = 0; i < pJson_GetArraySize(root); i++)
	{
		pJSON * item = pJson_GetArrayItem(root, i);
		if (pJson_Object == item->type) {
			Param param;
			param.fromJson = item;
			param.pEvent = pEvent;
			param.pManage = pManage;
			param.endFlg = (i == pJson_GetArraySize(root) - 1) ? 1 : 0;
			param.tbaleType = tbaleType;
			plg_MngRemoteCall(pManage, order, strlen(order), (char*)&param, sizeof(Param));
		}
	}

	//Because it is not a thread created by ptw32, ptw32 new cannot release memory leak
	plg_EventWait(pEvent);

	unsigned int eventLen;
	void * ptr = plg_EventRecvAlloc(pEvent, &eventLen);
	plg_EventFreePtr(ptr);

	pJson_Delete(root);
	plg_EventDestroyHandle(pEvent);
	plg_MngDestoryHandle(pManage);
}

int plg_MngTableIsInOrder(void* pvManage, void* order, short orderLen, void* table, short tableLen) {

	PManage pManage = pvManage;
	sds sdsOrder = plg_sdsNewLen(order, orderLen);	
	dict * dictTable = plg_DictSetValue(pManage->order_tableName, sdsOrder);
	plg_sdsFree(sdsOrder);
	if (!dictTable) {
		return 0;
	}

	sds sdsTable = plg_sdsNewLen(table, tableLen);
	dictEntry* entry = plg_dictFind(dictTable, sdsTable);
	plg_sdsFree(sdsTable);
	if (entry) {
		return 1;
	}

	return 0;
}

char** plg_MngOrderAllTable(void* pvManage, void* order, short orderLen, short* tableLen) {

	PManage pManage = pvManage;
	sds sdsOrder = plg_sdsNewLen(order, orderLen);

	dict * table = plg_DictSetValue(pManage->order_tableName, sdsOrder);
	if (!table) {
		return 0;
	}

	*tableLen = dictSize(table);
	short count = 0;
	char** arrary = malloc(dictSize(table) * sizeof(char*));
	dictIterator* dictIter = plg_dictGetSafeIterator(table);
	dictEntry* dictNode;
	while ((dictNode = plg_dictNext(dictIter)) != NULL) {
		arrary[count] = plg_sdsNewLen(dictGetKey(dictNode), plg_sdsLen(dictGetKey(dictNode)));
	}
	plg_dictReleaseIterator(dictIter);

	return arrary;
}

char* plg_MngOrderAllTableWithJson(void* pvManage, void* order, short orderLen) {

	short tableLen;
	char** arrary = plg_MngOrderAllTable(pvManage, order, orderLen, &tableLen);

	pJSON * root = pJson_CreateArray();
	for (int i = 0; i < tableLen; i++) {
		pJson_AddItemToArray(root, pJson_CreateStringWihtLen(arrary[i], plg_sdsLen(arrary[i])));
	}
	free(arrary);

	char* p = pJson_Print(root);
	pJson_Delete(root);
	return p;
}

#undef NORET
#undef CheckUsingThread