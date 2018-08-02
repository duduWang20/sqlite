//
//  PCacher-struct.h
//  sqlite
//
//  Created by wangjufan on 2018/8/2.
//  Copyright © 2018 wangjufan. All rights reserved.
//

#ifndef PCacher_struct_h
#define PCacher_struct_h

/*
 ** A complete page cache is an instance of this structure.  Every
 ** entry in the cache holds a single page of the database file.  The
 ** btree layer only operates on the cached copy of the database pages.
 **
 ** A page cache entry is "clean" if it exactly matches what is currently
 ** on disk.  A page is "dirty" if it has been modified and needs to be
 ** persisted to disk.
 **
 ** pDirty, pDirtyTail, pSynced:
 **   All dirty pages are linked into the doubly linked list using
 **   PgHdr.pDirtyNext and pDirtyPrev. The list is maintained in LRU order
 **   such that p was added to the list more recently than p->pDirtyNext.
 **   PCache.pDirty points to the first (newest) element in the list and
 **   pDirtyTail to the last (oldest).
 **
 **   The PCache.pSynced variable is used to optimize searching for a dirty
 **   page to eject from the cache mid-transaction. It is better to eject
 **   a page that does not require a journal sync than one that does.
 **   Therefore, pSynced is maintained to that it *almost* always points
 **   to either the oldest page in the pDirty/pDirtyTail list that has a
 **   clear PGHDR_NEED_SYNC flag or to a page that is older than this one
 **   (so that the right page to eject can be found by following pDirtyPrev
 **   pointers).
 */
struct PCache {//Least Recently Used (LRU)   Least Frequently Used (LFU)
    PgHdr *pDirty, *pDirtyTail;         /* List of dirty pages in LRU order */
    PgHdr *pSynced;                     /* Last synced page in dirty page list */
    int nRefSum;                        /* Sum of ref counts over all pages */
    int szCache;                        /* Configured cache size */
    int szSpill;                        /* Size before spilling occurs */
    int szPage;                         /* Size of every page in this cache */
    int szExtra;                        /* Size of extra space for each page */
    u8 bPurgeable;                      /* True if pages are on backing store */
    u8 eCreate;                         /* eCreate value for for xFetch() */
    int (*xStress)(void*,PgHdr*);       /* Call to try make a page clean */
    void *pStress;                      /* Argument to xStress */
    sqlite3_pcache *pCache;             /* Pluggable cache module */
};



typedef struct PgHdr PgHdr;
typedef struct PCache PCache;
//      one cacher
//op    one page
//over  one pghdr wjf

//struct sqlite3_pcache_page {
//    void *pBuf;        /* The content of the page */
//    void *pExtra;      /* Extra information associated with the page */
//};
/*
 ** Every page in the cache is controlled by an instance of the following structure.
 */
struct PgHdr {
    sqlite3_pcache_page *pPage;    /* Pcache object page handle */
    void *pData;                   /* Page data */
    void *pExtra;                  /* Extra content */
    
    PgHdr *pDirty;                 /* Transient list of dirty sorted by pgno */
    Pgno pgno;                     /* Page number for this page */
    
    PCache *pCache;                /* PRIVATE: Cache that owns this page */
    Pager *pPager;                 /* The pager this page is part of */
#ifdef SQLITE_CHECK_PAGES
    u32 pageHash;                  /* Hash of page content */
#endif
    u16 flags;                     /* PGHDR flags defined below */
    
    /**********************************************************************
     ** Elements above, except pCache, are public.  All that follow are
     ** private to pcache.c and should not be accessed by other modules.
     ** pCache is grouped with the public elements for efficiency.
     */
    i16 nRef;                      /* Number of users of this page */
    PgHdr *pDirtyNext;             /* Next element in list of dirty pages */
    PgHdr *pDirtyPrev;             /* Previous element in list of dirty pages */
    /* NB: pDirtyNext and pDirtyPrev are undefined if the
     ** PgHdr object is not dirty */
};


#endif /* PCacher_struct_h */
