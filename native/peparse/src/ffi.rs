//! The C functions the host calls (peparse.h). Each takes the file and, past the first, the headers that first call filled; each
//! returns NULL on success or the reason the file was refused. A caller's function returns false to stop the read, which then
//! returns the reason `STOPPED`.

use std::ffi::{c_char, c_void};

use crate::{read_resources, Directory, Error, Headers, Image, Key, STOPPED};

/// The bytes at `data`; a null pointer is an empty file.
unsafe fn slice<'a>(data: *const u8, size: usize) -> &'a [u8] {
    if data.is_null() || size == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(data, size)
    }
}

fn result(outcome: Result<(), Error>) -> *const c_char {
    match outcome {
        Ok(()) => std::ptr::null(),
        Err(reason) => reason.as_ptr(),
    }
}

#[repr(C)]
pub struct Text {
    pub data: *const u8,
    pub size: usize,
}

impl From<&[u8]> for Text {
    fn from(bytes: &[u8]) -> Self {
        Text { data: bytes.as_ptr(), size: bytes.len() }
    }
}

#[repr(C)]
pub struct RavenPeImport {
    pub module: Text,
    pub name: Text,
    pub ordinal: u16,
    pub by_ordinal: bool,
    pub iat_address: u32,
}

#[repr(C)]
pub struct RavenPeDelayDescriptor {
    pub module: Text,
    pub descriptor_address: u32,
    pub module_handle_address: u32,
    pub iat_address: u32,
    pub lookup_address: u32,
    pub bound_address: u32,
    pub unload_address: u32,
    pub timestamp: u32,
}

#[repr(C)]
pub struct RavenPeExport {
    pub ordinal: u32,
    pub address: u32,
    pub forwarder: Text,
}

/// A resource type or name: `id` when `name.data` is null, otherwise `name.size` bytes of little-endian UTF-16.
#[repr(C)]
pub struct RavenPeKey {
    pub id: u32,
    pub name: Text,
}

impl From<Key<'_>> for RavenPeKey {
    fn from(key: Key) -> Self {
        match key {
            Key::Id(id) => RavenPeKey { id, name: Text { data: std::ptr::null(), size: 0 } },
            Key::Name(bytes) => RavenPeKey { id: 0, name: bytes.into() },
        }
    }
}

#[repr(C)]
pub struct RavenPeResource {
    pub kind: RavenPeKey,
    pub name: RavenPeKey,
    pub language: u32,
    pub entry_address: u32,
    pub data_address: u32,
    pub size: u32,
    pub code_page: u32,
}

type ImportFound = unsafe extern "C" fn(*mut c_void, *const RavenPeImport) -> bool;
type DescriptorFound = unsafe extern "C" fn(*mut c_void, *const RavenPeDelayDescriptor) -> bool;
type ExportFound = unsafe extern "C" fn(*mut c_void, *const RavenPeExport) -> bool;
type NameFound = unsafe extern "C" fn(*mut c_void, u32, Text) -> bool;
type ResourceFound = unsafe extern "C" fn(*mut c_void, *const RavenPeResource) -> bool;
type RangeRead = unsafe extern "C" fn(*mut c_void, u64, u64);

fn import(found: crate::Import) -> RavenPeImport {
    RavenPeImport {
        module: found.module.into(),
        name: found.name.into(),
        ordinal: found.ordinal,
        by_ordinal: found.by_ordinal,
        iat_address: found.iat_address,
    }
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_parse(data: *const u8, size: usize, headers: *mut Headers) -> *const c_char {
    result(Image::parse(slice(data, size)).map(|image| headers.write(image.headers().clone())))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_file_span(
    data: *const u8,
    size: usize,
    headers: *const Headers,
    rva: u32,
    length: u64,
    offset: *mut usize,
) -> *const c_char {
    let bytes = slice(data, size);
    let image = Image::with_headers(bytes, &*headers);
    result(image.file_span(rva, length).map(|span| offset.write(span.as_ptr().offset_from(bytes.as_ptr()) as usize)))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_text_at(
    data: *const u8,
    size: usize,
    headers: *const Headers,
    rva: u32,
    text: *mut Text,
) -> *const c_char {
    let image = Image::with_headers(slice(data, size), &*headers);
    result(image.text_at(rva).map(|found| text.write(found.into())))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_il_only(data: *const u8, size: usize, headers: *const Headers, il_only: *mut bool) -> *const c_char {
    let image = Image::with_headers(slice(data, size), &*headers);
    result(image.il_only().map(|answer| il_only.write(answer)))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_imports(
    data: *const u8,
    size: usize,
    headers: *const Headers,
    context: *mut c_void,
    each: ImportFound,
) -> *const c_char {
    let image = Image::with_headers(slice(data, size), &*headers);
    result(image.imports(|found| each(context, &import(found))))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_delay_imports(
    data: *const u8,
    size: usize,
    headers: *const Headers,
    context: *mut c_void,
    descriptor: DescriptorFound,
    each: ImportFound,
) -> *const c_char {
    let image = Image::with_headers(slice(data, size), &*headers);
    result(image.delay_imports(
        |found| {
            let item = RavenPeDelayDescriptor {
                module: found.module.into(),
                descriptor_address: found.descriptor_address,
                module_handle_address: found.module_handle_address,
                iat_address: found.iat_address,
                lookup_address: found.lookup_address,
                bound_address: found.bound_address,
                unload_address: found.unload_address,
                timestamp: found.timestamp,
            };
            descriptor(context, &item)
        },
        |found| each(context, &import(found)),
    ))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_exports(
    data: *const u8,
    size: usize,
    headers: *const Headers,
    context: *mut c_void,
    each: ExportFound,
    named: NameFound,
) -> *const c_char {
    let image = Image::with_headers(slice(data, size), &*headers);
    result(image.exports(
        |found| each(context, &RavenPeExport { ordinal: found.ordinal, address: found.address, forwarder: found.forwarder.into() }),
        |index, name| named(context, index, name.into()),
    ))
}

fn resource(found: &crate::Resource) -> RavenPeResource {
    RavenPeResource {
        kind: found.kind.into(),
        name: found.name.into(),
        language: found.language,
        entry_address: found.entry_address,
        data_address: found.data_address,
        size: found.size,
        code_page: found.code_page,
    }
}

/// The resources of the image, each of whose data the file holds.
#[no_mangle]
pub unsafe extern "C" fn raven_pe_resources(
    data: *const u8,
    size: usize,
    headers: *const Headers,
    record_size: u64,
    context: *mut c_void,
    each: ResourceFound,
) -> *const c_char {
    let image = Image::with_headers(slice(data, size), &*headers);
    result(image.resources(record_size, |found| each(context, &resource(found))))
}

/// The resource tree of a directory whose bytes are given, as read_resources reads it; `read` may be null.
#[no_mangle]
pub unsafe extern "C" fn raven_pe_read_resources(
    directory: Directory,
    data: *const u8,
    size: usize,
    image_size: u32,
    record_size: u64,
    context: *mut c_void,
    each: ResourceFound,
    read: Option<RangeRead>,
) -> *const c_char {
    result(read_resources(
        directory,
        slice(data, size),
        image_size,
        record_size,
        |found| each(context, &resource(found)),
        |offset, length| {
            if let Some(read) = read {
                read(context, offset, length);
            }
        },
    ))
}

#[no_mangle]
pub unsafe extern "C" fn raven_pe_widen_il_only(data: *mut u8, size: usize) -> bool {
    if data.is_null() || size == 0 {
        return false;
    }
    crate::widen_il_only(std::slice::from_raw_parts_mut(data, size))
}

/// The reason a read stopped because the caller's function asked it to.
#[no_mangle]
pub extern "C" fn raven_pe_stopped() -> *const c_char {
    STOPPED.as_ptr()
}
