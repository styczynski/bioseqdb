/*-------------------------------------------------------------------------
 *
 * columnar_storage_physical.cppm
 *
 * Low-level storage layer for the bioseq columnar table format.
 *   - Translates columnar read/write operations on logical offsets into
 *     operations on pages/blocks.
 *   - Emits WAL.
 *   - Reads/writes the columnar metapage.
 *   - Reserves data offsets, stripe numbers, and row offsets.
 *   - Truncation.
 *
 * Higher-level columnar operations deal with logical offsets and large
 * contiguous buffers of data that need to be stored. But the buffer
 * manager and WAL depend on formatted pages with headers, so these
 * large buffers need to be written across many pages. This module
 * translates the contiguous buffers into individual block reads/writes,
 * and performs WAL when necessary.
 *
 * Storage layout: metapage in block 0, empty page in block 1, logical
 * data starting at the first byte after the page header in block 2
 * (logical offset BioseqColumnFirstLogicalOffset). A columnar table always
 * has at least 2 blocks.
 *
 * Reservation uses a relation extension lock and is concurrency-safe,
 * so callers only need an ordinary lock on the relation. Initializing
 * the metapage or truncating the relation requires AccessExclusiveLock.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include "safe_lib.h"

#include <algorithm>

export module bioseq.storage.physical;
import bioseq.common.mlogging;
import bioseq.config;     // DOC0322 §6.5 — BIOSEQ_COLUMN_BYTES_PER_PAGE, BIOSEQ_COLUMN_VERSION_* (moved from config.h)
import bioseq.pg_utils;

export constexpr uint64_t BIOSEQ_COLUMN_INVALID_ROW_NUMBER = static_cast<uint64_t>(0);
export constexpr uint64_t BIOSEQ_COLUMN_FIRST_ROW_NUMBER = static_cast<uint64_t>(1);

/*
 * Number of valid ItemPointer Offsets / BlockNumbers for the row-number <->
 * ItemPointer mapping (formerly the VALID_ITEMPOINTER_OFFSETS /
 * VALID_BLOCKNUMBERS macros in columnar/columnar_tableam.h). offsetNumber is
 * restricted to [FirstOffsetNumber, MaxHeapTuplesPerPage] (bitmap scan
 * tbm_add_tuples constraint) and blockNumber to [0, MaxBlockNumber].
 */
export constexpr uint64_t BIOSEQ_COLUMN_VALID_ITEMPOINTER_OFFSETS = (uint64_t)(MaxHeapTuplesPerPage - FirstOffsetNumber + 1);
export constexpr uint64_t BIOSEQ_COLUMN_VALID_BLOCKNUMBERS = (uint64_t)(MaxBlockNumber + 1);

export constexpr uint64_t BIOSEQ_COLUMN_MAX_ROW_NUMBER = static_cast<uint64_t>(BIOSEQ_COLUMN_FIRST_ROW_NUMBER + BIOSEQ_COLUMN_VALID_ITEMPOINTER_OFFSETS * BIOSEQ_COLUMN_VALID_BLOCKNUMBERS);


/*
 * Logical offsets never fall on the first two physical pages. See
 * comments in bioseq_storage.c.
 */
export constexpr uint64_t BioseqColumnInvalidLogicalOffset = 0;
export constexpr uint64_t BioseqColumnFirstLogicalOffset = ((BLCKSZ - SizeOfPageHeaderData) * 2);

export template<typename T>
[[nodiscard]] inline constexpr bool BioseqColumnLogicalOffsetIsValid(T logical_offset) {
	return logical_offset >= BioseqColumnFirstLogicalOffset;
}

/*
 * Content of the first page in main fork, which stores metadata at file
 * level.
 */
struct BioseqColumnMetapage
{
	/*
	 * Store version of file format used, so we can detect files from
	 * previous versions if we change file format.
	 */
	uint32_t versionMajor;
	uint32_t versionMinor;

	/*
	 * Each of the metadata table rows are identified by a storageId.
	 * We store it also in the main fork so we can link metadata rows
	 * with data files.
	 */
	uint64_t storageId;

	uint64_t reservedStripeId; /* first unused stripe id */
	uint64_t reservedRowNumber; /* first unused row number */
	uint64_t reservedOffset; /* first unused byte offset */

	/*
	 * Flag set to true in the init fork. After an unlogged table reset (due
	 * to a crash), the init fork will be copied over the main fork. When
	 * trying to read an unlogged table, if this flag is set to true, we must
	 * clear the metadata for the table (because the actual data is gone,
	 * too), and clear the flag. We can cross-check that the table is
	 * UNLOGGED, and that the main fork is at the minimum size (no actual
	 * data).
	 *
	 * XXX: Not used yet; reserved field for later support for UNLOGGED.
	 */
	bool unloggedReset;
};


/* represents a "physical" block+offset address */
struct PhysicalAddr
{
	BlockNumber blockno;
	uint32_t offset;
};


/*
 * Layout constants for the metapage and the first data page. The metapage
 * always lives at block 0; block 1 is reserved (empty) and logical data
 * starts at block 2 (see BioseqColumnFirstLogicalOffset).
 */
inline constexpr BlockNumber BIOSEQ_COLUMN_METAPAGE_BLOCKNO = 0;
inline constexpr BlockNumber BIOSEQ_COLUMN_EMPTY_BLOCKNO = 1;
inline constexpr uint64_t BIOSEQ_COLUMN_INVALID_STRIPE_ID = 0;
inline constexpr uint64_t BIOSEQ_COLUMN_FIRST_STRIPE_ID = 1;

inline constexpr const char* OLD_METAPAGE_VERSION_HINT =
	"Use \"VACUUM\" to upgrade the columnar table format "
	"version or run \"ALTER EXTENSION bioseq UPDATE\".";



/*
 * Map logical offsets to a physical page and offset where the data is kept.
 */
static inline PhysicalAddr
LogicalToPhysical(uint64_t logicalOffset)
{
	PhysicalAddr addr;

	addr.blockno = logicalOffset / BIOSEQ_COLUMN_BYTES_PER_PAGE;
	addr.offset = SizeOfPageHeaderData + (logicalOffset % BIOSEQ_COLUMN_BYTES_PER_PAGE);

	return addr;
}


/*
 * Map a physical page and offset address to a logical address.
 */
static inline uint64_t
PhysicalToLogical(PhysicalAddr addr)
{
	return BIOSEQ_COLUMN_BYTES_PER_PAGE * addr.blockno + addr.offset - SizeOfPageHeaderData;
}

/*
 * ReadFromBlock - read bytes from a page at the given offset. If 'force' is
 * true, don't check pd_lower; useful when reading a metapage of unknown
 * version.
 */
static void
ReadFromBlock(Relation rel, BlockNumber blockno, uint32_t offset, char *buf,
			  uint32_t len, bool force)
{
	Buffer buffer = ReadBuffer(rel, blockno);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	Page page = BufferGetPage(buffer);
	PageHeader phdr = reinterpret_cast<PageHeader>(page);

	if (BLCKSZ < offset + len || (!force && (phdr->pd_lower < offset + len)))
	{
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempt to read columnar data of length {} from offset "
					"{} of block {} of relation {}",
					len, offset, blockno, rel->rd_id),
			});
	}

	memcpy_s(buf, len, page + offset, len);
	UnlockReleaseBuffer(buffer);
}

/*
 * WriteToBlock - append data to a block, initializing if necessary, and emit
 * WAL. If 'clear' is true, always clear the data on the page and reinitialize
 * it first, and offset must be SizeOfPageHeaderData. Otherwise, offset must
 * be equal to pd_lower and pd_lower will be set to the end of the written
 * data.
 */
static void
WriteToBlock(Relation rel, BlockNumber blockno, uint32_t offset, char *buf,
			 uint32_t len, bool clear)
{
	Buffer buffer = ReadBuffer(rel, blockno);
	GenericXLogState *state = GenericXLogStart(rel);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	Page page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);

	PageHeader phdr = reinterpret_cast<PageHeader>(page);
	if (PageIsNew(page) || clear)
	{
		PageInit(page, BLCKSZ, 0);
	}

	if (phdr->pd_lower < offset || phdr->pd_upper - offset < len)
	{
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempt to write columnar data of length {} to offset "
					"{} of block {} of relation {}",
					len, offset, blockno, rel->rd_id),
			});
	}

	/*
	 * After a transaction has been rolled-back, we might be
	 * over-writing the rolledback write, so phdr->pd_lower can be
	 * different from addr.offset.
	 *
	 * We reset pd_lower to reset the rolledback write.
	 *
	 * Given that we always align page reservation to the next page as of
	 * 10.2, having such a disk page is only possible if write operaion
	 * failed in an older version of columnar, but now user attempts writing
	 * to that table in version >= 10.2.
	 */
	if (phdr->pd_lower > offset)
	{
		bioseq_report_pg(DEBUG4)
			.msg("overwriting page {}", blockno)
			.detail("This can happen after a roll-back.");
		phdr->pd_lower = offset;
	}

	memcpy_s(page + phdr->pd_lower, phdr->pd_upper - phdr->pd_lower, buf, len);
	phdr->pd_lower += len;

	GenericXLogFinish(state);

	UnlockReleaseBuffer(buffer);
}

/*
 * AlignReservation - given an unused logical byte offset, align it so that it
 * falls at the start of a page.
 *
 * XXX: Reconsider whether we want/need to do this at all.
 */
static uint64_t
AlignReservation(uint64_t prevReservation)
{
	PhysicalAddr prevAddr = LogicalToPhysical(prevReservation);
	uint64_t alignedReservation = prevReservation;

	if (prevAddr.offset != SizeOfPageHeaderData)
	{
		/* not aligned; align on beginning of next page */
		PhysicalAddr initial = { 0 };
		initial.blockno = prevAddr.blockno + 1;
		initial.offset = SizeOfPageHeaderData;
		alignedReservation = PhysicalToLogical(initial);
	}

	Assert(alignedReservation >= prevReservation);
	return alignedReservation;
}

/*
 * BioseqColumnMetapageIsCurrent - is the metapage at the latest version?
 */
static bool
BioseqColumnMetapageIsCurrent(BioseqColumnMetapage *metapage)
{
	return (metapage->versionMajor == BIOSEQ_COLUMN_VERSION_MAJOR &&
			metapage->versionMinor == BIOSEQ_COLUMN_VERSION_MINOR);
}

/*
 * BioseqColumnMetapageIsOlder - is the metapage older than the current version?
 */
static bool
BioseqColumnMetapageIsOlder(BioseqColumnMetapage *metapage)
{
	return (metapage->versionMajor < BIOSEQ_COLUMN_VERSION_MAJOR ||
			(metapage->versionMajor == BIOSEQ_COLUMN_VERSION_MAJOR &&
			 static_cast<int>(metapage->versionMinor) < static_cast<int>(BIOSEQ_COLUMN_VERSION_MINOR)));
}

/*
 * BioseqColumnMetapageIsNewer - is the metapage newer than the current version?
 */
static bool
BioseqColumnMetapageIsNewer(BioseqColumnMetapage *metapage)
{
	return (metapage->versionMajor > BIOSEQ_COLUMN_VERSION_MAJOR ||
			(metapage->versionMajor == BIOSEQ_COLUMN_VERSION_MAJOR &&
			 metapage->versionMinor > BIOSEQ_COLUMN_VERSION_MINOR));
}

/*
 * BioseqColumnMetapageCheckVersion - throw an error if accessing old
 * version of metapage.
 */
static void
BioseqColumnMetapageCheckVersion(Relation rel, BioseqColumnMetapage *metapage)
{
	if (!BioseqColumnMetapageIsCurrent(metapage))
	{
		const char* const relation_name =
			RelationGetRelationName(rel);
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_FEATURE_NOT_SUPPORTED,
			{
				.message = fmt::format(
					"attempted to access relation \"{}\", which uses an "
					"older columnar format",
					relation_name),
				.detail = fmt::format(
					"BioseqColumn format version {}.{} is required, \"{}\" "
					"has version {}.{}.",
					BIOSEQ_COLUMN_VERSION_MAJOR,
					BIOSEQ_COLUMN_VERSION_MINOR,
					relation_name,
					metapage->versionMajor,
					metapage->versionMinor),
				.hint = OLD_METAPAGE_VERSION_HINT,
			});
	}
}

/*
 * BioseqColumnMetapageRead - read the current contents of the metapage. Error if
 * it does not exist. Throw an error if the metapage is not the current
 * version, unless 'force' is true.
 *
 * NB: it's safe to read a different version of a metapage because we
 * guarantee that fields will only be added and existing fields will never be
 * changed. However, it's important that we don't depend on new fields being
 * set properly when we read an old metapage; an old metapage should only be
 * read for the purposes of upgrading or error checking.
 */
static BioseqColumnMetapage
BioseqColumnMetapageRead(Relation rel, bool force)
{
	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	if (nblocks == 0)
	{
		/*
		 * We only expect this to happen when upgrading citus.so. This is because,
		 * in current version of columnar, we immediately create the metapage
		 * for columnar tables, i.e right after creating the table.
		 * However in older versions, we were creating metapages lazily, i.e
		 * when ingesting data to columnar table.
		 */
		const char* const relation_name =
			RelationGetRelationName(rel);
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"columnar metapage for relation \"{}\" does not exist",
					relation_name),
				.hint = OLD_METAPAGE_VERSION_HINT,
			});
	}

	/*
	 * Regardless of "force" parameter, always force read metapage block.
	 * We will check metapage version in BioseqColumnMetapageCheckVersion
	 * depending on "force".
	 */
	bool forceReadBlock = true;
	BioseqColumnMetapage metapage;
	ReadFromBlock(rel, BIOSEQ_COLUMN_METAPAGE_BLOCKNO, SizeOfPageHeaderData,
				  reinterpret_cast<char *>(&metapage), sizeof(BioseqColumnMetapage), forceReadBlock);

	if (!force)
	{
		BioseqColumnMetapageCheckVersion(rel, &metapage);
	}

	return metapage;
}

/*
 * BioseqColumnOverwriteMetapage writes given bioseqColumnMetapage back to metapage
 * for given relation.
 */
static void
BioseqColumnOverwriteMetapage(Relation relation, BioseqColumnMetapage bioseqColumnMetapage)
{
	/* clear metapage because we are overwriting */
	bool clear = true;
	WriteToBlock(relation, BIOSEQ_COLUMN_METAPAGE_BLOCKNO, SizeOfPageHeaderData,
				 reinterpret_cast<char *>(&bioseqColumnMetapage), sizeof(BioseqColumnMetapage), clear);
}



/*
 * BioseqColumnStorageInit - initialize a new metapage in an empty relation
 * with the given storageId.
 *
 * Caller must hold AccessExclusiveLock on the relation.
 */
export void
BioseqColumnStorageInit(SMgrRelation srel, uint64_t storageId)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() entered storage_id={}", storageId);
	BlockNumber nblocks = smgrnblocks(srel, MAIN_FORKNUM);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() existing_blocks={}", nblocks);
	if (nblocks > 0)
	{
		bioseq_log<ERR, PG>(
			 "attempted to initialize metapage, but {} pages already exist",
			 nblocks);
	}

	/* create two pages */
	PGIOAlignedBlock block;
	Page page = block.data;

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() initialising metapage");
	PageInit(page, BLCKSZ, 0);
	PageHeader phdr = reinterpret_cast<PageHeader>(page);

	BioseqColumnMetapage metapage = { 0 };
	metapage.storageId = storageId;
	metapage.versionMajor = BIOSEQ_COLUMN_VERSION_MAJOR;
	metapage.versionMinor = BIOSEQ_COLUMN_VERSION_MINOR;
	metapage.reservedStripeId = BIOSEQ_COLUMN_FIRST_STRIPE_ID;
	metapage.reservedRowNumber = BIOSEQ_COLUMN_FIRST_ROW_NUMBER;
	metapage.reservedOffset = BioseqColumnFirstLogicalOffset;
	metapage.unloggedReset = false;
	memcpy_s(page + phdr->pd_lower, phdr->pd_upper - phdr->pd_lower,
			 reinterpret_cast<char *>(&metapage), sizeof(BioseqColumnMetapage));
	phdr->pd_lower += sizeof(BioseqColumnMetapage);
	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
				BIOSEQ_COLUMN_METAPAGE_BLOCKNO, page, true);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() flushing metapage to disk");
	PageSetChecksumInplace(page, BIOSEQ_COLUMN_METAPAGE_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, BIOSEQ_COLUMN_METAPAGE_BLOCKNO, page, true);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() initialising empty page");
	PageInit(page, BLCKSZ, 0);

	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
				BIOSEQ_COLUMN_EMPTY_BLOCKNO, page, true);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() flushing empty page to disk");
	PageSetChecksumInplace(page, BIOSEQ_COLUMN_EMPTY_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, BIOSEQ_COLUMN_EMPTY_BLOCKNO, page, true);

	/*
	 * An immediate sync is required even if we xlog'd the page, because the
	 * write did not go through shared_buffers and therefore a concurrent
	 * checkpoint may have moved the redo pointer past our xlog record.
	 */
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageInit() requesting smgr immediate sync");
	smgrimmedsync(srel, MAIN_FORKNUM);
}


/*
 * BioseqColumnStorageUpdateCurrent - update the metapage to the current
 * version. No effect if the version already matches. If 'upgrade' is true,
 * throw an error if metapage version is newer; if 'upgrade' is false, it's a
 * downgrade, so throw an error if the metapage version is older.
 *
 * NB: caller must ensure that metapage already exists, which might not be the
 * case on 10.0.
 */
export void
BioseqColumnStorageUpdateCurrent(Relation rel, bool upgrade, uint64_t reservedStripeId,
							 uint64_t reservedRowNumber, uint64_t reservedOffset)
{
	LockRelationForExtension(rel, ExclusiveLock);

	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, true);

	if (BioseqColumnMetapageIsCurrent(&metapage))
	{
		/* nothing to do */
		UnlockRelationForExtension(rel, ExclusiveLock);
		return;
	}

	if (upgrade && BioseqColumnMetapageIsNewer(&metapage))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq_log<ERR, PG>( "found newer columnar metapage while upgrading");
	}

	if (!upgrade && BioseqColumnMetapageIsOlder(&metapage))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq_log<ERR, PG>( "found older columnar metapage while downgrading");
	}

	metapage.versionMajor = BIOSEQ_COLUMN_VERSION_MAJOR;
	metapage.versionMinor = BIOSEQ_COLUMN_VERSION_MINOR;

	/* storageId remains the same */
	metapage.reservedStripeId = reservedStripeId;
	metapage.reservedRowNumber = reservedRowNumber;
	metapage.reservedOffset = reservedOffset;

	BioseqColumnOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);
}


/*
 * BioseqColumnStorageGetVersionMajor - return major version from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
BioseqColumnStorageGetVersionMajor(Relation rel, bool force)
{
	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, force);

	return metapage.versionMajor;
}


/*
 * BioseqColumnStorageGetVersionMinor - return minor version from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
BioseqColumnStorageGetVersionMinor(Relation rel, bool force)
{
	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, force);

	return metapage.versionMinor;
}


/*
 * BioseqColumnStorageGetStorageId - return storage ID from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
BioseqColumnStorageGetStorageId(Relation rel, bool force)
{
	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, force);

	return metapage.storageId;
}


/*
 * BioseqColumnStorageGetReservedStripeId - return reserved stripe ID from the
 * metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
BioseqColumnStorageGetReservedStripeId(Relation rel, bool force)
{
	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, force);

	return metapage.reservedStripeId;
}


/*
 * BioseqColumnStorageGetReservedRowNumber - return reserved row number from the
 * metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
BioseqColumnStorageGetReservedRowNumber(Relation rel, bool force)
{
	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, force);

	return metapage.reservedRowNumber;
}


/*
 * BioseqColumnStorageGetReservedOffset - return reserved offset from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
BioseqColumnStorageGetReservedOffset(Relation rel, bool force)
{
	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, force);

	return metapage.reservedOffset;
}

/*
 * BioseqColumnStorageIsCurrent - return true if metapage exists and is at
 * the current version.
 */
export bool
BioseqColumnStorageIsCurrent(Relation rel)
{
	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	if (nblocks < 2)
	{
		return false;
	}

	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, true);
	return BioseqColumnMetapageIsCurrent(&metapage);
}


/*
 * BioseqColumnStorageReserveRowNumber returns reservedRowNumber and advances
 * it for next row number reservation.
 */
export uint64_t
BioseqColumnStorageReserveRowNumber(Relation rel, uint64_t nrows)
{
	LockRelationForExtension(rel, ExclusiveLock);

	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, false);

	uint64_t firstRowNumber = metapage.reservedRowNumber;
	metapage.reservedRowNumber += nrows;

	BioseqColumnOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	return firstRowNumber;
}


/*
 * BioseqColumnStorageReserveStripeId returns stripeId and advances it for next
 * stripeId reservation.
 * Note that this function doesn't handle row number reservation.
 * See BioseqColumnStorageReserveRowNumber function.
 */
export uint64_t
BioseqColumnStorageReserveStripeId(Relation rel)
{
	LockRelationForExtension(rel, ExclusiveLock);

	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, false);

	uint64_t stripeId = metapage.reservedStripeId;
	metapage.reservedStripeId++;

	BioseqColumnOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	return stripeId;
}


/*
 * BioseqColumnStorageReserveData - reserve logical data offsets for writing.
 */
export uint64_t
BioseqColumnStorageReserveData(Relation rel, uint64_t amount)
{
	if (amount == 0)
	{
		return BioseqColumnInvalidLogicalOffset;
	}

	LockRelationForExtension(rel, ExclusiveLock);

	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, false);

	uint64_t alignedReservation = AlignReservation(metapage.reservedOffset);
	uint64_t nextReservation = alignedReservation + amount;
	metapage.reservedOffset = nextReservation;

	/* write new reservation */
	BioseqColumnOverwriteMetapage(rel, metapage);

	/* last used PhysicalAddr of new reservation */
	PhysicalAddr final = LogicalToPhysical(nextReservation - 1);

	/* extend with new pages */
	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	while (nblocks <= final.blockno)
	{
		Buffer newBuffer = ReadBuffer(rel, P_NEW);
		Assert(BufferGetBlockNumber(newBuffer) == nblocks);
		ReleaseBuffer(newBuffer);
		nblocks++;
	}

	UnlockRelationForExtension(rel, ExclusiveLock);

	return alignedReservation;
}


/*
 * BioseqColumnStorageRead - map the logical offset to a block and offset, then
 * read the buffer from multiple blocks if necessary.
 */
export void
BioseqColumnStorageRead(Relation rel, uint64_t logicalOffset, char *data, uint32_t amount)
{
	/* if there's no work to do, succeed even with invalid offset */
	if (amount == 0)
	{
		return;
	}

	if (!BioseqColumnLogicalOffsetIsValid(logicalOffset))
	{
		bioseq_log<ERR, PG>(
			 "attempted columnar read on relation {} from invalid logical offset: {}",
			 rel->rd_id, logicalOffset);
	}

	uint64_t read = 0;

	while (read < amount)
	{
		PhysicalAddr addr = LogicalToPhysical(logicalOffset + read);

		uint32_t to_read = std::min<uint64_t>(amount - read, BLCKSZ - addr.offset);
		ReadFromBlock(rel, addr.blockno, addr.offset, data + read, to_read,
					  false);

		read += to_read;
	}
}


/*
 * BioseqColumnStorageWrite - map the logical offset to a block and offset, then
 * write the buffer across multiple blocks if necessary.
 */
export void
BioseqColumnStorageWrite(Relation rel, uint64_t logicalOffset, char *data, uint32_t amount)
{
	/* if there's no work to do, succeed even with invalid offset */
	if (amount == 0)
	{
		return;
	}

	if (!BioseqColumnLogicalOffsetIsValid(logicalOffset))
	{
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempted columnar write on relation {} to invalid "
					"logical offset: {}",
					rel->rd_id,
					logicalOffset),
			});
	}

	uint64_t written = 0;

	while (written < amount)
	{
		PhysicalAddr addr = LogicalToPhysical(logicalOffset + written);

		uint64_t to_write = std::min<uint64_t>(amount - written, BLCKSZ - addr.offset);
		WriteToBlock(rel, addr.blockno, addr.offset, data + written, to_write,
					 false);

		written += to_write;
	}
}


/*
 * BioseqColumnStorageTruncate - truncate the columnar storage such that
 * newDataReservation will be the first unused logical offset available. Free
 * pages at the end of the relation.
 *
 * Caller must hold AccessExclusiveLock on the relation.
 *
 * Returns true if pages were truncated; false otherwise.
 */
export bool
BioseqColumnStorageTruncate(Relation rel, uint64_t newDataReservation)
{
	if (!BioseqColumnLogicalOffsetIsValid(newDataReservation))
	{
		bioseq_log<ERR, PG>(
			 "attempted to truncate relation {} to invalid logical offset: {}",
			 rel->rd_id, newDataReservation);
	}

	BlockNumber old_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	if (old_rel_pages == 0)
	{
		/* nothing to do */
		return false;
	}

	LockRelationForExtension(rel, ExclusiveLock);

	BioseqColumnMetapage metapage = BioseqColumnMetapageRead(rel, false);

	if (metapage.reservedOffset < newDataReservation)
	{
		bioseq_log<ERR, PG>(
			 "attempted to truncate relation {} to offset {} which is higher than existing offset {}",
			 rel->rd_id, newDataReservation, metapage.reservedOffset);
	}

	if (metapage.reservedOffset == newDataReservation)
	{
		/* nothing to do */
		UnlockRelationForExtension(rel, ExclusiveLock);
		return false;
	}

	metapage.reservedOffset = newDataReservation;

	/* write new reservation */
	BioseqColumnOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	PhysicalAddr final = LogicalToPhysical(newDataReservation - 1);
	BlockNumber new_rel_pages = final.blockno + 1;
	Assert(new_rel_pages <= old_rel_pages);

	/*
	 * Truncate the storage. Note that RelationTruncate() takes care of
	 * Write Ahead Logging.
	 */
	if (new_rel_pages < old_rel_pages)
	{
		RelationTruncate(rel, new_rel_pages);
		return true;
	}

	return false;
}
