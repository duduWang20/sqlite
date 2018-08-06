#ifndef PCache1_struct_h
#define PCache1_struct_h
#include <stdio.h>

/*
 ** 2008 November 05
 ** This file implements the default page cache implementation (the
 ** sqlite3_pcache interface). It also contains part of the implementation
 ** of the SQLITE_CONFIG_PAGECACHE and sqlite3_release_memory() features.
 ** If the default page cache implementation is overridden, then neither of
 ** these two features are available.
 **
 ** A Page cache line looks like this:
 **  -------------------------------------------------------------
 **  |  database page content   |  PgHdr1  |  MemPage  |  PgHdr  |
 **  -------------------------------------------------------------
 **
 ** The database page content is up front (so that buffer overreads tend to
 ** flow harmlessly into the PgHdr1, MemPage, and PgHdr extensions).   MemPage
 ** is the extension added by the btree.c module containing information such
 ** as the database page number and how that database page is used.  PgHdr
 ** is added by the pcache.c layer and contains information used to keep track
 ** of which pages are "dirty".  PgHdr1 is an extension added by this
 ** module (pcache1.c).  The PgHdr1 header is a subclass of sqlite3_pcache_page.
 ** PgHdr1 contains information needed to look up a page by its page number.
 ** The superclass sqlite3_pcache_page.pBuf points to the start of the
 ** database page content and sqlite3_pcache_page.pExtra points to PgHdr.
 **
 ** The size of the extension (MemPage+PgHdr+PgHdr1) can be determined at
 ** runtime using sqlite3_config(SQLITE_CONFIG_PCACHE_HDRSZ, &size).  The
 ** sizes of the extensions sum to 272 bytes on x64 for 3.8.10, but this
 ** size can vary according to architecture, compile-time options, and
 ** SQLite library version number.
 **
 ** If SQLITE_PCACHE_SEPARATE_HEADER is defined, then the extension is obtained
 ** using a separate memory allocation from the database page content.  This
 ** seeks to overcome the "clownshoe" problem (also called "internal
 ** fragmentation" in academic literature) of allocating a few bytes more
 ** than a power of two with the memory allocator rounding up to the next
 ** power of two, and leaving the rounded-up space unused.
 **
 ** This module tracks pointers to PgHdr1 objects.  Only pcache.c communicates
 ** with this module.  Information is passed back and forth as PgHdr1 pointers.
 **
 ** The pcache.c and pager.c modules deal pointers to PgHdr objects.
 ** The btree.c module deals with pointers to MemPage objects.
 **
 ** SOURCE OF PAGE CACHE MEMORY:
 **
 ** Memory for a page might come from any of three sources:
 **
 **    (1)  The general-purpose memory allocator - sqlite3Malloc()
 **    (2)  Global page-cache memory provided using sqlite3_config() with
 **         SQLITE_CONFIG_PAGECACHE.
 **    (3)  PCache-local bulk allocation.
 **
 ** The third case is a chunk of heap memory (defaulting to 100 pages worth)
 ** that is allocated when the page cache is created.  The size of the local
 ** bulk allocation can be adjusted using
 **
 **     sqlite3_config(SQLITE_CONFIG_PAGECACHE, (void*)0, 0, N).
 **
 ** If N is positive, then N pages worth of memory are allocated using a single
 ** sqlite3Malloc() call and that memory is used for the first N pages allocated.
 ** Or if N is negative, then -1024*N bytes of memory are allocated and used
 ** for as many pages as can be accomodated.
 **
 ** Only one of (2) or (3) can be used.  Once the memory available to (2) or
 ** (3) is exhausted, subsequent allocations fail over to the general-purpose
 ** memory allocator (1).
 **
 ** Earlier versions of SQLite used only methods (1) and (2).  But experiments
 ** show that method (3) with N==100 provides about a 5% performance boost for
 ** common workloads.
 */
#include "sqliteInt.h"

typedef struct PCache1 PCache1;
typedef struct PgHdr1 PgHdr1;
typedef struct PgFreeslot PgFreeslot;
typedef struct PGroup PGroup;

/*
 ** Each cache entry is represented by an instance of the following
 ** structure. Unless SQLITE_PCACHE_SEPARATE_HEADER is defined, a buffer of
 ** PgHdr1.pCache->szPage bytes is allocated directly before this structure
 ** in memory.
 */
struct PgHdr1 {
    sqlite3_pcache_page page;      /* Base class. Must be first. pBuf & pExtra */
    unsigned int iKey;             /* Key value (page number) */
    u8 isBulkLocal;                /* This page from bulk local storage */
    u8 isAnchor;                   /* This is the PGroup.lru element */
    PgHdr1 *pNext;                 /* Next in hash table chain */
    PCache1 *pCache;               /* Cache that currently owns this page */
    PgHdr1 *pLruNext;              /* Next in LRU list of unpinned pages */
    PgHdr1 *pLruPrev;              /* Previous in LRU list of unpinned pages */
};

/*
 ** A page is pinned if it is no on the LRU list
 */
#define PAGE_IS_PINNED(p)    ((p)->pLruNext==0)
#define PAGE_IS_UNPINNED(p)  ((p)->pLruNext!=0)

/* Each page cache (or PCache) belongs to a PGroup.  A PGroup is a set
 ** of one or more PCaches that are able to recycle each other's unpinned
 ** pages when they are under memory pressure.  A PGroup is an instance of
 ** the following object.
 **
 ** This page cache implementation works in one of two modes:
 **
 **   (1)  Every PCache is the sole member of its own PGroup.  There is
 **        one PGroup per PCache.
 **
 **   (2)  There is a single global PGroup that all PCaches are a member
 **        of.
 **
 ** Mode 1 uses more memory (since PCache instances are not able to rob
 ** unused pages from other PCaches) but it also operates without a mutex,
 ** and is therefore often faster.  Mode 2 requires a mutex in order to be
 ** threadsafe, but recycles pages more efficiently.
 **
 ** For mode (1), PGroup.mutex is NULL.  For mode (2) there is only a single
 ** PGroup which is the pcache1.grp global variable and its mutex is
 ** SQLITE_MUTEX_STATIC_LRU.
 */
struct PGroup {
    sqlite3_mutex *mutex;          /* MUTEX_STATIC_LRU or NULL */
    unsigned int nMaxPage;         /* Sum of nMax for purgeable caches */
    unsigned int nMinPage;         /* Sum of nMin for purgeable caches */
    unsigned int mxPinned;         /* nMaxpage + 10 - nMinPage */
    unsigned int nPurgeable;       /* Number of purgeable pages allocated */
    PgHdr1 lru;                    /* The beginning and end of the LRU list */
};

/* Each page cache is an instance of the following object.  Every
 ** open database file (including each in-memory database and each
 ** temporary or transient database) has a single page cache which
 ** is an instance of this object.
 **
 ** Pointers to structures of this type are cast and returned as
 ** opaque sqlite3_pcache* handles.
 */
struct PCache1 {
    /* Cache configuration parameters. Page size (szPage) and the purgeable
     ** flag (bPurgeable) and the pnPurgeable pointer are all set when the
     ** cache is created and are never changed thereafter. nMax may be
     ** modified at any time by a call to the pcache1Cachesize() method.
     ** The PGroup mutex must be held when accessing nMax.
     */
    PGroup *pGroup;                     /* PGroup this cache belongs to */
    unsigned int *pnPurgeable;          /* Pointer to pGroup->nPurgeable */
    int szPage;                         /* Size of database content section */
    int szExtra;                        /* sizeof(MemPage)+sizeof(PgHdr) */
    int szAlloc;                        /* Total size of one pcache line */
    int bPurgeable;                     /* True if cache is purgeable */
    unsigned int nMin;                  /* Minimum number of pages reserved */
    unsigned int nMax;                  /* Configured "cache_size" value */
    unsigned int n90pct;                /* nMax*9/10 */
    unsigned int iMaxKey;               /* Largest key seen since xTruncate() */
    
    /* Hash table of all pages. The following variables may only be accessed
     ** when the accessor is holding the PGroup mutex.
     */
    unsigned int nRecyclable;           /* Number of pages in the LRU list */
    unsigned int nPage;                 /* Total number of pages in apHash */
    unsigned int nHash;                 /* Number of slots in apHash[] */
    PgHdr1 **apHash;                    /* Hash table for fast lookup by key */
    PgHdr1 *pFree;                      /* List of unused pcache-local pages */
    void *pBulk;                        /* Bulk memory used by pcache-local */
};

/*
 ** Free slots in the allocator used to divide up the global page cache
 ** buffer provided using the SQLITE_CONFIG_PAGECACHE mechanism.
 */
struct PgFreeslot {
    PgFreeslot *pNext;  /* Next free slot */
};

/*
 ** Global data used by this cache.
 */
static SQLITE_WSD struct PCacheGlobal {
    PGroup grp;                    /* The global PGroup for mode (2) */
    
    /* Variables related to SQLITE_CONFIG_PAGECACHE settings.  The
     ** szSlot, nSlot, pStart, pEnd, nReserve, and isInit values are all
     ** fixed at sqlite3_initialize() time and do not require mutex protection.
     ** The nFreeSlot and pFree values do require mutex protection.
     */
    int isInit;                    /* True if initialized */
    int separateCache;             /* Use a new PGroup for each PCache */
    int nInitPage;                 /* Initial bulk allocation size */
    int szSlot;                    /* Size of each free slot */
    int nSlot;                     /* The number of pcache slots */
    int nReserve;                  /* Try to keep nFreeSlot above this */
    void *pStart, *pEnd;           /* Bounds of global page cache memory */
    /* Above requires no mutex.  Use mutex below for variable that follow. */
    sqlite3_mutex *mutex;          /* Mutex for accessing the following: */
    PgFreeslot *pFree;             /* Free page blocks */
    int nFreeSlot;                 /* Number of unused pcache slots */
    /* The following value requires a mutex to change.  We skip the mutex on
     ** reading because (1) most platforms read a 32-bit integer atomically and
     ** (2) even if an incorrect value is read, no great harm is done since this
     ** is really just an optimization. */
    int bUnderPressure;            /* True if low on PAGECACHE memory */
} pcache1_g;

#endif /* PCache1_struct_h */


