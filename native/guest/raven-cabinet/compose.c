/* FCI: packs files into cabinets. Compressed blocks wait in a temporary file until a volume is written; a volume that fills up takes the blocks it
   has room for and the folder goes on in the next volume, with the file cut there marked on both sides. Writes stored and MSZIP folders. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fci.h>
#include <zlib.h>

#include "format.h"

#define FCI_MAGIC 0x52464349u

struct pending_block
{
	uint32_t temp_offset, size, produced;
};

struct pending_folder
{
	uint32_t first_block, type;
};

struct pending_file
{
	char *name;
	uint32_t size, offset, folder, date, time, attributes;
	int continued;
};

struct composer
{
	uint32_t magic;
	ERF *error;
	void *user;
	PFNFCIFILEPLACED placed;
	PFNFCIALLOC alloc;
	PFNFCIFREE release;
	PFNFCIOPEN open;
	PFNFCIREAD read;
	PFNFCIWRITE write;
	PFNFCICLOSE close;
	PFNFCISEEK seek;
	PFNFCIDELETE remove;
	CCAB *caller;
	CCAB current;
	char previous_cabinet[CB_MAX_CABINET_NAME], previous_disk[CB_MAX_DISK_NAME];
	int has_previous;
	char temp_name[CB_MAX_FILENAME];
	INT_PTR temp;
	uint32_t temp_size;
	struct pending_block *blocks;
	uint32_t block_count, block_capacity;
	struct pending_folder *folders;
	uint32_t folder_count, folder_capacity;
	struct pending_file *files;
	uint32_t file_count, file_capacity;
	/* The folder files go into now: at the end of the list when open, and how much it has taken so far. */
	int folder_open;
	uint32_t folder_type, folder_position, folder_compressed;
	z_stream deflater;
	int deflater_ready;
	uint32_t buffered;
	unsigned char buffer[block_output_limit];
	unsigned char compressed[block_input_limit + 64];
};

static void set_failure(struct composer *fci, int operation, int code)
{
	fci->error->erfOper = operation;
	fci->error->erfType = code;
	fci->error->fError = TRUE;
	if (code)
		SetLastError((DWORD)code);
}

static voidpf zlib_alloc(voidpf context, uInt items, uInt size)
{
	struct composer *fci = context;
	return fci->alloc(items * size);
}

static void zlib_free(voidpf context, voidpf memory)
{
	struct composer *fci = context;
	fci->release(memory);
}

static int grow(struct composer *fci, void **list, uint32_t *capacity, uint32_t count, size_t size)
{
	if (count < *capacity)
		return 1;
	uint32_t wider = *capacity ? *capacity * 2 : 64;
	void *larger = fci->alloc((ULONG)(wider * size));
	if (!larger) {
		set_failure(fci, FCIERR_ALLOC_FAIL, ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	if (*list) {
		CopyMemory(larger, *list, count * size);
		fci->release(*list);
	}
	*list = larger;
	*capacity = wider;
	return 1;
}

static uint32_t directory_size(const struct composer *fci, const char *next_cabinet, const char *next_disk)
{
	const CCAB *cabinet = &fci->current;
	uint32_t size = header_size;
	if (cabinet->cbReserveCFHeader || cabinet->cbReserveCFFolder || cabinet->cbReserveCFData)
		size += 4 + cabinet->cbReserveCFHeader;
	if (fci->has_previous)
		size += (uint32_t)(lstrlenA(fci->previous_cabinet) + lstrlenA(fci->previous_disk) + 2);
	if (next_cabinet)
		size += (uint32_t)(lstrlenA(next_cabinet) + lstrlenA(next_disk) + 2);
	size += fci->folder_count * (folder_entry_size + cabinet->cbReserveCFFolder);
	for (uint32_t index = 0; index < fci->file_count; index++)
		size += file_entry_size + (uint32_t)lstrlenA(fci->files[index].name) + 1;
	return size;
}

static uint32_t data_size(const struct composer *fci)
{
	uint32_t size = 0;
	for (uint32_t index = 0; index < fci->block_count; index++)
		size += block_header_size + fci->current.cbReserveCFData + fci->blocks[index].size;
	return size;
}

static int write_out(struct composer *fci, INT_PTR file, const void *data, UINT size)
{
	int code = 0;
	if (fci->write(file, (void *)data, size, &code, fci->user) != size) {
		set_failure(fci, FCIERR_CAB_FILE, code);
		return 0;
	}
	return 1;
}

/* Writes one volume holding `taken` blocks, the first `fragment` bytes of the block after them (a block with no output size, which readers take
   as going on in the next volume) and the files all that completes; what is left stays pending for the next. */
static int write_volume(struct composer *fci, uint32_t taken, uint32_t fragment, const CCAB *next, PFNFCISTATUS status)
{
	const CCAB *cabinet = &fci->current;
	char path[CB_MAX_CAB_PATH + CB_MAX_CABINET_NAME + 2];
	lstrcpynA(path, cabinet->szCabPath, CB_MAX_CAB_PATH);
	lstrcatA(path, cabinet->szCab);

	/* Which folders the volume holds, and how far into the last it gets. */
	uint32_t held_folders = 0, cut_folder = fci->folder_count, cut_position = 0;
	for (uint32_t index = 0; index < fci->folder_count; index++) {
		uint32_t first = fci->folders[index].first_block;
		uint32_t end = index + 1 < fci->folder_count ? fci->folders[index + 1].first_block : fci->block_count;
		if (first > taken || (first == taken && end > first && !fragment))
			break;
		held_folders = index + 1;
		if (end > taken) {
			cut_folder = index;
			for (uint32_t block = first; block < taken; block++)
				cut_position += fci->blocks[block].produced;
			break;
		}
	}
	/* A file that starts in the block being split is listed here too, as running on: the reader meets it before it gets to the next volume. */
	uint32_t started = cut_position + (fragment ? fci->blocks[taken].produced : 0);
	uint32_t reserve_field = (cabinet->cbReserveCFHeader || cabinet->cbReserveCFFolder || cabinet->cbReserveCFData) ? 4 + cabinet->cbReserveCFHeader : 0;
	uint32_t held_files = 0, names_size = 0;
	for (uint32_t index = 0; index < fci->file_count; index++) {
		const struct pending_file *file = &fci->files[index];
		if (file->folder >= held_folders)
			continue;
		if (file->folder == cut_folder && file->offset >= started && file->size)
			continue;
		held_files++;
		names_size += file_entry_size + (uint32_t)lstrlenA(file->name) + 1;
	}
	uint32_t links_size = (fci->has_previous ? (uint32_t)(lstrlenA(fci->previous_cabinet) + lstrlenA(fci->previous_disk) + 2) : 0) +
			      (next ? (uint32_t)(lstrlenA(next->szCab) + lstrlenA(next->szDisk) + 2) : 0);
	uint32_t folders_offset = header_size + reserve_field + links_size;
	uint32_t files_offset = folders_offset + held_folders * (folder_entry_size + cabinet->cbReserveCFFolder);
	uint32_t data_offset = files_offset + names_size;
	uint32_t total = data_offset;
	for (uint32_t index = 0; index < taken; index++)
		total += block_header_size + cabinet->cbReserveCFData + fci->blocks[index].size;
	if (fragment)
		total += block_header_size + cabinet->cbReserveCFData + fragment;

	int code = 0;
	INT_PTR out = fci->open(path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE, &code, fci->user);
	if (out == -1) {
		set_failure(fci, FCIERR_CAB_FILE, code);
		return 0;
	}
	int good = 0;
	unsigned char header[header_size + 4] = { 0 };
	put_32(header, cabinet_signature);
	put_32(header + 8, total);
	put_32(header + 16, files_offset);
	header[24] = 3;
	header[25] = 1;
	put_16(header + 26, held_folders);
	put_16(header + 28, held_files);
	put_16(header + 30, (fci->has_previous ? flag_has_previous : 0u) | (next ? flag_has_next : 0u) | (reserve_field ? flag_has_reserve : 0u));
	put_16(header + 32, cabinet->setID);
	put_16(header + 34, (uint32_t)cabinet->iCab);
	put_16(header + 36, cabinet->cbReserveCFHeader);
	header[38] = (unsigned char)cabinet->cbReserveCFFolder;
	header[39] = (unsigned char)cabinet->cbReserveCFData;
	unsigned char blank[256];
	ZeroMemory(blank, sizeof(blank));
	if (!write_out(fci, out, header, header_size + (reserve_field ? 4 : 0)))
		goto done;
	for (uint32_t left = cabinet->cbReserveCFHeader; reserve_field && left;) {
		UINT run = left > sizeof(blank) ? sizeof(blank) : left;
		if (!write_out(fci, out, blank, run))
			goto done;
		left -= run;
	}
	if (fci->has_previous && (!write_out(fci, out, fci->previous_cabinet, (UINT)lstrlenA(fci->previous_cabinet) + 1) ||
				  !write_out(fci, out, fci->previous_disk, (UINT)lstrlenA(fci->previous_disk) + 1)))
		goto done;
	if (next && (!write_out(fci, out, next->szCab, (UINT)lstrlenA(next->szCab) + 1) || !write_out(fci, out, next->szDisk, (UINT)lstrlenA(next->szDisk) + 1)))
		goto done;

	uint32_t position = data_offset;
	for (uint32_t index = 0; index < held_folders; index++) {
		uint32_t first = fci->folders[index].first_block;
		uint32_t end = index + 1 < fci->folder_count ? fci->folders[index + 1].first_block : fci->block_count;
		int cut_here = end > taken;
		if (cut_here)
			end = taken;
		unsigned char entry[folder_entry_size];
		put_32(entry, position);
		put_16(entry + 4, end - first + (cut_here && fragment ? 1 : 0));
		put_16(entry + 6, fci->folders[index].type);
		if (!write_out(fci, out, entry, sizeof(entry)) || (cabinet->cbReserveCFFolder && !write_out(fci, out, blank, cabinet->cbReserveCFFolder)))
			goto done;
		for (uint32_t block = first; block < end; block++)
			position += block_header_size + cabinet->cbReserveCFData + fci->blocks[block].size;
	}

	for (uint32_t index = 0; index < fci->file_count; index++) {
		const struct pending_file *file = &fci->files[index];
		if (file->folder >= held_folders || (file->folder == cut_folder && file->offset >= started && file->size))
			continue;
		int runs_on = file->folder == cut_folder && file->offset + file->size > cut_position;
		uint32_t folder = file->folder;
		if (file->continued)
			folder = runs_on ? folder_both_ways : folder_from_previous;
		else if (runs_on)
			folder = folder_to_next;
		unsigned char entry[file_entry_size];
		put_32(entry, file->size);
		put_32(entry + 4, file->offset);
		put_16(entry + 8, folder);
		put_16(entry + 10, file->date);
		put_16(entry + 12, file->time);
		put_16(entry + 14, file->attributes);
		if (!write_out(fci, out, entry, sizeof(entry)) || !write_out(fci, out, file->name, (UINT)lstrlenA(file->name) + 1))
			goto done;
		fci->placed(&fci->current, file->name, (LONG)file->size, file->continued, fci->user);
	}

	uint32_t copied = 0, all = fragment;
	for (uint32_t index = 0; index < taken; index++)
		all += fci->blocks[index].size;
	for (uint32_t index = 0; index < taken + (fragment ? 1 : 0); index++) {
		const struct pending_block *block = &fci->blocks[index];
		uint32_t size = index < taken ? block->size : fragment;
		code = 0;
		if (fci->seek(fci->temp, (LONG)block->temp_offset, SEEK_SET, &code, fci->user) == -1 ||
		    fci->read(fci->temp, fci->compressed, size, &code, fci->user) != size) {
			set_failure(fci, FCIERR_TEMP_FILE, code);
			goto done;
		}
		unsigned char entry[block_header_size];
		put_16(entry + 4, size);
		put_16(entry + 6, index < taken ? block->produced : 0);
		put_32(entry, block_checksum(entry + 4, 4, block_checksum(fci->compressed, size, 0)));
		if (!write_out(fci, out, entry, sizeof(entry)) || (cabinet->cbReserveCFData && !write_out(fci, out, blank, cabinet->cbReserveCFData)) ||
		    !write_out(fci, out, fci->compressed, size))
			goto done;
		copied += size;
		if (status(statusFolder, copied, all, fci->user) == -1) {
			set_failure(fci, FCIERR_USER_ABORT, 0);
			goto done;
		}
	}
	good = 1;
done:
	code = 0;
	fci->close(out, &code, fci->user);
	if (!good)
		return 0;
	status(statusCabinet, total, total, fci->user);

	/* Keep what did not fit, rebased on the next volume: the folders it is in move to the front and offsets count from the cut. */
	uint32_t kept = 0;
	uint32_t removed_folders = cut_folder < held_folders ? cut_folder : held_folders;
	for (uint32_t index = 0; index < fci->file_count; index++) {
		struct pending_file file = fci->files[index];
		if (file.folder < held_folders && !(file.folder == cut_folder && file.offset + file.size > cut_position)) {
			fci->release(file.name);
			continue;
		}
		if (file.folder == cut_folder) {
			if (file.offset < started)
				file.continued = 1;
			file.offset = file.offset < cut_position ? 0 : file.offset - cut_position;
		}
		file.folder -= removed_folders;
		fci->files[kept++] = file;
	}
	fci->file_count = kept;
	if (fci->folder_open && cut_folder < held_folders && cut_folder == fci->folder_count - 1)
		fci->folder_position -= cut_position;
	uint32_t folders_kept = 0;
	for (uint32_t index = removed_folders; index < fci->folder_count; index++) {
		fci->folders[folders_kept] = fci->folders[index];
		fci->folders[folders_kept].first_block = fci->folders[index].first_block > taken ? fci->folders[index].first_block - taken : 0;
		folders_kept++;
	}
	fci->folder_count = folders_kept;
	MoveMemory(fci->blocks, fci->blocks + taken, (fci->block_count - taken) * sizeof(*fci->blocks));
	fci->block_count -= taken;
	if (fragment) {
		fci->blocks[0].temp_offset += fragment;
		fci->blocks[0].size -= fragment;
	}

	lstrcpynA(fci->previous_cabinet, cabinet->szCab, sizeof(fci->previous_cabinet));
	lstrcpynA(fci->previous_disk, cabinet->szDisk, sizeof(fci->previous_disk));
	fci->has_previous = next != NULL;
	return 1;
}

static int next_descriptor(struct composer *fci, PFNFCIGETNEXTCABINET next_cabinet, uint32_t estimate, CCAB *next)
{
	fci->caller->iCab++;
	if (!next_cabinet(fci->caller, estimate, fci->user)) {
		set_failure(fci, FCIERR_NONE, ERROR_FUNCTION_FAILED);
		return 0;
	}
	*next = *fci->caller;
	return 1;
}

static int block_starts_folder(const struct composer *fci, uint32_t block)
{
	for (uint32_t index = 0; index < fci->folder_count; index++) {
		if (fci->folders[index].first_block == block)
			return 1;
	}
	return 0;
}

/* Moves full volumes out while what is pending does not fit the current one. A volume always takes at least one block, so this ends. */
static int spill_volumes(struct composer *fci, PFNFCIGETNEXTCABINET next_cabinet, PFNFCISTATUS status)
{
	while (fci->block_count > 1) {
		CCAB next = fci->current;
		/* Link names count against the limit, and they are not known yet: an upper bound keeps the volume inside it. */
		uint32_t directory = directory_size(fci, NULL, NULL) + CB_MAX_CABINET_NAME + CB_MAX_DISK_NAME;
		uint32_t limit = fci->current.cb;
		if (directory + data_size(fci) <= limit)
			return 1;
		uint32_t room = limit > directory ? limit - directory : 0, taken = 0;
		while (taken < fci->block_count) {
			uint32_t cost = block_header_size + fci->current.cbReserveCFData + fci->blocks[taken].size;
			if (cost > room)
				break;
			room -= cost;
			taken++;
		}
		/* Inside a folder the volume ends on part of a block, however little room is left: giving back a whole block makes the room. */
		uint32_t fragment = 0;
		if (taken && block_starts_folder(fci, taken)) {
			fragment = 0;
		} else if (fci->blocks[taken].size > 1) {
			uint32_t overhead = block_header_size + fci->current.cbReserveCFData;
			if (room <= overhead && taken) {
				taken--;
				room += overhead + fci->blocks[taken].size;
			}
			fragment = room > overhead ? room - overhead : 1;
			if (fragment >= fci->blocks[taken].size)
				fragment = fci->blocks[taken].size - 1;
		} else if (!taken) {
			taken = 1;
		}
		if (!next_descriptor(fci, next_cabinet, limit, &next) || !write_volume(fci, taken, fragment, &next, status))
			return 0;
		fci->current = next;
	}
	return 1;
}

static int finish_block(struct composer *fci, PFNFCISTATUS status)
{
	if (!fci->buffered)
		return 1;
	const unsigned char *data = fci->buffer;
	uint32_t size = fci->buffered;
	if (fci->folder_type == type_mszip) {
		if (!fci->deflater_ready) {
			ZeroMemory(&fci->deflater, sizeof(fci->deflater));
			fci->deflater.zalloc = zlib_alloc;
			fci->deflater.zfree = zlib_free;
			fci->deflater.opaque = fci;
			if (deflateInit2(&fci->deflater, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
				set_failure(fci, FCIERR_ALLOC_FAIL, ERROR_NOT_ENOUGH_MEMORY);
				return 0;
			}
			fci->deflater_ready = 1;
		} else {
			deflateReset(&fci->deflater);
		}
		/* Every block deflates alone, without the block before as history, so a reader can start at a later volume. */
		fci->compressed[0] = 'C';
		fci->compressed[1] = 'K';
		fci->deflater.next_in = fci->buffer;
		fci->deflater.avail_in = fci->buffered;
		fci->deflater.next_out = fci->compressed + 2;
		fci->deflater.avail_out = sizeof(fci->compressed) - 2;
		if (deflate(&fci->deflater, Z_FINISH) != Z_STREAM_END) {
			set_failure(fci, FCIERR_MCI_FAIL, 0);
			return 0;
		}
		data = fci->compressed;
		size = (uint32_t)fci->deflater.total_out + 2;
	}

	if (!grow(fci, (void **)&fci->blocks, &fci->block_capacity, fci->block_count, sizeof(*fci->blocks)))
		return 0;
	int code = 0;
	if (fci->seek(fci->temp, (LONG)fci->temp_size, SEEK_SET, &code, fci->user) == -1 || fci->write(fci->temp, (void *)data, size, &code, fci->user) != size) {
		set_failure(fci, FCIERR_TEMP_FILE, code);
		return 0;
	}
	struct pending_block *block = &fci->blocks[fci->block_count++];
	block->temp_offset = fci->temp_size;
	block->size = size;
	block->produced = fci->buffered;
	fci->temp_size += size;
	fci->folder_compressed += size;
	fci->buffered = 0;
	if (status(statusFile, size, block->produced, fci->user) == -1) {
		set_failure(fci, FCIERR_USER_ABORT, 0);
		return 0;
	}
	return 1;
}

static int seal_folder(struct composer *fci, PFNFCIGETNEXTCABINET next_cabinet, PFNFCISTATUS status)
{
	if (!fci->folder_open)
		return 1;
	if (!finish_block(fci, status))
		return 0;
	fci->folder_open = 0;
	fci->folder_compressed = 0;
	fci->folder_position = 0;
	return spill_volumes(fci, next_cabinet, status);
}

static struct composer *composer_of(HFCI handle)
{
	struct composer *fci = (struct composer *)handle;
	if (!fci || fci->magic != FCI_MAGIC) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return fci;
}

HFCI __cdecl FCICreate(PERF error, PFNFCIFILEPLACED placed, PFNFCIALLOC alloc, PFNFCIFREE release, PFNFCIOPEN open, PFNFCIREAD read, PFNFCIWRITE write,
		       PFNFCICLOSE close, PFNFCISEEK seek, PFNFCIDELETE remove, PFNFCIGETTEMPFILE temp_name, PCCAB cabinet, void *user)
{
	if (!error) {
		SetLastError(ERROR_BAD_ARGUMENTS);
		return NULL;
	}
	if (!placed || !alloc || !release || !open || !read || !write || !close || !seek || !remove || !temp_name || !cabinet) {
		error->erfOper = FCIERR_NONE;
		error->erfType = ERROR_BAD_ARGUMENTS;
		error->fError = TRUE;
		SetLastError(ERROR_BAD_ARGUMENTS);
		return NULL;
	}
	struct composer *fci = alloc(sizeof(*fci));
	if (!fci) {
		error->erfOper = FCIERR_ALLOC_FAIL;
		error->erfType = ERROR_NOT_ENOUGH_MEMORY;
		error->fError = TRUE;
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	ZeroMemory(fci, offsetof(struct composer, buffer));
	fci->magic = FCI_MAGIC;
	fci->error = error;
	fci->user = user;
	fci->placed = placed;
	fci->alloc = alloc;
	fci->release = release;
	fci->open = open;
	fci->read = read;
	fci->write = write;
	fci->close = close;
	fci->seek = seek;
	fci->remove = remove;
	fci->caller = cabinet;
	fci->current = *cabinet;
	if (!fci->current.cb)
		fci->current.cb = CB_MAX_DISK;
	if (!fci->current.cbFolderThresh)
		fci->current.cbFolderThresh = CB_MAX_DISK;
	int code = 0;
	if (!temp_name(fci->temp_name, sizeof(fci->temp_name), user) ||
	    (fci->temp = open(fci->temp_name, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE, &code, user)) == -1) {
		set_failure(fci, FCIERR_TEMP_FILE, code ? code : ERROR_FUNCTION_FAILED);
		release(fci);
		return NULL;
	}
	return (HFCI)fci;
}

BOOL __cdecl FCIAddFile(HFCI handle, char *source, char *name, BOOL executable, PFNFCIGETNEXTCABINET next_cabinet, PFNFCISTATUS status, PFNFCIGETOPENINFO open_info,
			TCOMP compression)
{
	struct composer *fci = composer_of(handle);
	if (!fci)
		return FALSE;
	if (!source || !name || !next_cabinet || !status || !open_info || lstrlenA(name) >= CB_MAX_FILENAME) {
		set_failure(fci, FCIERR_NONE, ERROR_BAD_ARGUMENTS);
		return FALSE;
	}
	/* Quantum and LZX are read but not written, as stored folders stay valid wherever those were asked for. */
	uint32_t type = (compression & tcompMASK_TYPE) == tcompTYPE_MSZIP ? type_mszip : type_none;
	if (fci->folder_open && type != fci->folder_type && !seal_folder(fci, next_cabinet, status))
		return FALSE;
	if (!fci->folder_open) {
		if (!grow(fci, (void **)&fci->folders, &fci->folder_capacity, fci->folder_count, sizeof(*fci->folders)))
			return FALSE;
		fci->folders[fci->folder_count].first_block = fci->block_count;
		fci->folders[fci->folder_count].type = type;
		fci->folder_count++;
		fci->folder_open = 1;
		fci->folder_type = type;
	}

	USHORT date = 0, time = 0, attributes = 0;
	int code = 0;
	INT_PTR file = open_info(source, &date, &time, &attributes, &code, fci->user);
	if (file == -1) {
		set_failure(fci, FCIERR_OPEN_SRC, code);
		return FALSE;
	}
	LONG total = fci->seek(file, 0, SEEK_END, &code, fci->user);
	if (total == -1 || fci->seek(file, 0, SEEK_SET, &code, fci->user) == -1) {
		set_failure(fci, FCIERR_READ_SRC, code);
		fci->close(file, &code, fci->user);
		return FALSE;
	}
	int high_name = 0;
	for (const char *cursor = name; *cursor; cursor++)
		high_name |= (*cursor & 0x80) != 0;
	char *copy = fci->alloc((ULONG)lstrlenA(name) + 1);
	if (!copy || !grow(fci, (void **)&fci->files, &fci->file_capacity, fci->file_count, sizeof(*fci->files))) {
		set_failure(fci, FCIERR_ALLOC_FAIL, ERROR_NOT_ENOUGH_MEMORY);
		if (copy)
			fci->release(copy);
		fci->close(file, &code, fci->user);
		return FALSE;
	}
	lstrcpyA(copy, name);
	struct pending_file *entry = &fci->files[fci->file_count++];
	entry->name = copy;
	entry->size = (uint32_t)total;
	entry->offset = fci->folder_position;
	entry->folder = fci->folder_count - 1;
	entry->date = date;
	entry->time = time;
	entry->attributes = (attributes & 0x27u) | (executable ? attribute_executable : 0u) | (high_name ? attribute_utf8_name : 0u);
	entry->continued = 0;

	BOOL good = TRUE;
	uint32_t left = (uint32_t)total;
	while (good && left) {
		UINT wanted = block_output_limit - fci->buffered;
		if (wanted > left)
			wanted = left;
		UINT got = fci->read(file, fci->buffer + fci->buffered, wanted, &code, fci->user);
		if (got == (UINT)-1 || !got) {
			set_failure(fci, FCIERR_READ_SRC, code);
			good = FALSE;
			break;
		}
		fci->buffered += got;
		fci->folder_position += got;
		left -= got;
		if (fci->buffered == block_output_limit)
			good = finish_block(fci, status) && spill_volumes(fci, next_cabinet, status);
	}
	fci->close(file, &code, fci->user);
	if (!good)
		return FALSE;
	if (fci->folder_compressed + fci->buffered >= fci->current.cbFolderThresh)
		return seal_folder(fci, next_cabinet, status);
	return TRUE;
}

BOOL __cdecl FCIFlushFolder(HFCI handle, PFNFCIGETNEXTCABINET next_cabinet, PFNFCISTATUS status)
{
	struct composer *fci = composer_of(handle);
	if (!fci)
		return FALSE;
	if (!next_cabinet || !status) {
		set_failure(fci, FCIERR_NONE, ERROR_BAD_ARGUMENTS);
		return FALSE;
	}
	return seal_folder(fci, next_cabinet, status);
}

BOOL __cdecl FCIFlushCabinet(HFCI handle, BOOL get_next, PFNFCIGETNEXTCABINET next_cabinet, PFNFCISTATUS status)
{
	struct composer *fci = composer_of(handle);
	if (!fci)
		return FALSE;
	if (!next_cabinet || !status) {
		set_failure(fci, FCIERR_NONE, ERROR_BAD_ARGUMENTS);
		return FALSE;
	}
	if (!seal_folder(fci, next_cabinet, status))
		return FALSE;
	CCAB next = fci->current;
	if (get_next && !next_descriptor(fci, next_cabinet, directory_size(fci, NULL, NULL) + data_size(fci), &next))
		return FALSE;
	if (!write_volume(fci, fci->block_count, 0, get_next ? &next : NULL, status))
		return FALSE;
	if (get_next) {
		fci->current = next;
	} else {
		/* Next time starts a set of its own, under the name the caller's descriptor then has. */
		fci->has_previous = 0;
		fci->current = *fci->caller;
		if (!fci->current.cb)
			fci->current.cb = CB_MAX_DISK;
		if (!fci->current.cbFolderThresh)
			fci->current.cbFolderThresh = CB_MAX_DISK;
	}
	return TRUE;
}

BOOL __cdecl FCIDestroy(HFCI handle)
{
	struct composer *fci = composer_of(handle);
	if (!fci)
		return FALSE;
	int code = 0;
	fci->close(fci->temp, &code, fci->user);
	fci->remove(fci->temp_name, &code, fci->user);
	for (uint32_t index = 0; index < fci->file_count; index++)
		fci->release(fci->files[index].name);
	if (fci->files)
		fci->release(fci->files);
	if (fci->folders)
		fci->release(fci->folders);
	if (fci->blocks)
		fci->release(fci->blocks);
	if (fci->deflater_ready)
		deflateEnd(&fci->deflater);
	fci->magic = 0;
	fci->release(fci);
	return TRUE;
}
