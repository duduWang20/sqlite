//
//  pager-imp.c
//  sqlite
//
//  Created by wangjufan on 2018/8/3.
//  Copyright © 2018 wangjufan. All rights reserved.
//

#include "pager-imp.h"


/*
 ** This function is called by the pcache layer when it has reached some
 ** soft memory limit. The first argument is a pointer to a Pager object
 ** (cast as a void*). The pager is always 'purgeable' (not an in-memory
 ** database). The second argument is a reference to a page that is
 ** currently dirty but has no outstanding references.
 ** The page is always associated with the Pager object passed as the first argument.
 **
 ** The job of this function is to make pPg clean by writing its contents
 ** out to the database file, if possible. This may involve syncing the
 ** journal file.
 **
 ** If successful, sqlite3PcacheMakeClean() is called on the page and
 ** SQLITE_OK returned. If an IO error occurs while trying to make the
 ** page clean, the IO error code is returned. If the page cannot be
 ** made clean for some other reason, but no error occurs, then SQLITE_OK
 ** is returned by sqlite3PcacheMakeClean() is not called.
 */
static int pagerStress(void *p, PgHdr *pPg){
    Pager *pPager = (Pager *)p;
    int rc = SQLITE_OK;
    
    assert( pPg->pPager==pPager );
    assert( pPg->flags&PGHDR_DIRTY );
    
    /* The doNotSpill NOSYNC bit is set during times when doing a sync of
     ** journal (and adding a new header) is not allowed.  This occurs
     ** during calls to sqlite3PagerWrite() while trying to journal multiple
     ** pages belonging to the same sector.
     **
     ** The doNotSpill ROLLBACK and OFF bits inhibits all cache spilling
     ** regardless of whether or not a sync is required.  This is set during
     ** a rollback or by user request, respectively.
     **
     ** Spilling is also prohibited when in an error state since that could
     ** lead to database corruption.   In the current implementation it
     ** is impossible for sqlite3PcacheFetch() to be called with createFlag==3
     ** while in the error state, hence it is impossible for this routine to
     ** be called in the error state.  Nevertheless, we include a NEVER()
     ** test for the error state as a safeguard against future changes.
     */
    if( NEVER(pPager->errCode) ) return SQLITE_OK;
    testcase( pPager->doNotSpill & SPILLFLAG_ROLLBACK );
    testcase( pPager->doNotSpill & SPILLFLAG_OFF );
    testcase( pPager->doNotSpill & SPILLFLAG_NOSYNC );
    if( pPager->doNotSpill
       && ((pPager->doNotSpill & (SPILLFLAG_ROLLBACK|SPILLFLAG_OFF))!=0
           || (pPg->flags & PGHDR_NEED_SYNC)!=0)
       ){
        return SQLITE_OK;
    }
    
    pPager->aStat[PAGER_STAT_SPILL]++;
    pPg->pDirty = 0;
    if( pagerUseWal(pPager) ){
        /* Write a single frame for this page to the log. */
        rc = subjournalPageIfRequired(pPg);
        if( rc==SQLITE_OK ){
            rc = pagerWalFrames(pPager, pPg, 0, 0);
        }
    }else{
        
#ifdef SQLITE_ENABLE_BATCH_ATOMIC_WRITE
        if( pPager->tempFile==0 ){
            rc = sqlite3JournalCreate(pPager->jfd);
            if( rc!=SQLITE_OK ) return pager_error(pPager, rc);
        }
#endif
        
        /* Sync the journal file if required. */
        if( pPg->flags&PGHDR_NEED_SYNC
           || pPager->eState==PAGER_WRITER_CACHEMOD
           ){
            rc = syncJournal(pPager, 1);
        }
        
        /* Write the contents of the page out to the database file. */
        if( rc==SQLITE_OK ){
            assert( (pPg->flags&PGHDR_NEED_SYNC)==0 );
            rc = pager_write_pagelist(pPager, pPg);
        }
    }
    
    /* Mark the page as clean. */
    if( rc==SQLITE_OK ){
        PAGERTRACE(("STRESS %d page %d\n", PAGERID(pPager), pPg->pgno));
        sqlite3PcacheMakeClean(pPg);
    }
    
    return pager_error(pPager, rc);
}


/*
 ** The argument is the first in a linked list of dirty pages connected
 ** by the PgHdr.pDirty pointer. This function writes each one of the
 ** in-memory pages in the list to the database file. The argument may
 ** be NULL, representing an empty list. In this case this function is
 ** a no-op.
 **
 ** The pager must hold at least a RESERVED lock when this function
 ** is called. Before writing anything to the database file, this lock
 ** is upgraded to an EXCLUSIVE lock. If the lock cannot be obtained,
 ** SQLITE_BUSY is returned and no data is written to the database file.
 **
 ** If the pager is a temp-file pager and the actual file-system file
 ** is not yet open, it is created and opened before any data is
 ** written out.
 **
 ** Once the lock has been upgraded and, if necessary, the file opened,
 ** the pages are written out to the database file in list order. Writing
 ** a page is skipped if it meets either of the following criteria:
 **
 **   * The page number is greater than Pager.dbSize, or
 **   * The PGHDR_DONT_WRITE flag is set on the page.
 **
 ** If writing out a page causes the database file to grow, Pager.dbFileSize
 ** is updated accordingly. If page 1 is written out, then the value cached
 ** in Pager.dbFileVers[] is updated to match the new value stored in
 ** the database file.
 **
 ** If everything is successful, SQLITE_OK is returned. If an IO error
 ** occurs, an IO error code is returned. Or, if the EXCLUSIVE lock cannot
 ** be obtained, SQLITE_BUSY is returned.
 */
static int pager_write_pagelist(Pager *pPager, PgHdr *pList){
    int rc = SQLITE_OK;                  /* Return code */
    
    /* This function is only called for rollback pagers in WRITER_DBMOD state. */
    assert( !pagerUseWal(pPager) );
    assert( pPager->tempFile || pPager->eState==PAGER_WRITER_DBMOD );
    assert( pPager->eLock==EXCLUSIVE_LOCK );
    assert( isOpen(pPager->fd) || pList->pDirty==0 );
    
    /* If the file is a temp-file has not yet been opened, open it now.
     ** It is not possible for rc to be other than SQLITE_OK if this branch
     ** is taken, as pager_wait_on_lock() is a no-op for temp-files.
     */
    if( !isOpen(pPager->fd) ){
        assert( pPager->tempFile && rc==SQLITE_OK );
        rc = pagerOpentemp(pPager, pPager->fd, pPager->vfsFlags);
    }
    
    /* Before the first write, give the VFS a hint of what the final
     ** file size will be.
     */
    assert( rc!=SQLITE_OK || isOpen(pPager->fd) );
    if( rc==SQLITE_OK
       && pPager->dbHintSize<pPager->dbSize
       && (pList->pDirty || pList->pgno>pPager->dbHintSize)
       ){
        sqlite3_int64 szFile = pPager->pageSize * (sqlite3_int64)pPager->dbSize;
        sqlite3OsFileControlHint(pPager->fd, SQLITE_FCNTL_SIZE_HINT, &szFile);
        pPager->dbHintSize = pPager->dbSize;
    }
    
    while( rc==SQLITE_OK && pList ){
        Pgno pgno = pList->pgno;
        /* If there are dirty pages in the page cache with page numbers greater
         ** than Pager.dbSize, this means sqlite3PagerTruncateImage() was called to
         ** make the file smaller (presumably by auto-vacuum code). Do not write
         ** any such pages to the file.
         ** Also, do not write out any page that has the PGHDR_DONT_WRITE flag
         ** set (set by sqlite3PagerDontWrite()).
         */
        if( pgno<=pPager->dbSize && 0==(pList->flags&PGHDR_DONT_WRITE) ){
            i64 offset = (pgno-1)*(i64)pPager->pageSize;   /* Offset to write */
            char *pData;                                   /* Data to write */
            
            assert( (pList->flags&PGHDR_NEED_SYNC)==0 );
            if( pList->pgno==1 ) pager_write_changecounter(pList);
            
            /* Encode the database */
            CODEC2(pPager, pList->pData, pgno, 6, return SQLITE_NOMEM_BKPT, pData);
            
            /* Write out the page data. */
            rc = sqlite3OsWrite(pPager->fd, pData, pPager->pageSize, offset);
            
            /* If page 1 was just written, update Pager.dbFileVers to match
             ** the value now stored in the database file. If writing this
             ** page caused the database file to grow, update dbFileSize.
             */
            if( pgno==1 ){
                memcpy(&pPager->dbFileVers, &pData[24], sizeof(pPager->dbFileVers));
            }
            if( pgno>pPager->dbFileSize ){
                pPager->dbFileSize = pgno;
            }
            pPager->aStat[PAGER_STAT_WRITE]++;
            
            /* Update any backup objects copying the contents of this pager. */
            sqlite3BackupUpdate(pPager->pBackup, pgno, (u8*)pList->pData);
            
            PAGERTRACE(("STORE %d page %d hash(%08x)\n",
                        PAGERID(pPager), pgno, pager_pagehash(pList)));
            IOTRACE(("PGOUT %p %d\n", pPager, pgno));
            PAGER_INCR(sqlite3_pager_writedb_count);
        }else{
            PAGERTRACE(("NOSTORE %d page %d\n", PAGERID(pPager), pgno));
        }
        pager_set_pagehash(pList);
        pList = pList->pDirty;
    }
    
    return rc;
}

