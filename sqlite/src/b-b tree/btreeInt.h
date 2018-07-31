
#include "sqliteInt.h"

/* The following value is the maximum cell size assuming a maximum page
** size give above.
*/
#define MX_CELL_SIZE(pBt)  ((int)(pBt->pageSize-8))。//最小 512
/* The maximum number of cells on a single page of the database.
** This assumes a minimum cell size of 6 bytes  (4 bytes for the cell itself
**      plus 2 bytes for the index to the cell in the page header).
** Such small cells will be rare, but they are possible.
*/
#define MX_CELL(pBt) ((pBt->pageSize-8)/6)// （512 - 8）/ 6 = 86

/* Forward declarations */
typedef struct MemPage MemPage;
typedef struct BtLock BtLock;
typedef struct CellInfo CellInfo;

/*
** This is a magic string that appears at the beginning of every
** SQLite database in order to identify the file as a real database.
**
** You can change this value at compile-time by specifying a
** -DSQLITE_FILE_HEADER="..." on the compiler command-line.  The
** header must be exactly 16 bytes including the zero-terminator so
** the string itself should be 15 characters long.  If you change
** the header, then your custom library will not be able to read 
** databases generated by the standard tools and the standard tools
** will not be able to read databases created by your custom library.
*/
#ifndef SQLITE_FILE_HEADER /* 123456789 123456 */
#  define SQLITE_FILE_HEADER "SQLite format 3"
#endif
/*
** Page type flags.  An ORed combination of these flags appear as the
** first byte of on-disk image of every BTree page.
*/
#define PTF_INTKEY    0x01
#define PTF_ZERODATA  0x02
#define PTF_LEAFDATA  0x04
#define PTF_LEAF      0x08

/*
** An instance of this object stores
** information about each a single database page that has been loaded into memory. 内存中单页信息
 * The information in this object is derived from the raw on-disk page content.
**
** As each database page is loaded into memory,
** the pager allocats an instance of this object and zeros the first 8 bytes.
** (This is the "extra" information associated with each page of the pager.)
**
** Access to all fields of this structure is controlled by the mutex
** stored in MemPage.pBt->mutex.
*/
struct MemPage {//page description
  u8 isInit;           /* True if previously initialized. MUST BE FIRST! */
  u8 bBusy;            /* Prevent endless loops on corrupt database files */
  u8 intKey;           /* True if table b-trees.  False for index b-trees */
  u8 intKeyLeaf;       /* True if the leaf of an intKey table */
  Pgno pgno;           /* Page number for this page */              /////typedef u32 Pgno;
  /* Only the first 8 bytes (above) are zeroed by pager.c when a new page
  ** is allocated. All fields that follow must be initialized before use */
  u8 leaf;             /* True if a leaf page */
  u8 hdrOffset;        /* 100 for page 1.  0 otherwise */
  u8 childPtrSize;     /* 0 if leaf==1.  4 if leaf==0 */
  u8 max1bytePayload;  /* min(maxLocal,127) */
  u8 nOverflow;        /* Number of overflow cell bodies in aCell[] */
  u16 maxLocal;        /* Copy of BtShared.maxLocal or BtShared.maxLeaf */
  u16 minLocal;        /* Copy of BtShared.minLocal or BtShared.minLeaf */
  u16 cellOffset;      /* Index in aData of first cell pointer */
  u16 nFree;           /* Number of free bytes on the page */
  u16 nCell;           /* Number of cells on this page, local and ovfl */
  u16 maskPage;        /* Mask for page offset */
  u16 aiOvfl[4];       /* Insert the i-th overflow cell before the aiOvfl-th
                       ** non-overflow cell */
  u8 *apOvfl[4];       /* Pointers to the body of overflow cells */
  BtShared *pBt;       /* Pointer to BtShared that this page is part of */   //////////////////
  u8 *aData;           /* Pointer to disk image of the page data */
  u8 *aDataEnd;        /* One byte past the end of usable data */
  u8 *aCellIdx;        /* The cell index area */
  u8 *aDataOfst;       /* Same as aData for leaves.  aData+4 for interior */
  DbPage *pDbPage;     /* Pager page handle */                      //////////////////
  u16 (*xCellSize)(MemPage*,u8*);             /* cellSizePtr method */
  void (*xParseCell)(MemPage*,u8*,CellInfo*); /* btreeParseCell method */
};

/*
** A linked list of the following structures is stored at BtShared.pLock.
** Locks are added (or upgraded from READ_LOCK to WRITE_LOCK) when a cursor 
** is opened on the table with root page BtShared.iTable. Locks are removed
** from this list when a transaction is committed or rolled back, or when
** a btree handle is closed.
*/
struct BtLock {
  Btree *pBtree;        /* Btree handle holding this lock */
  Pgno iTable;          /* Root page of table */
  u8 eLock;             /* READ_LOCK or WRITE_LOCK */
  BtLock *pNext;        /* Next in BtShared.pLock list */
};

/* Candidate values for BtLock.eLock */
#define READ_LOCK     1
#define WRITE_LOCK    2

/* A Btree handle
**
** A database connection contains a pointer to an instance of
** this object for every database file that it has open.  This structure
** is opaque to the database connection.  The database connection cannot
** see the internals of this structure and only deals with pointers to
** this structure.
**
** For some database files, the same underlying database cache might be 
** shared between multiple connections.  In that case, each connection
** has it own instance of this object.  But each instance of this object
** points to the same BtShared object.  The database cache and the
** schema associated with the database file are all contained within
** the BtShared object.
**
** All fields in this structure are accessed under sqlite3.mutex.
** The pBt pointer itself may not be changed while there exists cursors 
** in the referenced BtShared that point back to this Btree since those
** cursors have to go through this Btree to find their BtShared and
** they often do so without holding sqlite3.mutex.
*/
struct Btree {
  sqlite3 *db;       /* The database connection holding this btree */
  BtShared *pBt;     /* Sharable content of this btree */
  u8 inTrans;        /* TRANS_NONE, TRANS_READ or TRANS_WRITE */
  u8 sharable;       /* True if we can share pBt with another db */
  u8 locked;         /* True if db currently has pBt locked */
  u8 hasIncrblobCur; /* True if there are one or more Incrblob cursors */
  int wantToLock;    /* Number of nested calls to sqlite3BtreeEnter() */
  int nBackup;       /* Number of backup operations reading this btree */
  u32 iDataVersion;  /* Combines with pBt->pPager->iDataVersion */
  Btree *pNext;      /* List of other sharable Btrees from the same db */
  Btree *pPrev;      /* Back pointer of the same list */
#ifndef SQLITE_OMIT_SHARED_CACHE
  BtLock lock;       /* Object used to lock page 1 */
#endif
};

/*
** Btree.inTrans may take one of the following values.
**
** If the shared-data extension is enabled, there may be multiple users
** of the Btree structure. At most one of these may open a write transaction,
** but any number may have active read transactions.
*/
#define TRANS_NONE  0
#define TRANS_READ  1
#define TRANS_WRITE 2

/*
** An instance of this object represents a single database file.
** 
** A single database file can be in use at the same time by two
** or more database connections.  When two or more connections are
** sharing the same database file, each connection has it own
** private Btree object for the file and each of those Btrees points
** to this one BtShared object.  BtShared.nRef is the number of
** connections currently sharing this database file.
**
** Fields in this structure are accessed under the BtShared.mutex
** mutex, except for nRef and pNext which are accessed under the
** global SQLITE_MUTEX_STATIC_MASTER mutex.  The pPager field
** may not be modified once it is initially set as long as nRef>0.
** The pSchema field may be set once under BtShared.mutex and
** thereafter is unchanged as long as nRef>0.
**
** isPending:
**
**   If a BtShared client fails to obtain a write-lock on a database
**   table (because there exists one or more read-locks on the table),
**   the shared-cache enters 'pending-lock' state and isPending is
**   set to true.
**
**   The shared-cache leaves the 'pending lock' state when either of
**   the following occur:
**
**     1) The current writer (BtShared.pWriter) concludes its transaction, OR
**     2) The number of locks held by other connections drops to zero.
**
**   while in the 'pending-lock' state, no connection may start a new
**   transaction.
**
**   This feature is included to help prevent writer-starvation.
*/
struct BtShared {
  Pager *pPager;        /* The page cache */
  sqlite3 *db;          /* Database connection currently using this Btree */
  BtCursor *pCursor;    /* A list of all open cursors */
  MemPage *pPage1;      /* First page of the database */
  u8 openFlags;         /* Flags to sqlite3BtreeOpen() */
#ifndef SQLITE_OMIT_AUTOVACUUM
  u8 autoVacuum;        /* True if auto-vacuum is enabled */
  u8 incrVacuum;        /* True if incr-vacuum is enabled */
  u8 bDoTruncate;       /* True to truncate db on commit */
#endif
  u8 inTransaction;     /* Transaction state */
  u8 max1bytePayload;   /* Maximum first byte of cell for a 1-byte payload */
#ifdef SQLITE_HAS_CODEC
  u8 optimalReserve;    /* Desired amount of reserved space per page */
#endif
  u16 btsFlags;         /* Boolean parameters.  See BTS_* macros below */
  u16 maxLocal;         /* Maximum local payload in non-LEAFDATA tables */
  u16 minLocal;         /* Minimum local payload in non-LEAFDATA tables */
  u16 maxLeaf;          /* Maximum local payload in a LEAFDATA table */
  u16 minLeaf;          /* Minimum local payload in a LEAFDATA table */
  u32 pageSize;         /* Total number of bytes on a page */
  u32 usableSize;       /* Number of usable bytes on each page */
  int nTransaction;     /* Number of open transactions (read + write) */
  u32 nPage;            /* Number of pages in the database */
  void *pSchema;        /* Pointer to space allocated by sqlite3BtreeSchema() */
  void (*xFreeSchema)(void*);  /* Destructor for BtShared.pSchema */
  sqlite3_mutex *mutex; /* Non-recursive mutex required to access this object */
  Bitvec *pHasContent;  /* Set of pages moved to free-list this transaction */
#ifndef SQLITE_OMIT_SHARED_CACHE
  int nRef;             /* Number of references to this structure */
  BtShared *pNext;      /* Next on a list of sharable BtShared structs */
  BtLock *pLock;        /* List of locks held on this shared-btree struct */
  Btree *pWriter;       /* Btree with currently open write transaction */
#endif
  u8 *pTmpSpace;        /* Temp space sufficient to hold a single cell */
};

/*
** Allowed values for BtShared.btsFlags
*/
#define BTS_READ_ONLY        0x0001   /* Underlying file is readonly */
#define BTS_PAGESIZE_FIXED   0x0002   /* Page size can no longer be changed */
#define BTS_SECURE_DELETE    0x0004   /* PRAGMA secure_delete is enabled */
#define BTS_OVERWRITE        0x0008   /* Overwrite deleted content with zeros */
#define BTS_FAST_SECURE      0x000c   /* Combination of the previous two */
#define BTS_INITIALLY_EMPTY  0x0010   /* Database was empty at trans start */
#define BTS_NO_WAL           0x0020   /* Do not open write-ahead-log files */
#define BTS_EXCLUSIVE        0x0040   /* pWriter has an exclusive lock */
#define BTS_PENDING          0x0080   /* Waiting for read-locks to clear */

/*
** An instance of the following structure is used to hold information
** about a cell.  The parseCellPtr() function fills in this structure
** based on information extract from the raw disk page.
*/
struct CellInfo {
  i64 nKey;      /* The key for INTKEY tables, or nPayload otherwise */
  u8 *pPayload;  /* Pointer to the start of payload */
  u32 nPayload;  /* Bytes of payload */
  u16 nLocal;    /* Amount of payload held locally, not on overflow */
  u16 nSize;     /* Size of the cell content on the main b-tree page */
};

/*
** Maximum depth of an SQLite B-Tree structure. Any B-Tree deeper than
** this will be declared corrupt. This value is calculated based on a
** maximum database size of 2^31 pages a minimum fanout of 2 for a
** root-node and 3 for all other internal nodes.
**
** If a tree that appears to be taller than this is encountered, it is
** assumed that the database is corrupt.
*/
#define BTCURSOR_MAX_DEPTH 20

/*
** A cursor is a pointer to a particular entry within a particular
** b-tree within a database file.
**
** The entry is identified by its MemPage and the index in
** MemPage.aCell[] of the entry.
**
** A single database file can be shared by two more database connections,
** but cursors cannot be shared.  Each cursor is associated with a
** particular database connection identified BtCursor.pBtree.db.
**
** Fields in this structure are accessed under the BtShared.mutex
** found at self->pBt->mutex. 
**
** skipNext meaning:
**    eState==SKIPNEXT && skipNext>0:  Next sqlite3BtreeNext() is no-op.
**    eState==SKIPNEXT && skipNext<0:  Next sqlite3BtreePrevious() is no-op.
**    eState==FAULT:                   Cursor fault with skipNext as error code.
*/
struct BtCursor {
  u8 eState;                /* One of the CURSOR_XXX constants (see below) */
  u8 curFlags;              /* zero or more BTCF_* flags defined below */
  u8 curPagerFlags;         /* Flags to send to sqlite3PagerGet() */
  u8 hints;                 /* As configured by CursorSetHints() */
  int skipNext;    /* Prev() is noop if negative. Next() is noop if positive.
                   ** Error code if eState==CURSOR_FAULT */
  Btree *pBtree;            /* The Btree to which this cursor belongs */
  Pgno *aOverflow;          /* Cache of overflow page locations */
  void *pKey;               /* Saved key that was cursor last known position */
  /* All fields above are zeroed when the cursor is allocated.  See
  ** sqlite3BtreeCursorZero().  Fields that follow must be manually
  ** initialized. */
#define BTCURSOR_FIRST_UNINIT pBt   /* Name of first uninitialized field */
  BtShared *pBt;            /* The BtShared this cursor points to */
  BtCursor *pNext;          /* Forms a linked list of all cursors */
  CellInfo info;            /* A parse of the cell we are pointing at */
  i64 nKey;                 /* Size of pKey, or last integer key */
  Pgno pgnoRoot;            /* The root page of this tree */
  i8 iPage;                 /* Index of current page in apPage */
  u8 curIntKey;             /* Value of apPage[0]->intKey */
  u16 ix;                   /* Current index for apPage[iPage] */
  u16 aiIdx[BTCURSOR_MAX_DEPTH-1];     /* Current index in apPage[i] */
  struct KeyInfo *pKeyInfo;            /* Arg passed to comparison function */
  MemPage *pPage;                        /* Current page */
  MemPage *apPage[BTCURSOR_MAX_DEPTH-1]; /* Stack of parents of current page */
};


    
/*
** Legal values for BtCursor.curFlags
*/
#define BTCF_WriteFlag    0x01   /* True if a write cursor */
#define BTCF_ValidNKey    0x02   /* True if info.nKey is valid */
#define BTCF_ValidOvfl    0x04   /* True if aOverflow is valid */
#define BTCF_AtLast       0x08   /* Cursor is pointing ot the last entry */
#define BTCF_Incrblob     0x10   /* True if an incremental I/O handle */
#define BTCF_Multiple     0x20   /* Maybe another cursor on the same btree */

/*
** Potential values for BtCursor.eState.
**
** CURSOR_INVALID:
**   Cursor does not point to a valid entry. This can happen (for example) 
**   because the table is empty or because BtreeCursorFirst() has not been
**   called.
**
** CURSOR_VALID:
**   Cursor points to a valid entry. getPayload() etc. may be called.
**
** CURSOR_SKIPNEXT:
**   Cursor is valid except that the Cursor.skipNext field is non-zero
**   indicating that the next sqlite3BtreeNext() or sqlite3BtreePrevious()
**   operation should be a no-op.
**
** CURSOR_REQUIRESEEK:
**   The table that this cursor was opened on still exists, but has been 
**   modified since the cursor was last used. The cursor position is saved
**   in variables BtCursor.pKey and BtCursor.nKey. When a cursor is in 
**   this state, restoreCursorPosition() can be called to attempt to
**   seek the cursor to the saved position.
**
** CURSOR_FAULT:
**   An unrecoverable error (an I/O error or a malloc failure) has occurred
**   on a different connection that shares the BtShared cache with this
**   cursor.  The error has left the cache in an inconsistent state.
**   Do nothing else with this cursor.  Any attempt to use the cursor
**   should return the error code stored in BtCursor.skipNext
*/
#define CURSOR_VALID             0
#define CURSOR_INVALID           1
#define CURSOR_SKIPNEXT          2
#define CURSOR_REQUIRESEEK       3
#define CURSOR_FAULT             4

/* 
** The database page the PENDING_BYTE occupies. This page is never used.
*/
# define PENDING_BYTE_PAGE(pBt) PAGER_MJ_PGNO(pBt)

/*
** These macros define the location of the pointer-map entry for a 
** database page. The first argument to each is the number of usable
** bytes on each page of the database (often 1024). The second is the
** page number to look up in the pointer map.
**
** PTRMAP_PAGENO returns the database page number of the pointer-map
** page that stores the required pointer. PTRMAP_PTROFFSET returns
** the offset of the requested map entry.
**
** If the pgno argument passed to PTRMAP_PAGENO is a pointer-map page,
** then pgno is returned. So (pgno==PTRMAP_PAGENO(pgsz, pgno)) can be
** used to test if pgno is a pointer-map page. PTRMAP_ISPAGE implements
** this test.
*/
#define PTRMAP_PAGENO(pBt, pgno) ptrmapPageno(pBt, pgno)
#define PTRMAP_PTROFFSET(pgptrmap, pgno) (5*(pgno-pgptrmap-1))
#define PTRMAP_ISPAGE(pBt, pgno) (PTRMAP_PAGENO((pBt),(pgno))==(pgno))

/*
** The pointer map is a lookup table that identifies the parent page for
** each child page in the database file.  The parent page is the page that
** contains a pointer to the child.  Every page in the database contains
** 0 or 1 parent pages.  (In this context 'database page' refers
** to any page that is not part of the pointer map itself.)  Each pointer map
** entry consists of a single byte 'type' and a 4 byte parent page number.
** The PTRMAP_XXX identifiers below are the valid types.
**
** The purpose of the pointer map is to facility moving pages from one
** position in the file to another as part of autovacuum.  When a page
** is moved, the pointer in its parent must be updated to point to the
** new location.  The pointer map is used to locate the parent page quickly.
**
** PTRMAP_ROOTPAGE: The database page is a root-page. The page-number is not
**                  used in this case.
**
** PTRMAP_FREEPAGE: The database page is an unused (free) page. The page-number 
**                  is not used in this case.
**
** PTRMAP_OVERFLOW1: The database page is the first page in a list of 
**                   overflow pages. The page number identifies the page that
**                   contains the cell with a pointer to this overflow page.
**
** PTRMAP_OVERFLOW2: The database page is the second or later page in a list of
**                   overflow pages. The page-number identifies the previous
**                   page in the overflow page list.
**
** PTRMAP_BTREE: The database page is a non-root btree page. The page number
**               identifies the parent page in the btree.
*/
#define PTRMAP_ROOTPAGE 1
#define PTRMAP_FREEPAGE 2
#define PTRMAP_OVERFLOW1 3
#define PTRMAP_OVERFLOW2 4
#define PTRMAP_BTREE 5

/* A bunch of assert() statements to check the transaction state variables
** of handle p (type Btree*) are internally consistent.
*/
#define btreeIntegrity(p) \
  assert( p->pBt->inTransaction!=TRANS_NONE || p->pBt->nTransaction==0 ); \
  assert( p->pBt->inTransaction>=p->inTrans ); 


/*
** The ISAUTOVACUUM macro is used within balance_nonroot() to determine
** if the database supports auto-vacuum or not. Because it is used
** within an expression that is an argument to another macro 
** (sqliteMallocRaw), it is not possible to use conditional compilation.
** So, this macro is defined instead.
*/
#ifndef SQLITE_OMIT_AUTOVACUUM
#define ISAUTOVACUUM (pBt->autoVacuum)
#else
#define ISAUTOVACUUM 0
#endif

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
** Routines to read or write a two- and four-byte big-endian integer values.
*/
#define get2byte(x)   ((x)[0]<<8 | (x)[1])
#define put2byte(p,v) ((p)[0] = (u8)((v)>>8), (p)[1] = (u8)(v))
#define get4byte sqlite3Get4byte
#define put4byte sqlite3Put4byte

/*
** get2byteAligned(), unlike get2byte(), requires that its argument point to a
** two-byte aligned address.  get2bytea() is only used for accessing the
** cell addresses in a btree header.
*/
#if SQLITE_BYTEORDER==4321
# define get2byteAligned(x)  (*(u16*)(x))
#elif SQLITE_BYTEORDER==1234 && GCC_VERSION>=4008000
# define get2byteAligned(x)  __builtin_bswap16(*(u16*)(x))
#elif SQLITE_BYTEORDER==1234 && MSVC_VERSION>=1300
# define get2byteAligned(x)  _byteswap_ushort(*(u16*)(x))
#else
# define get2byteAligned(x)  ((x)[0]<<8 | (x)[1])
#endif
