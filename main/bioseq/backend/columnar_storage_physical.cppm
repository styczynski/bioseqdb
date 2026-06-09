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
 * (logical offset ColumnarFirstLogicalOffset). A columnar table always
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

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

export module storage_physical;
import mlogging;
import pg_utils;

export constexpr uint64_t COLUMNAR_INVALID_ROW_NUMBER = static_cast<uint64_t>(0);
export constexpr uint64_t COLUMNAR_FIRST_ROW_NUMBER = static_cast<uint64_t>(1);
export constexpr uint64_t COLUMNAR_MAX_ROW_NUMBER = static_cast<uint64_t>(COLUMNAR_FIRST_ROW_NUMBER + VALID_ITEMPOINTER_OFFSETS * VALID_BLOCKNUMBERS);


/*
 * Logical offsets never fall on the first two physical pages. See
 * comments in bioseq_storage.c.
 */
export constexpr uint64_t ColumnarInvalidLogicalOffset = 0;
export constexpr uint64_t ColumnarFirstLogicalOffset = ((BLCKSZ - SizeOfPageHeaderData) * 2);

export template<typename T>
[[nodiscard]] inline constexpr bool ColumnarLogicalOffsetIsValid(T logical_offset) {
	return logical_offset >= ColumnarFirstLogicalOffset;
}

/*
 * Content of the first page in main fork, which stores metadata at file
 * level.
 */
struct ColumnarMetapage
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
 * starts at block 2 (see ColumnarFirstLogicalOffset).
 */
inline constexpr BlockNumber COLUMNAR_METAPAGE_BLOCKNO = 0;
inline constexpr BlockNumber COLUMNAR_EMPTY_BLOCKNO = 1;
inline constexpr uint64_t COLUMNAR_INVALID_STRIPE_ID = 0;
inline constexpr uint64_t COLUMNAR_FIRST_STRIPE_ID = 1;

inline constexpr const char* OLD_METAPAGE_VERSION_HINT =
	"Use \"VACUUM\" to upgrade the columnar table format "
	"version or run \"ALTER EXTENSION citus UPDATE\".";



/*
 * Map logical offsets to a physical page and offset where the data is kept.
 */
static inline PhysicalAddr
LogicalToPhysical(uint64_t logicalOffset)
{
	PhysicalAddr addr;

	addr.blockno = logicalOffset / COLUMNAR_BYTES_PER_PAGE;
	addr.offset = SizeOfPageHeaderData + (logicalOffset % COLUMNAR_BYTES_PER_PAGE);

	return addr;
}


/*
 * Map a physical page and offset address to a logical address.
 */
static inline uint64_t
PhysicalToLogical(PhysicalAddr addr)
{
	return COLUMNAR_BYTES_PER_PAGE * addr.blockno + addr.offset - SizeOfPageHeaderData;
}


static void ColumnarOverwriteMetapage(Relation relation,
									  ColumnarMetapage columnarMetapage);
static ColumnarMetapage ColumnarMetapageRead(Relation rel, bool force);
static void ReadFromBlock(Relation rel, BlockNumber blockno, uint32_t offset,
						  char *buf, uint32_t len, bool force);
static void WriteToBlock(Relation rel, BlockNumber blockno, uint32_t offset,
						 char *buf, uint32_t len, bool clear);
static uint64_t AlignReservation(uint64_t prevReservation);
static bool ColumnarMetapageIsCurrent(ColumnarMetapage *metapage);
static bool ColumnarMetapageIsOlder(ColumnarMetapage *metapage);
static bool ColumnarMetapageIsNewer(ColumnarMetapage *metapage);
static void ColumnarMetapageCheckVersion(Relation rel, ColumnarMetapage *metapage);


/*
 * ColumnarStorageInit - initialize a new metapage in an empty relation
 * with the given storageId.
 *
 * Caller must hold AccessExclusiveLock on the relation.
 */
export void
ColumnarStorageInit(SMgrRelation srel, uint64_t storageId)
{
	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() entered storage_id={}", storageId);
	BlockNumber nblocks = smgrnblocks(srel, MAIN_FORKNUM);

	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() existing_blocks={}", nblocks);
	if (nblocks > 0)
	{
		bioseq_log<ERR, PG>(
			 "attempted to initialize metapage, but {} pages already exist",
			 nblocks);
	}

	/* create two pages */
	PGIOAlignedBlock block;
	Page page = block.data;

	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() initialising metapage");
	PageInit(page, BLCKSZ, 0);
	PageHeader phdr = reinterpret_cast<PageHeader>(page);

	ColumnarMetapage metapage = { 0 };
	metapage.storageId = storageId;
	metapage.versionMajor = COLUMNAR_VERSION_MAJOR;
	metapage.versionMinor = COLUMNAR_VERSION_MINOR;
	metapage.reservedStripeId = COLUMNAR_FIRST_STRIPE_ID;
	metapage.reservedRowNumber = COLUMNAR_FIRST_ROW_NUMBER;
	metapage.reservedOffset = ColumnarFirstLogicalOffset;
	metapage.unloggedReset = false;
	memcpy_s(page + phdr->pd_lower, phdr->pd_upper - phdr->pd_lower,
			 reinterpret_cast<char *>(&metapage), sizeof(ColumnarMetapage));
	phdr->pd_lower += sizeof(ColumnarMetapage);
	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
				COLUMNAR_METAPAGE_BLOCKNO, page, true);

	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() flushing metapage to disk");
	PageSetChecksumInplace(page, COLUMNAR_METAPAGE_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, COLUMNAR_METAPAGE_BLOCKNO, page, true);

	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() initialising empty page");
	PageInit(page, BLCKSZ, 0);

	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
				COLUMNAR_EMPTY_BLOCKNO, page, true);

	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() flushing empty page to disk");
	PageSetChecksumInplace(page, COLUMNAR_EMPTY_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, COLUMNAR_EMPTY_BLOCKNO, page, true);

	/*
	 * An immediate sync is required even if we xlog'd the page, because the
	 * write did not go through shared_buffers and therefore a concurrent
	 * checkpoint may have moved the redo pointer past our xlog record.
	 */
	bioseq_log<DBG>("[bioseq] pg: ColumnarStorageInit() requesting smgr immediate sync");
	smgrimmedsync(srel, MAIN_FORKNUM);
}


/*
 * ColumnarStorageUpdateCurrent - update the metapage to the current
 * version. No effect if the version already matches. If 'upgrade' is true,
 * throw an error if metapage version is newer; if 'upgrade' is false, it's a
 * downgrade, so throw an error if the metapage version is older.
 *
 * NB: caller must ensure that metapage already exists, which might not be the
 * case on 10.0.
 */
export void
ColumnarStorageUpdateCurrent(Relation rel, bool upgrade, uint64_t reservedStripeId,
							 uint64_t reservedRowNumber, uint64_t reservedOffset)
{
	LockRelationForExtension(rel, ExclusiveLock);

	ColumnarMetapage metapage = ColumnarMetapageRead(rel, true);

	if (ColumnarMetapageIsCurrent(&metapage))
	{
		/* nothing to do */
		UnlockRelationForExtension(rel, ExclusiveLock);
		return;
	}

	if (upgrade && ColumnarMetapageIsNewer(&metapage))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq_log<ERR, PG>( "found newer columnar metapage while upgrading");
	}

	if (!upgrade && ColumnarMetapageIsOlder(&metapage))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq_log<ERR, PG>( "found older columnar metapage while downgrading");
	}

	metapage.versionMajor = COLUMNAR_VERSION_MAJOR;
	metapage.versionMinor = COLUMNAR_VERSION_MINOR;

	/* storageId remains the same */
	metapage.reservedStripeId = reservedStripeId;
	metapage.reservedRowNumber = reservedRowNumber;
	metapage.reservedOffset = reservedOffset;

	ColumnarOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);
}


/*
 * ColumnarStorageGetVersionMajor - return major version from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
ColumnarStorageGetVersionMajor(Relation rel, bool force)
{
	ColumnarMetapage metapage = ColumnarMetapageRead(rel, force);

	return metapage.versionMajor;
}


/*
 * ColumnarStorageGetVersionMinor - return minor version from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
ColumnarStorageGetVersionMinor(Relation rel, bool force)
{
	ColumnarMetapage metapage = ColumnarMetapageRead(rel, force);

	return metapage.versionMinor;
}


/*
 * ColumnarStorageGetStorageId - return storage ID from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
ColumnarStorageGetStorageId(Relation rel, bool force)
{
	ColumnarMetapage metapage = ColumnarMetapageRead(rel, force);

	return metapage.storageId;
}


/*
 * ColumnarStorageGetReservedStripeId - return reserved stripe ID from the
 * metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
ColumnarStorageGetReservedStripeId(Relation rel, bool force)
{
	ColumnarMetapage metapage = ColumnarMetapageRead(rel, force);

	return metapage.reservedStripeId;
}


/*
 * ColumnarStorageGetReservedRowNumber - return reserved row number from the
 * metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
ColumnarStorageGetReservedRowNumber(Relation rel, bool force)
{
	ColumnarMetapage metapage = ColumnarMetapageRead(rel, force);

	return metapage.reservedRowNumber;
}


/*
 * ColumnarStorageGetReservedOffset - return reserved offset from the metapage.
 *
 * Throw an error if the metapage is not the current version, unless
 * 'force' is true.
 */
export uint64_t
ColumnarStorageGetReservedOffset(Relation rel, bool force)
{
	ColumnarMetapage metapage = ColumnarMetapageRead(rel, force);

	return metapage.reservedOffset;
}

/*
 * ColumnarStorageIsCurrent - return true if metapage exists and is not
 * the current version.
 */
export bool
ColumnarStorageIsCurrent(Relation rel)
{
	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	if (nblocks < 2)
	{
		return false;
	}

	ColumnarMetapage metapage = ColumnarMetapageRead(rel, true);
	return ColumnarMetapageIsCurrent(&metapage);
}


/*
 * ColumnarStorageReserveRowNumber returns reservedRowNumber and advances
 * it for next row number reservation.
 */
export uint64_t
ColumnarStorageReserveRowNumber(Relation rel, uint64_t nrows)
{
	LockRelationForExtension(rel, ExclusiveLock);

	ColumnarMetapage metapage = ColumnarMetapageRead(rel, false);

	uint64_t firstRowNumber = metapage.reservedRowNumber;
	metapage.reservedRowNumber += nrows;

	ColumnarOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	return firstRowNumber;
}


/*
 * ColumnarStorageReserveStripeId returns stripeId and advances it for next
 * stripeId reservation.
 * Note that this function doesn't handle row number reservation.
 * See ColumnarStorageReserveRowNumber function.
 */
export uint64_t
ColumnarStorageReserveStripeId(Relation rel)
{
	LockRelationForExtension(rel, ExclusiveLock);

	ColumnarMetapage metapage = ColumnarMetapageRead(rel, false);

	uint64_t stripeId = metapage.reservedStripeId;
	metapage.reservedStripeId++;

	ColumnarOverwriteMetapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	return stripeId;
}


/*
 * ColumnarStorageReserveData - reserve logical data offsets for writing.
 */
export uint64_t
ColumnarStorageReserveData(Relation rel, uint64_t amount)
{
	if (amount == 0)
	{
		return ColumnarInvalidLogicalOffset;
	}

	LockRelationForExtension(rel, ExclusiveLock);

	ColumnarMetapage metapage = ColumnarMetapageRead(rel, false);

	uint64_t alignedReservation = AlignReservation(metapage.reservedOffset);
	uint64_t nextReservation = alignedReservation + amount;
	metapage.reservedOffset = nextReservation;

	/* write new reservation */
	ColumnarOverwriteMetapage(rel, metapage);

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
 * ColumnarStorageRead - map the logical offset to a block and offset, then
 * read the buffer from multiple blocks if necessary.
 */
export void
ColumnarStorageRead(Relation rel, uint64_t logicalOffset, char *data, uint32_t amount)
{
	/* if there's no work to do, succeed even with invalid offset */
	if (amount == 0)
	{
		return;
	}

	if (!ColumnarLogicalOffsetIsValid(logicalOffset))
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
 * ColumnarStorageWrite - map the logical offset to a block and offset, then
 * write the buffer across multiple blocks if necessary.
 */
export void
ColumnarStorageWrite(Relation rel, uint64_t logicalOffset, char *data, uint32_t amount)
{
	/* if there's no work to do, succeed even with invalid offset */
	if (amount == 0)
	{
		return;
	}

	if (!ColumnarLogicalOffsetIsValid(logicalOffset))
	{
		bioseq_log<ERR, PG>(
			 "attempted columnar write on relation {} to invalid logical offset: {}",
			 rel->rd_id, logicalOffset);
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
 * ColumnarStorageTruncate - truncate the columnar storage such that
 * newDataReservation will be the first unused logical offset available. Free
 * pages at the end of the relation.
 *
 * Caller must hold AccessExclusiveLock on the relation.
 *
 * Returns true if pages were truncated; false otherwise.
 */
export bool
ColumnarStorageTruncate(Relation rel, uint64_t newDataReservation)
{
	if (!ColumnarLogicalOffsetIsValid(newDataReservation))
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

	ColumnarMetapage metapage = ColumnarMetapageRead(rel, false);

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
	ColumnarOverwriteMetapage(rel, metapage);

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


/*
 * ColumnarOverwriteMetapage writes given columnarMetapage back to metapage
 * for given relation.
 */
static void
ColumnarOverwriteMetapage(Relation relation, ColumnarMetapage columnarMetapage)
{
	/* clear metapage because we are overwriting */
	bool clear = true;
	WriteToBlock(relation, COLUMNAR_METAPAGE_BLOCKNO, SizeOfPageHeaderData,
				 reinterpret_cast<char *>(&columnarMetapage), sizeof(ColumnarMetapage), clear);
}


/*
 * ColumnarMetapageRead - read the current contents of the metapage. Error if
 * it does not exist. Throw an error if the metapage is not the current
 * version, unless 'force' is true.
 *
 * NB: it's safe to read a different version of a metapage because we
 * guarantee that fields will only be added and existing fields will never be
 * changed. However, it's important that we don't depend on new fields being
 * set properly when we read an old metapage; an old metapage should only be
 * read for the purposes of upgrading or error checking.
 */
static ColumnarMetapage
ColumnarMetapageRead(Relation rel, bool force)
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
		bioseq_report<ERR, PG>()
			.msg("columnar metapage for relation \"{}\" does not exist",
				RelationGetRelationName(rel))
			.hint("{}", OLD_METAPAGE_VERSION_HINT);
	}

	/*
	 * Regardless of "force" parameter, always force read metapage block.
	 * We will check metapage version in ColumnarMetapageCheckVersion
	 * depending on "force".
	 */
	bool forceReadBlock = true;
	ColumnarMetapage metapage;
	ReadFromBlock(rel, COLUMNAR_METAPAGE_BLOCKNO, SizeOfPageHeaderData,
				  reinterpret_cast<char *>(&metapage), sizeof(ColumnarMetapage), forceReadBlock);

	if (!force)
	{
		ColumnarMetapageCheckVersion(rel, &metapage);
	}

	return metapage;
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
		bioseq_log<ERR, PG>(
			 "attempt to read columnar data of length {} from offset {} of block {} of relation {}",
			 len, offset, blockno, rel->rd_id);
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
		bioseq_log<ERR, PG>(
			 "attempt to write columnar data of length {} to offset {} of block {} of relation {}",
			 len, offset, blockno, rel->rd_id);
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
 * ColumnarMetapageIsCurrent - is the metapage at the latest version?
 */
static bool
ColumnarMetapageIsCurrent(ColumnarMetapage *metapage)
{
	return (metapage->versionMajor == COLUMNAR_VERSION_MAJOR &&
			metapage->versionMinor == COLUMNAR_VERSION_MINOR);
}


/*
 * ColumnarMetapageIsOlder - is the metapage older than the current version?
 */
static bool
ColumnarMetapageIsOlder(ColumnarMetapage *metapage)
{
	return (metapage->versionMajor < COLUMNAR_VERSION_MAJOR ||
			(metapage->versionMajor == COLUMNAR_VERSION_MAJOR &&
			 static_cast<int>(metapage->versionMinor) < static_cast<int>(COLUMNAR_VERSION_MINOR)));
}


/*
 * ColumnarMetapageIsNewer - is the metapage newer than the current version?
 */
static bool
ColumnarMetapageIsNewer(ColumnarMetapage *metapage)
{
	return (metapage->versionMajor > COLUMNAR_VERSION_MAJOR ||
			(metapage->versionMajor == COLUMNAR_VERSION_MAJOR &&
			 metapage->versionMinor > COLUMNAR_VERSION_MINOR));
}


/*
 * ColumnarMetapageCheckVersion - throw an error if accessing old
 * version of metapage.
 */
static void
ColumnarMetapageCheckVersion(Relation rel, ColumnarMetapage *metapage)
{
	if (!ColumnarMetapageIsCurrent(metapage))
	{
		bioseq_report<ERR, PG>()
			.code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("attempted to access relation \"{}\", which uses an older columnar format",
				RelationGetRelationName(rel))
			.detail("Columnar format version {}.{} is required, \"{}\" has version {}.{}.",
				COLUMNAR_VERSION_MAJOR, COLUMNAR_VERSION_MINOR,
				RelationGetRelationName(rel),
				metapage->versionMajor, metapage->versionMinor)
			.hint("{}", OLD_METAPAGE_VERSION_HINT);
	}
}


/*
 * test_bioseq_storage_write_new_page is a UDF only used for testing
 * purposes. It could make more sense to define this in bioseq_debug.c,
 * but the storage layer doesn't expose ColumnarMetapage to any other files,
 * so we define it here.
 */
POSTGRES_FN(test_bioseq_storage_write_new_page)
{
	Oid relationId = PG_GETARG_OID(0);

	Relation relation = safe_relation_open(relationId, AccessShareLock);

	/*
	 * Allocate a new page, write some data to there, and set reserved offset
	 * to the start of that page. That way, for a subsequent write operation,
	 * storage layer would try to overwrite the page that we allocated here.
	 */
	uint64_t newPageOffset = ColumnarStorageGetReservedOffset(relation, false);

	ColumnarStorageReserveData(relation, 100);
	ColumnarStorageWrite(relation, newPageOffset, "foo_bar", 8);

	ColumnarMetapage metapage = ColumnarMetapageRead(relation, false);
	metapage.reservedOffset = newPageOffset;
	ColumnarOverwriteMetapage(relation, metapage);

	relation_close(relation, AccessShareLock);

	PG_RETURN_VOID();
}
