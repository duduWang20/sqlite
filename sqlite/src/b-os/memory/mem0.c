/*
** 2008 October 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains a no-op memory allocation drivers for use when
** SQLITE_ZERO_MALLOC is defined.  The allocation drivers implemented
** here always fail.  SQLite will not operate with these drivers.  These
** are merely placeholders.  Real drivers must be substituted using
** sqlite3_config() before SQLite will operate.
*/
#include "sqliteInt.h"

/*
** This version of the memory allocator is the default.  It is
** used when no other memory allocator is specified using compile-time
** macros.
*/
#ifdef SQLITE_ZERO_MALLOC

/*
** No-op versions of all memory allocation routines
*/
static void *sqlite3MemMalloc(int nByte){ return 0; }
static void sqlite3MemFree(void *pPrior){ return; }
static void *sqlite3MemRealloc(void *pPrior, int nByte){ return 0; }
static int sqlite3MemSize(void *pPrior){ return 0; }
static int sqlite3MemRoundup(int n){ return n; }
static int sqlite3MemInit(void *NotUsed){ return SQLITE_OK; }
static void sqlite3MemShutdown(void *NotUsed){ return; }

/*
** This routine is the only routine in this file with external linkage.
**
** Populate the low-level memory allocation function pointers in
** sqlite3GlobalConfig.m with pointers to the routines in this file.
*/
void sqlite3MemSetDefault(void){
  static const sqlite3_mem_methods defaultMethods = {
     sqlite3MemMalloc,
     sqlite3MemFree,
     sqlite3MemRealloc,
     sqlite3MemSize,
     sqlite3MemRoundup,
     sqlite3MemInit,
     sqlite3MemShutdown,
     0
  };
  sqlite3_config(SQLITE_CONFIG_MALLOC, &defaultMethods);
}

#endif /* SQLITE_ZERO_MALLOC */




We can increase the efficiency with the following code replace:


static int memsys5Roundup(int n){//changed by wjf
    if( n > 0x40000000 ) return 0;
    int i = 0;
    while (_BlockSize[i++]<n);
    return _BlockSize[i-1];
}
//static int memsys5Roundup(int n){
//    int iFullSz;
//    if( n > 0x40000000 ) return 0;
//    for(iFullSz=mem5.szAtom; iFullSz<n; iFullSz *= 2);
//    return iFullSz;
//}

/*
 ** Initialize the memory allocator.
 **
 ** This routine is not threadsafe.  The caller must be holding a mutex
 ** to prevent multiple threads from entering at the same time.
 */
static int _BlockSize[32];//add by wjf
static int memsys5Init(void *NotUsed){
    int ii;            /* Loop counter */
    int nByte;         /* Number of bytes of memory available to this allocator */
    u8 *zByte;         /* Memory usable by this allocator */
    int nMinLog;       /* Log base 2 of minimum allocation size in bytes */
    int iOffset;       /* An offset into mem5.aCtrl[] */
    
    UNUSED_PARAMETER(NotUsed);
    
    /* For the purposes of this routine, disable the mutex */
    mem5.mutex = 0;
    
    /* The size of a Mem5Link object must be a power of two.  Verify that
     ** this is case.
     */
    assert( (sizeof(Mem5Link)&(sizeof(Mem5Link)-1))==0 );
    
    nByte = sqlite3GlobalConfig.nHeap;
    zByte = (u8*)sqlite3GlobalConfig.pHeap;
    assert( zByte!=0 );  /* sqlite3_config() does not allow otherwise */
    
    /* boundaries on sqlite3GlobalConfig.mnReq are enforced in sqlite3_config() */
    nMinLog = memsys5Log(sqlite3GlobalConfig.mnReq);
    mem5.szAtom = (1<<nMinLog);
    while( (int)sizeof(Mem5Link)>mem5.szAtom ){
        mem5.szAtom = mem5.szAtom << 1;
    }
    //add by wjf
    long long size = mem5.szAtom;
    int i = 0;
    while (size < 0x40000000) {
        _BlockSize[i++] = (int)size;
        size *= 2;
    }
    
    mem5.nBlock = (nByte / (mem5.szAtom+sizeof(u8)));
    mem5.zPool = zByte;
    mem5.aCtrl = (u8 *)&mem5.zPool[mem5.nBlock*mem5.szAtom];
    
    for(ii=0; ii<=LOGMAX; ii++){
        mem5.aiFreelist[ii] = -1;
    }
    
    iOffset = 0;
    for(ii=LOGMAX; ii>=0; ii--){
        int nAlloc = (1<<ii);
        if( (iOffset+nAlloc)<=mem5.nBlock ){
            mem5.aCtrl[iOffset] = ii | CTRL_FREE;
            memsys5Link(iOffset, ii);
            iOffset += nAlloc;
        }
        assert((iOffset+nAlloc)>mem5.nBlock);
    }
    
    /* If a mutex is required for normal operation, allocate one */
    if( sqlite3GlobalConfig.bMemstat==0 ){
        mem5.mutex = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MEM);
    }
    
    return SQLITE_OK;
}


