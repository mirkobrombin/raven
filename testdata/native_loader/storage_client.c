// Files and folders of the Windows Runtime as an app uses them: a folder found by its path makes, finds, lists, copies, renames, moves and
// deletes what it holds, FileIO reads and writes text, lines and bytes, the Buffer class makes buffers, a file opens as streams and in a
// transaction, and DataWriter and DataReader carry values over buffers, file streams and streams in memory. Every operation runs on its
// own and calls its completion handler when it ends; the exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllimport) HRESULT WINAPI RoInitialize(int type);
__declspec(dllimport) HRESULT WINAPI RoGetActivationFactory(void *name, const GUID *iid, void **factory);
__declspec(dllimport) HRESULT WINAPI RoActivateInstance(void *name, void **instance);
__declspec(dllimport) const WCHAR *WINAPI WindowsGetStringRawBuffer(void *string, UINT32 *length);
__declspec(dllimport) HRESULT WINAPI WindowsDeleteString(void *string);

/* The string header programs built with C++/WinRT write themselves for a reference. */
struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

typedef struct {
	void **table;
} object;

#define METHOD(self, index, type) ((type)(self)->table[index])
#define NAME(variable, literal) struct reference variable = {1, ARRAYSIZE(literal) - 1, {0, 0}, literal}

typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *query_method)(object *, const GUID *, void **);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *string_method)(object *, void **);
typedef HRESULT (WINAPI *number_method)(object *, UINT32 *);
typedef HRESULT (WINAPI *at_method)(object *, UINT32, void **);
typedef HRESULT (WINAPI *name_method)(object *, void *, object **);
typedef HRESULT (WINAPI *name_option_method)(object *, void *, INT32, object **);
typedef HRESULT (WINAPI *item_method)(object *, object *, object **);
typedef HRESULT (WINAPI *item_name_method)(object *, object *, void *, object **);
typedef HRESULT (WINAPI *item_text_method)(object *, object *, void *, object **);
typedef HRESULT (WINAPI *bytes_method)(object *, object *, UINT32, const BYTE *, object **);
typedef HRESULT (WINAPI *create_method)(object *, UINT32, object **);
typedef HRESULT (WINAPI *handler_method)(object *, object *);
typedef HRESULT (WINAPI *results_method)(object *, void *);
typedef HRESULT (WINAPI *action_results_method)(object *);
typedef HRESULT (WINAPI *size_method)(object *, UINT64 *);
typedef HRESULT (WINAPI *set_size_method)(object *, UINT64);
typedef HRESULT (WINAPI *flag_method)(object *, BYTE *);
typedef HRESULT (WINAPI *read_method)(object *, object *, UINT32, INT32, object **);
typedef HRESULT (WINAPI *bytes_out_method)(object *, BYTE **);
typedef HRESULT (WINAPI *plain_method)(object *);
typedef HRESULT (WINAPI *mode_method)(object *, INT32, object **);
typedef HRESULT (WINAPI *put_int32_method)(object *, INT32);
typedef HRESULT (WINAPI *put_uint16_method)(object *, UINT16);
typedef HRESULT (WINAPI *put_double_method)(object *, double);
typedef HRESULT (WINAPI *put_string_method)(object *, void *, UINT32 *);
typedef HRESULT (WINAPI *get_int32_method)(object *, INT32 *);
typedef HRESULT (WINAPI *get_uint16_method)(object *, UINT16 *);
typedef HRESULT (WINAPI *get_double_method)(object *, double *);
typedef HRESULT (WINAPI *get_string_method)(object *, UINT32, void **);
typedef HRESULT (WINAPI *position_method)(object *, UINT64, object **);

static const GUID storage_item = {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};
static const GUID folder_statics = {0x08f327ff, 0x85d5, 0x48b9, {0xae, 0xe9, 0x28, 0x51, 0x1e, 0x33, 0x9f, 0x9f}};
static const GUID file_io = {0x887411eb, 0x7f54, 0x4732, {0xa5, 0xf0, 0x5e, 0x43, 0xe3, 0xb8, 0xc2, 0xf5}};
static const GUID random_access_stream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const GUID input_stream = {0x905a0fe2, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const GUID output_stream = {0x905a0fe6, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const GUID byte_access = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const GUID closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
static const GUID stream_reference = {0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
static const GUID content_type_provider = {0x97d098a5, 0x3b99, 0x4de9, {0x88, 0xa5, 0xe1, 0x1d, 0x2f, 0x50, 0xc7, 0x95}};
static const GUID data_writer = {0x64b89265, 0xd341, 0x4922, {0xb3, 0x8a, 0xdd, 0x4a, 0xf8, 0x80, 0x8c, 0x4e}};
static const GUID data_writer_factory = {0x338c67c2, 0x8b84, 0x4c2b, {0x9c, 0x50, 0x7b, 0x87, 0x67, 0x84, 0x7a, 0x1f}};
static const GUID data_reader_factory = {0xd7527847, 0x57da, 0x4e15, {0x91, 0x4c, 0x06, 0x80, 0x66, 0x99, 0xa0, 0x98}};
static const GUID data_reader_statics = {0x11fcbfc8, 0xf93a, 0x471b, {0xb1, 0x21, 0xf3, 0x79, 0xe3, 0x49, 0x31, 0x3c}};
static const GUID buffer_factory = {0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};

#define ALREADY_THERE ((HRESULT)0x800700b7)
#define NOT_THERE ((HRESULT)0x80070002)
#define CLOSED ((HRESULT)0x80000013)

static object *statics(const WCHAR *name, const GUID *iid)
{
	struct reference text = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return SUCCEEDED(RoGetActivationFactory(&text, iid, (void **)&out)) ? out : NULL;
}

static void release(object *self)
{
	if (self)
		METHOD(self, 2, release_method)(self);
}

/* A completion handler that signals the end of the one operation waited for. */
static HANDLE ended;
static INT32 ended_status = -1;

static HRESULT WINAPI handler_query(object *self, const GUID *iid, void **out) { *out = self; return S_OK; }
static ULONG WINAPI handler_count(object *self) { return 1; }

static HRESULT WINAPI handler_invoke(object *self, void *operation, INT32 status)
{
	ended_status = status;
	SetEvent(ended);
	return S_OK;
}

static void *handler_table[] = {(void *)handler_query, (void *)handler_count, (void *)handler_count, (void *)handler_invoke};
static object handler = {handler_table};

/* Waits for the operation a call made and answered made, and takes its results; an action (result NULL) has none. The operation is
 * read through its address, after the call has filled it. */
static HRESULT finish_at(HRESULT made, object **made_operation, void *result, int first)
{
	object *operation = *made_operation;
	*made_operation = NULL;
	if (made != S_OK || !operation)
		return FAILED(made) ? made : E_FAIL;
	ResetEvent(ended);
	ended_status = -1;
	HRESULT outcome = METHOD(operation, first, handler_method)(operation, &handler);
	if (outcome == S_OK && WaitForSingleObject(ended, 10000) != WAIT_OBJECT_0)
		outcome = E_ABORT;
	if (outcome == S_OK)
		outcome = result ? METHOD(operation, first + 2, results_method)(operation, result) :
			METHOD(operation, first + 2, action_results_method)(operation);
	/* A completed operation succeeds; one that failed gives its error from GetResults. */
	if (outcome == S_OK && ended_status != 1)
		outcome = E_UNEXPECTED;
	release(operation);
	return outcome;
}

/* An operation, or one with progress, whose Progress methods come before Completed. */
static HRESULT finish(HRESULT made, object **operation, void *result) { return finish_at(made, operation, result, 6); }
static HRESULT finish_progress(HRESULT made, object **operation, void *result) { return finish_at(made, operation, result, 8); }

static BOOL same(void *string, const WCHAR *text)
{
	UINT32 length = 0;
	const WCHAR *raw = WindowsGetStringRawBuffer(string, &length);
	return length == (UINT32)lstrlenW(text) && !memcmp(raw, text, length * sizeof(WCHAR));
}

/* The name of an item, through its IStorageItem, is the text. */
static BOOL named(object *thing, const WCHAR *text)
{
	object *item = NULL;
	void *name = NULL;
	BOOL result = METHOD(thing, 0, query_method)(thing, &storage_item, (void **)&item) == S_OK &&
		METHOD(item, 11, string_method)(item, &name) == S_OK && same(name, text);
	WindowsDeleteString(name);
	release(item);
	return result;
}

static object *query(object *self, const GUID *iid)
{
	object *out = NULL;
	return self && METHOD(self, 0, query_method)(self, iid, (void **)&out) == S_OK ? out : NULL;
}

/* The bytes of a buffer. */
static BYTE *bytes_of(object *buffer)
{
	object *access = query(buffer, &byte_access);
	BYTE *bytes = NULL;
	if (access && METHOD(access, 3, bytes_out_method)(access, &bytes) != S_OK)
		bytes = NULL;
	release(access);
	return bytes;
}

/* Text read whole from a file is the text. */
static BOOL holds(object *io, object *file, const WCHAR *text)
{
	object *operation = NULL;
	void *read = NULL;
	const BOOL result = finish(METHOD(io, 6, item_method)(io, file, &operation), &operation, &read) == S_OK && same(read, text);
	WindowsDeleteString(read);
	return result;
}

/* A file holding "hello\r\nworld!" opened as streams: read, sought, written, flushed and closed, read only with its content type, and
 * written in a transaction that replaces it when committed. */
static int check_streams(object *io, object *buffers, object *folder, object *file)
{
	object *operation = NULL, *stream = NULL, *input, *output, *buffer = NULL, *got = NULL;
	if (finish(METHOD(file, 8, mode_method)(file, 1, &operation), &operation, &stream) != S_OK || !stream) return 60;
	UINT64 size = 0, position = 0;
	BYTE flag = 0;
	if (METHOD(stream, 6, size_method)(stream, &size) != S_OK || size != 13) return 61;
	if (METHOD(stream, 13, flag_method)(stream, &flag) != S_OK || !flag) return 62;
	if (METHOD(stream, 14, flag_method)(stream, &flag) != S_OK || !flag) return 63;
	if (!(input = query(stream, &input_stream)) || !(output = query(stream, &output_stream))) return 64;
	if (METHOD(buffers, 6, create_method)(buffers, 8, &buffer) != S_OK) return 65;
	if (finish_progress(METHOD(input, 6, read_method)(input, buffer, 5, 0, &operation), &operation, &got) != S_OK || got != buffer) return 66;
	UINT32 length = 0;
	if (METHOD(buffer, 7, number_method)(buffer, &length) != S_OK || length != 5 || memcmp(bytes_of(buffer), "hello", 5)) return 67;
	release(got);
	if (METHOD(stream, 10, size_method)(stream, &position) != S_OK || position != 5) return 68;
	if (METHOD(stream, 11, set_size_method)(stream, 13) != S_OK) return 69;
	bytes_of(buffer)[0] = '?';
	typedef HRESULT (WINAPI *length_method)(object *, UINT32);
	if (METHOD(buffer, 8, length_method)(buffer, 1) != S_OK) return 70;
	UINT32 written = 0;
	if (finish_progress(METHOD(output, 6, item_method)(output, buffer, &operation), &operation, &written) != S_OK || written != 1) return 71;
	BYTE flushed = 0;
	if (finish(METHOD(output, 7, object_method)(output, &operation), &operation, &flushed) != S_OK || !flushed) return 72;
	if (METHOD(stream, 6, size_method)(stream, &size) != S_OK || size != 14) return 73;
	object *closer = query(stream, &closable);
	if (!closer || METHOD(closer, 6, plain_method)(closer) != S_OK) return 74;
	if (METHOD(input, 6, read_method)(input, buffer, 5, 0, &operation) != CLOSED) return 75;
	release(closer);
	release(input);
	release(output);
	release(stream);
	if (!holds(io, file, L"hello\r\nworld!?")) return 76;

	/* Read only, with the type of its extension. */
	object *reference = query(file, &stream_reference), *provider;
	if (!reference) return 77;
	if (finish(METHOD(reference, 6, object_method)(reference, &operation), &operation, &stream) != S_OK || !stream) return 78;
	if (!(provider = query(stream, &content_type_provider))) return 79;
	void *type = NULL;
	if (METHOD(provider, 6, string_method)(provider, &type) != S_OK || !type) return 80;
	WindowsDeleteString(type);
	object *random = query(stream, &random_access_stream);
	if (!random || METHOD(random, 14, flag_method)(random, &flag) != S_OK || flag) return 81;
	release(random);
	if (!(output = query(stream, &output_stream)) || METHOD(output, 6, item_method)(output, buffer, &operation) != E_ACCESSDENIED) return 82;
	release(output);
	release(provider);
	release(stream);
	release(reference);

	/* A transaction over a copy: the file changes only when it is committed, and no copy is left. */
	object *transaction = NULL;
	if (finish(METHOD(file, 9, object_method)(file, &operation), &operation, &transaction) != S_OK || !transaction) return 83;
	if (METHOD(transaction, 6, object_method)(transaction, &stream) != S_OK || !stream) return 84;
	if (METHOD(stream, 6, size_method)(stream, &size) != S_OK || size != 14) return 85;
	if (METHOD(stream, 7, set_size_method)(stream, 2) != S_OK) return 86;
	if (!holds(io, file, L"hello\r\nworld!?")) return 87;
	if (finish(METHOD(transaction, 7, object_method)(transaction, &operation), &operation, NULL) != S_OK) return 88;
	if (!holds(io, file, L"he")) return 89;
	release(stream);
	if (!(closer = query(transaction, &closable)) || METHOD(closer, 6, plain_method)(closer) != S_OK) return 90;
	release(closer);
	release(transaction);
	object *list = NULL;
	UINT32 count = 0;
	if (finish(METHOD(folder, 15, object_method)(folder, &operation), &operation, &list) != S_OK || !list) return 91;
	if (METHOD(list, 7, number_method)(list, &count) != S_OK || count != 2) return 92;
	release(list);
	release(buffer);
	return 0;
}

/* Values written by a DataWriter in both byte orders and read back by a DataReader, through a buffer, then through the file's streams;
 * the file holds "he" when this starts. */
static int check_data(object *io, object *file)
{
	object *instance = NULL, *writer, *buffer = NULL, *reader = NULL, *operation = NULL;
	struct reference writer_name = {1, 34, {0, 0}, L"Windows.Storage.Streams.DataWriter"};
	if (RoActivateInstance(&writer_name, (void **)&instance) != S_OK || !instance) return 100;
	if (!(writer = query(instance, &data_writer))) return 101;
	release(instance);
	UINT32 count = 0;
	NAME(text, L"héllo");
	if (METHOD(writer, 18, put_int32_method)(writer, 0x01020304) != S_OK) return 102;
	if (METHOD(writer, 10, put_int32_method)(writer, 0) != S_OK) return 103;
	if (METHOD(writer, 20, put_uint16_method)(writer, 0x0506) != S_OK) return 104;
	if (METHOD(writer, 27, put_string_method)(writer, &text, &count) != S_OK || count != 6) return 105;
	if (METHOD(writer, 15, put_int32_method)(writer, 1) != S_OK) return 106;
	if (METHOD(writer, 24, put_double_method)(writer, 1.5) != S_OK) return 107;
	if (METHOD(writer, 6, number_method)(writer, &count) != S_OK || count != 21) return 108;
	if (METHOD(writer, 31, object_method)(writer, &buffer) != S_OK || !buffer) return 109;
	if (METHOD(buffer, 7, number_method)(buffer, &count) != S_OK || count != 21) return 110;
	if (memcmp(bytes_of(buffer), "\1\2\3\4\6\5h\xc3\xa9llo\1", 13)) return 111;
	if (METHOD(writer, 6, number_method)(writer, &count) != S_OK || count) return 112;
	release(writer);

	object *readers = statics(L"Windows.Storage.Streams.DataReader", &data_reader_statics);
	if (!readers || METHOD(readers, 6, item_method)(readers, buffer, &reader) != S_OK || !reader) return 113;
	INT32 number = 0;
	UINT16 small = 0;
	double real = 0;
	BYTE flag = 0;
	void *read = NULL;
	if (METHOD(reader, 19, get_int32_method)(reader, &number) != S_OK || number != 0x01020304) return 114;
	if (METHOD(reader, 10, put_int32_method)(reader, 0) != S_OK) return 115;
	if (METHOD(reader, 21, get_uint16_method)(reader, &small) != S_OK || small != 0x0506) return 116;
	if (METHOD(reader, 26, get_string_method)(reader, 6, &read) != S_OK || !same(read, L"héllo")) return 117;
	WindowsDeleteString(read);
	if (METHOD(reader, 16, flag_method)(reader, &flag) != S_OK || !flag) return 118;
	if (METHOD(reader, 25, get_double_method)(reader, &real) != S_OK || real != 1.5) return 119;
	if (METHOD(reader, 6, number_method)(reader, &count) != S_OK || count) return 120;
	if (METHOD(reader, 13, flag_method)(reader, &flag) != E_BOUNDS) return 121;
	release(reader);
	release(readers);
	release(buffer);

	/* Through the file: a writer over its output stores and flushes, a reader over its input loads what was stored. */
	object *stream = NULL, *output = NULL, *input = NULL;
	if (finish(METHOD(file, 8, mode_method)(file, 1, &operation), &operation, &stream) != S_OK || !stream) return 122;
	if (METHOD(stream, 9, position_method)(stream, 0, &output) != S_OK || !output) return 123;
	object *writers = statics(L"Windows.Storage.Streams.DataWriter", &data_writer_factory);
	if (!writers || METHOD(writers, 6, item_method)(writers, output, &writer) != S_OK || !writer) return 124;
	NAME(word, L"stream");
	if (METHOD(writer, 27, put_string_method)(writer, &word, &count) != S_OK || count != 6) return 125;
	count = 0;
	if (finish(METHOD(writer, 29, object_method)(writer, &operation), &operation, &count) != S_OK || count != 6) return 126;
	BYTE flushed = 0;
	if (finish(METHOD(writer, 30, object_method)(writer, &operation), &operation, &flushed) != S_OK || !flushed) return 127;
	object *closer = query(writer, &closable);
	if (!closer || METHOD(closer, 6, plain_method)(closer) != S_OK) return 128;
	release(closer);
	release(writer);
	release(output);
	if (!holds(io, file, L"stream")) return 129;
	if (METHOD(stream, 8, position_method)(stream, 0, &input) != S_OK || !input) return 130;
	object *makers = statics(L"Windows.Storage.Streams.DataReader", &data_reader_factory);
	if (!makers || METHOD(makers, 6, item_method)(makers, input, &reader) != S_OK || !reader) return 131;
	count = 0;
	typedef HRESULT (WINAPI *load_method)(object *, UINT32, object **);
	if (finish(METHOD(reader, 29, load_method)(reader, 100, &operation), &operation, &count) != S_OK || count != 6) return 132;
	if (METHOD(reader, 26, get_string_method)(reader, 6, &read) != S_OK || !same(read, L"stream")) return 133;
	WindowsDeleteString(read);
	release(reader);
	release(input);
	release(stream);

	/* A stream in memory grows as a writer stores into it, reads back what was stored, and is cut by its size. */
	struct reference memory_name = {1, 50, {0, 0}, L"Windows.Storage.Streams.InMemoryRandomAccessStream"};
	object *instance_memory = NULL;
	UINT64 size = 0;
	if (RoActivateInstance(&memory_name, (void **)&instance_memory) != S_OK || !instance_memory) return 134;
	if (!(stream = query(instance_memory, &random_access_stream))) return 135;
	release(instance_memory);
	if (METHOD(stream, 9, position_method)(stream, 0, &output) != S_OK || !output) return 136;
	if (METHOD(writers, 6, item_method)(writers, output, &writer) != S_OK || !writer) return 137;
	NAME(memory, L"memory");
	if (METHOD(writer, 27, put_string_method)(writer, &memory, &count) != S_OK || count != 6) return 138;
	if (METHOD(writer, 18, put_int32_method)(writer, -2) != S_OK) return 139;
	if (finish(METHOD(writer, 29, object_method)(writer, &operation), &operation, &count) != S_OK || count != 10) return 140;
	if (METHOD(stream, 6, size_method)(stream, &size) != S_OK || size != 10) return 141;
	if (METHOD(stream, 8, position_method)(stream, 0, &input) != S_OK || !input) return 142;
	if (METHOD(makers, 6, item_method)(makers, input, &reader) != S_OK || !reader) return 143;
	if (finish(METHOD(reader, 29, load_method)(reader, 100, &operation), &operation, &count) != S_OK || count != 10) return 144;
	if (METHOD(reader, 26, get_string_method)(reader, 6, &read) != S_OK || !same(read, L"memory")) return 145;
	WindowsDeleteString(read);
	if (METHOD(reader, 19, get_int32_method)(reader, &number) != S_OK || number != -2) return 146;
	if (METHOD(stream, 7, set_size_method)(stream, 3) != S_OK) return 147;
	if (METHOD(stream, 6, size_method)(stream, &size) != S_OK || size != 3) return 148;
	release(reader);
	release(input);
	release(writer);
	release(output);
	release(stream);
	release(writers);
	release(makers);
	return 0;
}

static int check_storage(const WCHAR *base)
{
	object *folders = statics(L"Windows.Storage.StorageFolder", &folder_statics), *io = statics(L"Windows.Storage.FileIO", &file_io);
	object *buffers = statics(L"Windows.Storage.Streams.Buffer", &buffer_factory);
	object *operation = NULL, *folder = NULL, *file = NULL, *other = NULL, *sub = NULL, *list = NULL, *buffer = NULL, *item = NULL;
	if (!folders) return 10;
	if (!io) return 11;
	if (!buffers) return 12;
	struct reference path = {1, (UINT32)lstrlenW(base), {0, 0}, base};
	if (finish(METHOD(folders, 6, name_method)(folders, &path, &operation), &operation, &folder) != S_OK || !folder) return 13;

	/* Making files: a name that is taken fails, or becomes free with a number. */
	NAME(a_name, L"a.txt");
	if (finish(METHOD(folder, 7, name_option_method)(folder, &a_name, 2, &operation), &operation, &file) != S_OK || !file) return 14;
	if (!named(file, L"a.txt")) return 15;
	if (finish(METHOD(folder, 7, name_option_method)(folder, &a_name, 2, &operation), &operation, &other) != ALREADY_THERE) return 16;
	if (finish(METHOD(folder, 7, name_option_method)(folder, &a_name, 0, &operation), &operation, &other) != S_OK || !other) return 17;
	if (!named(other, L"a (2).txt")) return 18;

	/* Text written whole, read back, appended to and read as lines. */
	NAME(text, L"hello\r\nworld");
	NAME(more, L"!");
	void *read = NULL;
	if (finish(METHOD(io, 8, item_text_method)(io, file, &text, &operation), &operation, NULL) != S_OK) return 19;
	if (finish(METHOD(io, 6, item_method)(io, file, &operation), &operation, &read) != S_OK || !same(read, L"hello\r\nworld")) return 20;
	WindowsDeleteString(read);
	if (finish(METHOD(io, 10, item_text_method)(io, file, &more, &operation), &operation, NULL) != S_OK) return 21;
	if (finish(METHOD(io, 12, item_method)(io, file, &operation), &operation, &list) != S_OK || !list) return 22;
	UINT32 count = 0;
	if (METHOD(list, 7, number_method)(list, &count) != S_OK || count != 2) return 23;
	if (METHOD(list, 6, at_method)(list, 1, &read) != S_OK || !same(read, L"world!")) return 24;
	WindowsDeleteString(read);
	release(list);

	/* Bytes written from an array and read back as a buffer, and a buffer made by its class. */
	static const BYTE bytes[] = {1, 2, 3};
	if (finish(METHOD(io, 20, bytes_method)(io, other, 3, bytes, &operation), &operation, NULL) != S_OK) return 25;
	if (finish(METHOD(io, 18, item_method)(io, other, &operation), &operation, &buffer) != S_OK || !buffer) return 26;
	if (METHOD(buffer, 7, number_method)(buffer, &count) != S_OK || count != 3) return 27;
	release(buffer);
	if (METHOD(buffers, 6, create_method)(buffers, 16, &buffer) != S_OK || !buffer) return 28;
	if (METHOD(buffer, 6, number_method)(buffer, &count) != S_OK || count != 16) return 29;
	if (METHOD(buffer, 7, number_method)(buffer, &count) != S_OK || count) return 30;
	release(buffer);

	/* A folder inside, listed before the files, and a copy of a file found in it by a path of two parts. */
	NAME(sub_name, L"sub");
	if (finish(METHOD(folder, 8, name_method)(folder, &sub_name, &operation), &operation, &sub) != S_OK || !sub) return 31;
	if (finish(METHOD(folder, 15, object_method)(folder, &operation), &operation, &list) != S_OK || !list) return 32;
	if (METHOD(list, 7, number_method)(list, &count) != S_OK || count != 3) return 33;
	if (METHOD(list, 6, at_method)(list, 0, (void **)&item) != S_OK || !named(item, L"sub")) return 34;
	release(item);
	release(list);
	if (finish(METHOD(file, 10, item_method)(file, sub, &operation), &operation, &item) != S_OK || !item) return 35;
	release(item);
	NAME(inside, L"sub\\a.txt");
	if (finish(METHOD(folder, 10, name_method)(folder, &inside, &operation), &operation, &item) != S_OK || !item) return 36;
	release(item);
	if (finish(METHOD(folder, 11, name_method)(folder, &inside, &operation), &operation, &item) != E_INVALIDARG) return 37;

	/* Renaming and moving change the item's own name and path. */
	object *storage = NULL;
	if (METHOD(other, 0, query_method)(other, &storage_item, (void **)&storage) != S_OK) return 38;
	NAME(b_name, L"b.bin");
	if (finish(METHOD(storage, 6, name_method)(storage, &b_name, &operation), &operation, NULL) != S_OK || !named(other, L"b.bin")) return 39;
	if (finish(METHOD(other, 14, item_method)(other, sub, &operation), &operation, NULL) != S_OK) return 40;
	void *moved = NULL;
	WCHAR expected[MAX_PATH];
	lstrcpyW(expected, base);
	lstrcatW(expected, L"\\sub\\b.bin");
	if (METHOD(storage, 12, string_method)(storage, &moved) != S_OK || !same(moved, expected)) return 41;
	WindowsDeleteString(moved);
	if (GetFileAttributesW(expected) == INVALID_FILE_ATTRIBUTES) return 42;
	release(storage);

	const int streams = check_streams(io, buffers, folder, file);
	if (streams) return streams;
	const int data = check_data(io, file);
	if (data) return data;

	/* Deleting a folder takes what it holds; it is not found afterwards. */
	if (METHOD(sub, 0, query_method)(sub, &storage_item, (void **)&storage) != S_OK) return 43;
	if (finish(METHOD(storage, 8, object_method)(storage, &operation), &operation, NULL) != S_OK) return 44;
	release(storage);
	if (finish(METHOD(folder, 11, name_method)(folder, &sub_name, &operation), &operation, &item) != NOT_THERE) return 45;
	if (METHOD(folder, 0, query_method)(folder, &storage_item, (void **)&storage) != S_OK) return 46;
	if (finish(METHOD(storage, 8, object_method)(storage, &operation), &operation, NULL) != S_OK) return 47;
	if (GetFileAttributesW(base) != INVALID_FILE_ATTRIBUTES) return 48;
	release(storage);
	release(sub);
	release(other);
	release(file);
	release(folder);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(1))) return 1;
	if (!(ended = CreateEventW(NULL, TRUE, FALSE, NULL))) return 2;
	WCHAR base[MAX_PATH];
	const DWORD length = GetTempPathW(MAX_PATH - 32, base);
	if (!length || length >= MAX_PATH - 32) return 3;
	wsprintfW(base + length, L"raven-storage-%lu", GetCurrentProcessId());
	if (!CreateDirectoryW(base, NULL)) return 4;
	return check_storage(base);
}
