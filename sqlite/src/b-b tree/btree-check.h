//
//  btree-check.h
//  sqlite
//
//  Created by wangjufan on 2018/7/31.
//  Copyright © 2018 wangjufan. All rights reserved.
//

#ifndef btree_check_h
#define btree_check_h

/*
 ** This routine does a complete check 完整性检测 of the given BTree file.
 
 ** aRoot[] is an array of pages numbers were each page number is the root page of
 ** a table.  nRoot is the number of entries in aRoot.表的根页号
 ** A read-only or read-write transaction must be opened before calling
 ** this function.
 **
 
 ** Write the number of error seen in *pnErr.  Except for some memory
 ** allocation errors,  an error message held in memory obtained from
 ** malloc is returned if *pnErr is non-zero.  If *pnErr==0 then NULL is
 ** returned.  If a memory allocation error occurs, NULL is returned.
 */
char *sqlite3BtreeIntegrityCheck(
     Btree *p,     /* The btree to be checked */
     
     int *aRoot,   /* An array of root pages numbers for individual trees */
     int nRoot,    /* Number of entries in aRoot[] */
     
     int mxErr,    /* Stop reporting errors after this many */
     int *pnErr    /* Write number of errors seen to this variable */
);

/*
 ** This structure is passed around through all the sanity checking routines
 ** in order to keep track of some global state information.
 **
 ** The aRef[] array is allocated so that there is --- 1 bit for each page  --- in the database.
 ** As the integrity-check proceeds, for each page used in the database the corresponding bit is set.
 ** This allows integrity-check to detect pages that are used twice and orphaned pages
 ** (both of which  indicate corruption).
 */
typedef struct IntegrityCk IntegrityCk;
struct IntegrityCk {
    BtShared *pBt;    /* The tree being checked out */
    Pager *pPager;    /* The associated pager.  Also accessible by pBt->pPager */
    u8 *aPgRef;       /* 1 bit per page in the db (see above) */
    Pgno nPage;       /* Number of pages in the database */
    int mxErr;        /* Stop accumulating errors when this reaches zero */
    int nErr;         /* Number of messages written to zErrMsg so far */
    int mallocFailed; /* A memory allocation error has occurred */
    const char *zPfx; /* Error message prefix */
    int v1, v2;       /* Values for up to two %d fields in zPfx */
    StrAccum errMsg;  /* Accumulate the error message text here */
    u32 *heap;        /* Min-heap used for analyzing cell coverage */
};


/*
 ** Do various sanity checks on a single page of a tree.  Return
 ** the tree depth.  Root pages return 0.  Parents of root pages
 ** return 1, and so forth.
 ** These checks are done:
 **      1.  Make sure that cells and freeblocks do not overlap
 **          but combine to completely cover the page.
 **      2.  Make sure integer cell keys are in order.
 **      3.  Check the integrity of overflow pages.
 **      4.  Recursively call checkTreePage on all children.
 **      5.  Verify that the depth of all children is the same.
 */
static int checkTreePage(
                         IntegrityCk *pCheck,  /* Context for the sanity check */
                         int iPage,            /* Page number of the page to check */
                         i64 *piMinKey,        /* Write minimum integer primary key here */
                         i64 maxKey            /* Error if integer primary key greater than this */
);

#endif /* btree_check_h */
