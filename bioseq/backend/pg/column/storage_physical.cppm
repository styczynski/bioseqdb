/**
 * @file storage_physical.cppm
 * @brief Read and write columnar data through PostgreSQL pages and WAL.
 *
 * Logical offsets skip page headers. Block 0 holds the metapage and block 1
 * is reserved, so data starts in block 2 at first_logical_offset.
 *
 * A relation-extension lock protects reservations for bytes, rows and stripe
 * ids. Callers hold their normal relation lock during reads and writes.
 * Initialization and truncation require AccessExclusiveLock.
 */
module;
#include <bioseq/pg_compat.h>

#include "safe_lib.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

export module bioseq.postgres.storage.physical;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.postgres.support;

/// @brief Columnar format version stored in each metapage.
export constexpr uint32_t column_storage_format_major = 2;
export constexpr uint32_t column_storage_format_minor = 0;
/// @brief Payload bytes available after a PostgreSQL page header.
export constexpr Size usable_column_bytes_per_page =
	BLCKSZ - SizeOfPageHeaderData;

/// @brief Zero marks an invalid row number. Valid row numbers start at one.
export constexpr uint64_t invalid_column_row_number = static_cast<uint64_t>(0);
export constexpr uint64_t first_column_row_number = static_cast<uint64_t>(1);

/* Limits used to encode a logical row number in a PostgreSQL item pointer.
 * Keep its offset within the heap-tuple range and its block number within
 * the valid PostgreSQL block range.
 */
export constexpr uint64_t item_pointer_row_capacity =
	static_cast<uint64_t>(MaxHeapTuplesPerPage - FirstOffsetNumber + 1);
constexpr uint64_t item_pointer_block_count =
	static_cast<uint64_t>(MaxBlockNumber) + 1;

export constexpr uint64_t maximum_column_row_number =
	item_pointer_row_capacity * item_pointer_block_count - 1;
constexpr uint64_t maximum_column_row_reservation =
	maximum_column_row_number + 1;


/* Data offsets skip the metapage and the reserved second page. */
export constexpr uint64_t invalid_logical_offset = 0;
export constexpr uint64_t first_logical_offset = ((BLCKSZ - SizeOfPageHeaderData) * 2);
/* Leave room below PostgreSQL's relation-extension page-count limit.
 * Every reserved page must be creatable through ReadBuffer(P_NEW). */
constexpr uint64_t maximum_column_storage_page_count =
	static_cast<uint64_t>(MaxBlockNumber) - 1;
constexpr uint64_t maximum_logical_reservation =
	static_cast<uint64_t>(usable_column_bytes_per_page)
	* maximum_column_storage_page_count;
constexpr uint64_t maximum_logical_offset = maximum_logical_reservation - 1;

[[nodiscard]] inline constexpr bool
is_valid_logical_offset(uint64_t logical_offset) noexcept
{
	return logical_offset >= first_logical_offset
		&& logical_offset <= maximum_logical_offset;
}

[[nodiscard]] inline constexpr bool
is_valid_logical_reservation(uint64_t logical_reservation) noexcept
{
	return logical_reservation >= first_logical_offset
		&& logical_reservation <= maximum_logical_reservation;
}

/** @brief Storage id, format version and reservation counters in block 0. */
struct column_metapage
{
	/* Persistent format identity used to reject incompatible metapages. */
	uint32_t format_major;
	uint32_t format_minor;

	/* Link this data file to its rows in the metadata catalogs. */
	uint64_t storage_id;

	uint64_t next_stripe_id; /* first unused stripe id */
	uint64_t next_row_number; /* first unused row number */
	uint64_t next_logical_offset; /* first unused logical byte offset */

	/* Reserved field for unlogged-table reset handling. Writers keep it false. */
	bool requires_unlogged_reset;
};

static_assert(std::is_standard_layout_v<column_metapage>);
static_assert(sizeof(column_metapage) == 48);
static_assert(offsetof(column_metapage, format_major) == 0);
static_assert(offsetof(column_metapage, format_minor) == 4);
static_assert(offsetof(column_metapage, storage_id) == 8);
static_assert(offsetof(column_metapage, next_stripe_id) == 16);
static_assert(offsetof(column_metapage, next_row_number) == 24);
static_assert(offsetof(column_metapage, next_logical_offset) == 32);
static_assert(offsetof(column_metapage, requires_unlogged_reset) == 40);


/** @brief A physical block number and byte offset within its page. */
struct physical_address
{
	BlockNumber block_number;
	uint32_t offset;
};


/* Block 0 stores metadata. Block 1 stays empty, and data begins in block 2. */
inline constexpr BlockNumber metadata_block_number = 0;
inline constexpr BlockNumber reserved_empty_block_number = 1;
inline constexpr uint64_t first_stripe_id = 1;

inline constexpr const char* incompatible_format_hint =
	"Use \"VACUUM\" to upgrade the columnar table format "
	"version or run \"ALTER EXTENSION bioseq UPDATE\".";



/** @brief Convert a logical byte offset to a page address, skipping its header. */
static inline physical_address
logical_to_physical_address(uint64_t logical_offset)
{
	Assert(is_valid_logical_offset(logical_offset));
	const uint64_t block_index =
		logical_offset / static_cast<uint64_t>(usable_column_bytes_per_page);
	Assert(block_index < maximum_column_storage_page_count);
	return physical_address{
		.block_number = static_cast<BlockNumber>(block_index),
		.offset = static_cast<uint32_t>(
			SizeOfPageHeaderData
			+ (logical_offset
			   % static_cast<uint64_t>(usable_column_bytes_per_page))),
	};
}

[[noreturn]] static void
raise_logical_storage_range_error(
	Relation relation,
	const char* operation,
	uint64_t logical_offset,
	uint64_t amount)
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_PROGRAM_LIMIT_EXCEEDED,
		{
			.message = fmt::format(
				"cannot {} {} bytes at logical offset {} for relation {}",
				operation,
				amount,
				logical_offset,
				relation->rd_id),
			.detail = fmt::format(
				"Column storage logical offsets must stay in [{}, {}].",
				first_logical_offset,
				maximum_logical_offset),
		});
}

[[noreturn]] static void
raise_logical_storage_reservation_error(
	Relation relation,
	const char* operation,
	uint64_t logical_reservation)
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_PROGRAM_LIMIT_EXCEEDED,
		{
			.message = fmt::format(
				"cannot {} logical offset {} for relation {}",
				operation,
				logical_reservation,
				relation->rd_id),
			.detail = fmt::format(
				"Column storage reservations must stay in [{}, {}].",
				first_logical_offset,
				maximum_logical_reservation),
		});
}

[[noreturn]] static void
raise_column_row_reservation_error(
	Relation relation,
	uint64_t first_row_number,
	uint64_t row_count)
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_PROGRAM_LIMIT_EXCEEDED,
		{
			.message = fmt::format(
				"cannot reserve {} column rows starting at {} for relation {}",
				row_count,
				first_row_number,
				relation->rd_id),
			.detail = fmt::format(
				"The largest row number representable by a PostgreSQL item "
				"pointer is {}.",
				maximum_column_row_number),
		});
}

[[noreturn]] static void
raise_column_stripe_reservation_error(Relation relation, uint64_t stripe_id)
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_PROGRAM_LIMIT_EXCEEDED,
		{
			.message = fmt::format(
				"cannot reserve column stripe {} for relation {}",
				stripe_id,
				relation->rd_id),
			.detail = "The next stripe identifier would overflow its 64-bit "
					  "storage field.",
		});
}

static void
validate_logical_storage_range(
	Relation relation,
	const char* operation,
	uint64_t logical_offset,
	uint64_t amount)
{
	Assert(amount > 0);
	const std::optional<uint64_t> last_logical_offset =
		bioseq::common::checked_add(logical_offset, amount - 1);
	if (!is_valid_logical_offset(logical_offset)
		|| !last_logical_offset.has_value()
		|| !is_valid_logical_offset(*last_logical_offset))
	{
		raise_logical_storage_range_error(
			relation, operation, logical_offset, amount);
	}
}

/** @brief Copy bytes from a page while holding a shared buffer lock.
 * force skips the written-data boundary check for metapage inspection.
 * The read must always fit within the page.
 */
static void read_from_block(Relation rel,
                            BlockNumber block_number,
                            uint32_t offset,
                            char* destination,
                            uint32_t len,
                            bool force) {
    Buffer buffer = ReadBuffer(rel, block_number);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	Page page = BufferGetPage(buffer);
	PageHeader phdr = reinterpret_cast<PageHeader>(page);
	const std::optional<uint64_t> read_end = bioseq::common::checked_add(
		static_cast<uint64_t>(offset), static_cast<uint64_t>(len));

	if (!read_end.has_value()
		|| *read_end > static_cast<uint64_t>(BLCKSZ)
		|| (!force
			&& static_cast<uint64_t>(phdr->pd_lower) < *read_end))
	{
		UnlockReleaseBuffer(buffer);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempt to read columnar data of length {} from offset "
					"{} of block {} of relation {}",
					len, offset, block_number, rel->rd_id),
			});
	}

    memcpy_s(destination, len, page + offset, len);
    UnlockReleaseBuffer(buffer);
}

/** @brief Write bytes to a page and record the change with generic WAL.
 * A clear write reinitializes the page and starts after its header. Other
 * writes may append at pd_lower or overwrite from an earlier offset.
 * The end of this write becomes the page's written-data boundary.
 */
static void write_to_block(Relation rel,
                           BlockNumber block_number,
                           uint32_t offset,
                           const char* source,
                           uint32_t len,
                           bool clear) {
    Buffer buffer = ReadBuffer(rel, block_number);
	GenericXLogState *state = GenericXLogStart(rel);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	Page page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);

	PageHeader phdr = reinterpret_cast<PageHeader>(page);
	if (PageIsNew(page) || clear)
	{
		PageInit(page, BLCKSZ, 0);
	}

	const std::optional<uint64_t> write_end = bioseq::common::checked_add(
		static_cast<uint64_t>(offset), static_cast<uint64_t>(len));
	if (!write_end.has_value()
		|| offset < SizeOfPageHeaderData
		|| (clear && offset != SizeOfPageHeaderData)
		|| offset > phdr->pd_upper
		|| phdr->pd_lower < offset
		|| *write_end > static_cast<uint64_t>(phdr->pd_upper))
	{
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buffer);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempt to write columnar data of length {} to offset "
					"{} of block {} of relation {}",
					len, offset, block_number, rel->rd_id),
			});
	}

	/* An abandoned append can leave pd_lower past our reserved offset.
	 * Move it back before overwriting those bytes. */
	if (phdr->pd_lower > offset)
	{
		bioseq_report_pg(DEBUG4)
			.with_message("overwriting page {}", block_number)
			.with_detail("This can happen after a roll-back.");
		phdr->pd_lower = offset;
	}

    memcpy_s(page + phdr->pd_lower, phdr->pd_upper - phdr->pd_lower, source, len);
    phdr->pd_lower = static_cast<LocationIndex>(*write_end);

	GenericXLogFinish(state);

	UnlockReleaseBuffer(buffer);
}

/// @brief Round a reservation up to the next page payload boundary.
/// Return no value if the offset is invalid or rounding exceeds storage limits.
static std::optional<uint64_t>
align_storage_reservation(uint64_t previous_reservation)
{
	if (!is_valid_logical_reservation(previous_reservation))
	{
		return std::nullopt;
	}

	const uint64_t page_payload_bytes =
		static_cast<uint64_t>(usable_column_bytes_per_page);
	const uint64_t page_offset = previous_reservation % page_payload_bytes;
	if (page_offset == 0)
	{
		return previous_reservation;
	}

	const std::optional<uint64_t> aligned_reservation =
		bioseq::common::checked_add(
			previous_reservation, page_payload_bytes - page_offset);
	if (!aligned_reservation.has_value()
		|| !is_valid_logical_reservation(*aligned_reservation))
	{
		return std::nullopt;
	}
	return aligned_reservation;
}

/// @brief Check whether the metapage uses the current format version.
static bool
column_metapage_uses_current_format(const column_metapage* metapage) noexcept
{
	return (metapage->format_major == column_storage_format_major &&
			metapage->format_minor == column_storage_format_minor);
}

/// @brief Check whether the metapage format is older than this build's format.
static bool
column_metapage_format_is_older(const column_metapage* metapage) noexcept
{
	return (metapage->format_major < column_storage_format_major ||
			(metapage->format_major == column_storage_format_major &&
			 metapage->format_minor < column_storage_format_minor));
}

/// @brief Check whether the metapage format is newer than this build's format.
static bool
column_metapage_format_is_newer(const column_metapage* metapage) noexcept
{
	return (metapage->format_major > column_storage_format_major ||
			(metapage->format_major == column_storage_format_major &&
			 metapage->format_minor > column_storage_format_minor));
}

/// @brief Reject a metapage whose format differs from this build's format.
static void
validate_column_metapage_format(Relation rel, column_metapage *metapage)
{
	if (!column_metapage_uses_current_format(metapage))
	{
		const char* const relation_name =
			RelationGetRelationName(rel);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_FEATURE_NOT_SUPPORTED,
			{
				.message = fmt::format(
					"attempted to access relation \"{}\", which uses an "
					"older columnar format",
					relation_name),
				.detail = fmt::format(
					"BioseqColumn format version {}.{} is required, \"{}\" "
					"has version {}.{}.",
					column_storage_format_major,
					column_storage_format_minor,
					relation_name,
					metapage->format_major,
					metapage->format_minor),
				.hint = incompatible_format_hint,
			});
	}
}

/**
 * @brief Read the metapage, checking its format unless force is true.
 *
 * A missing metapage raises an error. Forced reads support format changes
 * and diagnostics. Callers must only use fields defined by the stored version.
 */
static column_metapage
read_column_metapage(Relation rel, bool force)
{
	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	if (nblocks == 0)
	{
		/* An initialized column relation always has a metapage. */
		const char* const relation_name =
			RelationGetRelationName(rel);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"columnar metapage for relation \"{}\" does not exist",
					relation_name),
				.hint = incompatible_format_hint,
			});
	}

	/* Read the metapage without trusting pd_lower. Validate its format below
	 * unless the caller requested a forced read. */
	constexpr bool force_block_read = true;
	column_metapage metapage;
	read_from_block(rel, metadata_block_number, SizeOfPageHeaderData,
				  reinterpret_cast<char *>(&metapage), sizeof(column_metapage), force_block_read);

	if (!force)
	{
		validate_column_metapage_format(rel, &metapage);
	}

	return metapage;
}

/** @brief Replace the metapage contents and record the change in WAL. */
static void
overwrite_column_metapage(Relation relation, const column_metapage& metapage)
{
	/* Reinitialize the page before replacing its contents. */
	constexpr bool clear = true;
	write_to_block(relation, metadata_block_number, SizeOfPageHeaderData,
				 reinterpret_cast<const char*>(&metapage), sizeof(column_metapage), clear);
}



/**
 * @brief Create the metapage and reserved empty page for a new columnar relation.
 * @pre The caller holds AccessExclusiveLock and the main fork is empty.
 */
export void
initialize_column_storage(SMgrRelation srel, uint64_t storage_id)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() entered storage_id={}", storage_id);
	BlockNumber nblocks = smgrnblocks(srel, MAIN_FORKNUM);

	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() existing_blocks={}", nblocks);
	if (nblocks > 0)
	{
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempted to initialize a columnar metapage, but {} "
					"pages already exist",
					nblocks),
			});
	}

	/* Create the metapage and reserved second page. */
	PGIOAlignedBlock block;
	Page page = block.data;

	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() initialising metapage");
	PageInit(page, BLCKSZ, 0);
	PageHeader phdr = reinterpret_cast<PageHeader>(page);

	column_metapage metapage = { 0 };
	metapage.storage_id = storage_id;
	metapage.format_major = column_storage_format_major;
	metapage.format_minor = column_storage_format_minor;
	metapage.next_stripe_id = first_stripe_id;
	metapage.next_row_number = first_column_row_number;
	metapage.next_logical_offset = first_logical_offset;
	metapage.requires_unlogged_reset = false;
	memcpy_s(page + phdr->pd_lower, phdr->pd_upper - phdr->pd_lower,
			 reinterpret_cast<char *>(&metapage), sizeof(column_metapage));
	phdr->pd_lower += sizeof(column_metapage);
	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
				metadata_block_number, page, true);

	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() flushing metapage to disk");
#if PG_VERSION_NUM >= 190000
	PageSetChecksum(page, metadata_block_number);
#else
	PageSetChecksumInplace(page, metadata_block_number);
#endif
	smgrextend(srel, MAIN_FORKNUM, metadata_block_number, page, true);

	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() initialising empty page");
	PageInit(page, BLCKSZ, 0);

	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
				reserved_empty_block_number, page, true);

	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() flushing empty page to disk");
#if PG_VERSION_NUM >= 190000
	PageSetChecksum(page, reserved_empty_block_number);
#else
	PageSetChecksumInplace(page, reserved_empty_block_number);
#endif
	smgrextend(srel, MAIN_FORKNUM, reserved_empty_block_number, page, true);

	/* These pages bypass shared_buffers. Sync them now because a concurrent
	 * checkpoint could advance its redo point past the WAL records before
	 * the direct writes reach disk. */
	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_storage() requesting smgr immediate sync");
	smgrimmedsync(srel, MAIN_FORKNUM);
}


/** @brief Update a metapage's format and reconstructed reservation counters.
 * Require an existing metapage. is_upgrade allows an older source version,
 * while a downgrade allows a newer one. The storage id stays unchanged.
 */
export void
update_column_storage_format(Relation rel, bool is_upgrade, uint64_t next_stripe_id,
							 uint64_t next_row_number, uint64_t next_logical_offset)
{
	LockRelationForExtension(rel, ExclusiveLock);

	column_metapage metapage = read_column_metapage(rel, true);

	if (column_metapage_uses_current_format(&metapage))
	{
		/* The metapage already uses the requested format. */
		UnlockRelationForExtension(rel, ExclusiveLock);
		return;
	}

	if (is_upgrade && column_metapage_format_is_newer(&metapage))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = "found a newer columnar metapage while upgrading",
			});
	}

	if (!is_upgrade && column_metapage_format_is_older(&metapage))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = "found an older columnar metapage while downgrading",
			});
	}

	metapage.format_major = column_storage_format_major;
	metapage.format_minor = column_storage_format_minor;

	/* storage_id remains the same */
	metapage.next_stripe_id = next_stripe_id;
	metapage.next_row_number = next_row_number;
	metapage.next_logical_offset = next_logical_offset;

	overwrite_column_metapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);
}


/**
 * @brief Read the relation's persistent storage id from its metapage.
 * @param force Skip the format-version check when true.
 * @return Storage id. A missing metapage still raises an error.
 */
export uint64_t
column_storage_id(Relation rel, bool force)
{
	const column_metapage metapage = read_column_metapage(rel, force);

	return metapage.storage_id;
}


/** @return Whether the relation has a metapage in the current storage format. */
export bool
column_storage_uses_current_format(Relation rel)
{
	const BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	if (nblocks < 2)
	{
		return false;
	}

	const column_metapage metapage = read_column_metapage(rel, true);
	return column_metapage_uses_current_format(&metapage);
}


/**
 * @brief Reserve consecutive row numbers while holding the extension lock.
 * @return First row number in the range. Reject ranges beyond TID capacity.
 */
export uint64_t
reserve_column_row_numbers(Relation rel, uint64_t nrows)
{
	LockRelationForExtension(rel, ExclusiveLock);

	column_metapage metapage = read_column_metapage(rel, false);

	const uint64_t first_row_number = metapage.next_row_number;
	const std::optional<uint64_t> next_row_number =
		bioseq::common::checked_add(first_row_number, nrows);
	if (first_row_number < first_column_row_number
		|| !next_row_number.has_value()
		|| *next_row_number > maximum_column_row_reservation)
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		raise_column_row_reservation_error(rel, first_row_number, nrows);
	}
	metapage.next_row_number = *next_row_number;

	overwrite_column_metapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	return first_row_number;
}


/**
 * @brief Reserve a stripe id while holding the relation-extension lock.
 * @return The reserved id. Row numbers are reserved separately.
 */
export uint64_t
reserve_column_stripe_id(Relation rel)
{
	LockRelationForExtension(rel, ExclusiveLock);

	column_metapage metapage = read_column_metapage(rel, false);

	const uint64_t stripe_id = metapage.next_stripe_id;
	const std::optional<uint64_t> next_stripe_id =
		bioseq::common::checked_add(stripe_id, static_cast<uint64_t>(1));
	if (stripe_id < first_stripe_id || !next_stripe_id.has_value())
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		raise_column_stripe_reservation_error(rel, stripe_id);
	}
	metapage.next_stripe_id = *next_stripe_id;

	overwrite_column_metapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	return stripe_id;
}


/**
 * @brief Reserve a separate page-aligned byte range for a columnar write.
 * Extend the relation to contain the range while holding the extension lock.
 * @return First logical byte offset, or invalid_logical_offset for zero bytes.
 */
export uint64_t
reserve_column_storage_bytes(Relation rel, uint64_t amount)
{
	if (amount == 0)
	{
		return invalid_logical_offset;
	}

	LockRelationForExtension(rel, ExclusiveLock);

	column_metapage metapage = read_column_metapage(rel, false);

	const std::optional<uint64_t> aligned_reservation =
		align_storage_reservation(metapage.next_logical_offset);
	const std::optional<uint64_t> next_reservation =
		aligned_reservation.has_value()
			? bioseq::common::checked_add(*aligned_reservation, amount)
			: std::nullopt;
	if (!aligned_reservation.has_value()
		|| !next_reservation.has_value()
		|| !is_valid_logical_reservation(*next_reservation))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		raise_logical_storage_range_error(
			rel, "reserve", metapage.next_logical_offset, amount);
	}
	metapage.next_logical_offset = *next_reservation;

	/* Save the new end of reserved storage. */
	overwrite_column_metapage(rel, metapage);

	/* Find the page containing the range's last byte. */
	const physical_address final =
		logical_to_physical_address(*next_reservation - 1);

	/* Add enough pages to hold the reserved range. */
	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	while (nblocks <= final.block_number)
	{
		Buffer new_buffer = ReadBuffer(rel, P_NEW);
		Assert(BufferGetBlockNumber(new_buffer) == nblocks);
		ReleaseBuffer(new_buffer);
		if (nblocks == final.block_number)
		{
			break;
		}
		++nblocks;
	}

	UnlockRelationForExtension(rel, ExclusiveLock);

	return *aligned_reservation;
}


/** @brief Reads a logical byte range across the required PostgreSQL pages. */
export void
read_column_storage(Relation rel, uint64_t logical_offset, char *data, uint32_t amount)
{
	/* A zero-length read accepts any offset. */
	if (amount == 0)
	{
		return;
	}

	validate_logical_storage_range(rel, "read", logical_offset, amount);

	uint64_t read = 0;

	while (read < amount)
	{
		const physical_address addr =
			logical_to_physical_address(logical_offset + read);

		const uint32_t to_read = static_cast<uint32_t>(
			std::min<uint64_t>(amount - read, BLCKSZ - addr.offset));
		read_from_block(rel, addr.block_number, addr.offset, data + read, to_read,
					  false);

		read += to_read;
	}
}


/** @brief Write a reserved logical byte range across PostgreSQL pages. */
export void
write_column_storage(Relation rel, uint64_t logical_offset, const char *data, uint32_t amount)
{
	/* A zero-length write accepts any offset. */
	if (amount == 0)
	{
		return;
	}

	validate_logical_storage_range(rel, "write", logical_offset, amount);

	uint64_t written = 0;

	while (written < amount)
	{
		const physical_address addr =
			logical_to_physical_address(logical_offset + written);

		const uint32_t to_write = static_cast<uint32_t>(
			std::min<uint64_t>(amount - written, BLCKSZ - addr.offset));
		write_to_block(rel, addr.block_number, addr.offset, data + written, to_write,
					 false);

		written += to_write;
	}
}


/**
 * @brief Reduce the reserved byte range and remove pages beyond its end.
 * @pre The caller holds AccessExclusiveLock on the relation.
 * @return true if pages were removed. The reservation can shrink within
 * the last page while this function returns false.
 */
export bool
truncate_column_storage(Relation rel, uint64_t new_data_reservation)
{
	if (!is_valid_logical_reservation(new_data_reservation))
	{
		raise_logical_storage_reservation_error(
			rel, "truncate column storage at", new_data_reservation);
	}
	// Empty data still keeps the metapage and reserved page.
 // It has no final data byte to convert to a physical address.
	const BlockNumber final_block = new_data_reservation == first_logical_offset
		? reserved_empty_block_number
		: logical_to_physical_address(new_data_reservation - 1).block_number;
	const std::optional<uint64_t> new_page_count =
		bioseq::common::checked_add(
			static_cast<uint64_t>(final_block),
			static_cast<uint64_t>(1));
	if (!new_page_count.has_value()
		|| *new_page_count > maximum_column_storage_page_count)
	{
		raise_logical_storage_reservation_error(
			rel, "truncate column storage at", new_data_reservation);
	}
	const BlockNumber new_rel_pages =
		static_cast<BlockNumber>(*new_page_count);

	BlockNumber old_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	if (old_rel_pages == 0)
	{
		/* The relation has no pages to truncate. */
		return false;
	}

	LockRelationForExtension(rel, ExclusiveLock);

	column_metapage metapage = read_column_metapage(rel, false);
	if (new_rel_pages > old_rel_pages)
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"cannot truncate relation {} from {} pages to {} pages",
					rel->rd_id,
					old_rel_pages,
					new_rel_pages),
			});
	}

	if (metapage.next_logical_offset < new_data_reservation)
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempted to truncate relation {} to offset {}, which "
					"is higher than its reserved offset {}",
					rel->rd_id,
					new_data_reservation,
					metapage.next_logical_offset),
			});
	}

	if (metapage.next_logical_offset == new_data_reservation)
	{
		/* The reservation already ends at this offset. */
		UnlockRelationForExtension(rel, ExclusiveLock);
		return false;
	}

	metapage.next_logical_offset = new_data_reservation;

	/* Save the reduced reservation before removing pages. */
	overwrite_column_metapage(rel, metapage);

	UnlockRelationForExtension(rel, ExclusiveLock);

	/* RelationTruncate records page removal in WAL. */
	if (new_rel_pages < old_rel_pages)
	{
		RelationTruncate(rel, new_rel_pages);
		return true;
	}

	return false;
}
