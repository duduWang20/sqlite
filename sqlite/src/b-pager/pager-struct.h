#ifndef pager_struct_h
#define pager_struct_h

/******************* NOTES ON THE DESIGN OF THE PAGER ************************
 **
 ** This comment block describes invariants that hold when using a rollback
 ** journal.  These invariants do not apply for journal_mode=WAL,
 ** journal_mode=MEMORY, or journal_mode=OFF.
 **
 ** Within this comment block, a page is deemed to have been synced
 ** automatically as soon as it is written when PRAGMA synchronous=OFF.
 ** Otherwise, the page is not synced until the xSync method of the VFS
 ** is called successfully on the file containing the page.
 **
 ** Definition:  A page of the database file is said to be "overwriteable" if
 ** one or more of the following are true about the page:
 **
 **     (a)  The original content of the page as it was at the beginning of
 **          the transaction has been written into the rollback journal and
 **          synced.
 **
 **     (b)  The page was a freelist leaf page at the start of the transaction.
 **
 **     (c)  The page number is greater than the largest page that existed in
 **          the database file at the start of the transaction.
 **
 ** (1) A page of the database file is never overwritten unless one of the
 **     following are true:
 **
 **     (a) The page and all other pages on the same sector are overwriteable.
 **
 **     (b) The atomic page write optimization is enabled, and the entire
 **         transaction other than the update of the transaction sequence
 **         number consists of a single page change.
 **
 ** (2) The content of a page written into the rollback journal exactly matches
 **     both the content in the database when the rollback journal was written
 **     and the content in the database at the beginning of the current
 **     transaction.
 **
 ** (3) Writes to the database file are an integer multiple of the page size
 **     in length and are aligned on a page boundary.
 **
 ** (4) Reads from the database file are either aligned on a page boundary and
 **     an integer multiple of the page size in length or are taken from the
 **     first 100 bytes of the database file.
 **
 ** (5) All writes to the database file are synced prior to the rollback journal
 **     being deleted, truncated, or zeroed.
 **
 ** (6) If a master journal file is used, then all writes to the database file
 **     are synced prior to the master journal being deleted.
 **
 ** Definition: Two databases (or the same database at two points it time)
 ** are said to be "logically equivalent" if they give the same answer to
 ** all queries.  Note in particular the content of freelist leaf
 ** pages can be changed arbitrarily without affecting the logical equivalence
 ** of the database.
 **
 ** (7) At any time, if any subset, including the empty set and the total set,
 **     of the unsynced changes to a rollback journal are removed and the
 **     journal is rolled back, the resulting database file will be logically
 **     equivalent to the database file at the beginning of the transaction.
 **
 ** (8) When a transaction is rolled back, the xTruncate method of the VFS
 **     is called to restore the database file to the same size it was at
 **     the beginning of the transaction.  (In some VFSes, the xTruncate
 **     method is a no-op, but that does not change the fact the SQLite will
 **     invoke it.)
 **
 ** (9) Whenever the database file is modified, at least one bit in the range
 **     of bytes from 24 through 39 inclusive will be changed prior to releasing
 **     the EXCLUSIVE lock, thus signaling other connections on the same
 **     database to flush their caches.
 **
 ** (10) The pattern of bits in bytes 24 through 39 shall not repeat in less
 **      than one billion transactions.
 **
 ** (11) A database file is well-formed at the beginning and at the conclusion
 **      of every transaction.
 **
 ** (12) An EXCLUSIVE lock is held on the database file when writing to
 **      the database file.
 **
 ** (13) A SHARED lock is held on the database file while reading any
 **      content out of the database file.
 **
 ******************************************************************************/

/*
 ** Macros for troubleshooting.  Normally turned off
 */
#if 0
int sqlite3PagerTrace=1;  /* True to enable tracing */
#define sqlite3DebugPrintf printf
#define PAGERTRACE(X)     if( sqlite3PagerTrace ){ sqlite3DebugPrintf X; }
#else
#define PAGERTRACE(X)
#endif

/*
 ** The following two macros are used within the PAGERTRACE() macros above
 ** to print out file-descriptors.
 **
 ** PAGERID() takes a pointer to a Pager struct as its argument. The
 ** associated file-descriptor is returned. FILEHANDLEID() takes an sqlite3_file
 ** struct as its argument.
 */
#define PAGERID(p) (SQLITE_PTR_TO_INT(p->fd))
#define FILEHANDLEID(fd) (SQLITE_PTR_TO_INT(fd))

/*
 ** The Pager.eState variable stores the current 'state' of a pager. A
 ** pager may be in any one of the seven states shown in the following
 ** state diagram.
 **
 **                            OPEN <------+------+
 **                              |         |      |
 **                              V         |      |
 **               +---------> READER-------+      |
 **               |              |                |
 **               |              V                |
 **               |<-------WRITER_LOCKED------> ERROR
 **               |              |                ^
 **               |              V                |
 **               |<------WRITER_CACHEMOD-------->|
 **               |              |                |
 **               |              V                |
 **               |<-------WRITER_DBMOD---------->|
 **               |              |                |
 **               |              V                |
 **               +<------WRITER_FINISHED-------->+
 **
 ** List of state transitions and the C [function] that performs each:
 **
 **   OPEN              -> READER              [sqlite3PagerSharedLock]
 **   READER            -> OPEN                [pager_unlock]
 **
 **   READER            -> WRITER_LOCKED       [sqlite3PagerBegin]
 **   WRITER_LOCKED     -> WRITER_CACHEMOD     [pager_open_journal]
 **   WRITER_CACHEMOD   -> WRITER_DBMOD        [syncJournal]
 **   WRITER_DBMOD      -> WRITER_FINISHED     [sqlite3PagerCommitPhaseOne]
 **   WRITER_***        -> READER              [pager_end_transaction]
 **
 **   WRITER_***        -> ERROR               [pager_error]
 **   ERROR             -> OPEN                [pager_unlock]
 **
 **
 **  OPEN:
 **
 **    The pager starts up in this state. Nothing is guaranteed in this
 **    state - the file may or may not be locked and the database size is
 **    unknown. The database may not be read or written.
 **
 **    * No read or write transaction is active.
 **    * Any lock, or no lock at all, may be held on the database file.
 **    * The dbSize, dbOrigSize and dbFileSize variables may not be trusted.
 **
 **  READER:
 **
 **    In this state all the requirements for reading the database in
 **    rollback (non-WAL) mode are met. Unless the pager is (or recently
 **    was) in exclusive-locking mode, a user-level read transaction is
 **    open. The database size is known in this state.
 **
 **    A connection running with locking_mode=normal enters this state when
 **    it opens a read-transaction on the database and returns to state
 **    OPEN after the read-transaction is completed. However a connection
 **    running in locking_mode=exclusive (including temp databases) remains in
 **    this state even after the read-transaction is closed. The only way
 **    a locking_mode=exclusive connection can transition from READER to OPEN
 **    is via the ERROR state (see below).
 **
 **    * A read transaction may be active (but a write-transaction cannot).
 **    * A SHARED or greater lock is held on the database file.
 **    * The dbSize variable may be trusted (even if a user-level read
 **      transaction is not active). The dbOrigSize and dbFileSize variables
 **      may not be trusted at this point.
 **    * If the database is a WAL database, then the WAL connection is open.
 **    * Even if a read-transaction is not open, it is guaranteed that
 **      there is no hot-journal in the file-system.
 **
 **  WRITER_LOCKED:
 **
 **    The pager moves to this state from READER when a write-transaction
 **    is first opened on the database. In WRITER_LOCKED state, all locks
 **    required to start a write-transaction are held, but no actual
 **    modifications to the cache or database have taken place.
 **
 **    In rollback mode, a RESERVED or (if the transaction was opened with
 **    BEGIN EXCLUSIVE) EXCLUSIVE lock is obtained on the database file when
 **    moving to this state, but the journal file is not written to or opened
 **    to in this state. If the transaction is committed or rolled back while
 **    in WRITER_LOCKED state, all that is required is to unlock the database
 **    file.
 **
 **    IN WAL mode, WalBeginWriteTransaction() is called to lock the log file.
 **    If the connection is running with locking_mode=exclusive, an attempt
 **    is made to obtain an EXCLUSIVE lock on the database file.
 **
 **    * A write transaction is active.
 **    * If the connection is open in rollback-mode, a RESERVED or greater
 **      lock is held on the database file.
 **    * If the connection is open in WAL-mode, a WAL write transaction
 **      is open (i.e. sqlite3WalBeginWriteTransaction() has been successfully
 **      called).
 **    * The dbSize, dbOrigSize and dbFileSize variables are all valid.
 **    * The contents of the pager cache have not been modified.
 **    * The journal file may or may not be open.
 **    * Nothing (not even the first header) has been written to the journal.
 **
 **  WRITER_CACHEMOD:
 **
 **    A pager moves from WRITER_LOCKED state to this state when a page is
 **    first modified by the upper layer. In rollback mode the journal file
 **    is opened (if it is not already open) and a header written to the
 **    start of it. The database file on disk has not been modified.
 **
 **    * A write transaction is active.
 **    * A RESERVED or greater lock is held on the database file.
 **    * The journal file is open and the first header has been written
 **      to it, but the header has not been synced to disk.
 **    * The contents of the page cache have been modified.
 **
 **  WRITER_DBMOD:
 **
 **    The pager transitions from WRITER_CACHEMOD into WRITER_DBMOD state
 **    when it modifies the contents of the database file. WAL connections
 **    never enter this state (since they do not modify the database file,
 **    just the log file).
 **
 **    * A write transaction is active.
 **    * An EXCLUSIVE or greater lock is held on the database file.
 **    * The journal file is open and the first header has been written
 **      and synced to disk.
 **    * The contents of the page cache have been modified (and possibly
 **      written to disk).
 **
 **  WRITER_FINISHED:
 **
 **    It is not possible for a WAL connection to enter this state.
 **
 **    A rollback-mode pager changes to WRITER_FINISHED state from WRITER_DBMOD
 **    state after the entire transaction has been successfully written into the
 **    database file. In this state the transaction may be committed simply
 **    by finalizing the journal file. Once in WRITER_FINISHED state, it is
 **    not possible to modify the database further. At this point, the upper
 **    layer must either commit or rollback the transaction.
 **
 **    * A write transaction is active.
 **    * An EXCLUSIVE or greater lock is held on the database file.
 **    * All writing and syncing of journal and database data has finished.
 **      If no error occurred, all that remains is to finalize the journal to
 **      commit the transaction. If an error did occur, the caller will need
 **      to rollback the transaction.
 **
 **  ERROR:
 **
 **    The ERROR state is entered when an IO or disk-full error (including
 **    SQLITE_IOERR_NOMEM) occurs at a point in the code that makes it
 **    difficult to be sure that the in-memory pager state (cache contents,
 **    db size etc.) are consistent with the contents of the file-system.
 **
 **    Temporary pager files may enter the ERROR state, but in-memory pagers
 **    cannot.
 **
 **    For example, if an IO error occurs while performing a rollback,
 **    the contents of the page-cache may be left in an inconsistent state.
 **    At this point it would be dangerous to change back to READER state
 **    (as usually happens after a rollback). Any subsequent readers might
 **    report database corruption (due to the inconsistent cache), and if
 **    they upgrade to writers, they may inadvertently corrupt the database
 **    file. To avoid this hazard, the pager switches into the ERROR state
 **    instead of READER following such an error.
 **
 **    Once it has entered the ERROR state, any attempt to use the pager
 **    to read or write data returns an error. Eventually, once all
 **    outstanding transactions have been abandoned, the pager is able to
 **    transition back to OPEN state, discarding the contents of the
 **    page-cache and any other in-memory state at the same time. Everything
 **    is reloaded from disk (and, if necessary, hot-journal rollback peformed)
 **    when a read-transaction is next opened on the pager (transitioning
 **    the pager into READER state). At that point the system has recovered
 **    from the error.
 **
 **    Specifically, the pager jumps into the ERROR state if:
 **
 **      1. An error occurs while attempting a rollback. This happens in
 **         function sqlite3PagerRollback().
 **
 **      2. An error occurs while attempting to finalize a journal file
 **         following a commit in function sqlite3PagerCommitPhaseTwo().
 **
 **      3. An error occurs while attempting to write to the journal or
 **         database file in function pagerStress() in order to free up
 **         memory.
 **
 **    In other cases, the error is returned to the b-tree layer. The b-tree
 **    layer then attempts a rollback operation. If the error condition
 **    persists, the pager enters the ERROR state via condition (1) above.
 **
 **    Condition (3) is necessary because it can be triggered by a read-only
 **    statement executed within a transaction. In this case, if the error
 **    code were simply returned to the user, the b-tree layer would not
 **    automatically attempt a rollback, as it assumes that an error in a
 **    read-only statement cannot leave the pager in an internally inconsistent
 **    state.
 **
 **    * The Pager.errCode variable is set to something other than SQLITE_OK.
 **    * There are one or more outstanding references to pages (after the
 **      last reference is dropped the pager should move back to OPEN state).
 **    * The pager is not an in-memory pager.
 **
 **
 ** Notes:
 **
 **   * A pager is never in WRITER_DBMOD or WRITER_FINISHED state if the
 **     connection is open in WAL mode. A WAL connection is always in one
 **     of the first four states.
 **
 **   * Normally, a connection open in exclusive mode is never in PAGER_OPEN
 **     state. There are two exceptions: immediately after exclusive-mode has
 **     been turned on (and before any read or write transactions are
 **     executed), and when the pager is leaving the "error state".
 **
 **   * See also: assert_pager_state().
 */
#define PAGER_OPEN                  0
#define PAGER_READER                1
#define PAGER_WRITER_LOCKED         2
#define PAGER_WRITER_CACHEMOD       3
#define PAGER_WRITER_DBMOD          4
#define PAGER_WRITER_FINISHED       5
#define PAGER_ERROR                 6

/*
 ** The Pager.eLock variable is almost always set to one of the
 ** following locking-states, according to the lock currently held on
 ** the database file: NO_LOCK, SHARED_LOCK, RESERVED_LOCK or EXCLUSIVE_LOCK.
 ** This variable is kept up to date as locks are taken and released by
 ** the pagerLockDb() and pagerUnlockDb() wrappers.
 **
 ** If the VFS xLock() or xUnlock() returns an error other than SQLITE_BUSY
 ** (i.e. one of the SQLITE_IOERR subtypes), it is not clear whether or not
 ** the operation was successful. In these circumstances pagerLockDb() and
 ** pagerUnlockDb() take a conservative approach - eLock is always updated
 ** when unlocking the file, and only updated when locking the file if the
 ** VFS call is successful. This way, the Pager.eLock variable may be set
 ** to a less exclusive (lower) value than the lock that is actually held
 ** at the system level, but it is never set to a more exclusive value.
 **
 ** This is usually safe. If an xUnlock fails or appears to fail, there may
 ** be a few redundant xLock() calls or a lock may be held for longer than
 ** required, but nothing really goes wrong.
 **
 ** The exception is when the database file is unlocked as the pager moves
 ** from ERROR to OPEN state. At this point there may be a hot-journal file
 ** in the file-system that needs to be rolled back (as part of an OPEN->SHARED
 ** transition, by the same pager or any other). If the call to xUnlock()
 ** fails at this point and the pager is left holding an EXCLUSIVE lock, this
 ** can confuse the call to xCheckReservedLock() call made later as part
 ** of hot-journal detection.
 **
 ** xCheckReservedLock() is defined as returning true "if there is a RESERVED
 ** lock held by this process or any others". So xCheckReservedLock may
 ** return true because the caller itself is holding an EXCLUSIVE lock (but
 ** doesn't know it because of a previous error in xUnlock). If this happens
 ** a hot-journal may be mistaken for a journal being created by an active
 ** transaction in another process, causing SQLite to read from the database
 ** without rolling it back.
 **
 ** To work around this, if a call to xUnlock() fails when unlocking the
 ** database in the ERROR state, Pager.eLock is set to UNKNOWN_LOCK. It
 ** is only changed back to a real locking state after a successful call
 ** to xLock(EXCLUSIVE). Also, the code to do the OPEN->SHARED state transition
 ** omits the check for a hot-journal if Pager.eLock is set to UNKNOWN_LOCK
 ** lock. Instead, it assumes a hot-journal exists and obtains an EXCLUSIVE
 ** lock on the database file before attempting to roll it back. See function
 ** PagerSharedLock() for more detail.
 **
 ** Pager.eLock may only be set to UNKNOWN_LOCK when the pager is in
 ** PAGER_OPEN state.
 */
#define UNKNOWN_LOCK                (EXCLUSIVE_LOCK+1)

/*
 ** A macro used for invoking the codec if there is one
 */
#ifdef SQLITE_HAS_CODEC
# define CODEC1(P,D,N,X,E) \
if( P->xCodec && P->xCodec(P->pCodec,D,N,X)==0 ){ E; }
# define CODEC2(P,D,N,X,E,O) \
if( P->xCodec==0 ){ O=(char*)D; }else \
if( (O=(char*)(P->xCodec(P->pCodec,D,N,X)))==0 ){ E; }
#else
# define CODEC1(P,D,N,X,E)   /* NO-OP */
# define CODEC2(P,D,N,X,E,O) O=(char*)D
#endif

/*
 ** The maximum allowed sector size. 64KiB. If the xSectorsize() method
 ** returns a value larger than this, then MAX_SECTOR_SIZE is used instead.
 ** This could conceivably cause corruption following a power failure on
 ** such a system. This is currently an undocumented limit.
 */
#define MAX_SECTOR_SIZE 0x10000


/*
 ** An instance of the following structure is allocated for each active
 ** savepoint and statement transaction in the system. All such structures
 ** are stored in the Pager.aSavepoint[] array, which is allocated and
 ** resized using sqlite3Realloc().
 **
 ** When a savepoint is created, the PagerSavepoint.iHdrOffset field is
 ** set to 0. If a journal-header is written into the main journal while
 ** the savepoint is active, then iHdrOffset is set to the byte offset
 ** immediately following the last journal record written into the main
 ** journal before the journal-header. This is required during savepoint
 ** rollback (see pagerPlaybackSavepoint()).
 */
typedef struct PagerSavepoint PagerSavepoint;
struct PagerSavepoint {
    i64 iOffset;                 /* Starting offset in main journal */
    i64 iHdrOffset;              /* See above */
    Bitvec *pInSavepoint;        /* Set of pages in this savepoint */
    Pgno nOrig;                  /* Original number of pages in file */
    Pgno iSubRec;                /* Index of first record in sub-journal */
#ifndef SQLITE_OMIT_WAL
    u32 aWalData[WAL_SAVEPOINT_NDATA];        /* WAL savepoint context */
#endif
};

/*
 ** Bits of the Pager.doNotSpill flag.  See further description below.
 */
#define SPILLFLAG_OFF         0x01 /* Never spill cache.  Set via pragma */
#define SPILLFLAG_ROLLBACK    0x02 /* Current rolling back, so do not spill */
#define SPILLFLAG_NOSYNC      0x04 /* Spill is ok, but do not sync */

/*
 ** An open page cache is an instance of struct Pager. A description of
 ** some of the more important member variables follows:
 **
 ** eState
 **   The current 'state' of the pager object. See the comment and state
 **   diagram above for a description of the pager state.
 **
 ** eLock
 **
 **   For a real on-disk database, the current lock held on the database file -
 **   NO_LOCK, SHARED_LOCK, RESERVED_LOCK or EXCLUSIVE_LOCK.
 **
 **   For a temporary or in-memory database (neither of which require any
 **   locks), this variable is always set to EXCLUSIVE_LOCK. Since such
 **   databases always have Pager.exclusiveMode==1, this tricks the pager
 **   logic into thinking that it already has all the locks it will ever
 **   need (and no reason to release them).
 **
 **   In some (obscure) circumstances, this variable may also be set to
 **   UNKNOWN_LOCK. See the comment above the #define of UNKNOWN_LOCK for
 **   details.
 **
 ** changeCountDone
 **
 **   This boolean variable is used to make sure that the change-counter
 **   (the 4-byte header field at byte offset 24 of the database file) is
 **   not updated more often than necessary.
 **
 **   It is set to true when the change-counter field is updated, which
 **   can only happen if an exclusive lock is held on the database file.
 **   It is cleared (set to false) whenever an exclusive lock is
 **   relinquished on the database file. Each time a transaction is committed,
 **   The changeCountDone flag is inspected. If it is true, the work of
 **   updating the change-counter is omitted for the current transaction.
 **
 **   This mechanism means that when running in exclusive mode, a connection
 **   need only update the change-counter once, for the first transaction
 **   committed.
 **
 ** setMaster
 **
 **   When PagerCommitPhaseOne() is called to commit a transaction, it may
 **   (or may not) specify a master-journal name to be written into the
 **   journal file before it is synced to disk.
 **
 **   Whether or not a journal file contains a master-journal pointer affects
 **   the way in which the journal file is finalized after the transaction is
 **   committed or rolled back when running in "journal_mode=PERSIST" mode.
 **   If a journal file does not contain a master-journal pointer, it is
 **   finalized by overwriting the first journal header with zeroes. If
 **   it does contain a master-journal pointer the journal file is finalized
 **   by truncating it to zero bytes, just as if the connection were
 **   running in "journal_mode=truncate" mode.
 **
 **   Journal files that contain master journal pointers cannot be finalized
 **   simply by overwriting the first journal-header with zeroes, as the
 **   master journal pointer could interfere with hot-journal rollback of any
 **   subsequently interrupted transaction that reuses the journal file.
 **
 **   The flag is cleared as soon as the journal file is finalized (either
 **   by PagerCommitPhaseTwo or PagerRollback). If an IO error prevents the
 **   journal file from being successfully finalized, the setMaster flag
 **   is cleared anyway (and the pager will move to ERROR state).
 **
 ** doNotSpill
 **
 **   This variables control the behavior of cache-spills  (calls made by
 **   the pcache module to the pagerStress() routine to write cached data
 **   to the file-system in order to free up memory).
 **
 **   When bits SPILLFLAG_OFF or SPILLFLAG_ROLLBACK of doNotSpill are set,
 **   writing to the database from pagerStress() is disabled altogether.
 **   The SPILLFLAG_ROLLBACK case is done in a very obscure case that
 **   comes up during savepoint rollback that requires the pcache module
 **   to allocate a new page to prevent the journal file from being written
 **   while it is being traversed by code in pager_playback().  The SPILLFLAG_OFF
 **   case is a user preference.
 **
 **   If the SPILLFLAG_NOSYNC bit is set, writing to the database from
 **   pagerStress() is permitted, but syncing the journal file is not.
 **   This flag is set by sqlite3PagerWrite() when the file-system sector-size
 **   is larger than the database page-size in order to prevent a journal sync
 **   from happening in between the journalling of two pages on the same sector.
 **
 ** subjInMemory
 **
 **   This is a boolean variable. If true, then any required sub-journal
 **   is opened as an in-memory journal file. If false, then in-memory
 **   sub-journals are only used for in-memory pager files.
 **
 **   This variable is updated by the upper layer each time a new
 **   write-transaction is opened.
 **
 ** dbSize, dbOrigSize, dbFileSize
 **
 **   Variable dbSize is set to the number of pages in the database file.
 **   It is valid in PAGER_READER and higher states (all states except for
 **   OPEN and ERROR).
 **
 **   dbSize is set based on the size of the database file, which may be
 **   larger than the size of the database (the value stored at offset
 **   28 of the database header by the btree). If the size of the file
 **   is not an integer multiple of the page-size, the value stored in
 **   dbSize is rounded down (i.e. a 5KB file with 2K page-size has dbSize==2).
 **   Except, any file that is greater than 0 bytes in size is considered
 **   to have at least one page. (i.e. a 1KB file with 2K page-size leads
 **   to dbSize==1).
 **
 **   During a write-transaction, if pages with page-numbers greater than
 **   dbSize are modified in the cache, dbSize is updated accordingly.
 **   Similarly, if the database is truncated using PagerTruncateImage(),
 **   dbSize is updated.
 **
 **   Variables dbOrigSize and dbFileSize are valid in states
 **   PAGER_WRITER_LOCKED and higher. dbOrigSize is a copy of the dbSize
 **   variable at the start of the transaction. It is used during rollback,
 **   and to determine whether or not pages need to be journalled before
 **   being modified.
 **
 **   Throughout a write-transaction, dbFileSize contains the size of
 **   the file on disk in pages. It is set to a copy of dbSize when the
 **   write-transaction is first opened, and updated when VFS calls are made
 **   to write or truncate the database file on disk.
 **
 **   The only reason the dbFileSize variable is required is to suppress
 **   unnecessary calls to xTruncate() after committing a transaction. If,
 **   when a transaction is committed, the dbFileSize variable indicates
 **   that the database file is larger than the database image (Pager.dbSize),
 **   pager_truncate() is called. The pager_truncate() call uses xFilesize()
 **   to measure the database file on disk, and then truncates it if required.
 **   dbFileSize is not used when rolling back a transaction. In this case
 **   pager_truncate() is called unconditionally (which means there may be
 **   a call to xFilesize() that is not strictly required). In either case,
 **   pager_truncate() may cause the file to become smaller or larger.
 **
 ** dbHintSize
 **
 **   The dbHintSize variable is used to limit the number of calls made to
 **   the VFS xFileControl(FCNTL_SIZE_HINT) method.
 **
 **   dbHintSize is set to a copy of the dbSize variable when a
 **   write-transaction is opened (at the same time as dbFileSize and
 **   dbOrigSize). If the xFileControl(FCNTL_SIZE_HINT) method is called,
 **   dbHintSize is increased to the number of pages that correspond to the
 **   size-hint passed to the method call. See pager_write_pagelist() for
 **   details.
 **
 ** errCode
 **   The Pager.errCode variable is only ever used in PAGER_ERROR state. It
 **   is set to zero in all other states. In PAGER_ERROR state, Pager.errCode
 **   is always set to SQLITE_FULL, SQLITE_IOERR or one of the SQLITE_IOERR_XXX
 **   sub-codes.
 **
 ** syncFlags, walSyncFlags
 **   syncFlags is either SQLITE_SYNC_NORMAL (0x02) or SQLITE_SYNC_FULL (0x03).
 **   syncFlags is used for rollback mode.  walSyncFlags is used for WAL mode
 **   and contains the flags used to sync the checkpoint operations in the
 **   lower two bits, and sync flags used for transaction commits in the WAL
 **   file in bits 0x04 and 0x08.  In other words, to get the correct sync flags
 **   for checkpoint operations, use (walSyncFlags&0x03) and to get the correct
 **   sync flags for transaction commit, use ((walSyncFlags>>2)&0x03).  Note
 **   that with synchronous=NORMAL in WAL mode, transaction commit is not synced
 **   meaning that the 0x04 and 0x08 bits are both zero.
 */
struct Pager {
    sqlite3_vfs *pVfs;          /* OS functions to use for IO */
    u8 exclusiveMode;           /* Boolean. True if locking_mode==EXCLUSIVE */
    u8 journalMode;             /* One of the PAGER_JOURNALMODE_* values */
    u8 useJournal;              /* Use a rollback journal on this file */
    u8 noSync;                  /* Do not sync the journal if true */
    u8 fullSync;                /* Do extra syncs of the journal for robustness */
    u8 extraSync;               /* sync directory after journal delete */
    u8 syncFlags;               /* SYNC_NORMAL or SYNC_FULL otherwise */
    u8 walSyncFlags;            /* See description above */
    u8 tempFile;                /* zFilename is a temporary or immutable file */
    u8 noLock;                  /* Do not lock (except in WAL mode) */
    u8 readOnly;                /* True for a read-only database */
    u8 memDb;                   /* True to inhibit all file I/O */
    
    /**************************************************************************
     ** The following block contains those class members that change during
     ** routine operation.  Class members not in this block are either fixed
     ** when the pager is first created or else only change when there is a
     ** significant mode change (such as changing the page_size, locking_mode,
     ** or the journal_mode).
     ** From another view, these class members describe the "state" of the pager,
     ** while other class members describe the "configuration" of the pager.
     */
    u8 eState;                  /* Pager state (OPEN, READER, WRITER_LOCKED..) */
    u8 eLock;                   /* Current lock held on database file */
    u8 changeCountDone;         /* Set after incrementing the change-counter */
    u8 setMaster;               /* True if a m-j name has been written to jrnl */
    u8 doNotSpill;              /* Do not spill the cache when non-zero */
    u8 subjInMemory;            /* True to use in-memory sub-journals */
    u8 bUseFetch;               /* True to use xFetch() */
    u8 hasHeldSharedLock;       /* True if a shared lock has ever been held */
    
    Pgno dbSize;                /* Number of pages in the database */
    Pgno dbOrigSize;            /* dbSize before the current transaction */
    Pgno dbFileSize;            /* Number of pages in the database file */
    Pgno dbHintSize;            /* Value passed to FCNTL_SIZE_HINT call */
    
    int errCode;                /* One of several kinds of errors */
    int nRec;                   /* Pages journalled since last j-header written */
    u32 cksumInit;              /* Quasi-random value added to every checksum */
    u32 nSubRec;                /* Number of records written to sub-journal */
    Bitvec *pInJournal;         /* One bit for each page in the database file */
    
    sqlite3_file *fd;           /* File descriptor for database */
    sqlite3_file *jfd;          /* File descriptor for main journal */
    sqlite3_file *sjfd;         /* File descriptor for sub-journal */
    i64 journalOff;             /* Current write offset in the journal file */
    i64 journalHdr;             /* Byte offset to previous journal header */
    sqlite3_backup *pBackup;    /* Pointer to list of ongoing backup processes */
    PagerSavepoint *aSavepoint; /* Array of active savepoints */
    int nSavepoint;             /* Number of elements in aSavepoint[] */
    u32 iDataVersion;           /* Changes whenever database content changes */
    char dbFileVers[16];        /* Changes whenever database file changes */
    
    int nMmapOut;               /* Number of mmap pages currently outstanding */
    sqlite3_int64 szMmap;       /* Desired maximum mmap size */
    PgHdr *pMmapFreelist;       /* List of free mmap page headers (pDirty) */
    /*
     ** End of the routinely-changing class members
     ***************************************************************************/
    
    u16 nExtra;                 /* Add this many bytes to each in-memory page */
    i16 nReserve;               /* Number of unused bytes at end of each page */
    u32 vfsFlags;               /* Flags for sqlite3_vfs.xOpen() */
    u32 sectorSize;             /* Assumed sector size during rollback */
    int pageSize;               /* Number of bytes in a page */
    Pgno mxPgno;                /* Maximum allowed size of the database */
    i64 journalSizeLimit;       /* Size limit for persistent journal files */
    char *zFilename;            /* Name of the database file */
    char *zJournal;             /* Name of the journal file */
    int (*xBusyHandler)(void*); /* Function to call when busy */
    void *pBusyHandlerArg;      /* Context argument for xBusyHandler */
    int aStat[4];               /* Total cache hits, misses, writes, spills */
#ifdef SQLITE_TEST
    int nRead;                  /* Database pages read */
#endif
    
    void (*xReiniter)(DbPage*); /* Call this routine when reloading pages */
    int (*xGet)(Pager*,Pgno,DbPage**,int); /* Routine to fetch a patch */
    
#ifdef SQLITE_HAS_CODEC
    void *(*xCodec)(void*,void*,Pgno,int); /* Routine for en/decoding data */
    void (*xCodecSizeChng)(void*,int,int); /* Notify of page size changes */
    void (*xCodecFree)(void*);             /* Destructor for the codec */
    void *pCodec;               /* First argument to xCodec... methods */
#endif
    
    char *pTmpSpace;            /* Pager.pageSize bytes of space for tmp use */
    PCache *pPCache;            /* Pointer to page cache object */  /////////////////////
    
#ifndef SQLITE_OMIT_WAL
    Wal *pWal;                  /* Write-ahead log used by "journal_mode=wal" */
    char *zWal;                 /* File name for write-ahead log */
#endif
};

#endif /* pager_struct_h */

