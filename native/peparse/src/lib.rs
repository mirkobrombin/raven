//! The PE file format (https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) as Raven's loader reads it: the headers and
//! sections, the import, delay import and export tables, the resource tree, and the rewrite that lets a 64-bit process load a .NET
//! program built for any CPU. Every offset and size is checked against the bytes it points into and every sum against overflow; a file
//! that breaks a rule is refused with the reason, never read past. The host reaches it through the C functions of `ffi`.

use std::borrow::Cow;
use std::collections::BTreeSet;
use std::ffi::CStr;
use std::ops::Deref;

mod ffi;

/// Why a file was refused, as text the host reports.
pub type Error = &'static CStr;

fn range(offset: u64, size: u64, limit: u64) -> Result<(), Error> {
    if offset > limit || size > limit - offset {
        Err(c"PE range exceeds its backing data")
    } else {
        Ok(())
    }
}

fn read_le(bytes: &[u8], offset: u64, size: u64) -> Result<u64, Error> {
    range(offset, size, bytes.len() as u64)?;
    let start = offset as usize;
    Ok(bytes[start..start + size as usize]
        .iter()
        .enumerate()
        .fold(0, |value, (index, byte)| value | u64::from(*byte) << (index * 8)))
}

fn read16(bytes: &[u8], offset: u64) -> Result<u16, Error> {
    Ok(read_le(bytes, offset, 2)? as u16)
}

fn read32(bytes: &[u8], offset: u64) -> Result<u32, Error> {
    Ok(read_le(bytes, offset, 4)? as u32)
}

fn subslice(bytes: &[u8], offset: u64, size: u64) -> Result<&[u8], Error> {
    range(offset, size, bytes.len() as u64)?;
    Ok(&bytes[offset as usize..(offset + size) as usize])
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Directory {
    pub address: u32,
    pub size: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Section {
    /// The name up to its first zero byte, at most eight bytes.
    pub name: [u8; 8],
    pub name_size: u8,
    pub address: u32,
    pub virtual_size: u32,
    pub file_offset: u32,
    pub file_size: u32,
    pub characteristics: u32,
}

impl Section {
    fn extent(&self) -> u32 {
        self.virtual_size.max(self.file_size)
    }
}

/// The most sections a file may have.
pub const MAX_SECTIONS: usize = 96;

/// The headers of a PE file, read and checked, in the form the host keeps them and hands back with the file for each later read.
#[repr(C)]
#[derive(Clone, Debug)]
pub struct Headers {
    pub is_64: bool,
    pub machine: u16,
    pub characteristics: u16,
    pub image_base: u64,
    pub stack_reserve: u64,
    /// IMAGE_SUBSYSTEM_*: 2 for a windowed program, 3 for a console one.
    pub subsystem: u16,
    pub heap_reserve: u64,
    pub heap_commit: u64,
    pub image_size: u32,
    pub headers_size: u32,
    pub entry: u32,
    pub section_alignment: u32,
    pub file_alignment: u32,
    pub directories: [Directory; 16],
    pub section_count: u32,
    pub section_table: [Section; MAX_SECTIONS],
}

// The layout peparse.h declares for the host.
const _: () = assert!(std::mem::size_of::<Headers>() == 3272 && std::mem::size_of::<Section>() == 32);

impl Headers {
    pub fn sections(&self) -> &[Section] {
        &self.section_table[..(self.section_count as usize).min(MAX_SECTIONS)]
    }
}

/// A PE file and its headers; the tables are read from its bytes on demand.
#[derive(Clone, Debug)]
pub struct Image<'a> {
    bytes: &'a [u8],
    headers: Cow<'a, Headers>,
}

impl Deref for Image<'_> {
    type Target = Headers;

    fn deref(&self) -> &Headers {
        &self.headers
    }
}

pub struct Import<'a> {
    pub module: &'a [u8],
    /// Empty for an import by ordinal.
    pub name: &'a [u8],
    pub ordinal: u16,
    pub by_ordinal: bool,
    pub iat_address: u32,
}

pub struct DelayDescriptor<'a> {
    pub module: &'a [u8],
    pub descriptor_address: u32,
    pub module_handle_address: u32,
    pub iat_address: u32,
    pub lookup_address: u32,
    pub bound_address: u32,
    pub unload_address: u32,
    pub timestamp: u32,
}

pub struct Export<'a> {
    pub ordinal: u32,
    pub address: u32,
    /// Empty unless the export forwards to another library's.
    pub forwarder: &'a [u8],
}

/// A resource type or name: a number, or a string of UTF-16 code units kept as the little-endian bytes of the directory.
#[derive(Clone, Copy, Debug)]
pub enum Key<'a> {
    Id(u32),
    Name(&'a [u8]),
}

impl Key<'_> {
    fn units(&self) -> impl Iterator<Item = u16> + '_ {
        let bytes = match self {
            Key::Id(_) => &[][..],
            Key::Name(bytes) => bytes,
        };
        bytes.chunks_exact(2).map(|unit| u16::from_le_bytes([unit[0], unit[1]]))
    }

    /// Whether this key sorts before the other of the same kind, numbers by value and names by code unit.
    fn before(&self, other: &Key) -> Option<bool> {
        match (self, other) {
            (Key::Id(a), Key::Id(b)) => Some(a < b),
            (Key::Name(_), Key::Name(_)) => Some(self.units().lt(other.units())),
            _ => None,
        }
    }

    fn text_size(&self) -> u64 {
        match self {
            Key::Id(_) => 0,
            Key::Name(bytes) => bytes.len() as u64,
        }
    }
}

pub struct Resource<'a> {
    pub kind: Key<'a>,
    pub name: Key<'a>,
    pub language: u32,
    pub entry_address: u32,
    pub data_address: u32,
    pub size: u32,
    pub code_page: u32,
}

fn power_of_two(value: u32) -> bool {
    value != 0 && value & (value - 1) == 0
}

impl<'a> Image<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, Error> {
        if read16(bytes, 0)? != 0x5a4d {
            return Err(c"missing DOS signature");
        }
        let pe = u64::from(read32(bytes, 0x3c)?);
        range(pe, 24, bytes.len() as u64)?;
        if read32(bytes, pe)? != 0x4550 {
            return Err(c"missing PE signature");
        }
        let machine = read16(bytes, pe + 4)?;
        let characteristics = read16(bytes, pe + 22)?;
        let count = u64::from(read16(bytes, pe + 6)?);
        let optional_size = u64::from(read16(bytes, pe + 20)?);
        let optional = pe + 24;
        range(optional, optional_size, bytes.len() as u64)?;
        if optional_size < 2 {
            return Err(c"missing PE optional header");
        }
        let magic = read16(bytes, optional)?;
        if magic != 0x10b && magic != 0x20b {
            return Err(c"unsupported PE optional header");
        }
        let is_64 = magic == 0x20b;
        // A PE32+ header on an x86 machine is a .NET image built for any CPU in the form a 64-bit process loads it (widen_il_only).
        if machine != if is_64 { 0x8664 } else { 0x14c } && !(is_64 && machine == 0x14c) {
            return Err(c"unsupported or inconsistent PE architecture");
        }
        let directory_offset = if is_64 { 112 } else { 96 };
        if optional_size < directory_offset {
            return Err(c"truncated PE optional header");
        }
        let wide = if is_64 { 8 } else { 4 };
        let mut image = Headers {
            is_64,
            machine,
            characteristics,
            entry: read32(bytes, optional + 16)?,
            image_base: read_le(bytes, optional + if is_64 { 24 } else { 28 }, wide)?,
            subsystem: read16(bytes, optional + 68)?,
            stack_reserve: read_le(bytes, optional + 72, wide)?,
            heap_reserve: read_le(bytes, optional + if is_64 { 88 } else { 80 }, wide)?,
            heap_commit: read_le(bytes, optional + if is_64 { 96 } else { 84 }, wide)?,
            section_alignment: read32(bytes, optional + 32)?,
            file_alignment: read32(bytes, optional + 36)?,
            image_size: read32(bytes, optional + 56)?,
            headers_size: read32(bytes, optional + 60)?,
            directories: [Directory::default(); 16],
            section_count: 0,
            section_table: [Section::default(); MAX_SECTIONS],
        };
        let (section_alignment, file_alignment) = (image.section_alignment, image.file_alignment);
        if !power_of_two(section_alignment)
            || !power_of_two(file_alignment)
            || section_alignment < file_alignment
            || (section_alignment < 4096 && file_alignment != section_alignment)
            || (section_alignment >= 4096 && !(512..=65536).contains(&file_alignment))
        {
            return Err(c"invalid PE alignment");
        }
        if image.image_size == 0
            || image.image_size % section_alignment != 0
            || image.headers_size > image.image_size
            || image.headers_size % file_alignment != 0
            || (image.entry != 0 && image.entry >= image.image_size)
        {
            return Err(c"invalid PE image dimensions");
        }
        let address_limit = if is_64 { u64::MAX } else { 0x1_0000_0000 };
        if image.image_base > address_limit || u64::from(image.image_size) > address_limit - image.image_base {
            return Err(c"PE image address overflows its architecture");
        }
        let directory_count = u64::from(read32(bytes, optional + directory_offset - 4)?);
        if directory_count > (optional_size - directory_offset) / 8 {
            return Err(c"PE directories exceed optional header");
        }
        for index in 0..directory_count.min(16) {
            let at = optional + directory_offset + index * 8;
            let directory = Directory { address: read32(bytes, at)?, size: read32(bytes, at + 4)? };
            // The certificate directory uses file offsets rather than RVAs.
            if directory.size != 0 {
                let limit = if index == 4 { bytes.len() as u64 } else { u64::from(image.image_size) };
                range(u64::from(directory.address), u64::from(directory.size), limit)?;
            }
            image.directories[index as usize] = directory;
        }
        // Only a .NET image comes as a PE32+ header on an x86 machine.
        if is_64 && machine == 0x14c && image.directories[14].address == 0 {
            return Err(c"unsupported or inconsistent PE architecture");
        }
        let table = optional + optional_size;
        if count == 0 || count > MAX_SECTIONS as u64 {
            return Err(c"invalid PE section count");
        }
        range(table, count * 40, bytes.len() as u64)?;
        if u64::from(image.headers_size) < table + count * 40 || u64::from(image.headers_size) > bytes.len() as u64 {
            return Err(c"PE headers do not contain the section table");
        }
        for index in 0..count {
            let offset = table + index * 40;
            let raw_name = subslice(bytes, offset, 8)?;
            let name_size = raw_name.iter().position(|&byte| byte == 0).unwrap_or(8);
            let mut name = [0; 8];
            name[..name_size].copy_from_slice(&raw_name[..name_size]);
            let mut section = Section {
                name,
                name_size: name_size as u8,
                address: read32(bytes, offset + 12)?,
                virtual_size: read32(bytes, offset + 8)?,
                file_offset: read32(bytes, offset + 20)?,
                file_size: read32(bytes, offset + 16)?,
                characteristics: read32(bytes, offset + 36)?,
            };
            // Windows maps a section's virtual size and copies no more of its raw data than that size fills: self-extracting setups
            // keep their payload as raw data far past the virtual size of their last section.
            if section.virtual_size != 0 {
                let alignment = u64::from(section_alignment);
                let mapped = u64::from(section.virtual_size).div_ceil(alignment) * alignment;
                if u64::from(section.file_size) > mapped {
                    section.file_size = mapped as u32;
                }
            }
            let extent = section.extent();
            range(u64::from(section.address), u64::from(extent), u64::from(image.image_size))?;
            if section.address % section_alignment != 0 || (extent != 0 && section.address < image.headers_size) {
                return Err(c"invalid PE section address");
            }
            if section.file_size != 0 {
                range(u64::from(section.file_offset), u64::from(section.file_size), bytes.len() as u64)?;
                if section.file_offset < image.headers_size || section.file_offset % file_alignment != 0 {
                    return Err(c"invalid PE section file offset");
                }
            }
            for previous in image.sections() {
                let previous_size = previous.extent();
                let previous_end = u64::from(previous.address) + u64::from(previous_size);
                if extent != 0
                    && previous_size != 0
                    && previous_end > u64::from(section.address)
                    && u64::from(section.address) + u64::from(extent) > u64::from(previous.address)
                {
                    return Err(c"overlapping PE sections");
                }
            }
            image.section_table[image.section_count as usize] = section;
            image.section_count += 1;
        }
        Ok(Image { bytes, headers: Cow::Owned(image) })
    }

    /// The file with headers an earlier parse of the same bytes returned.
    pub fn with_headers(bytes: &'a [u8], headers: &'a Headers) -> Self {
        Image { bytes, headers: Cow::Borrowed(headers) }
    }

    pub fn headers(&self) -> &Headers {
        &self.headers
    }

    /// The bytes the file holds for `size` bytes at the image address `rva`.
    pub fn file_span(&self, rva: u32, size: u64) -> Result<&'a [u8], Error> {
        range(u64::from(rva), size, u64::from(self.image_size))?;
        if rva < self.headers_size {
            range(u64::from(rva), size, u64::from(self.headers_size))?;
            return subslice(self.bytes, u64::from(rva), size);
        }
        for section in self.sections() {
            if rva < section.address || u64::from(rva) >= u64::from(section.address) + u64::from(section.file_size) {
                continue;
            }
            let relative = u64::from(rva - section.address);
            range(relative, size, u64::from(section.file_size))?;
            return subslice(self.bytes, u64::from(section.file_offset) + relative, size);
        }
        Err(c"PE RVA has no file-backed data")
    }

    /// The zero-terminated string at `rva`, without its terminator.
    pub fn text_at(&self, rva: u32) -> Result<&'a [u8], Error> {
        let mut remaining = if rva < self.headers_size { u64::from(self.headers_size - rva) } else { 0 };
        for section in self.sections() {
            if rva >= section.address && u64::from(rva) < u64::from(section.address) + u64::from(section.file_size) {
                remaining = u64::from(section.file_size - (rva - section.address));
            }
        }
        let data = self.file_span(rva, remaining)?;
        let end = data.iter().position(|&byte| byte == 0).ok_or(c"unterminated PE string")?;
        Ok(&data[..end])
    }

    /// Whether this is a .NET image with no native code (COMIMAGE_FLAGS_ILONLY).
    pub fn il_only(&self) -> Result<bool, Error> {
        let directory = self.directories[14];
        if directory.address == 0 || directory.size < 20 {
            return Ok(false);
        }
        Ok(read32(self.file_span(directory.address, 20)?, 16)? & 1 != 0)
    }

    fn mapped(&self, address: u32) -> bool {
        address < self.headers_size
            || self.sections().iter().any(|section| {
                address >= section.address && u64::from(address) < u64::from(section.address) + u64::from(section.extent())
            })
    }

    pub fn imports(&self, mut each: impl FnMut(Import<'a>) -> bool) -> Result<(), Error> {
        let directory = self.directories[1];
        if directory.address == 0 && directory.size == 0 {
            return Ok(());
        }
        if directory.address == 0 || directory.size < 20 {
            return Err(c"invalid PE import directory");
        }
        let descriptors = self.file_span(directory.address, u64::from(directory.size))?;
        let width: u64 = if self.is_64 { 8 } else { 4 };
        for descriptor in descriptors.chunks_exact(20) {
            if descriptor.iter().all(|&byte| byte == 0) {
                return Ok(());
            }
            let module = self.text_at(read32(descriptor, 12)?)?;
            let iat = read32(descriptor, 16)?;
            let lookup = match read32(descriptor, 0)? {
                0 => iat,
                lookup => lookup,
            };
            if iat == 0 || lookup == 0 || module.is_empty() {
                return Err(c"invalid PE import descriptor");
            }
            for index in 0u64.. {
                let slot = u64::from(lookup) + index * width;
                let destination = u64::from(iat) + index * width;
                if slot > u64::from(u32::MAX) || destination > u64::from(u32::MAX) {
                    return Err(c"PE thunk address overflows");
                }
                let value = read_le(self.file_span(slot as u32, width)?, 0, width)?;
                if value == 0 {
                    break;
                }
                range(destination, width, u64::from(self.image_size))?;
                let ordinal = value & 1 << (width * 8 - 1) != 0;
                if !ordinal && value > u64::from(u32::MAX - 2) {
                    return Err(c"PE import name RVA overflows");
                }
                let name = if ordinal {
                    &[][..]
                } else {
                    self.file_span(value as u32, 2)?;
                    self.text_at(value as u32 + 2)?
                };
                let import = Import {
                    module,
                    name,
                    ordinal: if ordinal { value as u16 } else { 0 },
                    by_ordinal: ordinal,
                    iat_address: destination as u32,
                };
                if !each(import) {
                    return Err(STOPPED);
                }
            }
        }
        Err(c"unterminated PE import directory")
    }

    pub fn delay_imports(
        &self,
        mut descriptor_found: impl FnMut(DelayDescriptor<'a>) -> bool,
        mut each: impl FnMut(Import<'a>) -> bool,
    ) -> Result<(), Error> {
        let directory = self.directories[13];
        if directory.address == 0 && directory.size == 0 {
            return Ok(());
        }
        if directory.address == 0 || directory.size < 32 {
            return Err(c"invalid PE delay import directory");
        }
        // A packed program keeps the table in the part of a section the file holds no data for, where its unpacker writes it at run
        // time; the table is read when a delayed function is first called. The file itself holds no table yet.
        let (start, end) = (u64::from(directory.address), u64::from(directory.address) + u64::from(directory.size));
        if self.sections().iter().any(|section| {
            start >= u64::from(section.address) + u64::from(section.file_size)
                && end <= u64::from(section.address) + u64::from(section.virtual_size)
        }) {
            return Ok(());
        }
        let descriptors = self.file_span(directory.address, u64::from(directory.size))?;
        let width: u64 = if self.is_64 { 8 } else { 4 };
        let mut descriptor_count = 0usize;
        let mut import_count = 0usize;
        for (index, descriptor) in descriptors.chunks_exact(32).enumerate() {
            if descriptor.iter().all(|&byte| byte == 0) {
                return Ok(());
            }
            if descriptor_count >= 10000 {
                return Err(c"PE delay descriptors exceed initial resource limit");
            }
            descriptor_count += 1;
            let attributes = read32(descriptor, 0)?;
            if attributes & !1 != 0 {
                return Err(c"reserved PE delay import attributes");
            }
            let rva = |value: u64| -> Result<u32, Error> {
                if value == 0 {
                    return Ok(0);
                }
                let mut value = value;
                if attributes & 1 == 0 {
                    if value < self.image_base {
                        return Err(c"PE delay import VA precedes image");
                    }
                    value -= self.image_base;
                }
                if value >= u64::from(self.image_size) {
                    return Err(c"PE delay import address exceeds image");
                }
                Ok(value as u32)
            };
            let module_name = rva(u64::from(read32(descriptor, 4)?))?;
            if module_name == 0 {
                return Err(c"missing PE delay import module name");
            }
            let item = DelayDescriptor {
                module: self.text_at(module_name)?,
                descriptor_address: (u64::from(directory.address) + index as u64 * 32) as u32,
                module_handle_address: rva(u64::from(read32(descriptor, 8)?))?,
                iat_address: rva(u64::from(read32(descriptor, 12)?))?,
                lookup_address: rva(u64::from(read32(descriptor, 16)?))?,
                bound_address: rva(u64::from(read32(descriptor, 20)?))?,
                unload_address: rva(u64::from(read32(descriptor, 24)?))?,
                timestamp: read32(descriptor, 28)?,
            };
            if item.module.is_empty() || item.module_handle_address == 0 || item.iat_address == 0 || item.lookup_address == 0 {
                return Err(c"invalid PE delay import descriptor");
            }
            // Mutable delay-load storage may be in a zero-filled section.
            let mapped = |address: u64, size: u64| -> Result<(), Error> {
                range(address, size, u64::from(self.image_size))?;
                let headers = u64::from(self.headers_size);
                if address < headers && size <= headers - address {
                    return Ok(());
                }
                let inside = self.sections().iter().any(|section| {
                    let end = u64::from(section.address) + u64::from(section.extent());
                    address >= u64::from(section.address) && address < end && size <= end - address
                });
                if inside {
                    Ok(())
                } else {
                    Err(c"PE delay import storage is not mapped")
                }
            };
            mapped(u64::from(item.module_handle_address), width)?;
            let (module, lookup, iat, bound, unload) =
                (item.module, item.lookup_address, item.iat_address, item.bound_address, item.unload_address);
            if !descriptor_found(item) {
                return Err(STOPPED);
            }
            for index in 0u64.. {
                let slot = u64::from(lookup) + index * width;
                let destination = u64::from(iat) + index * width;
                if slot > u64::from(u32::MAX) || destination > u64::from(u32::MAX) {
                    return Err(c"PE delay thunk address overflows");
                }
                let value = read_le(self.file_span(slot as u32, width)?, 0, width)?;
                mapped(destination, width)?;
                if bound != 0 {
                    self.file_span(bound, (index + 1) * width)?;
                }
                if unload != 0 {
                    self.file_span(unload, (index + 1) * width)?;
                }
                if value == 0 {
                    break;
                }
                import_count += 1;
                if import_count > 1_000_000 {
                    return Err(c"PE delay imports exceed initial resource limit");
                }
                let top = 1u64 << (width * 8 - 1);
                let ordinal = value & top != 0;
                if ordinal && value & !(top | 0xffff) != 0 {
                    return Err(c"reserved PE delay ordinal bits");
                }
                let name = if ordinal {
                    &[][..]
                } else {
                    let at = rva(value)?;
                    self.file_span(at, 2)?;
                    let symbol = self.text_at(at.checked_add(2).ok_or(c"PE range exceeds its backing data")?)?;
                    if symbol.is_empty() {
                        return Err(c"empty PE delay import name");
                    }
                    symbol
                };
                let import = Import {
                    module,
                    name,
                    ordinal: if ordinal { value as u16 } else { 0 },
                    by_ordinal: ordinal,
                    iat_address: destination as u32,
                };
                if !each(import) {
                    return Err(STOPPED);
                }
            }
        }
        Err(c"unterminated PE delay import directory")
    }

    /// The exports in ordinal order, then the names in the order of the name table, each with the index of the export it names.
    pub fn exports(
        &self,
        mut each: impl FnMut(Export<'a>) -> bool,
        mut named: impl FnMut(u32, &'a [u8]) -> bool,
    ) -> Result<(), Error> {
        let directory = self.directories[0];
        if directory.address == 0 && directory.size == 0 {
            return Ok(());
        }
        if directory.address == 0 || directory.size < 40 {
            return Err(c"invalid PE export directory");
        }
        let header = self.file_span(directory.address, 40)?;
        let base = read32(header, 16)?;
        let count = read32(header, 20)?;
        let name_count = read32(header, 24)?;
        if count > 1_000_000 || name_count > 1_000_000 {
            return Err(c"PE exports exceed initial resource limit");
        }
        if count != 0 && u64::from(base) + u64::from(count) - 1 > u64::from(u32::MAX) {
            return Err(c"PE export ordinal overflows");
        }
        if (count != 0 && read32(header, 28)? == 0) || (name_count != 0 && (read32(header, 32)? == 0 || read32(header, 36)? == 0)) {
            return Err(c"missing PE export table address");
        }
        let addresses = if count != 0 { self.file_span(read32(header, 28)?, u64::from(count) * 4)? } else { &[][..] };
        let names = if name_count != 0 { self.file_span(read32(header, 32)?, u64::from(name_count) * 4)? } else { &[][..] };
        let ordinals = if name_count != 0 { self.file_span(read32(header, 36)?, u64::from(name_count) * 2)? } else { &[][..] };
        let directory_end = u64::from(directory.address) + u64::from(directory.size);
        for index in 0..count {
            let address = read32(addresses, u64::from(index) * 4)?;
            let mut forwarder = &[][..];
            if address >= directory.address && u64::from(address) < directory_end {
                let data = self.file_span(address, directory_end - u64::from(address))?;
                let end = data.iter().position(|&byte| byte == 0);
                match end {
                    Some(end) if end > 0 => forwarder = &data[..end],
                    _ => return Err(c"invalid PE export forwarder string"),
                }
            } else if address != 0 && !self.mapped(address) {
                return Err(c"PE export target is outside mapped image data");
            }
            if !each(Export { ordinal: base + index, address, forwarder }) {
                return Err(STOPPED);
            }
        }
        let mut previous: &[u8] = &[];
        for index in 0..name_count {
            let ordinal = read16(ordinals, u64::from(index) * 2)?;
            if u32::from(ordinal) >= count {
                return Err(c"PE export name ordinal exceeds address table");
            }
            let name = self.text_at(read32(names, u64::from(index) * 4)?)?;
            // Tools that fill the table after linking leave placeholder slots with no name, which no lookup can reach; they sort first.
            if name.is_empty() {
                continue;
            }
            if !previous.is_empty() && name <= previous {
                return Err(c"PE export names are empty, duplicated or unsorted");
            }
            previous = name;
            if !named(u32::from(ordinal), name) {
                return Err(STOPPED);
            }
        }
        Ok(())
    }

    /// The resources of the image, each of whose data the file holds.
    pub fn resources(&self, record_size: u64, mut each: impl FnMut(&Resource<'a>) -> bool) -> Result<(), Error> {
        let directory = self.directories[2];
        if directory.address == 0 && directory.size == 0 {
            return Ok(());
        }
        let bytes = self.file_span(directory.address, u64::from(directory.size))?;
        let mut found = Vec::new();
        read_resources(directory, bytes, self.image_size, record_size, |resource| {
            found.push((resource.data_address, resource.size));
            each(resource)
        }, |_, _| ())?;
        for (address, size) in found {
            self.file_span(address, u64::from(size))?;
        }
        Ok(())
    }
}

/// Returned when a caller's function asked to stop.
pub const STOPPED: Error = c"PE reading was stopped";

/// The resource tree of a directory whose bytes are given: directory offsets are relative to its root, payload addresses
/// image-relative. `record_size` is what each resource costs its caller before its names, for the limit on what a file may make it
/// hold; `read` receives every range of the directory the walk read, as offset and size.
pub fn read_resources<'a>(
    directory: Directory,
    bytes: &'a [u8],
    image_size: u32,
    record_size: u64,
    mut each: impl FnMut(&Resource<'a>) -> bool,
    mut read: impl FnMut(u64, u64),
) -> Result<(), Error> {
    if directory.address == 0 && directory.size == 0 {
        return Ok(());
    }
    if directory.address == 0
        || directory.size == 0
        || bytes.len() as u64 != u64::from(directory.size)
        || directory.address > image_size
        || directory.size > image_size - directory.address
    {
        return Err(c"invalid PE resource directory extent");
    }
    let mut walk = Walk { bytes, image_size, record_size, root: directory.address, visited: BTreeSet::new(), entries: 0, result_bytes: 0 };
    walk.directory(0, 0, Key::Id(0), Key::Id(0), &mut each, &mut read)
}

struct Walk<'a> {
    bytes: &'a [u8],
    image_size: u32,
    record_size: u64,
    root: u32,
    visited: BTreeSet<u32>,
    entries: u64,
    result_bytes: u64,
}

impl<'a> Walk<'a> {
    fn range(&self, offset: u64, size: u64, read: &mut impl FnMut(u64, u64)) -> Result<(), Error> {
        let limit = self.bytes.len() as u64;
        if offset > limit || size > limit - offset {
            return Err(c"PE resource metadata exceeds directory");
        }
        read(offset, size);
        Ok(())
    }

    fn number(&self, offset: u64, width: u64, read: &mut impl FnMut(u64, u64)) -> Result<u32, Error> {
        self.range(offset, width, read)?;
        Ok(read_le(self.bytes, offset, width)? as u32)
    }

    fn key(&self, value: u32, read: &mut impl FnMut(u64, u64)) -> Result<Key<'a>, Error> {
        if value & 0x8000_0000 == 0 {
            return Ok(Key::Id(value));
        }
        let offset = u64::from(value & 0x7fff_ffff);
        if offset % 2 != 0 {
            return Err(c"unaligned PE resource name");
        }
        let count = u64::from(self.number(offset, 2, read)?);
        self.range(offset + 2, count * 2, read)?;
        Ok(Key::Name(subslice(self.bytes, offset + 2, count * 2)?))
    }

    fn directory(
        &mut self,
        offset: u32,
        depth: u32,
        kind: Key<'a>,
        name: Key<'a>,
        each: &mut impl FnMut(&Resource<'a>) -> bool,
        read: &mut impl FnMut(u64, u64),
    ) -> Result<(), Error> {
        if offset % 4 != 0 || !self.visited.insert(offset) {
            return Err(c"unaligned or reused PE resource directory");
        }
        let at = u64::from(offset);
        self.range(at, 16, read)?;
        let named = u64::from(self.number(at + 12, 2, read)?);
        let count = named + u64::from(self.number(at + 14, 2, read)?);
        self.range(at + 16, count * 8, read)?;
        // Every entry takes eight bytes of the directory, so the directory's own size bounds them; large resource libraries hold
        // hundreds of thousands.
        if self.entries + count > self.bytes.len() as u64 / 8 {
            return Err(c"PE resource entries exceed policy limit");
        }
        self.entries += count;
        let mut previous: Option<Key<'a>> = None;
        for index in 0..count {
            let position = at + 16 + index * 8;
            let raw_name = self.number(position, 4, read)?;
            if (raw_name & 0x8000_0000 != 0) != (index < named) {
                return Err(c"PE resource name and ID partitions differ");
            }
            let identity = self.key(raw_name, read)?;
            if let Some(previous) = previous {
                if previous.before(&identity) == Some(false) {
                    return Err(c"PE resource entries are unordered or duplicated");
                }
            }
            previous = Some(identity);
            let child = self.number(position + 4, 4, read)?;
            if depth < 2 {
                if child & 0x8000_0000 == 0 {
                    return Err(c"PE resource tree ends before language");
                }
                let (next_kind, next_name) = (if depth == 0 { identity } else { kind }, if depth == 1 { identity } else { name });
                self.directory(child & 0x7fff_ffff, depth + 1, next_kind, next_name, each, read)?;
                continue;
            }
            let Key::Id(language) = identity else {
                return Err(c"invalid PE resource language leaf");
            };
            if child & 0x8000_0000 != 0 || child % 4 != 0 {
                return Err(c"invalid PE resource language leaf");
            }
            let leaf = u64::from(child);
            self.range(leaf, 16, read)?;
            let data = self.number(leaf, 4, read)?;
            let size = self.number(leaf + 4, 4, read)?;
            if data > self.image_size || size > self.image_size - data {
                return Err(c"PE resource payload exceeds image");
            }
            let record = self.record_size + kind.text_size() + name.text_size();
            if record > 64 * 1024 * 1024 - self.result_bytes.min(64 * 1024 * 1024) {
                return Err(c"PE resource identities exceed policy limit");
            }
            self.result_bytes += record;
            let resource = Resource {
                kind,
                name,
                language,
                entry_address: self.root.wrapping_add(child),
                data_address: data,
                size,
                code_page: self.number(leaf + 8, 4, read)?,
            };
            if !each(&resource) {
                return Err(STOPPED);
            }
        }
        Ok(())
    }
}

/// A .NET program built for any CPU (a PE32 file with only IL code, neither 32BITREQUIRED nor 32BITPREFERRED) runs as a 64-bit
/// process, as on 64-bit Windows: its headers are rewritten in place to the PE32+ form the loader of such a process expects. Its import
/// data stays as it was, for the runtime to check. True when the file was rewritten.
pub fn widen_il_only(file: &mut [u8]) -> bool {
    widen(file).unwrap_or(false)
}

fn widen(file: &mut [u8]) -> Result<bool, Error> {
    let size = file.len() as u64;
    if size < 0x40 || file[0] != b'M' || file[1] != b'Z' {
        return Ok(false);
    }
    let header = u64::from(read32(file, 0x3c)?);
    if header > size || size - header < 24 + 224 || read32(file, header)? != 0x4550 {
        return Ok(false);
    }
    let optional = header + 24;
    let optional_size = u64::from(read16(file, header + 20)?);
    let sections = u64::from(read16(file, header + 6)?);
    if read16(file, optional)? != 0x10b || optional_size < 224 || read16(file, header + 4)? != 0x14c || read32(file, optional + 92)? < 15 {
        return Ok(false);
    }
    // The runtime header (directory 14): flags 1 (IL only) without 2 (32-bit required) or 0x20000 (32-bit preferred).
    let runtime = u64::from(read32(file, optional + 96 + 14 * 8)?);
    let mut flags = None;
    for index in 0..sections {
        let entry = optional + optional_size + index * 40;
        if entry + 40 > size {
            return Ok(false);
        }
        let address = u64::from(read32(file, entry + 12)?);
        let extent = u64::from(read32(file, entry + 8)?.max(read32(file, entry + 16)?));
        let raw = u64::from(read32(file, entry + 20)?);
        if runtime >= address && runtime - address < extent && raw + (runtime - address) + 20 <= size {
            flags = Some(read32(file, raw + (runtime - address) + 16)?);
            break;
        }
    }
    match flags {
        Some(flags) if flags & 1 != 0 && flags & 0x20002 == 0 => {}
        _ => return Ok(false),
    }
    // The PE32+ optional header is 16 bytes longer: the section table moves down by as much, into the headers' padding.
    let table_end = optional + optional_size + sections * 40;
    let headers = u64::from(read32(file, optional + 60)?);
    if table_end + 16 > headers || headers > size {
        return Ok(false);
    }
    for index in 0..sections {
        let raw = read32(file, optional + optional_size + index * 40 + 20)?;
        if raw != 0 && u64::from(raw) < table_end + 16 {
            return Ok(false);
        }
    }
    let read = |file: &[u8], at: u64| read32(file, optional + at).map(u64::from);
    let mut widened = vec![0u8; 240 + (optional_size - 224) as usize];
    let put = |widened: &mut [u8], at: usize, value: u64, width: usize| {
        widened[at..at + width].copy_from_slice(&value.to_le_bytes()[..width]);
    };
    let start = optional as usize;
    widened[..24].copy_from_slice(&file[start..start + 24]);
    put(&mut widened, 0, 0x20b, 2);
    put(&mut widened, 24, read(file, 28)?, 8);
    widened[32..72].copy_from_slice(&file[start + 32..start + 72]);
    put(&mut widened, 72, read(file, 72)?, 8);
    put(&mut widened, 80, read(file, 76)?, 8);
    put(&mut widened, 88, read(file, 80)?, 8);
    put(&mut widened, 96, read(file, 84)?, 8);
    put(&mut widened, 104, read(file, 88)?, 4);
    put(&mut widened, 108, read(file, 92)?, 4);
    widened[112..].copy_from_slice(&file[start + 96..start + optional_size as usize]);
    let table = file[(optional + optional_size) as usize..table_end as usize].to_vec();
    file[start..(table_end + 16) as usize].fill(0);
    file[start..start + widened.len()].copy_from_slice(&widened);
    file[start + widened.len()..start + widened.len() + table.len()].copy_from_slice(&table);
    // The machine stays the one the file names: the runtime tells an image built for any CPU by its x86 machine and IL-only flag.
    let length = widened.len() as u16;
    file[header as usize + 20..header as usize + 22].copy_from_slice(&length.to_le_bytes());
    Ok(true)
}
