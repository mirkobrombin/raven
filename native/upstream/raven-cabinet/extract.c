/* FDI: walks a cabinet's directory, decodes its folders in order and hands every file to the caller's notifications. A folder that runs over the
   end of a cabinet goes on in the first folder of the next one, so one stream can cross volumes while a single file is written. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <fdi.h>
#include <zlib.h>

#include "format.h"

#define FDI_MAGIC 0x52464449u

struct extractor
{
	uint32_t magic;
	PFNALLOC alloc;
	PFNFREE release;
	PFNOPEN open;
	PFNREAD read;
	PFNWRITE write;
	PFNCLOSE close;
	PFNSEEK seek;
	ERF *error;
};

struct folder_entry
{
	uint32_t data_offset, block_count, type;
};

struct file_entry
{
	uint32_t size, folder_offset, folder, date, time, attributes;
	char *name;
};

struct volume
{
	INT_PTR handle;
	uint32_t cabinet_size, files_offset, flags, set_id, index, folder_count, file_count;
	uint32_t header_reserve, folder_reserve, block_reserve;
	char *previous_name, *previous_disk, *next_name, *next_disk;
	struct folder_entry *folders;
	struct file_entry *files;
	struct volume *next;
};

/* Reads the directory through a buffer: asking the caller's callback for one name byte at a time would be a host call per byte. */
struct directory_reader
{
	struct extractor *owner;
	INT_PTR handle;
	unsigned char buffer[4096];
	UINT position, available;
};

struct stream
{
	struct extractor *owner;
	struct volume *root, *volume;
	const struct folder_entry *origin;
	uint32_t block_offset, blocks_left, type;
	uint32_t consumed;
	struct lzx_decoder *lzx;
	struct quantum_decoder *quantum;
	z_stream inflater;
	int inflater_ready, has_history;
	uint32_t output_position, output_size;
	char *cabinet_path;
	PFNFDINOTIFY notify;
	void *user;
	unsigned char input[65536 + 8];
	unsigned char output[block_output_limit + 8];
};

enum decode_result { decode_ok, decode_corrupt, decode_abort, decode_memory, decode_target };

static void set_failure(struct extractor *fdi, int operation, DWORD code)
{
	if (fdi->error) {
		fdi->error->erfOper = operation;
		fdi->error->erfType = (int)code;
		fdi->error->fError = TRUE;
	}
	if (code)
		SetLastError(code);
}

static void *owner_alloc(size_t size, void *context)
{
	struct extractor *fdi = context;
	return fdi->alloc((ULONG)size);
}

static void owner_free(void *memory, void *context)
{
	struct extractor *fdi = context;
	fdi->release(memory);
}

static voidpf zlib_alloc(voidpf context, uInt items, uInt size)
{
	struct extractor *fdi = context;
	return fdi->alloc(items * size);
}

static void zlib_free(voidpf context, voidpf memory)
{
	struct extractor *fdi = context;
	fdi->release(memory);
}

static int directory_get(struct directory_reader *reader, void *target, UINT size)
{
	unsigned char *bytes = target;
	while (size) {
		if (reader->position == reader->available) {
			UINT got = reader->owner->read(reader->handle, reader->buffer, sizeof(reader->buffer));
			if (got == (UINT)-1 || !got)
				return 0;
			reader->position = 0;
			reader->available = got;
		}
		UINT run = reader->available - reader->position;
		if (run > size)
			run = size;
		CopyMemory(bytes, reader->buffer + reader->position, run);
		reader->position += run;
		bytes += run;
		size -= run;
	}
	return 1;
}

static int directory_skip(struct directory_reader *reader, UINT size)
{
	unsigned char byte;
	while (size--) {
		if (!directory_get(reader, &byte, 1))
			return 0;
	}
	return 1;
}

static char *directory_string(struct directory_reader *reader)
{
	char text[CB_MAX_CAB_PATH + CB_MAX_FILENAME + 2];
	UINT length = 0;
	for (;;) {
		if (length == sizeof(text) || !directory_get(reader, &text[length], 1))
			return NULL;
		if (!text[length++])
			break;
	}
	char *copy = reader->owner->alloc(length);
	if (copy)
		CopyMemory(copy, text, length);
	return copy;
}

static void free_volume(struct extractor *fdi, struct volume *volume)
{
	if (volume->files) {
		for (uint32_t index = 0; index < volume->file_count; index++)
			fdi->release(volume->files[index].name);
		fdi->release(volume->files);
	}
	fdi->release(volume->folders);
	fdi->release(volume->previous_name);
	fdi->release(volume->previous_disk);
	fdi->release(volume->next_name);
	fdi->release(volume->next_disk);
	fdi->release(volume);
}

/* The fixed header, which both FDIIsCabinet and a real open start with. Leaves the reader on the first name or the first folder entry. */
static int read_header(struct directory_reader *reader, FDICABINETINFO *info, uint32_t *files_offset, uint32_t *reserves, int *failure)
{
	unsigned char header[header_size], reserve[4];
	*failure = FDIERROR_NOT_A_CABINET;
	if (!directory_get(reader, header, sizeof(header)) || get_32(header) != cabinet_signature)
		return 0;
	if (header[25] > 1 || (header[25] == 1 && header[24] > 3)) {
		*failure = FDIERROR_UNKNOWN_CABINET_VERSION;
		return 0;
	}
	uint32_t flags = get_16(header + 30);
	info->cbCabinet = (LONG)get_32(header + 8);
	info->cFolders = (USHORT)get_16(header + 26);
	info->cFiles = (USHORT)get_16(header + 28);
	info->setID = (USHORT)get_16(header + 32);
	info->iCabinet = (USHORT)get_16(header + 34);
	info->fReserve = (flags & flag_has_reserve) != 0;
	info->hasprev = (flags & flag_has_previous) != 0;
	info->hasnext = (flags & flag_has_next) != 0;
	*files_offset = get_32(header + 16);
	reserves[0] = reserves[1] = reserves[2] = 0;
	*failure = FDIERROR_CORRUPT_CABINET;
	if (flags & flag_has_reserve) {
		if (!directory_get(reader, reserve, sizeof(reserve)))
			return 0;
		reserves[0] = get_16(reserve);
		reserves[1] = reserve[2];
		reserves[2] = reserve[3];
		if (!directory_skip(reader, reserves[0]))
			return 0;
	}
	*failure = 0;
	return 1;
}

static struct volume *open_volume(struct extractor *fdi, INT_PTR handle, FDICABINETINFO *info, int *failure)
{
	struct directory_reader *reader = fdi->alloc(sizeof(*reader));
	struct volume *volume = fdi->alloc(sizeof(*volume));
	if (!reader || !volume) {
		*failure = FDIERROR_ALLOC_FAIL;
		goto fail;
	}
	ZeroMemory(volume, sizeof(*volume));
	reader->owner = fdi;
	reader->handle = handle;
	reader->position = reader->available = 0;
	uint32_t reserves[3];
	if (!read_header(reader, info, &volume->files_offset, reserves, failure))
		goto fail;
	volume->handle = handle;
	volume->cabinet_size = (uint32_t)info->cbCabinet;
	volume->set_id = info->setID;
	volume->index = info->iCabinet;
	volume->folder_count = info->cFolders;
	volume->file_count = info->cFiles;
	volume->header_reserve = reserves[0];
	volume->folder_reserve = reserves[1];
	volume->block_reserve = reserves[2];
	*failure = FDIERROR_CORRUPT_CABINET;
	if (info->hasprev && (!(volume->previous_name = directory_string(reader)) || !(volume->previous_disk = directory_string(reader))))
		goto fail;
	if (info->hasnext && (!(volume->next_name = directory_string(reader)) || !(volume->next_disk = directory_string(reader))))
		goto fail;

	*failure = FDIERROR_ALLOC_FAIL;
	if (!(volume->folders = fdi->alloc((volume->folder_count + 1) * sizeof(*volume->folders))) ||
	    !(volume->files = fdi->alloc((volume->file_count + 1) * sizeof(*volume->files))))
		goto fail;
	ZeroMemory(volume->files, (volume->file_count + 1) * sizeof(*volume->files));

	*failure = FDIERROR_CORRUPT_CABINET;
	for (uint32_t index = 0; index < volume->folder_count; index++) {
		unsigned char entry[folder_entry_size];
		if (!directory_get(reader, entry, sizeof(entry)) || !directory_skip(reader, volume->folder_reserve))
			goto fail;
		volume->folders[index].data_offset = get_32(entry);
		volume->folders[index].block_count = get_16(entry + 4);
		volume->folders[index].type = get_16(entry + 6);
	}
	for (uint32_t index = 0; index < volume->file_count; index++) {
		unsigned char entry[file_entry_size];
		struct file_entry *file = &volume->files[index];
		if (!directory_get(reader, entry, sizeof(entry)) || !(file->name = directory_string(reader)))
			goto fail;
		file->size = get_32(entry);
		file->folder_offset = get_32(entry + 4);
		file->folder = get_16(entry + 8);
		file->date = get_16(entry + 10);
		file->time = get_16(entry + 12);
		file->attributes = get_16(entry + 14);
	}
	fdi->release(reader);
	*failure = 0;
	return volume;

fail:
	if (reader)
		fdi->release(reader);
	if (volume)
		free_volume(fdi, volume);
	return NULL;
}

static const struct folder_entry *folder_of(const struct volume *volume, uint32_t folder)
{
	if (!volume->folder_count)
		return NULL;
	if (folder == folder_from_previous)
		return &volume->folders[0];
	if (folder == folder_to_next || folder == folder_both_ways)
		return &volume->folders[volume->folder_count - 1];
	return folder < volume->folder_count ? &volume->folders[folder] : NULL;
}

/* Asks for the next volume until the caller gives one that fits or aborts: a wrong disk gets reported and asked for again. */
static enum decode_result chain_next_volume(struct stream *stream, const char *continuing)
{
	struct extractor *fdi = stream->owner;
	struct volume *current = stream->volume;
	if (!current->next_name)
		return decode_corrupt;
	char path[CB_MAX_CAB_PATH + CB_MAX_CABINET_NAME + 2];
	path[0] = 0;
	if (stream->cabinet_path)
		lstrcpynA(path, stream->cabinet_path, CB_MAX_CAB_PATH);
	FDINOTIFICATION note;
	ZeroMemory(&note, sizeof(note));
	note.pv = stream->user;
	note.psz1 = current->next_name;
	note.psz2 = current->next_disk ? current->next_disk : (char *)"";
	note.psz3 = path;
	note.fdie = FDIERROR_NONE;
	for (;;) {
		if (stream->notify(fdintNEXT_CABINET, &note) == -1)
			return decode_abort;
		char full[sizeof(path)];
		lstrcpynA(full, path, CB_MAX_CAB_PATH);
		lstrcatA(full, current->next_name);
		INT_PTR handle = fdi->open(full, _O_BINARY | _O_RDONLY | _O_SEQUENTIAL, 0);
		if (handle == 0 || handle == -1) {
			note.fdie = FDIERROR_CABINET_NOT_FOUND;
			continue;
		}
		FDICABINETINFO info;
		int failure;
		struct volume *next = open_volume(fdi, handle, &info, &failure);
		if (!next) {
			fdi->close(handle);
			if (failure == FDIERROR_ALLOC_FAIL)
				return decode_memory;
			note.fdie = failure;
			continue;
		}
		int fits = 0;
		if (next->set_id == current->set_id && next->index == current->index + 1 && next->folder_count) {
			for (uint32_t index = 0; index < next->file_count && !fits; index++) {
				uint32_t folder = next->files[index].folder;
				if (folder != folder_from_previous && folder != folder_both_ways)
					break;
				fits = !lstrcmpiA(next->files[index].name, continuing);
			}
		}
		if (!fits) {
			fdi->close(handle);
			free_volume(fdi, next);
			note.fdie = FDIERROR_WRONG_CABINET;
			continue;
		}
		ZeroMemory(&note, sizeof(note));
		note.pv = stream->user;
		note.psz1 = next->next_name ? next->next_name : (char *)"";
		note.psz2 = next->next_disk ? next->next_disk : (char *)"";
		note.psz3 = stream->cabinet_path;
		note.setID = (USHORT)next->set_id;
		note.iCabinet = (USHORT)next->index;
		if (stream->notify(fdintCABINET_INFO, &note) == -1) {
			fdi->close(handle);
			free_volume(fdi, next);
			return decode_abort;
		}
		current->next = next;
		stream->volume = next;
		stream->block_offset = next->folders[0].data_offset;
		stream->blocks_left = next->folders[0].block_count;
		return decode_ok;
	}
}

static int read_at(struct stream *stream, uint32_t offset, void *target, UINT size)
{
	struct extractor *fdi = stream->owner;
	if (fdi->seek(stream->volume->handle, (long)offset, SEEK_SET) == -1)
		return 0;
	return fdi->read(stream->volume->handle, target, size) == size;
}

static enum decode_result decode_mszip(struct stream *stream, uint32_t size, uint32_t produce)
{
	if (size < 2 || stream->input[0] != 'C' || stream->input[1] != 'K')
		return decode_corrupt;
	if (!stream->inflater_ready) {
		ZeroMemory(&stream->inflater, sizeof(stream->inflater));
		stream->inflater.zalloc = zlib_alloc;
		stream->inflater.zfree = zlib_free;
		stream->inflater.opaque = stream->owner;
		if (inflateInit2(&stream->inflater, -MAX_WBITS) != Z_OK)
			return decode_memory;
		stream->inflater_ready = 1;
	} else if (inflateReset(&stream->inflater) != Z_OK) {
		return decode_corrupt;
	}
	/* Every block is its own deflate stream that may reach back into the block before it. */
	if (stream->has_history && inflateSetDictionary(&stream->inflater, stream->output, stream->output_size) != Z_OK)
		return decode_corrupt;
	stream->inflater.next_in = stream->input + 2;
	stream->inflater.avail_in = size - 2;
	stream->inflater.next_out = stream->output;
	stream->inflater.avail_out = produce;
	int status = inflate(&stream->inflater, Z_FINISH);
	if (stream->inflater.total_out != produce || (status != Z_STREAM_END && status != Z_BUF_ERROR && status != Z_OK))
		return decode_corrupt;
	stream->has_history = 1;
	return decode_ok;
}

static enum decode_result next_block(struct stream *stream, const char *continuing)
{
	uint32_t gathered = 0, produce = 0;
	while (!produce) {
		if (!stream->blocks_left) {
			enum decode_result chained = chain_next_volume(stream, continuing);
			if (chained != decode_ok)
				return chained;
		}
		unsigned char header[block_header_size];
		const struct volume *volume = stream->volume;
		if (!read_at(stream, stream->block_offset, header, sizeof(header)))
			return decode_corrupt;
		uint32_t stored = get_32(header), size = get_16(header + 4);
		produce = get_16(header + 6);
		if (produce > block_output_limit || gathered + size > sizeof(stream->input))
			return decode_corrupt;
		uint32_t data_offset = stream->block_offset + block_header_size + volume->block_reserve;
		if (size && !read_at(stream, data_offset, stream->input + gathered, size))
			return decode_corrupt;
		if (stored) {
			uint32_t sum = block_checksum(stream->input + gathered, size, 0);
			if (block_checksum(header + 4, 4, sum) != stored)
				return decode_corrupt;
		}
		stream->block_offset = data_offset + size;
		stream->blocks_left--;
		gathered += size;
	}
	int done = 1;
	switch (stream->type & 15) {
	case type_none:
		if (gathered != produce)
			return decode_corrupt;
		CopyMemory(stream->output, stream->input, produce);
		break;
	case type_mszip: {
		enum decode_result result = decode_mszip(stream, gathered, produce);
		if (result != decode_ok)
			return result;
		break;
	}
	case type_quantum:
		done = quantum_decode(stream->quantum, stream->input, gathered, stream->output, produce);
		break;
	case type_lzx:
		done = lzx_decode(stream->lzx, stream->input, gathered, stream->output, produce);
		break;
	default:
		return decode_corrupt;
	}
	if (!done)
		return decode_corrupt;
	stream->output_position = 0;
	stream->output_size = produce;
	return decode_ok;
}

static void drop_decoders(struct stream *stream)
{
	if (stream->lzx)
		lzx_destroy(stream->lzx, owner_free, stream->owner);
	if (stream->quantum)
		quantum_destroy(stream->quantum, owner_free, stream->owner);
	if (stream->inflater_ready)
		inflateEnd(&stream->inflater);
	stream->lzx = NULL;
	stream->quantum = NULL;
	stream->inflater_ready = 0;
	stream->has_history = 0;
}

/* Puts the stream on the first byte of a folder. Decoder state cannot be rewound, so going backwards starts over. */
static enum decode_result restart_folder(struct stream *stream, const struct folder_entry *folder)
{
	drop_decoders(stream);
	stream->origin = folder;
	stream->volume = stream->root;
	stream->type = folder->type;
	stream->block_offset = folder->data_offset;
	stream->blocks_left = folder->block_count;
	stream->consumed = 0;
	stream->output_position = stream->output_size = 0;
	unsigned window = (folder->type >> 8) & 31;
	switch (folder->type & 15) {
	case type_none:
	case type_mszip:
		return decode_ok;
	case type_quantum:
		if (window < 10 || window > 21)
			return decode_corrupt;
		return (stream->quantum = quantum_create(window, owner_alloc, stream->owner)) ? decode_ok : decode_memory;
	case type_lzx:
		if (window < 15 || window > 21)
			return decode_corrupt;
		return (stream->lzx = lzx_create(window, owner_alloc, stream->owner)) ? decode_ok : decode_memory;
	default:
		return decode_corrupt;
	}
}

/* Hands the next `amount` bytes of the folder to `target`, or drops them when target is zero. */
static enum decode_result pump(struct stream *stream, INT_PTR target, uint32_t amount, const char *continuing)
{
	while (amount) {
		if (stream->output_position == stream->output_size) {
			enum decode_result result = next_block(stream, continuing);
			if (result != decode_ok)
				return result;
			if (!stream->output_size)
				return decode_corrupt;
		}
		uint32_t run = stream->output_size - stream->output_position;
		if (run > amount)
			run = amount;
		if (target && stream->owner->write(target, stream->output + stream->output_position, run) != run)
			return decode_target;
		stream->output_position += run;
		stream->consumed += run;
		amount -= run;
	}
	return decode_ok;
}

static void fail_decode(struct extractor *fdi, enum decode_result result)
{
	switch (result) {
	case decode_abort:
		set_failure(fdi, FDIERROR_USER_ABORT, 0);
		break;
	case decode_memory:
		set_failure(fdi, FDIERROR_ALLOC_FAIL, ERROR_NOT_ENOUGH_MEMORY);
		break;
	case decode_target:
		set_failure(fdi, FDIERROR_TARGET_FILE, 0);
		break;
	default:
		set_failure(fdi, FDIERROR_CORRUPT_CABINET, 0);
		break;
	}
}

static BOOL extract_all(struct stream *stream, struct volume *root, PFNFDINOTIFY notify, void *user)
{
	struct extractor *fdi = stream->owner;
	FDINOTIFICATION note;
	ZeroMemory(&note, sizeof(note));
	note.pv = user;
	note.psz1 = root->next_name ? root->next_name : (char *)"";
	note.psz2 = root->next_disk ? root->next_disk : (char *)"";
	note.psz3 = stream->cabinet_path;
	note.setID = (USHORT)root->set_id;
	note.iCabinet = (USHORT)root->index;
	if (notify(fdintCABINET_INFO, &note) == -1) {
		set_failure(fdi, FDIERROR_USER_ABORT, 0);
		return FALSE;
	}

	for (uint32_t index = 0; index < root->file_count; index++) {
		const struct file_entry *file = &root->files[index];
		ZeroMemory(&note, sizeof(note));
		note.pv = user;
		note.psz1 = file->name;
		if (file->folder == folder_from_previous || file->folder == folder_both_ways) {
			/* Its head lives in an earlier volume, which is where it gets written from. */
			note.psz2 = root->previous_name ? root->previous_name : (char *)"";
			note.psz3 = root->previous_disk ? root->previous_disk : (char *)"";
			if (notify(fdintPARTIAL_FILE, &note) == -1) {
				set_failure(fdi, FDIERROR_USER_ABORT, 0);
				return FALSE;
			}
			continue;
		}
		note.cb = (LONG)file->size;
		note.date = (USHORT)file->date;
		note.time = (USHORT)file->time;
		note.attribs = (USHORT)file->attributes;
		note.iFolder = (USHORT)file->folder;
		INT_PTR target = notify(fdintCOPY_FILE, &note);
		if (target == -1) {
			set_failure(fdi, FDIERROR_USER_ABORT, 0);
			return FALSE;
		}
		if (!target)
			continue;

		const struct folder_entry *folder = folder_of(root, file->folder);
		enum decode_result result = decode_ok;
		if (!folder)
			result = decode_corrupt;
		else if (stream->origin != folder || file->folder_offset < stream->consumed)
			result = restart_folder(stream, folder);
		if (result == decode_ok && file->folder_offset > stream->consumed)
			result = pump(stream, 0, file->folder_offset - stream->consumed, file->name);
		if (result == decode_ok)
			result = pump(stream, target, file->size, file->name);
		/* The target goes back to the caller even after a failure: only its notification may close it. */
		if (result != decode_ok)
			stream->origin = NULL;
		ZeroMemory(&note, sizeof(note));
		note.pv = user;
		note.psz1 = file->name;
		note.hf = target;
		note.cb = (file->attributes & attribute_executable) != 0;
		note.date = (USHORT)file->date;
		note.time = (USHORT)file->time;
		note.attribs = (USHORT)file->attributes;
		note.iFolder = (USHORT)file->folder;
		INT_PTR closed = notify(fdintCLOSE_FILE_INFO, &note);
		if (result != decode_ok) {
			fail_decode(fdi, result);
			return FALSE;
		}
		if (closed == -1) {
			set_failure(fdi, FDIERROR_USER_ABORT, 0);
			return FALSE;
		}
	}
	return TRUE;
}

static struct extractor *extractor_of(HFDI handle)
{
	struct extractor *fdi = (struct extractor *)handle;
	if (!fdi || fdi->magic != FDI_MAGIC) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return fdi;
}

HFDI __cdecl FDICreate(PFNALLOC alloc, PFNFREE release, PFNOPEN open, PFNREAD read, PFNWRITE write, PFNCLOSE close, PFNSEEK seek, int cpu, PERF error)
{
	(void)cpu;
	if (!alloc || !release) {
		if (error) {
			error->erfOper = FDIERROR_NONE;
			error->erfType = ERROR_BAD_ARGUMENTS;
			error->fError = TRUE;
		}
		SetLastError(ERROR_BAD_ARGUMENTS);
		return NULL;
	}
	struct extractor *fdi = alloc(sizeof(*fdi));
	if (!fdi) {
		if (error) {
			error->erfOper = FDIERROR_ALLOC_FAIL;
			error->erfType = 0;
			error->fError = TRUE;
		}
		return NULL;
	}
	fdi->magic = FDI_MAGIC;
	fdi->alloc = alloc;
	fdi->release = release;
	fdi->open = open;
	fdi->read = read;
	fdi->write = write;
	fdi->close = close;
	fdi->seek = seek;
	fdi->error = error;
	return (HFDI)fdi;
}

BOOL __cdecl FDIIsCabinet(HFDI handle, INT_PTR file, PFDICABINETINFO info)
{
	struct extractor *fdi = extractor_of(handle);
	if (!fdi)
		return FALSE;
	if (!info) {
		SetLastError(ERROR_BAD_ARGUMENTS);
		return FALSE;
	}
	struct directory_reader *reader = fdi->alloc(sizeof(*reader));
	if (!reader) {
		set_failure(fdi, FDIERROR_ALLOC_FAIL, ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	reader->owner = fdi;
	reader->handle = file;
	reader->position = reader->available = 0;
	uint32_t files_offset, reserves[3];
	int failure;
	BOOL result = read_header(reader, info, &files_offset, reserves, &failure);
	if (result && reader->position < reader->available)
		fdi->seek(file, -(long)(reader->available - reader->position), SEEK_CUR);
	fdi->release(reader);
	return result;
}

BOOL __cdecl FDICopy(HFDI handle, char *cabinet, char *path, int flags, PFNFDINOTIFY notify, PFNFDIDECRYPT decrypt, void *user)
{
	(void)flags;
	(void)decrypt;
	struct extractor *fdi = extractor_of(handle);
	if (!fdi)
		return FALSE;
	if (!notify) {
		set_failure(fdi, FDIERROR_NONE, ERROR_BAD_ARGUMENTS);
		return FALSE;
	}
	char full[CB_MAX_CAB_PATH + CB_MAX_CABINET_NAME + 2];
	if ((path ? lstrlenA(path) : 0) + lstrlenA(cabinet) >= (int)sizeof(full)) {
		set_failure(fdi, FDIERROR_CABINET_NOT_FOUND, ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	full[0] = 0;
	if (path)
		lstrcpyA(full, path);
	lstrcatA(full, cabinet);
	INT_PTR file = fdi->open(full, _O_BINARY | _O_RDONLY | _O_SEQUENTIAL, 0);
	if (file == -1 || file == 0) {
		set_failure(fdi, FDIERROR_CABINET_NOT_FOUND, ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	FDICABINETINFO info;
	int failure;
	struct volume *root = open_volume(fdi, file, &info, &failure);
	if (!root) {
		fdi->close(file);
		set_failure(fdi, failure, failure == FDIERROR_ALLOC_FAIL ? ERROR_NOT_ENOUGH_MEMORY : 0);
		return FALSE;
	}
	struct stream *stream = fdi->alloc(sizeof(*stream));
	BOOL result = FALSE;
	if (!stream) {
		set_failure(fdi, FDIERROR_ALLOC_FAIL, ERROR_NOT_ENOUGH_MEMORY);
	} else {
		ZeroMemory(stream, offsetof(struct stream, input));
		stream->owner = fdi;
		stream->root = stream->volume = root;
		stream->cabinet_path = path;
		stream->notify = notify;
		stream->user = user;
		result = extract_all(stream, root, notify, user);
		drop_decoders(stream);
		fdi->release(stream);
	}
	while (root) {
		struct volume *next = root->next;
		fdi->close(root->handle);
		free_volume(fdi, root);
		root = next;
	}
	return result;
}

BOOL __cdecl FDIDestroy(HFDI handle)
{
	struct extractor *fdi = extractor_of(handle);
	if (!fdi)
		return FALSE;
	fdi->magic = 0;
	fdi->release(fdi);
	return TRUE;
}

BOOL __cdecl FDITruncateCabinet(HFDI handle, char *cabinet, USHORT folder)
{
	(void)cabinet;
	(void)folder;
	if (extractor_of(handle))
		SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

uint32_t block_checksum(const unsigned char *data, size_t size, uint32_t seed)
{
	uint32_t sum = seed;
	size_t whole = size / 4;
	for (size_t index = 0; index < whole; index++)
		sum ^= get_32(data + index * 4);
	data += whole * 4;
	uint32_t tail = 0;
	switch (size % 4) {
	case 3:
		tail |= (uint32_t)*data++ << 16;
		/* fall through */
	case 2:
		tail |= (uint32_t)*data++ << 8;
		/* fall through */
	case 1:
		tail |= *data;
		break;
	}
	return sum ^ tail;
}
