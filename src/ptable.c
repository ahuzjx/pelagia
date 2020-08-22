/* table.c - Table structure related
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

#include "pelog.h"
#include "pinterface.h"
#include "psds.h"
#include "padlist.h"
#include "pquicksort.h"
#include "ptable.h"
#include "pdictexten.h"
#include "pcrc16.h"
#include "pstringmatch.h"
#include "ptimesys.h"
#include "pjson.h"
#include "pbase64.h"
#include "prandomlevel.h"
#include "pelagia.h"

/*
PTableInFile pTableInFile: disk table
void* pageOperateHandle:Leader
unsigned int pageSize: disk page size
sds nameaTable: for pTableInFile
unsigned int hitStamp: for pTableInFile
*/
typedef struct _TableHandle
{
	PTableInFile pTableInFile;
	void* pageOperateHandle;
	unsigned int pageSize;
	sds nameaTable;
	unsigned long long hitStamp;
	PTableHandleCallBack pTableHandleCallBack;
}*PTableHandle, TableHandle;

typedef struct _SkipListPoint
{
	unsigned int skipListAddr;
	unsigned short skipListOffset;
	void* page;
	PDiskTableElement pDiskTableElement;
} *PSkipListPoint, SkipListPoint;

typedef SkipListPoint(ARRAY_SKIPLISTPOINT)[SKIPLIST_MAXLEVEL];

void* plg_TableOperateHandle(void* pvTableHandle) {
	PTableHandle pTableHandle = pvTableHandle;
	return pTableHandle->pageOperateHandle;
}

void* plg_TableCreateHandle(void* pTableInFile, void* pageOperateHandle, unsigned int pageSize,
	sds	nameaTable, PTableHandleCallBack pTableHandleCallBack) {
	PTableHandle pTableHandle = malloc(sizeof(TableHandle));
	pTableHandle->pageOperateHandle = pageOperateHandle;
	pTableHandle->pageSize = pageSize;
	pTableHandle->nameaTable = nameaTable;
	pTableHandle->pTableInFile = pTableInFile;
	pTableHandle->pTableHandleCallBack = pTableHandleCallBack;
	return pTableHandle;
}

void plg_TableDestroyHandle(void* pvTableHandle) {
	PTableHandle pTableHandle = pvTableHandle;
	free(pTableHandle);
}

typedef struct _TableIterator
{
	PTableHandle pTableHandle;
	unsigned int elementPage;
	unsigned short elementOffset;
}*PTableIterator, TableIterator;

/*
Ϊ��ɾ��,���ر�ɾ�����ǰһ��.
find table name in skip list
*/
static int TableTailFindCmpFun(void* key1, unsigned int key1Len, void* key2, unsigned int Key2Len) {
	
	//cmp keyStr
	NOTUSED(key1);
	NOTUSED(key1Len);
	NOTUSED(key2);
	NOTUSED(Key2Len);
	return 1;
}

/*
key or head
*/
void* plg_TableGetIteratorToTail(void* pvTableHandle) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableIterator pTableIterator = malloc(sizeof(TableIterator));
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};

	if (plg_TableFindWithName(pTableHandle, NULL, 0, &skipListPoint, TableTailFindCmpFun) == 0) {
		return 0;
	}

	//init for loop
	pTableIterator->pTableHandle = pTableHandle;
	pTableIterator->elementPage = skipListPoint[0].skipListAddr;
	pTableIterator->elementOffset = skipListPoint[0].skipListOffset;

	return pTableIterator;
}

/*
key or head
*/
void* plg_TableGetIteratorWithKey(void* pvTableHandle, void* vKey, short keyLen) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableIterator pTableIterator = malloc(sizeof(TableIterator));
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	PTableInFile pTableInFile;
	if (pTableHandle->pTableInFile->isSetHead) {
		pTableInFile = pTableHandle->pTableInFile;
	} else {
		pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	}

	if (vKey != 0) {
		if (plg_TableFindWithName(pTableHandle, vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
			return 0;
		}
	} else {
		skipListPoint[0].pDiskTableElement = &pTableInFile->tableHead[0];
	}

	//init for loop
	pTableIterator->pTableHandle = pTableHandle;
	pTableIterator->elementPage = skipListPoint[0].pDiskTableElement->nextElementPage;
	pTableIterator->elementOffset = skipListPoint[0].pDiskTableElement->nextElementOffset;

	return pTableIterator;
}

void* plg_TablePrevIterator(void* pvTableIterator) {

	PTableIterator pTableIterator = pvTableIterator;
	if (pTableIterator->elementPage == 0) {
		return 0;
	}

	void *nextPage = 0;
	if (pTableIterator->pTableHandle->pTableHandleCallBack->findPage(pTableIterator->pTableHandle, pTableIterator->elementPage, &nextPage) == 0){
		return 0;
	}
		
	//get PDiskTableKey
	PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(nextPage, pTableIterator->elementOffset);

	PDiskTableKey ptrDiskTableKey;
	ptrDiskTableKey = (PDiskTableKey)POINTER(nextPage, pDiskTableElement->keyOffset);

	//next loop
	pTableIterator->elementPage = ptrDiskTableKey->prevElementPage;
	pTableIterator->elementOffset = ptrDiskTableKey->prevElementOffset;

	return ptrDiskTableKey;
}


void* plg_TableNextIterator(void* pvTableIterator) {

	PTableIterator pTableIterator = pvTableIterator;
	if (pTableIterator == 0) {
		return 0;
	}

	if (pTableIterator->elementPage == 0) {
		return 0;
	}

	void *nextPage = 0;
	if (pTableIterator->pTableHandle->pTableHandleCallBack->findPage(pTableIterator->pTableHandle,  pTableIterator->elementPage, &nextPage) == 0){
		return 0;
	}

	//get PDiskTableKey
	PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(nextPage, pTableIterator->elementOffset);
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(nextPage, pDiskTableElement->keyOffset);
	
	//next loop
	pTableIterator->elementPage = pDiskTableElement->nextElementPage;
	pTableIterator->elementOffset = pDiskTableElement->nextElementOffset;

	return pDiskTableKey;
}

static void* table_DupIterator(void* pvTableIterator) {
	PTableIterator pTableIterator = pvTableIterator;
	PTableIterator pRetTableIterator = malloc(sizeof(TableIterator));
	*pRetTableIterator = *pTableIterator;
	return pRetTableIterator;
}

void plg_TableReleaseIterator(void* pvTableIterator) {
	PTableIterator pTableIterator = pvTableIterator;
	free(pTableIterator);
}

void* plg_TableName(void* pvTableHandle) {
	PTableHandle pTableHandle = pvTableHandle;
	return pTableHandle->nameaTable;
}

unsigned int plg_TableHitStamp(void* pvTableHandle) {
	PTableHandle pTableHandle = pvTableHandle;
	return pTableHandle->hitStamp;
}

int plg_TableHandleCmpFun(void* left, void* right) {

	PTableHandle leftHandle = (PTableHandle)left;
	PTableHandle rightHandle = (PTableHandle)right;

	if (leftHandle->hitStamp > rightHandle->hitStamp) {
		return 1;
	} else if (leftHandle->hitStamp == rightHandle->hitStamp) {
		return 0;
	} else {
		return -1;
	}
}

int plg_TableCheckSpace(void* page) {
	NOTUSED(page);
	//PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)page + sizeof(DiskPageHead));
	unsigned short r = 0;// plg_crc16((unsigned char*)page + pDiskTablePage->spaceAddr, pDiskTablePage->spaceLength);
	plg_assert(!r);
	return !r;
}

int plg_TableCheckLength(void* page, unsigned int pageSize) {
	PDiskPageHead pDiskPageHead = (PDiskPageHead)page;
	NOTUSED(pDiskPageHead);
	PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)page + sizeof(DiskPageHead));

	unsigned int pagesize = pDiskTablePage->delSize + pDiskTablePage->spaceLength + pDiskTablePage->usingLength + sizeof(DiskTablePage) + sizeof(DiskPageHead) + (pDiskTablePage->tableSize - pDiskTablePage->tableLength) * sizeof(DiskTableElement);
	plg_assert(pagesize == FULLSIZE(pageSize));

	return pagesize == FULLSIZE(pageSize);
}

static void table_SetElementKeyOffset(void* page, void* pvZeroElement, unsigned short keyOffset) {
	
	PDiskTableElement pZeroElement = pvZeroElement;
	PDiskTableElement pHighElement = pZeroElement;
	while (1) {
		pHighElement->keyOffset = keyOffset;
		if (pHighElement->highElementOffset == 0) {
			break;
		}
		pHighElement = (PDiskTableElement)POINTER(page, pHighElement->highElementOffset);

	}
}

int plg_SortPDiskTableElementCmp(void* v1, void* v2) {

	PDiskTableElement* vv1 = (PDiskTableElement*)v1;
	PDiskTableElement* vv2 = (PDiskTableElement*)v2;
	if ((*vv1) == 0) {
		return 1;
	} else if ((*vv2) == 0) {
		return -1;
	}

	if ((*vv1)->keyOffset > (*vv2)->keyOffset) {
		return -1;
	} else if ((*vv1)->keyOffset == (*vv2)->keyOffset) {
		return 0;
	} else {
		return 1;
	}
}

void plg_TableArrangementPage(unsigned int pageSize, void* page){

	elog(log_fun, "plg_TableArrangementPage");
	PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)page + sizeof(DiskPageHead));

	plg_assert(plg_TableCheckLength(page, pageSize));
	plg_assert(plg_TableCheckSpace(page));

	PDiskTableElement* pElement = calloc(sizeof(PDiskTableElement), pDiskTablePage->tableLength);

	int count = 0;
	for (unsigned short l = 0; l < pDiskTablePage->tableSize; l++) {
		PDiskTableElement pDiskTableElement = &pDiskTablePage->element[l];
		if (pDiskTableElement->currentLevel == 0 && pDiskTableElement->keyOffset != 0) {		
			pElement[count++] = pDiskTableElement;
		}
	}

	if (count) {
		plg_SortArrary(pElement, sizeof(unsigned short*), pDiskTablePage->tableLength, plg_SortPDiskTableElementCmp);

		unsigned short nextOffest = FULLSIZE(pageSize);
		for (int l = 0; l < count; l++) {
			PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(page, pElement[l]->keyOffset);
			unsigned short allSize = sizeof(DiskTableKey) + pDiskTableKey->keyStrSize + pDiskTableKey->valueSize;
			unsigned short tail = pElement[l]->keyOffset + allSize;
			if (tail != nextOffest) {
				unsigned short move = nextOffest - tail;
				memmove((unsigned char*)pDiskTableKey + move, pDiskTableKey, allSize);
				nextOffest = pElement[l]->keyOffset = OFFSET(page, (unsigned char*)pDiskTableKey + move);

				table_SetElementKeyOffset(page, pElement[l], pElement[l]->keyOffset);
			} else {
				nextOffest = pElement[l]->keyOffset;
			}
		};

		pDiskTablePage->spaceLength += nextOffest - (pDiskTablePage->spaceAddr + pDiskTablePage->spaceLength);
		memset(POINTER(page, pDiskTablePage->spaceAddr), 0, pDiskTablePage->spaceLength);	
	}
	pDiskTablePage->delSize = 0;

	plg_assert(plg_TableCheckLength(page, pageSize));
	free(pElement);
}

/*
find table name in skip list
*/
int plg_TablePrevFindCmpFun(void* key1, unsigned int key1Len, void* key2, unsigned int Key2Len) {
	//cmp keyStr
	if (key1Len > Key2Len) {
		return 1;
	} else if (key1Len == Key2Len) {
		if (memcmp(key1, key2, key1Len) > 0) {
			return 1;
		}
	}

	return 0;
}

/*
TableFindInSkipList��һ���Ͳ�ĺ���,����table name��
����ɾ���Ͳ����е����ӵ��ȷ��.
���ҾͿ��Է������ӵ��еĵ�0��.
�ڴ�����Ҫ�����������ӵ��е�����.
find table name in skip list
*/
int plg_TableTailFindCmpFun(void* key1, unsigned int key1Len, void* key2, unsigned int Key2Len) {
	//cmp keyStr
	if (key1Len > Key2Len) {
		return 1;
	} else if (key1Len == Key2Len) {
		if (memcmp(key1, key2, key1Len) >= 0) {
			return 1;
		}
	}

	return 0;
}

unsigned int plg_TableFindWithName(void* pvTableHandle, char* key, short keyLen, void* vskipListPoint, FindCmpFun pFindCmpFun) {

	//init from pCacheHandle
	ARRAY_SKIPLISTPOINT* skipListPoint = vskipListPoint;
	PTableHandle pTableHandle = pvTableHandle;
	unsigned int pageAddr = 0;
	void* page = 0;
	PTableInFile pTableInFile;
	if (pTableHandle->pTableInFile->isSetHead) {
		pTableInFile = pTableHandle->pTableInFile;
	} else {
		pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	}
	PDiskTableElement tableElement = &pTableInFile->tableHead[SKIPLIST_MAXLEVEL - 1];
	pTableHandle->hitStamp = plg_GetCurrentSec();
	do {
		//If the next level is not equal to zero, load the next level and compare. If it is greater than or equal to, switch to the next level
		if (tableElement->nextElementPage != 0) {
			//load next page
			void* nextPage;
			if (pageAddr == tableElement->nextElementPage) {
				nextPage = page;
			} else {
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, tableElement->nextElementPage, &nextPage) == 0){
					return 0;
				}
			}

			//get keyStr
			PDiskTableElement nextItem;
			nextItem = (PDiskTableElement)POINTER(nextPage, tableElement->nextElementOffset);
			PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(nextPage, nextItem->keyOffset);
			
			if (pFindCmpFun(key, keyLen, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize)) {
				//switch
				pageAddr = tableElement->nextElementPage;
				page = nextPage;
				tableElement = nextItem;
				continue;
			}
		}

		//Put the link point of this level into the return array
		(*skipListPoint)[tableElement->currentLevel].skipListAddr = pageAddr;
		(*skipListPoint)[tableElement->currentLevel].skipListOffset = OFFSET(page, tableElement);
		(*skipListPoint)[tableElement->currentLevel].page = page;
		(*skipListPoint)[tableElement->currentLevel].pDiskTableElement = tableElement;

		//End search if current level is level 0
		if (tableElement->currentLevel == 0) {
			return 1;
		}

		//switch
		if (pageAddr == 0) {
			tableElement = &pTableInFile->tableHead[tableElement->currentLevel - 1];
		} else {
			PDiskTableElement lowElement = (PDiskTableElement)POINTER(page, tableElement->lowElementOffset);
			tableElement = lowElement;
		}
		
	} while (1);
}
/*
To create a page, you must synchronize the operation and get the page number
The page number is the basis for the next step
This leads to the deletion of cross page number if rollback occurs
Otherwise, memory leak will occur. Therefore, the page created by the current transaction will be recorded
If the created page is not submitted successfully, submitting the page modification data will result in serious page address corruption
But the creation succeeds, but the page data is not modified, only the page address is damaged
*/
static unsigned int table_FindOrNewPage(void* pvTableHandle, unsigned short requireLegth, void** page) {

	//Special handling if tableusingpage is zero
	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile;
	if (pTableHandle->pTableInFile->isSetHead) {
		pTableInFile = pTableHandle->pTableInFile;
	} else {
		pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	}
	unsigned int* nextPageAddr = &pTableInFile->tableUsingPage;
	void* usingPage;
	unsigned int prevPage = 0;
	PDiskPageHead pUsingPageHead = 0;
	PDiskTableUsingPage pDiskTableUsingPage = 0;
	int emptySlot = -1;

	//Loop the using page to find the appropriate using. If not, create a using. Then create a table
	do {
		if (*nextPageAddr == 0) {

			//Changed up to 3 using pages
			if (pTableHandle->pTableHandleCallBack->createPage(pTableHandle, &usingPage, TABLEUSING) == 0) {
				return 0;
			}
			pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);

			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->addr, usingPage);
			pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
			pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
			pDiskTableUsingPage->usingPageSize = (FULLSIZE(pTableHandle->pageSize) - sizeof(DiskPageHead) - sizeof(DiskTableUsingPage)) / sizeof(DiskTableUsing);
			pDiskTableUsingPage->allSpace = 0;
			emptySlot = 0;

			pUsingPageHead->prevPage = prevPage;
			*nextPageAddr = pUsingPageHead->addr;
			

		} else {
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, *nextPageAddr, &usingPage) == 0) {
				return 0;
			}
			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, *nextPageAddr, usingPage);

			//find in page
			pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
			pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));

			//The remaining pages and blank pages of the current page can not meet the needs of skipping
			int noTry = 1;
			if (pDiskTableUsingPage->usingPageLength < pDiskTableUsingPage->usingPageSize) {
				noTry = 0;
			} else if (requireLegth < pDiskTableUsingPage->allSpace) {
				double r = 1 - (requireLegth / pDiskTableUsingPage->allSpace);
				if ((rand() % 100) <= (r * 100)){
					noTry = 0;
				}
			}

			if (noTry) {
				PDiskPageHead pDiskPageHead = (PDiskPageHead)usingPage;
				nextPageAddr = &pDiskPageHead->nextPage;
				prevPage = pDiskPageHead->addr;
				continue;
			}

			//All items in the loop using page find blank slots
			unsigned int cur = 0, count = 0;
			do {

				if (count >= pDiskTableUsingPage->usingPageLength) {
					break;
				}

				if (pDiskTableUsingPage->element[cur].pageAddr == 0) {
					if (emptySlot == -1) {
						emptySlot = cur;
					}
					cur += 1;
					continue;
				}

				//Return table page if the requirements are met
				if (pDiskTableUsingPage->element[cur].usingSpaceLength >= requireLegth) {
				
					int r = pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTableUsingPage->element[cur].pageAddr, page);
					*page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTableUsingPage->element[cur].pageAddr, *page);

					PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)*page + sizeof(DiskPageHead));
					NOTUSED(pDiskTablePage);
					plg_assert(pDiskTableUsingPage->element[cur].usingSpaceLength == pDiskTablePage->spaceLength);
					plg_assert(pDiskTablePage->spaceLength >= requireLegth);
					return r;
				}

				cur += 1;
				count += 1;

			} while (1);
		}

		//no find slot
		if (pDiskTableUsingPage->usingPageLength < pDiskTableUsingPage->usingPageSize && emptySlot == -1) {
			emptySlot = pDiskTableUsingPage->usingPageLength;
		}

		//to create table page, no find to next using page
		if (emptySlot != -1) {

			//prev create
			unsigned int tableNextPageAddr;

			PTableInFile pTableInFile;
			if (pTableHandle->pTableInFile->isSetHead) {
				pTableInFile = pTableHandle->pTableInFile;
			} else {
				pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
			}
			tableNextPageAddr = pTableInFile->tablePageHead;
			pTableHandle->hitStamp = plg_GetCurrentSec();
			
			//Create table page up to 2 table pages have been changed
			if (pTableHandle->pTableHandleCallBack->createPage(pTableHandle, page, TABLEPAGE) == 0) {
				return 0;
			} else {
				PDiskPageHead pDiskPageHead = (PDiskPageHead)*page;

				//init table page
				pDiskPageHead = (PDiskPageHead)*page;
				PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)*page + sizeof(DiskPageHead));

				pTableInFile->tablePageHead = pDiskPageHead->addr;
				pDiskPageHead->nextPage = tableNextPageAddr;
				pDiskPageHead->prevPage = 0;

				if (tableNextPageAddr != 0) {
					void* nextPage;
					if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, tableNextPageAddr, &nextPage) == 0) {
						return 0;
					}
					nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, tableNextPageAddr, nextPage);

					PDiskPageHead pDiskNextPageHead = (PDiskPageHead)nextPage;
					pDiskNextPageHead->prevPage = pDiskPageHead->addr;
					pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, tableNextPageAddr);
				}

				pDiskTablePage->usingPageAddr = pUsingPageHead->addr;
				pDiskTablePage->usingPageOffset = OFFSET(usingPage, &pDiskTableUsingPage->element[emptySlot]);
				pDiskTablePage->spaceAddr = OFFSET(*page, (unsigned char*)pDiskTablePage + sizeof(DiskTablePage));
				pDiskTablePage->spaceLength = FULLSIZE(pTableHandle->pageSize) - pDiskTablePage->spaceAddr;

				//write to using page
				pDiskTableUsingPage->element[emptySlot].pageAddr = pDiskPageHead->addr;
				pDiskTableUsingPage->element[emptySlot].usingSpaceLength = pDiskTablePage->spaceLength;
				pDiskTableUsingPage->usingPageLength += 1;
				pDiskTableUsingPage->allSpace += pDiskTableUsingPage->element[emptySlot].usingSpaceLength;

				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pUsingPageHead->addr);
				*page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->addr, *page);

				plg_assert(plg_TableCheckLength(*page, pTableHandle->pageSize));
				return 1;
			}
		}

		PDiskPageHead pDiskPageHead = (PDiskPageHead)usingPage;
		nextPageAddr = &pDiskPageHead->nextPage;
		prevPage = pDiskPageHead->addr;
		continue;

	} while (1);
}

unsigned short plg_TableGetTableType(void* pvTableHandle) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile;

	pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	return pTableInFile->tableType;
}

unsigned short plg_TableSetTableType(void* pvTableHandle, unsigned short tableType) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile;

	pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	unsigned short oldType = pTableInFile->tableType;
	pTableInFile->tableType = tableType;
	return oldType;
}

unsigned short plg_TableSetTableTypeIfByte(void* pvTableHandle, unsigned short tableType) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile;
	pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);

	unsigned short oldType = pTableInFile->tableType;
	if (oldType == TT_Byte || oldType == tableType) {
		pTableInFile->tableType = tableType;
		return tableType;
	} else {
		return oldType;
	}
}

/*
Inverse function of table_GetTablePage Up to 6 pages
The deleted page must be modified with the data of other pages
Therefore, it shall be submitted to the document together with the modified page
If you delete a page without mentioning the relevant modification page, it will result in serious page address corruption
Deleting data without deleting a page will only result in page leaks
*/
static unsigned int table_DelPage(void* pvTableHandle, unsigned int pageAddr) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile;
	if (pTableHandle->pTableInFile->isSetHead) {
		pTableInFile = pTableHandle->pTableInFile;
	} else {
		pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	}
	//find page
	void* page;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pageAddr, &page) == 0){
		return 0;
	}
		
	page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pageAddr, page);

	PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)page);
	PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)page + sizeof(DiskPageHead));

	plg_assert(plg_TableCheckLength(page, pTableHandle->pageSize));

	void* usingPage;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTablePage->usingPageAddr, &usingPage) == 0){
		return 0;
	}
		
	usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTablePage->usingPageAddr, usingPage);

	//init point
	PDiskPageHead pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
	PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
	PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskTablePage->usingPageOffset);

	//clear in using page
	pDiskTableUsingPage->allSpace -= pDiskTableUsing->usingSpaceLength;
	pDiskTableUsing->pageAddr = 0;
	pDiskTableUsing->usingSpaceLength = 0;
	pDiskTableUsingPage->usingPageLength -= 1;

	//del using page
	if (pDiskTableUsingPage->usingPageLength == 0) {
		if (pUsingPageHead->prevPage != 0) {
			void* prevUsingPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pUsingPageHead->prevPage, &prevUsingPage) == 0){
				return 0;
			}
				
			prevUsingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->prevPage, prevUsingPage);

			PDiskPageHead pPrevUsingPageHead = (PDiskPageHead)((unsigned char*)prevUsingPage);		
			pPrevUsingPageHead->nextPage = pUsingPageHead->nextPage;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pPrevUsingPageHead->addr);

			if (pUsingPageHead->nextPage != 0) {
				void* nextUsingPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pUsingPageHead->nextPage, &nextUsingPage) == 0){
					return 0;
				}
					
				nextUsingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->nextPage, nextUsingPage);

				PDiskPageHead pNextUsingPageHead = (PDiskPageHead)(nextUsingPage);
				pNextUsingPageHead->prevPage = pPrevUsingPageHead->addr;
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextUsingPageHead->addr);
			}
			
			pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pUsingPageHead->addr);

		} else {

			pTableInFile->tableUsingPage = pUsingPageHead->nextPage;

			if (pUsingPageHead->nextPage != 0) {
				void* nextUsingPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pUsingPageHead->nextPage, &nextUsingPage) == 0){
					return 0;
				}
					
				nextUsingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->nextPage, nextUsingPage);

				PDiskPageHead pNextUsingPageHead = (PDiskPageHead)(nextUsingPage);
				pNextUsingPageHead->prevPage = 0;
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextUsingPageHead->addr);
			}
			
			pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pUsingPageHead->addr);
		}
	}

	//del table page
	if (pDiskPageHead->prevPage != 0) {

		void* prevPage;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskPageHead->prevPage, &prevPage) == 0){
			return 0;
		}
			
		prevPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->prevPage, prevPage);

		PDiskPageHead pPrevPageHead = (PDiskPageHead)prevPage;
		pPrevPageHead->nextPage = pDiskPageHead->nextPage;
		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pPrevPageHead->addr);

		//seting next page
		if (pDiskPageHead->nextPage != 0) {
			void* nextPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskPageHead->nextPage, &nextPage) == 0)
				return 0;
			nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->nextPage, nextPage);

			PDiskPageHead pNextPageHead = (PDiskPageHead)nextPage;
			pNextPageHead->prevPage = pPrevPageHead->addr;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextPageHead->addr);
		}

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pageAddr);
	} else {

		pTableInFile->tablePageHead = pDiskPageHead->nextPage;

		//seting next page
		if (pDiskPageHead->nextPage != 0) {
			void* nextPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskPageHead->nextPage, &nextPage) == 0)
				return 0;
			nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->nextPage, nextPage);

			PDiskPageHead pNextPageHead = (PDiskPageHead)((unsigned char*)nextPage);
			pNextPageHead->prevPage = 0;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextPageHead->addr);
		}

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pageAddr);
	}
	return 0;
}

static void table_CheckElementNext(void* pvTableHandle, PDiskTableElement pZeroElement, short count) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int nextPage = pZeroElement->nextElementPage;
	unsigned short nextOffset = pZeroElement->nextElementOffset;
	for (int i = 0; i < count; i++) {
		if (nextPage == 0) {
			return;
		}
		void* page;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPage, &page) == 0) {
			return;
		}

		PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(page, nextOffset);
		nextPage = pDiskTableElement->nextElementPage;
		nextOffset = pDiskTableElement->nextElementOffset;
	}
}

static void table_CheckElementPrev(void* pvTableHandle, PDiskTableElement pZeroElement, short count) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int prevPage = pZeroElement->nextElementPage;
	unsigned short prevOffset = pZeroElement->nextElementOffset;
	PDiskTableKey pDiskTableKey;
	for (int i = 0; i < count; i++) {

		if (prevPage == 0) {
			return;
		}
		void* page;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, prevPage, &page) == 0) {
			return;
		}

		PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(page, prevOffset);
		pDiskTableKey = (PDiskTableKey)POINTER(page, pDiskTableElement->keyOffset);
		prevPage = pDiskTableKey->prevElementPage;
		prevOffset = pDiskTableKey->prevElementOffset;
	}
}

int table_CheckElement(void* pvTableHandle, void* page, void* pvZeroElement) {
	PDiskTableElement pZeroElement = pvZeroElement;

	int r = 1;
	return r;
	if (!page) {
		return r;
	}

	PDiskTableElement pHighElement = pZeroElement;
	while (1) {
		if (!pHighElement->keyOffset) {
			r = 0;
		}
		table_CheckElementPrev(pvTableHandle, pHighElement, 5);
		table_CheckElementNext(pvTableHandle, pHighElement, 6);

		if (pHighElement->highElementOffset == 0) {
			break;
		}
		pHighElement = (PDiskTableElement)POINTER(page, pHighElement->highElementOffset);

	}

	while (1) {
		if (!pHighElement->keyOffset) {
			r = 0;
		}
		if (pHighElement->lowElementOffset == 0) {
			break;
		}
		pHighElement = (PDiskTableElement)POINTER(page, pHighElement->lowElementOffset);
	}

	return r;
}

/*
if find alter, if no find new.
1, find in skip list to find connect point.
2, find table page in using for write skip list.
if find return table page, if no find and using page is zero,
create using page and create table page.
3, create skip list in table page
If you create up to 6 new ones, at least 4 new ones
If no more than 2 are created
*/
static unsigned int table_InsideNew(void* pvTableHandle, char* key, unsigned short keySize, char valueType, void* value, unsigned short length, void* vskipListPoint) {

	//variable
	ARRAY_SKIPLISTPOINT* skipListPoint = vskipListPoint;
	void* tablePage;
	PTableHandle pTableHandle = pvTableHandle;
	unsigned short level = plg_RandomLevel();
	unsigned short kvLength = sizeof(DiskTableKey) + keySize + length;
	unsigned short requireLength = sizeof(DiskTableElement) * level + kvLength;

	//get table page can put requireLength
	if (table_FindOrNewPage(pTableHandle, requireLength, &tablePage) == 0) {
		return 0;
	}

	//write to table page	
	PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)tablePage);
	PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)tablePage + sizeof(DiskPageHead));

	plg_assert(pDiskTablePage->spaceLength >= requireLength);
	plg_assert(plg_TableCheckLength(tablePage, pTableHandle->pageSize));
	plg_assert(plg_TableCheckSpace(tablePage));
	
	//write pDiskTableKey
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(tablePage, pDiskTablePage->spaceAddr + pDiskTablePage->spaceLength - kvLength);
	pDiskTableKey->prevElementPage = (*skipListPoint)[0].skipListAddr;
	pDiskTableKey->prevElementOffset = (*skipListPoint)[0].skipListOffset;
	pDiskTableKey->valueType = valueType;
	pDiskTableKey->keyStrSize = keySize;
	pDiskTableKey->valueSize = length;
	void* valuePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;

	//Contains \0, so you don't need to copy it when you use it.
	memcpy(pDiskTableKey->keyStr, key, keySize);

	//alloce key and value
	if (value != NULL) {
		memcpy(valuePtr, value, length);
	}

	pDiskTablePage->spaceLength -= kvLength;
	pDiskTablePage->usingLength += kvLength;

	//write PDiskTableElement; loop level repair link
	unsigned short curLevel = level - 1;
	short prevItem = -1;
	if (pDiskTablePage->tableLength < pDiskTablePage->tableSize) {

		//loop evement
		for (unsigned short l = 0; l < pDiskTablePage->tableSize; l++) {
			if (pDiskTablePage->element[l].keyOffset != 0) {
				continue;
			}

			pDiskTablePage->tableLength += 1;
			pDiskTablePage->usingLength += sizeof(DiskTableElement);

			//If you fail to find the link point in front of you, you will fail
			if ((*skipListPoint)[curLevel].skipListAddr) {

				//Note! Pagecopyonwrite returns different results under different files
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, (*skipListPoint)[curLevel].skipListAddr);
				void* page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, (*skipListPoint)[curLevel].skipListAddr, (*skipListPoint)[curLevel].page);
				if (page != (*skipListPoint)[curLevel].page) {
					(*skipListPoint)[curLevel].page = page;
					(*skipListPoint)[curLevel].pDiskTableElement = (PDiskTableElement)POINTER(page, (*skipListPoint)[curLevel].skipListOffset);
				}
			} else {
				PTableInFile pTableInFile;
				if (pTableHandle->pTableInFile->isSetHead) {
					pTableInFile = pTableHandle->pTableInFile;
				} else {
					pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
				}
				(*skipListPoint)[curLevel].pDiskTableElement = &pTableInFile->tableHead[curLevel];
				pTableHandle->hitStamp = plg_GetCurrentSec();

				if (!pTableHandle->pTableInFile->isSetHead) {
					pTableHandle->pTableHandleCallBack->addDirtyTable(pTableHandle, pTableHandle->nameaTable);
				}
			}
			
			PDiskTableElement pPrevDiskTablePageElement = (*skipListPoint)[curLevel].pDiskTableElement;
			pDiskTablePage->element[l].nextElementPage = pPrevDiskTablePageElement->nextElementPage;
			pDiskTablePage->element[l].nextElementOffset = pPrevDiskTablePageElement->nextElementOffset;
			pPrevDiskTablePageElement->nextElementPage = pDiskPageHead->addr;
			pPrevDiskTablePageElement->nextElementOffset = OFFSET(tablePage, &pDiskTablePage->element[l]);

			//set nextItem
			if (curLevel == 0 && pDiskTablePage->element[l].nextElementPage) {
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskTablePage->element[l].nextElementPage);
				void* nextElementPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTablePage->element[l].nextElementPage, &nextElementPage) == 0)
					return 0;
				void* page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTablePage->element[l].nextElementPage, nextElementPage);

				PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(page, pDiskTablePage->element[l].nextElementOffset);
				PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(page, pDiskTableElement->keyOffset);
				pDiskTableKey->prevElementPage = pDiskPageHead->addr;
				pDiskTableKey->prevElementOffset = OFFSET(tablePage, &pDiskTablePage->element[l]);
			}

			////Set lowElement and highElement
			if (prevItem != -1) {
				pDiskTablePage->element[prevItem].lowElementOffset = OFFSET(tablePage, &pDiskTablePage->element[l]);
				pDiskTablePage->element[l].highElementOffset = OFFSET(tablePage, &pDiskTablePage->element[prevItem]);
			}

			//set current element
			pDiskTablePage->element[l].currentLevel = curLevel;
			pDiskTablePage->element[l].keyOffset = OFFSET(tablePage, pDiskTableKey);

			//next loop
			prevItem = l;
			//all complete
			if (curLevel == 0) {

				//To update TableUsing
				void* usingPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTablePage->usingPageAddr, &usingPage) == 0) {
					plg_assert(0);
					return 0;
				}
				usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTablePage->usingPageAddr, usingPage);
				PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
				PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskTablePage->usingPageOffset);

				pDiskTableUsingPage->allSpace += (int)pDiskTablePage->spaceLength - pDiskTableUsing->usingSpaceLength;
				pDiskTableUsing->usingSpaceLength = pDiskTablePage->spaceLength;
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskTablePage->usingPageAddr);

				plg_assert(table_CheckElement(pvTableHandle, tablePage, &pDiskTablePage->element[l]));
				plg_assert(plg_TableCheckSpace(tablePage));
				plg_assert(plg_TableCheckLength(tablePage, pTableHandle->pageSize));
				return 1;
			}

			curLevel -= 1;
			if (pDiskTablePage->tableLength == pDiskTablePage->tableSize) {
				break;
			}
		}
	}

	//loop remains curLevel
	unsigned short end = pDiskTablePage->tableSize + curLevel + 1;
	for (unsigned short l = pDiskTablePage->tableSize; l < end; l++) {

		plg_assert((int)pDiskTablePage->spaceLength >= sizeof(DiskTableElement));
		plg_assert((int)pDiskTablePage->usingLength + sizeof(DiskTableElement) <= 65536);

		pDiskTablePage->tableSize += 1;
		pDiskTablePage->tableLength += 1;
		pDiskTablePage->spaceAddr += sizeof(DiskTableElement);
		pDiskTablePage->spaceLength -= sizeof(DiskTableElement);
		pDiskTablePage->usingLength += sizeof(DiskTableElement);

		//If you fail to find the link point in front of you, you will fail
		if ((*skipListPoint)[curLevel].skipListAddr) {
			void* page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, (*skipListPoint)[curLevel].skipListAddr, (*skipListPoint)[curLevel].page);
			if (page != (*skipListPoint)[curLevel].page) {
				(*skipListPoint)[curLevel].pDiskTableElement = (PDiskTableElement)POINTER(page, (*skipListPoint)[curLevel].skipListOffset);
			}
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, (*skipListPoint)[curLevel].skipListAddr);
		} else {
			PTableInFile pTableInFile;
			if (pTableHandle->pTableInFile->isSetHead) {
				pTableInFile = pTableHandle->pTableInFile;
			} else {
				pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
			}
			(*skipListPoint)[curLevel].pDiskTableElement = &pTableInFile->tableHead[curLevel];
			pTableHandle->hitStamp = plg_GetCurrentSec();
			if (!pTableHandle->pTableInFile->isSetHead) {
				pTableHandle->pTableHandleCallBack->addDirtyTable(pTableHandle, pTableHandle->nameaTable);
			}
		}

		PDiskTableElement pPrevDiskTablePageElement = (*skipListPoint)[curLevel].pDiskTableElement;
		pDiskTablePage->element[l].nextElementPage = pPrevDiskTablePageElement->nextElementPage;
		pDiskTablePage->element[l].nextElementOffset = pPrevDiskTablePageElement->nextElementOffset;
		pPrevDiskTablePageElement->nextElementPage = pDiskPageHead->addr;
		pPrevDiskTablePageElement->nextElementOffset = OFFSET(tablePage, &pDiskTablePage->element[l]);

		//set nextItem
		if (curLevel == 0 && pDiskTablePage->element[l].nextElementPage) {
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskTablePage->element[l].nextElementPage);
			void* nextElementPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTablePage->element[l].nextElementPage, &nextElementPage) == 0) {
				return 0;
			}
				
			void* page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTablePage->element[l].nextElementPage, nextElementPage);

			PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(page, pDiskTablePage->element[l].nextElementOffset);
			PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(page, pDiskTableElement->keyOffset);
			pDiskTableKey->prevElementPage = pDiskPageHead->addr;
			pDiskTableKey->prevElementOffset = OFFSET(tablePage, &pDiskTablePage->element[l]);
		}

		//Set lowElement and highElement
		if (prevItem != -1) {
			pDiskTablePage->element[prevItem].lowElementOffset = OFFSET(tablePage, &pDiskTablePage->element[l]);
			pDiskTablePage->element[l].highElementOffset = OFFSET(tablePage, &pDiskTablePage->element[prevItem]);
		}

		//set current element
		pDiskTablePage->element[l].currentLevel = curLevel;
		pDiskTablePage->element[l].keyOffset = OFFSET(tablePage, pDiskTableKey);

		//next loop
		prevItem = l;

		if (curLevel == 0) {
			plg_assert(table_CheckElement(pvTableHandle, tablePage, &pDiskTablePage->element[l]));
			plg_assert(plg_TableCheckSpace(tablePage));
			plg_assert(plg_TableCheckLength(tablePage, pTableHandle->pageSize));
			break;
		}
		curLevel -= 1;
	}

	//To update TableUsing
	void* usingPage;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTablePage->usingPageAddr, &usingPage) == 0) {
		plg_assert(0);
		return 0;
	}
		
	usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTablePage->usingPageAddr, usingPage);
	PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
	PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskTablePage->usingPageOffset);

	pDiskTableUsingPage->allSpace += (int)pDiskTablePage->spaceLength - pDiskTableUsing->usingSpaceLength;
	pDiskTableUsing->usingSpaceLength = pDiskTablePage->spaceLength;
	pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskTablePage->usingPageAddr);

	plg_assert(plg_TableCheckLength(tablePage, pTableHandle->pageSize));
	plg_assert(plg_TableCheckSpace(tablePage));
	return 1;
}


typedef struct TailPoint
{
	unsigned int addr;
	unsigned short offset;
	void* page;
	unsigned short keyOffset;
}PTailPoint, TailPoint;

void* plg_TablePTableInFile(void* pvTableHandle) {

	PTableHandle pTableHandle = pvTableHandle;
	return pTableHandle->pTableInFile;
}

SDS_TYPE
static void table_CheckIterator(void* pvTableHandle, PTableIterator pDiskIterator) {

	PTableHandle pTableHandle = pvTableHandle;
	if (pDiskIterator->elementPage == 0) {
		return;
	}
	void *nextPage = 0;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskIterator->elementPage, &nextPage) == 0)
		return;

	//get PDiskTableKey
	PDiskTableElement pDiskTableElement = (PDiskTableElement)POINTER(nextPage, pDiskIterator->elementOffset);

	if (pDiskTableElement->keyOffset == 0) {
		elog(log_error, "plg_DiskCheckIterator pDiskTableElement->keyOffset!");
		return;
	}

	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(nextPage, pDiskTableElement->keyOffset);
	PDiskTableElement pHighElement = pDiskTableElement;
	int currentLevel = 0;
	do {
		if (pHighElement->currentLevel != currentLevel++) {
			sds s = plg_sdsCatPrintf(plg_sdsEmpty(), "plg_DiskCheckIterator pHighElement->currentLevel key:%s error!", pDiskTableKey->keyStr);
			elog(log_error, s);
			plg_sdsFree(s);
		} else if (pHighElement->keyOffset != pDiskTableElement->keyOffset) {
			sds s = plg_sdsCatPrintf(plg_sdsEmpty(), "plg_DiskCheckIterator pHighElement->keyOffset key:%s error!", pDiskTableKey->keyStr);
			elog(log_error, s);
			plg_sdsFree(s);
		}
		if (pHighElement->highElementOffset == 0) {
			break;
		}
		pHighElement = (PDiskTableElement)POINTER(nextPage, pHighElement->highElementOffset);
	} while (1);

	SDS_CHECK(pTableHandle->hitStamp, pDiskTableKey->keyStr);
	PDiskTableElement pLowElement = pHighElement;
	currentLevel--;
	while (1) {
		if (pLowElement->currentLevel != currentLevel--) {
			sds s = plg_sdsCatPrintf(plg_sdsEmpty(), "plg_DiskCheckIterator pLowElement->currentLevel key:%s error!", pDiskTableKey->keyStr);
			elog(log_error, s);
			plg_sdsFree(s);
		} else if (pLowElement->keyOffset != pDiskTableElement->keyOffset) {
			sds s = plg_sdsCatPrintf(plg_sdsEmpty(), "plg_DiskCheckIterator pLowElement->keyOffset key:%s error!", pDiskTableKey->keyStr);
			elog(log_error, s);
			plg_sdsFree(s);
		}
		if (pLowElement->lowElementOffset == 0) {
			break;
		}
		pLowElement = (PDiskTableElement)POINTER(nextPage, pLowElement->lowElementOffset);
	}
}

void plg_TableCheckTable(void* pvTableHandle) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
	while (plg_TableNextIterator(iter)) {
		table_CheckIterator(pTableHandle, iter);
	};
	plg_TableReleaseIterator(iter);
}

unsigned int plg_TableIteratorAddr(void* pvTableIterator){
	PTableIterator pTableIterator = pvTableIterator;
	return pTableIterator->elementPage;
}

unsigned short plg_TableIteratorOffset(void* pvTableIterator){
	PTableIterator pTableIterator = pvTableIterator;
	return pTableIterator->elementOffset;
}

unsigned int plg_TableLength(void* pvTableHandle) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int count = 0;
	PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
	while (plg_TableNextIterator(iter)) {
		count++;
	};
	plg_TableReleaseIterator(iter);

	return count;
}

void plg_TableResetHandle(void* pvTableHandle, void* pTableInFile, sds tableName) {
	PTableHandle pTableHandle = pvTableHandle;
	pTableHandle->nameaTable = tableName;
	pTableHandle->pTableInFile = pTableInFile;
}

static unsigned int table_CreateValuePage(void* pvTableHandle, PTableInFile pTableInFile, void** page, unsigned int emptySlot, void* usingPage, PDiskPageHead pUsingPageHead, PDiskTableUsingPage pDiskTableUsingPage) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int tableNextPageAddr;
	tableNextPageAddr = pTableInFile->valuePage;
	pTableHandle->hitStamp = plg_GetCurrentSec();
	
	//create table page Changed up to 3 value pages
	if (pTableHandle->pTableHandleCallBack->createPage(pTableHandle, page, VALUEPAGE) == 0) {
		return 0;
	} else {
		PDiskPageHead pDiskPageHead = (PDiskPageHead)*page;
		*page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->addr, *page);

		elog(log_details, "table_CreateValuePage.createPage:%i", pDiskPageHead->addr);
		//init table page
		pDiskPageHead = (PDiskPageHead)*page;
		PDiskValuePage pDiskValuePage = (PDiskValuePage)((unsigned char*)*page + sizeof(DiskPageHead));

		pDiskPageHead->nextPage = tableNextPageAddr;
		pTableInFile->valuePage = pDiskPageHead->addr;
		pDiskPageHead->prevPage = 0;

		if (tableNextPageAddr != 0) {
			void* nextPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, tableNextPageAddr, &nextPage) == 0) {
				return 0;
			}
			nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, tableNextPageAddr, nextPage);

			PDiskPageHead pDiskNextPageHead = (PDiskPageHead)nextPage;
			pDiskNextPageHead->prevPage = pDiskPageHead->addr;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, tableNextPageAddr);
		}

		pDiskValuePage->valueUsingPageAddr = pUsingPageHead->addr;
		pDiskValuePage->valueUsingPageOffset = OFFSET(usingPage, &pDiskTableUsingPage->element[emptySlot]);
		pDiskValuePage->valueSpaceAddr = OFFSET(*page, (unsigned char*)pDiskValuePage + sizeof(DiskTablePage));
		pDiskValuePage->valueSpaceLength = FULLSIZE(pTableHandle->pageSize) - pDiskValuePage->valueSpaceAddr;

		//write to using page
		pDiskTableUsingPage->element[emptySlot].pageAddr = pDiskPageHead->addr;
		pDiskTableUsingPage->element[emptySlot].usingSpaceLength = pDiskValuePage->valueSpaceLength;
		pDiskTableUsingPage->usingPageLength += 1;
		pDiskTableUsingPage->allSpace += pDiskTableUsingPage->element[emptySlot].usingSpaceLength;

		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pUsingPageHead->addr);
		return 1;
	}
}

static unsigned int table_ValueFindOrNewPage(void* pvTableHandle, unsigned short requireLegth, void** page) {

	//Special handling if tableusingpage is zero
	PTableInFile pTableInFile;
	PTableHandle pTableHandle = pvTableHandle;
	if (pTableHandle->pTableInFile->isSetHead) {
		pTableInFile = pTableHandle->pTableInFile;
	} else {
		pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	}
	unsigned int* nextPageAddr = &pTableInFile->valueUsingPage;
	void* usingPage;
	unsigned int prevPage = 0;
	PDiskPageHead pUsingPageHead = 0;
	PDiskTableUsingPage pDiskTableUsingPage = 0;
	int emptySlot = -1;

	//Loop the using page to find the appropriate using. If not, create a using. Then create a value
	do {
		if (*nextPageAddr == 0) {

			//Changed up to 3 using pages
			if (pTableHandle->pTableHandleCallBack->createPage(pTableHandle, &usingPage, VALUEUSING) == 0) {
				return 0;
			}
			pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->addr, usingPage);

			pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
			pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
			pDiskTableUsingPage->usingPageSize = (FULLSIZE(pTableHandle->pageSize) - sizeof(DiskPageHead) - sizeof(DiskTableUsingPage)) / sizeof(DiskTableUsing);
			emptySlot = 0;

			pUsingPageHead->prevPage = prevPage;
			*nextPageAddr = pUsingPageHead->addr;

		} else {
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, *nextPageAddr, &usingPage) == 0) {
				return 0;
			}
			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, *nextPageAddr, usingPage);

			//find in page
			pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
			pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));

			//The remaining pages and blank pages of the current page can not meet the needs of skipping
			int noTry = 1;
			if (pDiskTableUsingPage->usingPageLength < pDiskTableUsingPage->usingPageSize) {
				noTry = 0;
			} else if (requireLegth && requireLegth < pDiskTableUsingPage->allSpace) {
				double r = 1 - (requireLegth / pDiskTableUsingPage->allSpace);
				if ((rand() % 100) <= (r * 100)){
					noTry = 0;
				}
			}

			if (noTry) {
				PDiskPageHead pDiskPageHead = (PDiskPageHead)usingPage;
				nextPageAddr = &pDiskPageHead->nextPage;
				prevPage = pDiskPageHead->addr;
				continue;
			}

			//init loop using
			unsigned int count, cur = count = 0;
			do {

				if (count >= pDiskTableUsingPage->usingPageLength) {
					break;
				}

				if (pDiskTableUsingPage->element[cur].pageAddr == 0) {
					if (emptySlot == -1) {
						emptySlot = cur;
					}
					cur += 1;
					continue;
				}

				//Return table page if the requirements are met
				if (requireLegth && pDiskTableUsingPage->element[cur].usingSpaceLength >= requireLegth) {

					int r = pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTableUsingPage->element[cur].pageAddr, page);
					*page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTableUsingPage->element[cur].pageAddr, *page);

					PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)*page + sizeof(DiskPageHead));
					NOTUSED(pDiskTablePage);
					plg_assert(pDiskTablePage->spaceLength >= requireLegth);
					return r;
				}

				count += 1;
				cur += 1;

			} while (1);
		}

		//no find slot
		if (pDiskTableUsingPage->usingPageLength < pDiskTableUsingPage->usingPageSize && emptySlot == -1) {
			emptySlot = pDiskTableUsingPage->usingPageLength;
		}

		//to create table page
		if (emptySlot != -1) {
			return table_CreateValuePage(pTableHandle, pTableInFile, page, emptySlot, usingPage, pUsingPageHead, pDiskTableUsingPage);
		}

		//next page
		PDiskPageHead pDiskPageHead = (PDiskPageHead)usingPage;
		nextPageAddr = &pDiskPageHead->nextPage;
		prevPage = pDiskPageHead->addr;

	} while (1);
}


unsigned int plg_TableNewBigValue(void* pvTableHandle, char* value, unsigned int valueLen, void* vpDiskKeyBigValue) {
	
	//Value of cycle splitting greater than page size
	PDiskKeyBigValue pDiskKeyBigValue = vpDiskKeyBigValue;
	PTableHandle pTableHandle = pvTableHandle;
	char* curPtr = value;
	unsigned int curLen = valueLen;
	unsigned int savaSize = FULLSIZE(pTableHandle->pageSize) - (sizeof(DiskPageHead) + sizeof(DiskValuePage) + sizeof(DiskValueElement) + sizeof(DiskBigValue));
	PDiskValueElement prevValueElement = 0;
	pDiskKeyBigValue->valuePageAddr = 0;
	pDiskKeyBigValue->valueOffset = 0;
	pDiskKeyBigValue->crc = plg_crc16(value, valueLen);
	pDiskKeyBigValue->allSize = valueLen;

	do {
		if (curLen > savaSize) {
			void* valuePage = 0;
			if (0 == table_ValueFindOrNewPage(pTableHandle, 0, &valuePage))
				return 0;

			//write to value page
			PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)valuePage);
			PDiskValuePage pDiskValuePage = (PDiskValuePage)((unsigned char*)valuePage + sizeof(DiskPageHead));
			PDiskBigValue valuePtr = (PDiskBigValue)((unsigned char*)pDiskValuePage->valueElement + sizeof(DiskValueElement));
			
			valuePtr->valueSize = savaSize;
			memcpy(valuePtr->valueBuff, curPtr, savaSize);
			pDiskValuePage->valueElement[0].valueOffset = OFFSET(valuePage, valuePtr);
			pDiskValuePage->valueElement[0].nextElementPage = 0;
			pDiskValuePage->valueElement[0].nextElementOffset = 0;

			curLen -= savaSize;
			curPtr += savaSize;

			if (pDiskKeyBigValue->valuePageAddr == 0) {
				pDiskKeyBigValue->valuePageAddr = pDiskPageHead->addr;
				pDiskKeyBigValue->valueOffset = OFFSET(valuePage, &pDiskValuePage->valueElement[0]);
			}

			if (prevValueElement != 0) {
				prevValueElement->nextElementPage = pDiskPageHead->addr;
				prevValueElement->nextElementOffset = OFFSET(valuePage, &pDiskValuePage->valueElement[0]);
			}
			prevValueElement = &pDiskValuePage->valueElement[0];

			pDiskValuePage->valueSpaceAddr = OFFSET(valuePage, valuePtr);
			pDiskValuePage->valueLength = 1;
			pDiskValuePage->valueSize = 1;
			pDiskValuePage->valueSpaceLength = 0;
			pDiskValuePage->valueUsingLength += sizeof(DiskValueElement) + sizeof(DiskBigValue) + savaSize;

			//alter using page
			void* usingPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskValuePage->valueUsingPageAddr, &usingPage) == 0)
				return 0;

			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskValuePage->valueUsingPageAddr, usingPage);
			PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskValuePage->valueUsingPageOffset);
			PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
			
			pDiskTableUsingPage->allSpace += (int)pDiskTableUsing->usingSpaceLength - (int)pDiskValuePage->valueSpaceLength;
			pDiskTableUsing->usingSpaceLength = pDiskValuePage->valueSpaceLength;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskValuePage->valueUsingPageAddr);
		} else {
			break;
		}
	} while (1);

	if (curLen > 0) {

		unsigned short elementValueLength = curLen + sizeof(DiskValueElement);
		void* valuePage;
		if (0 == table_ValueFindOrNewPage(pTableHandle, elementValueLength, &valuePage))
			return 0;

		//write to value page
		PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)valuePage);
		PDiskValuePage pDiskValuePage = (PDiskValuePage)((unsigned char*)valuePage + sizeof(DiskPageHead));

		PDiskBigValue valuePtr = (PDiskBigValue)POINTER(valuePage, pDiskValuePage->valueSpaceAddr + pDiskValuePage->valueSpaceLength) - (sizeof(DiskBigValue) + curLen);
		memcpy(valuePtr->valueBuff, curPtr, curLen);
		valuePtr->valueSize = curLen;

		short emptySlot = -1;
		unsigned short cur = 0;
		do {
			if (cur > pDiskValuePage->valueLength) {
				emptySlot = pDiskValuePage->valueLength;
				pDiskValuePage->valueLength += 1;
				break;
			}

			if (pDiskValuePage->valueElement[cur].valueOffset != 0) {
				cur += 1;
				continue;
			}

			emptySlot = cur;
			break;
		} while (1);

		if (pDiskKeyBigValue->valuePageAddr == 0) {
			pDiskKeyBigValue->valuePageAddr = pDiskPageHead->addr;
			pDiskKeyBigValue->valueOffset = OFFSET(valuePage, &pDiskValuePage->valueElement[emptySlot]);
		}

		pDiskValuePage->valueElement[emptySlot].valueOffset = OFFSET(valuePage, valuePtr);
		pDiskValuePage->valueElement[emptySlot].nextElementPage = 0;
		pDiskValuePage->valueElement[emptySlot].nextElementOffset = 0;

		if (prevValueElement != 0) {
			prevValueElement->nextElementPage = pDiskPageHead->addr;
			prevValueElement->nextElementOffset = OFFSET(valuePage, &pDiskValuePage->valueElement[emptySlot]);
		}

		pDiskValuePage->valueSpaceAddr += sizeof(DiskValueElement);
		pDiskValuePage->valueSpaceLength -= curLen + sizeof(DiskBigValue);
		pDiskValuePage->valueUsingLength += sizeof(DiskValueElement) + curLen + sizeof(DiskBigValue);
		pDiskValuePage->valueLength += 1;
		pDiskValuePage->valueSize += 1;

		//alter using page
		void* usingPage;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskValuePage->valueUsingPageAddr, &usingPage) == 0)
			return 0;

		usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskValuePage->valueUsingPageAddr, usingPage);
		PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskValuePage->valueUsingPageOffset);
		PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
		
		pDiskTableUsingPage->allSpace += (int)pDiskTableUsing->usingSpaceLength - (int)pDiskValuePage->valueSpaceLength;
		pDiskTableUsing->usingSpaceLength = pDiskValuePage->valueSpaceLength;
		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskValuePage->valueUsingPageAddr);
	}

	return 1;
}

static unsigned int table_DelValuePage(void* pvTableHandle, unsigned int pageAddr) {

	elog(log_fun, "table_DelValuePage.pageAddr:%i", pageAddr);
	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile;
	if (pTableHandle->pTableInFile->isSetHead) {
		pTableInFile = pTableHandle->pTableInFile;
	} else {
		pTableInFile = pTableHandle->pTableHandleCallBack->findTableInFile(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	}
	//find page
	void* page;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pageAddr, &page) == 0)
		return 0;
	page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pageAddr, page);

	PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)page);
	PDiskValuePage pDiskValuePage = (PDiskValuePage)((unsigned char*)page + sizeof(DiskPageHead));

	void* usingPage;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskValuePage->valueUsingPageAddr, &usingPage) == 0)
		return 0;
	usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskValuePage->valueUsingPageAddr, usingPage);

	//init point
	PDiskPageHead pUsingPageHead = (PDiskPageHead)((unsigned char*)usingPage);
	PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
	PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskValuePage->valueUsingPageOffset);

	//clear in using page
	pDiskTableUsingPage->allSpace -= pDiskTableUsing->usingSpaceLength;
	pDiskTableUsing->pageAddr = 0;
	pDiskTableUsing->usingSpaceLength = 0;
	pDiskTableUsingPage->usingPageLength -= 1;

	//del using page
	if (pDiskTableUsingPage->usingPageLength == 0) {
		if (pUsingPageHead->prevPage != 0) {
			void* prevUsingPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pUsingPageHead->prevPage, &prevUsingPage) == 0)
				return 0;
			prevUsingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->prevPage, prevUsingPage);

			PDiskPageHead pPrevUsingPageHead = (PDiskPageHead)((unsigned char*)prevUsingPage);
			pPrevUsingPageHead->nextPage = pUsingPageHead->nextPage;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pPrevUsingPageHead->addr);

			if (pUsingPageHead->nextPage != 0) {
				void* nextUsingPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pUsingPageHead->nextPage, &nextUsingPage) == 0)
					return 0;
				nextUsingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->nextPage, nextUsingPage);

				PDiskPageHead pNextUsingPageHead = (PDiskPageHead)(nextUsingPage);
				pNextUsingPageHead->prevPage = pPrevUsingPageHead->addr;
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextUsingPageHead->addr);
			}

			pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pUsingPageHead->addr);

		} else {

			pTableInFile->valueUsingPage = pUsingPageHead->nextPage;

			if (pUsingPageHead->nextPage != 0) {
				void* nextUsingPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pUsingPageHead->nextPage, &nextUsingPage) == 0)
					return 0;
				nextUsingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pUsingPageHead->nextPage, nextUsingPage);

				PDiskPageHead pNextUsingPageHead = (PDiskPageHead)(nextUsingPage);
				pNextUsingPageHead->prevPage = 0;
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextUsingPageHead->addr);
			}

			pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pUsingPageHead->addr);
		}
	}

	//del table page
	if (pDiskPageHead->prevPage != 0) {

		void* prevPage;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskPageHead->prevPage, &prevPage) == 0)
			return 0;
		prevPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->prevPage, prevPage);

		PDiskPageHead pPrevPageHead = (PDiskPageHead)prevPage;
		pPrevPageHead->nextPage = pDiskPageHead->nextPage;
		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pPrevPageHead->addr);

		//seting next page
		if (pDiskPageHead->nextPage != 0) {
			void* nextPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskPageHead->nextPage, &nextPage) == 0)
				return 0;
			nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->nextPage, nextPage);

			PDiskPageHead pNextPageHead = (PDiskPageHead)nextPage;
			pNextPageHead->prevPage = pPrevPageHead->addr;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextPageHead->addr);
		}

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pageAddr);
	} else {

		pTableInFile->valuePage = pDiskPageHead->nextPage;

		//seting next page
		if (pDiskPageHead->nextPage != 0) {
			void* nextPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskPageHead->nextPage, &nextPage) == 0)
				return 0;
			nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskPageHead->nextPage, nextPage);

			PDiskPageHead pNextPageHead = (PDiskPageHead)((unsigned char*)nextPage);
			pNextPageHead->prevPage = 0;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pNextPageHead->addr);
		}

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pageAddr);
	}
	return 0;
}

/*
ѭ��element ��ɾ��,���޸�using
ɾ�������pageΪ�վ�ɾ��page, ��ɾ��using, ���usingΪ����ɾ��using
*/
static int table_DelBigValue(void* pvTableHandle, PDiskKeyBigValue pDiskKeyBigValue) {

	elog(log_fun, "table_DelBigValue");
	PTableHandle pTableHandle = pvTableHandle;
	unsigned int nextPage = pDiskKeyBigValue->valuePageAddr;
	unsigned short nextOffset = pDiskKeyBigValue->valueOffset;

	do {
		if (nextPage == 0) {
			break;
		}

		void* valuePage;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPage, &valuePage) == 0)
			return 0;
		valuePage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextPage, valuePage);

		PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)valuePage);
		PDiskValuePage pDiskValuePage = (PDiskValuePage)((unsigned char*)valuePage + sizeof(DiskPageHead));
		PDiskValueElement pDiskValueElement = (PDiskValueElement)POINTER(valuePage, nextOffset);
		PDiskBigValue valuePtr = (PDiskBigValue)POINTER(valuePage, pDiskValueElement->valueOffset);
		memset(valuePtr, 0, valuePtr->valueSize);

		if (pDiskValueElement == &pDiskValuePage->valueElement[pDiskValuePage->valueLength - 1]) {
			pDiskValuePage->valueSpaceAddr += sizeof(DiskValueElement);
			pDiskValuePage->valueSpaceLength += sizeof(DiskValueElement);
		}

		if (pDiskValuePage->valueSpaceAddr + pDiskValuePage->valueSpaceLength == pDiskValueElement->valueOffset) {
			pDiskValuePage->valueSpaceLength += valuePtr->valueSize;
		} else {
			pDiskValuePage->valueDelSize += valuePtr->valueSize;
		}
		pDiskValuePage->valueLength -= 1;
		nextPage = pDiskValueElement->nextElementPage;
		nextOffset = pDiskValueElement->nextElementOffset;
		memset(pDiskValueElement, 0, sizeof(DiskValueElement));

		if (pDiskValuePage->valueLength != 0) {
			void* usingPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskValuePage->valueUsingPageAddr, &usingPage) == 0)
				return 0;

			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskValuePage->valueUsingPageAddr, usingPage);
			PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskValuePage->valueUsingPageOffset);
			PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));

			pDiskTableUsingPage->allSpace += (int)pDiskTableUsing->usingSpaceLength - (int)pDiskValuePage->valueSpaceLength;
			pDiskTableUsing->usingSpaceLength = pDiskValuePage->valueSpaceLength;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskValuePage->valueUsingPageAddr);

		} else {
			table_DelValuePage(pTableHandle, pDiskPageHead->addr);
		}
		
	} while (1);

	return 1;
}

/*
malloc and return
*/
static void* table_GetBigValue(void* pvTableHandle, PDiskKeyBigValue pDiskKeyBigValue) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int nextPage = pDiskKeyBigValue->valuePageAddr;
	unsigned short nextOffset = pDiskKeyBigValue->valueOffset;
	char* retPtr = malloc(pDiskKeyBigValue->allSize);
	unsigned int addSize = 0;

	do {
		if (nextPage == 0) {
			break;
		}

		void* valuePage;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPage, &valuePage) == 0)
			return 0;

		PDiskValueElement pDiskValueElement = (PDiskValueElement)POINTER(valuePage, nextOffset);
		PDiskBigValue valuePtr = (PDiskBigValue)POINTER(valuePage, pDiskValueElement->valueOffset);

		memcpy(retPtr + addSize, valuePtr->valueBuff, valuePtr->valueSize);
		addSize += valuePtr->valueSize;

		nextPage = pDiskValueElement->nextElementPage;
		nextOffset = pDiskValueElement->nextElementOffset;
	} while (1);

	unsigned short crc = plg_crc16(retPtr, pDiskKeyBigValue->allSize);
	if (pDiskKeyBigValue->crc == 0 || crc != pDiskKeyBigValue->crc) {
		elog(log_error, "big value crc check error !");
		return 0;
	} else {
		return retPtr;
	}	
}

void plg_TableArrangmentBigValue(unsigned int pageSize, void* page){

	elog(log_fun, "plg_TableArrangementPage");
	PDiskValuePage pDiskValuePage = (PDiskValuePage)((unsigned char*)page + sizeof(DiskPageHead));

	plg_assert(plg_TableCheckLength(page, pageSize));
	plg_assert(plg_TableCheckSpace(page));

	PDiskValueElement* pElement = calloc(sizeof(PDiskValueElement), pDiskValuePage->valueLength);

	int count = 0;
	for (unsigned short l = 0; l < pDiskValuePage->valueSize; l++) {
		PDiskValueElement pDiskValueElement = &pDiskValuePage->valueElement[l];
		if (pDiskValueElement->valueOffset != 0) {
			pElement[count++] = pDiskValueElement;
		}
	}

	if (count) {
		plg_SortArrary(pElement, sizeof(unsigned short*), pDiskValuePage->valueLength, plg_SortPDiskTableElementCmp);

		unsigned short nextOffest = FULLSIZE(pageSize);
		for (int l = 0; l < count; l++) {
			PDiskBigValue pDiskBigValue = (PDiskBigValue)POINTER(page, pElement[l]->valueOffset);
			unsigned short allSize = sizeof(DiskBigValue) + pDiskBigValue->valueSize;
			unsigned short tail = pElement[l]->valueOffset + allSize;
			if (tail != nextOffest) {
				unsigned short move = nextOffest - tail;
				memmove((unsigned char*)pDiskBigValue + move, pDiskBigValue, allSize);
				nextOffest = pElement[l]->valueOffset = OFFSET(page, (unsigned char*)pDiskBigValue + move);
			} else {
				nextOffest = pElement[l]->valueOffset;
			}
		};

		pDiskValuePage->valueSpaceAddr += nextOffest - (pDiskValuePage->valueSpaceAddr + pDiskValuePage->valueSpaceLength);
		memset(POINTER(page, pDiskValuePage->valueSpaceAddr), 0, pDiskValuePage->valueSpaceLength);
	}
	pDiskValuePage->valueDelSize = 0;

	plg_assert(plg_TableCheckLength(page, pageSize));
	free(pElement);
}

/*
�ɹ���1��ҳ��
*/
static unsigned int table_InsideAlter(void* pvTableHandle, char* key, short keyLen, ARRAY_SKIPLISTPOINT* pSkipListPoint, char valueType, void* value, unsigned short length) {

	//load table page retrun PDiskTableKey
	PTableHandle pTableHandle = pvTableHandle;
	PDiskTableElement pDiskTableElement = (*pSkipListPoint)[0].pDiskTableElement;
	if (pDiskTableElement->nextElementPage == 0) {
		return 0;
	}

	unsigned int nextElementPage = pDiskTableElement->nextElementPage;
	unsigned short nextElementOffset = pDiskTableElement->nextElementOffset;

	void* page;
	if ((*pSkipListPoint)[0].skipListAddr == nextElementPage) {
		page = (*pSkipListPoint)[0].page;
	} else {
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextElementPage, &page) == 0)
			return 0;
	}
	page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextElementPage, page);

	//get PDiskTableKey
	pDiskTableElement = (PDiskTableElement)POINTER(page, nextElementOffset);
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(page, pDiskTableElement->keyOffset);
	void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;

	if (pDiskTableKey->valueType != valueType) {
		return 0;
	}

	//check size
	if (keyLen != pDiskTableKey->keyStrSize) {
		return 0;
	}

	//check
	if (memcmp(pDiskTableKey->keyStr, key, pDiskTableKey->keyStrSize) != 0) {
		return 0;
	}

	if (pDiskTableKey->valueType == VALUE_NORMAL)  {

		if (pDiskTableKey->valueSize != length) {
			return 0;
		}

		pDiskTableKey->valueSize = length;
		memcpy(vluePtr, value, length);

		PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)page);
		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskPageHead->addr);
		return 1;
	}

	return 0;
}

static unsigned int table_InsideAlterFroSet(void* pvTableHandle, char* key, short keyLen, ARRAY_SKIPLISTPOINT* pSkipListPoint, char valueType, void* value, unsigned short length) {

	//load table page retrun PDiskTableKey
	PTableHandle pTableHandle = pvTableHandle;
	PDiskTableElement pDiskTableElement = (*pSkipListPoint)[0].pDiskTableElement;
	if (pDiskTableElement->nextElementPage == 0) {
		return 0;
	}

	unsigned int nextElementPage = pDiskTableElement->nextElementPage;
	unsigned short nextElementOffset = pDiskTableElement->nextElementOffset;

	void* page;
	if ((*pSkipListPoint)[0].skipListAddr == nextElementPage) {
		page = (*pSkipListPoint)[0].page;
	} else {
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextElementPage, &page) == 0)
			return 0;
	}
	page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextElementPage, page);

	//get PDiskTableKey
	pDiskTableElement = (PDiskTableElement)POINTER(page, nextElementOffset);
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(page, pDiskTableElement->keyOffset);
	void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;

	if (pDiskTableKey->valueType != valueType) {
		return 0;
	}

	//check size
	if (keyLen != pDiskTableKey->keyStrSize) {
		return 0;
	}

	//check
	if (memcmp(pDiskTableKey->keyStr, key, pDiskTableKey->keyStrSize) != 0) {
		return 0;
	}

	//To ensure that the set address is correct, only some properties have changed and need to be updated.
	//Because the deletion of other sets will cause table cleaning.
	if (pDiskTableKey->valueType == VALUE_SETHEAD)  {

		pDiskTableKey->valueSize = length;
		memcpy(vluePtr, value, length);

		PDiskPageHead pDiskPageHead = (PDiskPageHead)((unsigned char*)page);
		PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)page + sizeof(DiskPageHead));
		pDiskTablePage->usingLength -= (pDiskTableKey->valueSize - length);

		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskPageHead->addr);
		return 1;
	}

	return 0;
}

unsigned int plg_InsideTableAlterFroSet(void* pvTableHandle, char* key, short keyLen, char valueType, void* value, unsigned short length) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle, key, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	//add new
	return table_InsideAlterFroSet(pTableHandle, key, keyLen, &skipListPoint, valueType, value, length);
}

/*
�ɹ���1��ҳ��
*/
static unsigned int table_InsideIsKeyExist(void* pvTableHandle, void* vKey, short keyLen, ARRAY_SKIPLISTPOINT* pSkipListPoint) {

	//load table page retrun PDiskTableKey
	PTableHandle pTableHandle = pvTableHandle;
	PDiskTableElement pDiskTableElement = (*pSkipListPoint)[0].pDiskTableElement;
	if (pDiskTableElement->nextElementPage == 0) {
		return 0;
	}

	unsigned int nextElementPage = pDiskTableElement->nextElementPage;
	unsigned short nextElementOffset = pDiskTableElement->nextElementOffset;

	void* page;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextElementPage, &page) == 0)
		return 0;

	//get PDiskTableKey
	pDiskTableElement = (PDiskTableElement)POINTER(page, nextElementOffset);
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(page, pDiskTableElement->keyOffset);

	//check size
	if (keyLen != pDiskTableKey->keyStrSize) {
		return 0;
	}

	//check
	if (memcmp(pDiskTableKey->keyStr, vKey, pDiskTableKey->keyStrSize) != 0) {
		return 0;
	}

	return 1;
}

/*
If you delete and modify at most 6, at least 1
noSet:Prevent nesting caused by deleting collections
*/
static unsigned int table_InsideDel(void* pvTableHandle, char* key, unsigned short keySize, ARRAY_SKIPLISTPOINT* pSkipListPoint, unsigned int noSet) {

	//init for loop
	PTableHandle pTableHandle = pvTableHandle;
	TailPoint tailPoint[SKIPLIST_MAXLEVEL] = { { 0 } };
	int tailLevel = -1;
	unsigned int isBreak = 0;
	//del element and find tail point
	unsigned int curPageAddr = 0;
	void* curPage = 0;

	//To delete the data in the page, you also need to pagecopyonwrite the page. Is another way to write to a page.
	if ((*pSkipListPoint)[0].skipListAddr) {
		void* page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, (*pSkipListPoint)[0].skipListAddr, (*pSkipListPoint)[0].page);
		if (page != (*pSkipListPoint)[0].page) {
			(*pSkipListPoint)[0].page = page;
			(*pSkipListPoint)[0].pDiskTableElement = (PDiskTableElement)POINTER(page, (*pSkipListPoint)[0].skipListOffset);
		}
		pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, (*pSkipListPoint)[0].skipListAddr);

		curPageAddr = (*pSkipListPoint)[0].skipListAddr;
		curPage = page;
	} else {
		PTableInFile pTableInFile;
		if (pTableHandle->pTableInFile->isSetHead) {
			pTableInFile = pTableHandle->pTableInFile;
		} else {
			pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
		}
		(*pSkipListPoint)[0].pDiskTableElement = &pTableInFile->tableHead[0];
		pTableHandle->hitStamp = plg_GetCurrentSec();
		if (!pTableHandle->pTableInFile->isSetHead) {
			pTableHandle->pTableHandleCallBack->addDirtyTable(pTableHandle, pTableHandle->nameaTable);
		}
	}

	PDiskTableElement pZeroElement = (*pSkipListPoint)[0].pDiskTableElement;
	if (pZeroElement->nextElementPage == 0) {
		return 1;
	}
	unsigned int nextElementPage = pZeroElement->nextElementPage;
	unsigned short nextElementOffset = pZeroElement->nextElementOffset;
	
	do {
		if (nextElementPage == 0) {
			break;
		}

		void *nextPage = 0;
		if (curPageAddr == nextElementPage) {
			nextPage = curPage;
		} else {
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextElementPage, &nextPage) == 0) { 
				return 0;
			}
			nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextElementPage, nextPage);
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, nextElementPage);
		}
		
		//get PDiskTableKey
		PDiskTablePage pDiskTablePage = (PDiskTablePage)((unsigned char*)nextPage + sizeof(DiskPageHead));
		PDiskTableElement pNextElement = (PDiskTableElement)POINTER(nextPage, nextElementOffset);
		PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(nextPage, pNextElement->keyOffset);

		plg_assert(plg_TableCheckSpace(nextPage));
		plg_assert(plg_TableCheckLength(nextPage, pTableHandle->pageSize));
		
		unsigned short keyVlaueSize = sizeof(DiskTableKey) + pDiskTableKey->keyStrSize + pDiskTableKey->valueSize;
		void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;

		//cmp size
		if (keySize != pDiskTableKey->keyStrSize) {
			break;
		}

		//check key If the same start deletion
		if (memcmp(key, pDiskTableKey->keyStr, keySize) != 0) {
			break;
		}

		if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			table_DelBigValue(pTableHandle, pDiskKeyBigValue);
		} else if (!noSet && pDiskTableKey->valueType == VALUE_SETHEAD) {
			PTableInFile pTableInFile = (PTableInFile)vluePtr;
			PTableInFile pRecTableInFile = pTableHandle->pTableInFile;
			pTableHandle->pTableInFile = pTableInFile;
			plg_TableClear(pTableHandle, 0);
			pTableHandle->pTableInFile = pRecTableInFile;
		}

		//recycling space and del key string
		if (pNextElement->keyOffset == pDiskTablePage->spaceAddr + pDiskTablePage->spaceLength) {
			pDiskTablePage->spaceLength += keyVlaueSize;
		} else {
			pDiskTablePage->delSize += keyVlaueSize;
		}
		pDiskTablePage->usingLength -= keyVlaueSize;
		memset(pDiskTableKey, 0, keyVlaueSize);
		
		//init high loop element
		curPageAddr = nextElementPage;
		curPage = nextPage;
		unsigned short highElementOffset = nextElementOffset;
		isBreak = 1;
		
		//set next loop
		nextElementPage = pNextElement->nextElementPage;
		nextElementOffset = pNextElement->nextElementOffset;
		do {

			//zero element
			PDiskTableElement pHighElement = (PDiskTableElement)POINTER(nextPage, highElementOffset);	
			if (pHighElement->currentLevel > tailLevel) {
				tailLevel = pHighElement->currentLevel;
			}

			tailPoint[pHighElement->currentLevel].addr = pHighElement->nextElementPage;
			tailPoint[pHighElement->currentLevel].offset = pHighElement->nextElementOffset;
			
			pDiskTablePage->tableLength -= 1;
			pDiskTablePage->usingLength -= sizeof(DiskTableElement);
			
			highElementOffset = pHighElement->highElementOffset;
			memset(pHighElement, 0, sizeof(DiskTableElement));

			if (highElementOffset == 0)
				break;
		} while (1);

		plg_assert(plg_TableCheckLength(nextPage, pTableHandle->pageSize));

		do {
			if (pDiskTablePage->tableSize == 0) {
				break;
			}

			//recycling tableSize
			if (pDiskTablePage->element[pDiskTablePage->tableSize - 1].keyOffset == 0){

				plg_assert(pDiskTablePage->element[pDiskTablePage->tableSize - 1].nextElementPage == 0);
				plg_assert(pDiskTablePage->element[pDiskTablePage->tableSize - 1].nextElementOffset == 0);
				plg_assert(pDiskTablePage->element[pDiskTablePage->tableSize - 1].highElementOffset == 0);
				plg_assert(pDiskTablePage->element[pDiskTablePage->tableSize - 1].lowElementOffset == 0);
				plg_assert(pDiskTablePage->element[pDiskTablePage->tableSize - 1].currentLevel == 0);
				plg_assert(pDiskTablePage->element[pDiskTablePage->tableSize - 1].keyOffset == 0);

				memset(&pDiskTablePage->element[pDiskTablePage->tableSize - 1], 0, sizeof(DiskTableElement));
				pDiskTablePage->spaceAddr = OFFSET(nextPage, &pDiskTablePage->element[pDiskTablePage->tableSize - 1]);
				pDiskTablePage->spaceLength += sizeof(DiskTableElement);
				pDiskTablePage->tableSize -= 1;

			} else {
				break;
			}
		} while (1);

		pTableHandle->pTableHandleCallBack->arrangementCheck(pTableHandle, nextPage);

		if (pDiskTablePage->tableLength != 0) {

			//To updat PDiskTableUsing
			void* usingPage;
			if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, pDiskTablePage->usingPageAddr, &usingPage) == 0) {
				return 0;
			}
				
			usingPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, pDiskTablePage->usingPageAddr, usingPage);

			PDiskTableUsingPage pDiskTableUsingPage = (PDiskTableUsingPage)((unsigned char*)usingPage + sizeof(DiskPageHead));
			PDiskTableUsing pDiskTableUsing = (PDiskTableUsing)POINTER(usingPage, pDiskTablePage->usingPageOffset);
			
			pDiskTableUsingPage->allSpace += (int)pDiskTablePage->spaceLength - pDiskTableUsing->usingSpaceLength;
			pDiskTableUsing->usingSpaceLength = pDiskTablePage->spaceLength;
			pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, pDiskTablePage->usingPageAddr);
		} else {
			table_DelPage(pTableHandle, curPageAddr);
		}

		plg_assert(plg_TableCheckSpace(nextPage));
		plg_assert(plg_TableCheckLength(nextPage, pTableHandle->pageSize));
	} while (1);

	if (isBreak) {

		//reset all tail point
		for (int l = 0; l <= tailLevel; l++) {
			if ((*pSkipListPoint)[l].skipListAddr) {
				void* page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, (*pSkipListPoint)[l].skipListAddr, (*pSkipListPoint)[l].page);
				if (page != (*pSkipListPoint)[l].page) {
					(*pSkipListPoint)[l].page = page;
					(*pSkipListPoint)[l].pDiskTableElement = (PDiskTableElement)POINTER(page, (*pSkipListPoint)[l].skipListOffset);
				}
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, (*pSkipListPoint)[l].skipListAddr);
			} else {
				PTableInFile pTableInFile;
				if (pTableHandle->pTableInFile->isSetHead) {
					pTableInFile = pTableHandle->pTableInFile;
				} else {
					pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
				}
				(*pSkipListPoint)[l].pDiskTableElement = &pTableInFile->tableHead[l];
				pTableHandle->hitStamp = plg_GetCurrentSec();
				if (!pTableHandle->pTableInFile->isSetHead) {
					pTableHandle->pTableHandleCallBack->addDirtyTable(pTableHandle, pTableHandle->nameaTable);
				}
			}

			PDiskTableElement pDiskTableElement = (*pSkipListPoint)[l].pDiskTableElement;
			pDiskTableElement->nextElementPage = tailPoint[l].addr;
			pDiskTableElement->nextElementOffset = tailPoint[l].offset;

			//set prevElementPage prevElementOffset
			if (l == 0 && tailPoint[l].addr) {
				pTableHandle->pTableHandleCallBack->addDirtyPage(pTableHandle, tailPoint[l].addr);
				void* tailPointPage;
				if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, tailPoint[l].addr, &tailPointPage) == 0) {
					return 0;
				}	
				void* nextPage = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, tailPoint[l].addr, tailPointPage);
				tailPoint[l].page = nextPage;
				PDiskTableElement pNextDiskTableElement = (PDiskTableElement)POINTER(nextPage, tailPoint[l].offset);
				tailPoint[l].keyOffset = pNextDiskTableElement->keyOffset;
				PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(nextPage, pNextDiskTableElement->keyOffset);
				pDiskTableKey->prevElementPage = (*pSkipListPoint)[l].skipListAddr;
				pDiskTableKey->prevElementOffset = (*pSkipListPoint)[l].skipListOffset;
			}
		}
	}

	return 1;
}

static unsigned int plg_InsideTableAdd(void* pvTableHandle, void* vKey, short keyLen, char valueType, void* value, unsigned short length) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle, vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	//add new
	return table_InsideNew(pTableHandle, vKey, keyLen, valueType, value, length, &skipListPoint);
}

unsigned int plg_TableAdd(void* pvTableHandle, void* vKey, short keyLen, void* value, unsigned short length) {

	//add new
	PTableHandle pTableHandle = pvTableHandle;
	return plg_InsideTableAdd(pTableHandle, vKey, keyLen, VALUE_NORMAL, value, length);
}

unsigned int plg_TableDelForSet(void* pvTableHandle, void* vKey, short keyLen) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = { { 0 } };
	if (plg_TableFindWithName(pTableHandle, vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}
	//Pay attention to different parameters
	if (table_InsideDel(pTableHandle, vKey, keyLen, &skipListPoint, 1) == 0) {
		return 0;
	}
	return 1;
}

/*
Note that this is only table deletion, not cache deletion.
Deleting a table without deleting the cahce will cause the cache to fail to delete
*/
unsigned int plg_TableDel(void* pvTableHandle, void* vKey, short keyLen) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle, vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	if (table_InsideDel(pTableHandle, vKey, keyLen, &skipListPoint, 0) == 0) {
		return 0;
	}
	return 1;
}

unsigned int plg_TableAlter(void* pvTableHandle, void* vKey, short keyLen, void* value, unsigned short length) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle, vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	if (table_InsideAlter(pTableHandle,vKey, keyLen, &skipListPoint, 1, value, length) == 0) {
		return 0;
	}

	return 1;
}

int plg_TableFind(void* pvTableHandle, void* vKey, short keyLen, void* pDictExten, short isSet) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle,vKey, keyLen, &skipListPoint, plg_TableTailFindCmpFun) == 0) {
		return -1;
	}

	if (skipListPoint[0].skipListAddr == 0) {
		return 0;
	}

	//load table page retrun PDiskTableKey
	void *prevPage = skipListPoint[0].page;

	//get PDiskTableKey
	PDiskTableElement pDiskTableElement = skipListPoint[0].pDiskTableElement;
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(prevPage, pDiskTableElement->keyOffset);
	void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;

	//copy value
	int strSize = 0;
	if (keyLen < pDiskTableKey->keyStrSize) {
		strSize = keyLen;
	} else {
		strSize = pDiskTableKey->keyStrSize;
	}

	//check
	if (memcmp(pDiskTableKey->keyStr, vKey, strSize) != 0) {
		return 0;
	}

	if (pDiskTableKey->valueSize != 0 && pDictExten) {
		if (pDiskTableKey->valueType == VALUE_NORMAL && !isSet) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			return pDiskTableKey->valueSize;
		} else if (pDiskTableKey->valueType == VALUE_SETHEAD && isSet) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			return pDiskTableKey->valueSize;
		} else if (pDiskTableKey->valueType == VALUE_BIGVALUE && !isSet) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

			if (bigValuePtr == 0) {
				elog(log_error, "plg_TableFind.bigValuePtr is empty!");
				return -1;
			}
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
			return pDiskKeyBigValue->allSize;
		} else {
			plg_assert(0);
			elog(log_error, "WRONGTYPE Operation against a key holding the wrong kind of value!");
			return -1;
		}
	}

	return 1;
}

//Keep only one correct add
unsigned int table_InsideAddWithAlter(void* pvTableHandle, char* Key, short keyLen, char valueType, void* value, unsigned short length) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle, Key, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	if (table_InsideAlter(pTableHandle, Key, keyLen, &skipListPoint, valueType, value, length) == 1) {
		return 1;
	}

	//del
	if (table_InsideDel(pTableHandle, Key, keyLen, &skipListPoint, 0) == 0) {
		return 0;
	}

	//add new
	return table_InsideNew(pTableHandle, Key, keyLen, valueType, value, length, &skipListPoint);
}

//Keep only one correct add
unsigned int plg_TableAddWithAlter(void* pvTableHandle, void* vKey, short keyLen, char valueType, void* value, unsigned short length) {
	return table_InsideAddWithAlter(pvTableHandle, vKey, keyLen, valueType, value, length);
}

//Keep only one correct add
unsigned int plg_TableAddIfNoExist(void* pvTableHandle, void* vKey, short keyLen, char valueType, void* value, unsigned short length) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle,vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	if (table_InsideIsKeyExist(pTableHandle, vKey, keyLen, &skipListPoint) == 1) {
		return 0;
	}

	//add new
	return table_InsideNew(pTableHandle,vKey, keyLen, valueType, value, length, &skipListPoint);
}

unsigned int plg_TableIsKeyExist(void* pvTableHandle, void* vKey, short keyLen) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle,vKey, keyLen, &skipListPoint, plg_TablePrevFindCmpFun) == 0) {
		return 0;
	}

	return table_InsideIsKeyExist(pTableHandle, vKey, keyLen, &skipListPoint);
}

unsigned int plg_TableRename(void* pvTableHandle, void* vKey, short keyLen, void* vNewKey, short newKeyLen) {

	//find skip list point
	PTableHandle pTableHandle = pvTableHandle;
	SkipListPoint skipListPoint[SKIPLIST_MAXLEVEL] = {{ 0 }};
	if (plg_TableFindWithName(pTableHandle,vKey, keyLen, &skipListPoint, plg_TableTailFindCmpFun) == 0) {
		return 0;
	}

	if (skipListPoint[0].skipListAddr == 0) {
		return 0;
	}

	//load table page retrun PDiskTableKey
	void *prevPage = 0;
	if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, skipListPoint[0].skipListAddr, &prevPage) == 0)
		return 0;

	//get PDiskTableKey
	PDiskTableElement pDiskTableElement = skipListPoint[0].pDiskTableElement;
	PDiskTableKey pDiskTableKey = (PDiskTableKey)POINTER(prevPage, pDiskTableElement->keyOffset);
	void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;

	//copy value
	int strSize = 0;
	if (keyLen < pDiskTableKey->keyStrSize) {
		strSize = keyLen;
	} else {
		strSize = pDiskTableKey->keyStrSize;
	}

	//check
	if (memcmp(pDiskTableKey->keyStr, vKey, strSize) != 0) {
		return 0;
	}

	if (pDiskTableKey->valueSize != 0 ) {
		unsigned int r = 0;
		do {
			//find skip list point
			SkipListPoint skipListPointAlter[SKIPLIST_MAXLEVEL];
			memset(skipListPointAlter, 0, sizeof(SkipListPoint)*SKIPLIST_MAXLEVEL);
			if (plg_TableFindWithName(pTableHandle, vNewKey, newKeyLen, &skipListPointAlter, plg_TablePrevFindCmpFun) == 0) {
				return 0;
			}

			if (table_InsideAlter(pTableHandle, vNewKey, newKeyLen, &skipListPointAlter, pDiskTableKey->valueType, vluePtr, pDiskTableKey->valueSize) == 1) {
				return 1;
			}

			//del
			if (table_InsideDel(pTableHandle, vNewKey, newKeyLen, &skipListPointAlter, 0) == 0) {
				return 0;
			}


			r = table_InsideNew(pTableHandle, vNewKey, newKeyLen, pDiskTableKey->valueType, vluePtr, pDiskTableKey->valueSize, &skipListPointAlter);
		} while (0);

		//del
		if (table_InsideDel(pTableHandle,vKey, keyLen, &skipListPoint, 0) == 0) {
			return 0;
		}

		return r;
	}

	return 1;
}

void plg_TableLimite(void* pvTableHandle, void* vKey, short keyLen, unsigned int left, unsigned int right, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, vKey, keyLen);
	void* preIter = table_DupIterator(iter);
	PDiskTableKey pDiskTableKey;
	unsigned int count = 0;
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
		if (count++ > right) {
			break;
		}

		void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
		if (pDiskTableKey->valueType == VALUE_NORMAL) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
		} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

			if (bigValuePtr == 0) {
				continue;
			}
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
		}
	};
	plg_TableReleaseIterator(iter);

	count = 0;
	while ((pDiskTableKey = plg_TablePrevIterator(preIter)) != NULL) {
		if (count++ > left) {
			break;
		}

		void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
		if (pDiskTableKey->valueType == VALUE_NORMAL) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
		} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

			if (bigValuePtr == 0) {
				continue;
			}
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
		}
	};
	plg_TableReleaseIterator(preIter);
}

void plg_TableOrder(void* pvTableHandle, short order, unsigned int limite, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	if (order == 0) {
		void* iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
		PDiskTableKey pDiskTableKey;
		unsigned int count = 0;
		while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
			if (++count > limite) {
				break;
			}

			void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
			if (pDiskTableKey->valueType == VALUE_NORMAL) {
				plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
				PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
				void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

				if (bigValuePtr == 0) {
					continue;
				}
				plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
				free(bigValuePtr);
			}
		};
		plg_TableReleaseIterator(iter);
	} else {
		void* iter = plg_TableGetIteratorToTail(pTableHandle);
		PDiskTableKey pDiskTableKey;
		unsigned int count = 0;
		while ((pDiskTableKey = plg_TablePrevIterator(iter))!=NULL) {
			if (++count > limite) {
				break;
			}

			void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
			if (pDiskTableKey->valueType == VALUE_NORMAL) {
				plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
				PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
				void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

				if (bigValuePtr == 0) {
					continue;
				}
				plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
				free(bigValuePtr);
			}
		};
		plg_TableReleaseIterator(iter);
	}
}

/*
If the PLG_Tablegetiteratorwithkey search fails, will it return invalid results?
*/
void plg_TableRang(void* pvTableHandle, void* beginKey, short beginKeyLen, void* endKey, short endKeyLen, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, beginKey, beginKeyLen);
	PDiskTableKey pDiskTableKey;
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {

		void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
		if (pDiskTableKey->valueType == VALUE_NORMAL) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
		} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

			if (bigValuePtr == 0) {
				continue;
			}
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
		}

		if (endKeyLen == pDiskTableKey->keyStrSize && memcmp(pDiskTableKey->keyStr, endKey, pDiskTableKey->keyStrSize) == 0) {
			break;
		}
	};
	plg_TableReleaseIterator(iter);
}

void plg_TablePoint(void* pvTableHandle, void* beginKey, short beginKeyLen, unsigned int direction, unsigned int offset, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, beginKey, beginKeyLen);
	PDiskTableKey pDiskTableKey;
	unsigned int count = 0;

	if (direction) {
		while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {

			if (count++ == offset) {
				void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
				if (pDiskTableKey->valueType == VALUE_NORMAL) {
					plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
				} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
					PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
					void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

					if (bigValuePtr == 0) {
						continue;
					}
					plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
					free(bigValuePtr);
				}
				break;
			}
		}
	} else {
		while ((pDiskTableKey = plg_TablePrevIterator(iter)) != NULL) {

			if (count++ == offset) {
				void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
				if (pDiskTableKey->valueType == VALUE_NORMAL) {
					plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
				} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
					PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
					void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

					if (bigValuePtr == 0) {
						continue;
					}
					plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
					free(bigValuePtr);
				}
				break;
			}
		}
	}
	plg_TableReleaseIterator(iter);
}

static unsigned int table_RangCount(void* pvTableHandle, void* beginKey, short beginKeyLen, void* endKey, short endKeyLen) {

	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, beginKey, beginKeyLen);
	PDiskTableKey pDiskTableKey;
	unsigned int count = 0;
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {

		count += 1;
		if (endKeyLen == pDiskTableKey->keyStrSize && memcmp(pDiskTableKey->keyStr, endKey, pDiskTableKey->keyStrSize) == 0) {
			break;
		}
	};
	plg_TableReleaseIterator(iter);

	return count;
}

void plg_TableMembers(void* pvTableHandle, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
	PDiskTableKey pDiskTableKey;
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {

		void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
		if (pDiskTableKey->valueType == VALUE_NORMAL) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
		} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

			if (bigValuePtr == 0) {
				continue;
			}
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
		}
	};
	plg_TableReleaseIterator(iter);
}

void plg_TablePattern(void* pvTableHandle, void* beginKey, short beginKeyLen, void* endKey, short endKeyLen, void* pattern, short patternLen, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, beginKey, beginKeyLen);
	PDiskTableKey pDiskTableKey;
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {

		if (plg_StringMatchLen(pattern, patternLen, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, 0)) {
			void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
			if (pDiskTableKey->valueType == VALUE_NORMAL) {
				plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
				PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
				void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

				if (bigValuePtr == 0) {
					continue;
				}
				plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
				free(bigValuePtr);
			}
		}

		if (endKeyLen == pDiskTableKey->keyStrSize && memcmp(pDiskTableKey->keyStr, endKey, pDiskTableKey->keyStrSize) == 0) {
			break;
		}
	};
	plg_TableReleaseIterator(iter);
}

unsigned int plg_TableMultiAdd(void* pvTableHandle, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int s = 0;
	void* dictIter = plg_DictExtenGetIterator(pDictExten);
	void* dictNode;
	while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {

		unsigned int keyLen, valueLen;
		char* pKey = plg_DictExtenKey(dictNode, &keyLen);
		char* pValue = plg_DictExtenValue(dictNode, &valueLen);
		
		if (valueLen > plg_TableBigValueSize()) {
			DiskKeyBigValue diskKeyBigValue;
			if (0 == plg_TableNewBigValue(pTableHandle, pValue, valueLen, &diskKeyBigValue))
				return s;

			unsigned int r = table_InsideAddWithAlter(pTableHandle, pKey, keyLen, VALUE_BIGVALUE, &diskKeyBigValue, sizeof(DiskKeyBigValue));
			if (!s && r) {
				s = 1;
			}
		} else {
			unsigned int r = table_InsideAddWithAlter(pTableHandle, pKey, keyLen, VALUE_NORMAL, pValue, valueLen);
			if (!s && r) {
				s = 1;
			}
		}
	}
	plg_DictExtenReleaseIterator(dictIter);
	return s;
}

void plg_TableMultiFind(void* pvTableHandle, void* pKeyDictExten, void* pValueDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* dictIter = plg_DictExtenGetIterator(pKeyDictExten);
	void* dictNode;
	while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {

		unsigned int keyLen;
		char* pKey = plg_DictExtenKey(dictNode, &keyLen);

		plg_TableFind(pTableHandle, pKey, keyLen, pValueDictExten, 0);
	}
	plg_DictExtenReleaseIterator(dictIter);

}

static void table_MultiDel(void* pvTableHandle, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* dictIter = plg_DictExtenGetIterator(pDictExten);
	void* dictNode;
	while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {

		unsigned int keyLen;
		char* pKey = plg_DictExtenKey(dictNode, &keyLen);
	
		plg_TableDel(pTableHandle, pKey, keyLen);
	}
	plg_DictExtenReleaseIterator(dictIter);

}

unsigned int plg_TableRand(void* pvTableHandle, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int count = 0;
	PDiskTableKey pDiskTableKey = 0;
	PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
		count++;
	};
	
	if (!count) {
		plg_TableReleaseIterator(iter);
		return 0;
	}

	unsigned int cur = rand() % count;
	if (cur < count / 2) {
		count = 0;
		PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
		while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
			if (++count >= cur) {
				break;
			}
		};
		plg_TableReleaseIterator(iter);
	} else {
		PTableIterator iter = plg_TableGetIteratorToTail(pTableHandle);
		while ((pDiskTableKey = plg_TablePrevIterator(iter)) != NULL) {
			if (--count <= cur) {
				break;
			}
		};
		plg_TableReleaseIterator(iter);
	}

	unsigned int r = 0;
	void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
	if (pDiskTableKey->valueType == VALUE_NORMAL) {
		plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
		r = 1;
	} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
		PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
		void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

		if (bigValuePtr != 0) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
			r = 1;
		}
	}

	plg_TableReleaseIterator(iter);

	return r;
}

unsigned int table_Pop(void* pvTableHandle, void* pDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	unsigned int count = 0;
	PDiskTableKey pDiskTableKey = 0;
	PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
		count++;
	};

	if (!count) {
		plg_TableReleaseIterator(iter);
		return 0;
	}

	unsigned int cur = rand() % count;
	if (cur < count / 2) {
		count = 0;
		PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
		while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
			if (++count >= cur) {
				break;
			}
		};
		plg_TableReleaseIterator(iter);
	} else {
		PTableIterator iter = plg_TableGetIteratorToTail(pTableHandle);
		while ((pDiskTableKey = plg_TablePrevIterator(iter)) != NULL) {
			if (--count <= cur) {
				break;
			}
		};
		plg_TableReleaseIterator(iter);
	}

	unsigned int r = 0;
	void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
	if (pDiskTableKey->valueType == VALUE_NORMAL) {
		plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
		r = 1;
	} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
		PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
		void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

		if (bigValuePtr != 0) {
			plg_DictExtenAdd(pDictExten, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, bigValuePtr, pDiskKeyBigValue->allSize);
			free(bigValuePtr);
			r = 1;
		}
	}

	plg_TableReleaseIterator(iter);
	plg_TableDel(pTableHandle, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize);
	return r;
}

void plg_TableClear(void* pvTableHandle, short recursive) {

	PTableHandle pTableHandle = pvTableHandle;
	PTableInFile pTableInFile = 0;
	if (recursive) {
		PDiskTableKey pDiskTableKey = 0;
		PTableIterator iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
		while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {
			if (pDiskTableKey->valueType == VALUE_SETHEAD) {
				void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
				PTableInFile pTableInFile = (PTableInFile)vluePtr;
				PTableInFile pRecTableInFile = pTableHandle->pTableInFile;
				pTableHandle->pTableInFile = pTableInFile;
				plg_TableClear(pTableHandle, 0);
				pTableHandle->pTableInFile = pRecTableInFile;
			}
		}
		plg_TableReleaseIterator(iter);
		pTableInFile = pTableHandle->pTableHandleCallBack->tableCopyOnWrite(pTableHandle, pTableHandle->nameaTable, pTableHandle->pTableInFile);
	} else {
		pTableInFile = pTableHandle->pTableInFile;
	}

	unsigned int nextPageAddr = pTableInFile->tablePageHead;
	pTableInFile->tablePageHead = 0;
	
	do {
		if (nextPageAddr == 0) {
			break;
		}

		void* page;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPageAddr, &page) == 0) {
			break;
		}
		page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextPageAddr, page);

		//find in page
		PDiskPageHead pPageHead = (PDiskPageHead)((unsigned char*)page);
		nextPageAddr = pPageHead->nextPage;

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pPageHead->addr);
	} while (1);

	

	nextPageAddr = pTableInFile->tableUsingPage;
	pTableInFile->tableUsingPage = 0;
	
	do {
		if (nextPageAddr == 0) {
			break;
		}

		void* page;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPageAddr, &page) == 0) {
			break;
		}
		page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextPageAddr, page);

		//find in page
		PDiskPageHead pPageHead = (PDiskPageHead)((unsigned char*)page);
		nextPageAddr = pPageHead->nextPage;

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pPageHead->addr);
	} while (1);
	

	nextPageAddr = pTableInFile->valuePage;
	pTableInFile->valuePage = 0;

	do {
		if (nextPageAddr == 0) {
			break;
		}

		void* page;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPageAddr, &page) == 0) {
			break;
		}
		page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextPageAddr, page);

		//find in page
		PDiskPageHead pPageHead = (PDiskPageHead)((unsigned char*)page);
		nextPageAddr = pPageHead->nextPage;

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pPageHead->addr);
	} while (1);
	

	nextPageAddr = pTableInFile->valueUsingPage;
	pTableInFile->valueUsingPage = 0;

	do {
		if (nextPageAddr == 0) {
			break;
		}

		void* page;
		if (pTableHandle->pTableHandleCallBack->findPage(pTableHandle, nextPageAddr, &page) == 0) {
			break;
		}
		page = pTableHandle->pTableHandleCallBack->pageCopyOnWrite(pTableHandle, nextPageAddr, page);

		//find in page
		PDiskPageHead pPageHead = (PDiskPageHead)((unsigned char*)page);
		nextPageAddr = pPageHead->nextPage;

		pTableHandle->pTableHandleCallBack->delPage(pTableHandle, pPageHead->addr);
	} while (1);

	plg_TableInitTableInFile(pTableInFile);
}

unsigned short plg_TableBigValueSize() {

	unsigned retSize = sizeof(DiskKeyBigValue);
	if (retSize < sizeof(TableInFile)) {
		retSize = sizeof(TableInFile);
	}

	return retSize;
}

/*
�Ȳ���
�����
*/
unsigned int plg_TableSetAdd(void* pvTableHandle, void* vKey, short keyLen, void* vValue, short valueLen) {

	PTableHandle pTableHandle = pvTableHandle;
	short ret = 0, find = 0;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	TableInFile oldTableInFile;
	memset(&oldTableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	//find
	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		find = 1;
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int entryValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &entryValueLen);
			if (entryValueLen) {
				memcpy(&tableInFile, valuePtr, entryValueLen);
				memcpy(&oldTableInFile, valuePtr, entryValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				if (1 == plg_TableAddWithAlter(pTableHandle, vValue, valueLen, VALUE_NORMAL, NULL, 0)){
					ret = 1;
					if (memcmp(&tableInFile, &oldTableInFile, entryValueLen) != 0) {
						pTableHandle->pTableInFile = pTableInFile;
						if (0 == plg_InsideTableAlterFroSet(pTableHandle, vKey, keyLen, VALUE_SETHEAD, &tableInFile, sizeof(TableInFile))) {
							ret = 0;
						}
					}
				}
			}
		}
	} 
	plg_DictExtenDestroy(pDictExten);
	
	//no find new set
	if (!find) {

		plg_TableInitTableInFile(&tableInFile);
		tableInFile.isSetHead = 1;
		pTableHandle->pTableInFile = &tableInFile;
		if (0 == plg_InsideTableAdd(pTableHandle, vValue, valueLen, VALUE_NORMAL, NULL, 0)) {
			ret = 0;
		} else {
			pTableHandle->pTableInFile = pTableInFile;
			if (0 == plg_InsideTableAdd(pTableHandle, vKey, keyLen, VALUE_SETHEAD, &tableInFile, sizeof(TableInFile))) {
				ret = 0;
			}
			ret = 1;
		}
	}

	pTableHandle->pTableInFile = pTableInFile;
	return ret;
}

void plg_TableSetRang(void* pvTableHandle, void* vKey, short keyLen, void* beginValue, short beginValueLen, void* endValue, short endValueLen, void* pInDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				plg_TableRang(pTableHandle, beginValue, beginValueLen, endValue, endValueLen, pInDictExten);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
}

void plg_TableSetPoint(void* pvTableHandle, void* vKey, short keyLen, void* beginValue, short beginValueLen, unsigned int direction, unsigned int offset, void* pInDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				plg_TablePoint(pTableHandle, beginValue, beginValueLen, direction, offset, pInDictExten);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
}

void plg_TableSetLimite(void* pvTableHandle, void* vKey, short keyLen, void* vValue, short valueLen, unsigned int left, unsigned int right, void* pInDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				plg_TableLimite(pTableHandle, vValue, valueLen, left, right, pInDictExten);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
}

unsigned int plg_TableSetLength(void* pvTableHandle, void* vKey, short keyLen) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();
	unsigned int len = 0;

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				len = plg_TableLength(pTableHandle);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
	return len;
}

unsigned int plg_TableSetIsKeyExist(void* pvTableHandle, void* vKey, short keyLen, void* vValue, short valueLen) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();
	unsigned int is = 0;

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* retValuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, retValuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				is = plg_TableIsKeyExist(pTableHandle, vValue, valueLen);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
	return is;
}

void plg_TableSetMembers(void* pvTableHandle, void* vKey, short keyLen, void* pInDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				plg_TableMembers(pTableHandle, pInDictExten);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
}

unsigned int plg_TableSetRand(void* pvTableHandle, void* vKey, short keyLen, void* pInDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();
	unsigned int r = 0;

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				r = plg_TableRand(pTableHandle, pInDictExten);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
	return r;
}

//for set Multiple
void plg_TableSetDel(void* pvTableHandle, void* vKey, short keyLen, void* pValueDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	TableInFile oldTableInFile;
	memset(&oldTableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);
				memcpy(&oldTableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				table_MultiDel(pTableHandle, pValueDictExten);

				pTableHandle->pTableInFile = pTableInFile;
				if (memcmp(&tableInFile, &oldTableInFile, retValueLen) != 0) {
					if (tableInFile.tablePageHead) {
						plg_InsideTableAlterFroSet(pTableHandle, vKey, keyLen, VALUE_SETHEAD, &tableInFile, sizeof(TableInFile));
					} else {
						plg_TableDelForSet(pTableHandle, vKey, keyLen);
					}
				}
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);

	pTableHandle->pTableInFile = pTableInFile;	
}

//for set move
static void table_InsideSetDel(void* pvTableHandle, void* vKey, short keyLen, void* vValue, short valueLen) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	TableInFile oldTableInFile;
	memset(&oldTableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);
				memcpy(&oldTableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				plg_TableDel(pTableHandle, vValue, valueLen);

				pTableHandle->pTableInFile = pTableInFile;
				if (memcmp(&tableInFile, &oldTableInFile, retValueLen) != 0) {
					if (tableInFile.tablePageHead) {
						plg_InsideTableAlterFroSet(pTableHandle, vKey, keyLen, VALUE_SETHEAD, &tableInFile, sizeof(TableInFile));
					} else {
						plg_TableDelForSet(pTableHandle, vKey, keyLen);
					}
				}
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);

	pTableHandle->pTableInFile = pTableInFile;
}

unsigned int plg_TableSetPop(void* pvTableHandle, void* vKey, short keyLen, void* pInDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	TableInFile oldTableInFile;
	memset(&oldTableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();
	unsigned int r = 0;;

	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);
				memcpy(&oldTableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				r = table_Pop(pTableHandle, pInDictExten);

				pTableHandle->pTableInFile = pTableInFile;
				if (memcmp(&tableInFile, &oldTableInFile, retValueLen) != 0) {
					if (tableInFile.tablePageHead) {
						plg_InsideTableAlterFroSet(pTableHandle, vKey, keyLen, VALUE_SETHEAD, &tableInFile, sizeof(TableInFile));
					} else {
						plg_TableDelForSet(pTableHandle, vKey, keyLen);
					}
				}
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;
	return r;
}

unsigned int plg_TableSetRangCount(void* pvTableHandle, void* vKey, short keyLen, void* beginValue, short beginValueLen, void* endValue, short endValueLen) {

	PTableHandle pTableHandle = pvTableHandle;
	TableInFile tableInFile;
	memset(&tableInFile, 0, sizeof(TableInFile));
	PTableInFile pTableInFile = pTableHandle->pTableInFile;
	void* pDictExten = plg_DictExtenCreate();
	unsigned int count = 0;
	if (0 < plg_TableFind(pTableHandle, vKey, keyLen, pDictExten, 1)) {
		if (plg_DictExtenSize(pDictExten)) {
			void* entry = plg_DictExtenGetHead(pDictExten);
			unsigned int retValueLen;
			void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
			if (retValueLen) {
				memcpy(&tableInFile, valuePtr, retValueLen);

				pTableHandle->pTableInFile = &tableInFile;
				count = table_RangCount(pTableHandle, beginValue, beginValueLen, endValue, endValueLen);
			}
		}
	}
	plg_DictExtenDestroy(pDictExten);
	pTableHandle->pTableInFile = pTableInFile;

	return count;
}

void plg_TableSetUion(void* pvTableHandle, void* pSetDictExten, void* pKeyDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* dictIter = plg_DictExtenGetIterator(pSetDictExten);
	void* dictNode;
	while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {

		unsigned int keyLen;
		char* pKey = plg_DictExtenKey(dictNode, &keyLen);

		//set members
		TableInFile tableInFile;
		memset(&tableInFile, 0, sizeof(TableInFile));
		PTableInFile pTableInFile = pTableHandle->pTableInFile;
		void* pDictExten = plg_DictExtenCreate();

		if (0 < plg_TableFind(pTableHandle, pKey, keyLen, pDictExten, 1)) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				unsigned int retValueLen;
				void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
				if (retValueLen) {
					memcpy(&tableInFile, valuePtr, retValueLen);

					pTableHandle->pTableInFile = &tableInFile;
					plg_TableMembers(pTableHandle, pKeyDictExten);
				}
			}
		}
		plg_DictExtenDestroy(pDictExten);
		pTableHandle->pTableInFile = pTableInFile;
	}
	plg_DictExtenReleaseIterator(dictIter);
}

void plg_TableSetUionStore(void* pvTableHandle, void* pSetDictExten, void* vKey, short keyLen) {

	PTableHandle pTableHandle = pvTableHandle;
	void* pDictExten = plg_DictExtenCreate();
	plg_TableSetUion(pTableHandle, pSetDictExten, pDictExten);

	//set to key
	if (plg_DictExtenSize(pDictExten)) {

		void* dictIter = plg_DictExtenGetIterator(pDictExten);
		void* dictNode;
		while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {
			unsigned int retKeyLen = 0;
			void* keyPtr = plg_DictExtenKey(dictNode, &retKeyLen);
			plg_TableSetAdd(pTableHandle, vKey, keyLen, keyPtr, retKeyLen);
		}
		plg_DictExtenReleaseIterator(dictIter);
	}
	plg_DictExtenDestroy(pDictExten);
}

void plg_TableSetInter(void* pvTableHandle, void* pSetDictExten, void* pKeyDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* dictIter = plg_DictExtenGetIterator(pSetDictExten);
	void* dictNode;
	while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {

		unsigned int keyLen;
		char* pKey = plg_DictExtenKey(dictNode, &keyLen);

		//set members
		TableInFile tableInFile;
		memset(&tableInFile, 0, sizeof(TableInFile));
		PTableInFile pTableInFile = pTableHandle->pTableInFile;
		void* pDictExten = plg_DictExtenCreate();

		if (0 < plg_TableFind(pTableHandle, pKey, keyLen, pDictExten, 1)) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				unsigned int retValueLen = 0;
				void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
				if (retValueLen) {
					memcpy(&tableInFile, valuePtr, retValueLen);
					pTableHandle->pTableInFile = &tableInFile;

					//find inter
					if (plg_DictExtenSize(pKeyDictExten) == 0) {
						plg_TableMembers(pTableHandle, pKeyDictExten);
					} else {
						void* dictInterIter = plg_DictExtenGetIterator(pKeyDictExten);
						void* dictInterNode;
						while ((dictInterNode = plg_DictExtenNext(dictInterIter)) != NULL) {

							unsigned int interKeyLen;
							char* interKey = plg_DictExtenKey(dictInterNode, &interKeyLen);
							if (1 > plg_TableFind(pTableHandle, interKey, interKeyLen, 0, 1)) {
								plg_DictExtenDel(pKeyDictExten, interKey, interKeyLen);
							}
						}
						plg_DictExtenReleaseIterator(dictInterIter);
					}
					//end find
				}
			}
		}
		plg_DictExtenDestroy(pDictExten);
		pTableHandle->pTableInFile = pTableInFile;
	}
	plg_DictExtenReleaseIterator(dictIter);
}

void plg_TableSetInterStore(void* pvTableHandle, void* pSetDictExten, void* vKey, short keyLen) {

	PTableHandle pTableHandle = pvTableHandle;
	void* pDictExten = plg_DictExtenCreate();
	plg_TableSetInter(pTableHandle, pSetDictExten, pDictExten);

	//set to key
	if (plg_DictExtenSize(pDictExten)) {

		void* dictIter = plg_DictExtenGetIterator(pDictExten);
		void* dictNode;
		while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {
			unsigned int retKeyLen = 0;
			void* keyPtr = plg_DictExtenKey(dictNode, &retKeyLen);
			plg_TableSetAdd(pTableHandle, vKey, keyLen, keyPtr, retKeyLen);
		}
		plg_DictExtenReleaseIterator(dictIter);
	}
	plg_DictExtenDestroy(pDictExten);
}

void plg_TableSetDiff(void* pvTableHandle, void* pSetDictExten, void* pKeyDictExten) {

	PTableHandle pTableHandle = pvTableHandle;
	void* dictIter = plg_DictExtenGetIterator(pSetDictExten);
	void* dictNode;
	while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {

		unsigned int keyLen;
		char* pKey = plg_DictExtenKey(dictNode, &keyLen);

		//set members
		TableInFile tableInFile;
		memset(&tableInFile, 0, sizeof(TableInFile));
		PTableInFile pTableInFile = pTableHandle->pTableInFile;
		void* pDictExten = plg_DictExtenCreate();

		if (0 != plg_TableFind(pTableHandle, pKey, keyLen, pDictExten, 1)) {
			if (plg_DictExtenSize(pDictExten)) {
				void* entry = plg_DictExtenGetHead(pDictExten);
				unsigned int retValueLen = 0;
				void* valuePtr = plg_DictExtenValue(entry, &retValueLen);
				if (retValueLen) {
					memcpy(&tableInFile, valuePtr, retValueLen);
					pTableHandle->pTableInFile = &tableInFile;

					//find diff
					if (plg_DictExtenSize(pKeyDictExten) == 0) {
						plg_TableMembers(pTableHandle, pKeyDictExten);
					} else {
						void* dictInterIter = plg_DictExtenGetIterator(pKeyDictExten);
						void* dictInterNode;
						while ((dictInterNode = plg_DictExtenNext(dictInterIter)) != NULL) {

							unsigned int interKeyLen;
							char* interKey = plg_DictExtenKey(dictInterNode, &interKeyLen);
							if (0 < plg_TableFind(pTableHandle, interKey, interKeyLen, 0, 1)) {
								plg_DictExtenDel(pKeyDictExten, interKey, interKeyLen);
							}
						}
						plg_DictExtenReleaseIterator(dictInterIter);
					}
					//end find
				}
			}
		}
		plg_DictExtenDestroy(pDictExten);
		pTableHandle->pTableInFile = pTableInFile;
	}
	plg_DictExtenReleaseIterator(dictIter);
}

void plg_TableSetDiffStore(void* pvTableHandle, void* pSetDictExten, void* vKey, short keyLen) {

	PTableHandle pTableHandle = pvTableHandle;
	void* pDictExten = plg_DictExtenCreate();
	plg_TableSetDiff(pTableHandle, pSetDictExten, pDictExten);

	//set to key
	if (plg_DictExtenSize(pDictExten)) {

		void* dictIter = plg_DictExtenGetIterator(pDictExten);
		void* dictNode;
		while ((dictNode = plg_DictExtenNext(dictIter)) != NULL) {
			unsigned int retKeyLen = 0;
			void* keyPtr = plg_DictExtenKey(dictNode, &retKeyLen);
			plg_TableSetAdd(pTableHandle, vKey, keyLen, keyPtr, retKeyLen);
		}
		plg_DictExtenReleaseIterator(dictIter);
	}
	plg_DictExtenDestroy(pDictExten);
}

void plg_TableSetMove(void* pvTableHandle, void* vSrcKey, short  srcKeyLen, void* vDesKey, short desKeyLen, void* vValue, short valueLen) {

	PTableHandle pTableHandle = pvTableHandle;
	table_InsideSetDel(pTableHandle, vSrcKey, srcKeyLen, vValue, valueLen);
	plg_TableSetAdd(pTableHandle, vDesKey, desKeyLen, vValue, valueLen);
}

void plg_TableMembersWithJson(void* pvTableHandle, void* vjsonRoot) {

	pJSON* jsonRoot = vjsonRoot;
	PTableHandle pTableHandle = pvTableHandle;
	void* iter = plg_TableGetIteratorWithKey(pTableHandle, NULL, 0);
	PDiskTableKey pDiskTableKey;
	while ((pDiskTableKey = plg_TableNextIterator(iter)) != NULL) {

		void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
		if (pDiskTableKey->valueType == VALUE_NORMAL) {
			if (pTableHandle->pTableInFile->tableType == TT_Byte) {
				sds key = plg_sdsNewLen(pDiskTableKey->keyStr, pDiskTableKey->keyStrSize);
				char* value = plg_B64Encode(vluePtr, pDiskTableKey->valueSize);
				pJson_AddStringToObject(jsonRoot, key, value);
				free(value);
				plg_sdsFree(key);
			} else if (pTableHandle->pTableInFile->tableType == TT_String) {
				pJson_AddStringToObjectWithLen(jsonRoot, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			} else if (pTableHandle->pTableInFile->tableType == TT_Double) {
				sds key = plg_sdsNewLen(pDiskTableKey->keyStr, pDiskTableKey->keyStrSize);

				double num = 0;
				memcpy(&num, vluePtr, sizeof(double));
				pJson_AddNumberToObject(jsonRoot, key, num);
				plg_sdsFree(key);
			}
		} else if (pDiskTableKey->valueType == VALUE_BIGVALUE) {
			PDiskKeyBigValue pDiskKeyBigValue = (PDiskKeyBigValue)vluePtr;
			void* bigValuePtr = table_GetBigValue(pTableHandle, pDiskKeyBigValue);

			if (bigValuePtr == 0) {
				continue;
			}

			if (pTableHandle->pTableInFile->tableType == TT_Byte) {
				sds key = plg_sdsNewLen(pDiskTableKey->keyStr, pDiskTableKey->keyStrSize);
				char* value = plg_B64Encode(vluePtr, pDiskTableKey->valueSize);
				pJson_AddStringToObject(jsonRoot, key, value);
				free(value);
				plg_sdsFree(key);
			} else if (pTableHandle->pTableInFile->tableType == TT_String) {
				pJson_AddStringToObjectWithLen(jsonRoot, pDiskTableKey->keyStr, pDiskTableKey->keyStrSize, vluePtr, pDiskTableKey->valueSize);
			}

			free(bigValuePtr);
		} else if (pDiskTableKey->valueType == VALUE_SETHEAD) {
			void* vluePtr = (unsigned char*)pDiskTableKey + sizeof(DiskTableKey) + pDiskTableKey->keyStrSize;
			sds key = plg_sdsNewLen(pDiskTableKey->keyStr, pDiskTableKey->keyStrSize);
			PTableInFile pTableInFile = (PTableInFile)vluePtr;
			PTableInFile pRecTableInFile = pTableHandle->pTableInFile;

			pTableHandle->pTableInFile = pTableInFile;
			pJSON* josnSet = pJson_CreateObject();
			pJson_AddItemToObject(jsonRoot, key, josnSet);
			plg_TableMembersWithJson(pTableHandle, josnSet);
			pTableHandle->pTableInFile = pRecTableInFile;
			plg_sdsFree(key);
		}
	};
	plg_TableReleaseIterator(iter);
}

void plg_TableInitTableInFile(void* pvTableInFile) {

	PTableInFile pTableInFile = pvTableInFile;
	memset(pTableInFile, 0, sizeof(TableInFile));
	for (int i = 1; i < SKIPLIST_MAXLEVEL; i++) {
		pTableInFile->tableHead[i].currentLevel = i;
	}
}