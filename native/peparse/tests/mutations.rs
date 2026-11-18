//! A PE32+ file made here with every table the reader knows, read as it is, then read again after hundreds of thousands of
//! random changes: whatever the bytes, every read either returns or refuses the file, and never reads past it or panics.

use std::cell::{Cell, RefCell};

use raven_peparse::{read_resources, widen_il_only, Directory, Image, Key};

const TEXT: u32 = 0x1000;
const DATA: u32 = 0x2000;
const DATA_FILE: usize = 0x400;

struct File(Vec<u8>);

impl File {
    fn put(&mut self, at: usize, bytes: &[u8]) {
        self.0[at..at + bytes.len()].copy_from_slice(bytes);
    }

    fn u16(&mut self, at: usize, value: u16) {
        self.put(at, &value.to_le_bytes());
    }

    fn u32(&mut self, at: usize, value: u32) {
        self.put(at, &value.to_le_bytes());
    }

    fn u64(&mut self, at: usize, value: u64) {
        self.put(at, &value.to_le_bytes());
    }

    /// The file offset of an address in the data section.
    fn data(rva: u32) -> usize {
        DATA_FILE + (rva - DATA) as usize
    }
}

fn sample() -> Vec<u8> {
    let mut file = File(vec![0; 0x1400]);
    file.put(0, b"MZ");
    file.u32(0x3c, 0x40);
    file.put(0x40, b"PE\0\0");
    file.u16(0x44, 0x8664);
    file.u16(0x46, 2);
    file.u16(0x54, 240);
    file.u16(0x56, 0x2022);
    let optional = 0x58;
    file.u16(optional, 0x20b);
    file.u32(optional + 16, TEXT);
    file.u64(optional + 24, 0x1_4000_0000);
    file.u32(optional + 32, 0x1000);
    file.u32(optional + 36, 0x200);
    file.u32(optional + 56, 0x3000);
    file.u32(optional + 60, 0x200);
    file.u16(optional + 68, 3);
    file.u64(optional + 72, 0x10_0000);
    file.u64(optional + 88, 0x10_0000);
    file.u64(optional + 96, 0x1000);
    file.u32(optional + 108, 16);
    let directories: [(usize, u32, u32); 5] = [(0, 0x2300, 0x100), (1, 0x2000, 40), (2, 0x2600, 0x100), (13, 0x2400, 64), (14, 0x2800, 72)];
    for (index, address, size) in directories {
        file.u32(optional + 112 + index * 8, address);
        file.u32(optional + 116 + index * 8, size);
    }
    let table = optional + 240;
    for (index, (name, address, virtual_size, raw, raw_size)) in
        [(b".text\0\0\0", TEXT, 0x200, 0x200, 0x200), (b".rdata\0\0", DATA, 0x1000, DATA_FILE as u32, 0x1000)].into_iter().enumerate()
    {
        let at = table + index * 40;
        file.put(at, name);
        file.u32(at + 8, virtual_size);
        file.u32(at + 12, address);
        file.u32(at + 16, raw_size);
        file.u32(at + 20, raw);
        file.u32(at + 36, 0x4000_0040);
    }
    file.0[0x200] = 0xc3;
    // Imports from kernel32.dll: one by name, one by ordinal.
    let at = File::data(0x2000);
    file.u32(at, 0x2100);
    file.u32(at + 12, 0x2200);
    file.u32(at + 16, 0x2140);
    for thunks in [0x2100, 0x2140] {
        file.u64(File::data(thunks), 0x2210);
        file.u64(File::data(thunks) + 8, 0x8000_0000_0000_0005);
    }
    file.put(File::data(0x2200), b"kernel32.dll\0");
    file.put(File::data(0x2212), b"ExitProcess\0");
    // Three exports from ordinal 1: code, a forwarder and a hole; the first two named.
    let at = File::data(0x2300);
    file.u32(at + 16, 1);
    file.u32(at + 20, 3);
    file.u32(at + 24, 2);
    file.u32(at + 28, 0x2340);
    file.u32(at + 32, 0x2350);
    file.u32(at + 36, 0x2360);
    file.u32(File::data(0x2340), TEXT);
    file.u32(File::data(0x2344), 0x2380);
    file.u32(File::data(0x2350), 0x23a0);
    file.u32(File::data(0x2354), 0x23b0);
    file.u16(File::data(0x2362), 1);
    file.put(File::data(0x2380), b"ntdll.RtlZeroMemory\0");
    file.put(File::data(0x23a0), b"Alpha\0");
    file.put(File::data(0x23b0), b"Beta\0");
    // A delayed import from user32.dll, by address relative to the image.
    let at = File::data(0x2400);
    file.u32(at, 1);
    file.u32(at + 4, 0x2480);
    file.u32(at + 8, 0x2490);
    file.u32(at + 12, 0x24a0);
    file.u32(at + 16, 0x24c0);
    file.put(File::data(0x2480), b"user32.dll\0");
    file.u64(File::data(0x24c0), 0x24e0);
    file.put(File::data(0x24e2), b"MessageBoxW\0");
    // One string table resource, named "AB", in US English.
    let root = File::data(0x2600);
    file.u16(root + 14, 1);
    file.u32(root + 16, 6);
    file.u32(root + 20, 0x8000_0020);
    file.u16(root + 0x20 + 12, 1);
    file.u32(root + 0x30, 0x8000_0080);
    file.u32(root + 0x34, 0x8000_0040);
    file.u16(root + 0x40 + 14, 1);
    file.u32(root + 0x50, 0x409);
    file.u32(root + 0x54, 0x60);
    file.u32(root + 0x60, 0x2700);
    file.u32(root + 0x64, 4);
    file.u16(root + 0x80, 2);
    file.put(root + 0x82, &[b'A', 0, b'B', 0]);
    // A runtime header whose flags say IL only.
    file.u32(File::data(0x2800) + 16, 1);
    file.0
}

/// Every read the reader offers, with the number of things it found; refusals are counted as nothing.
fn read_everything(bytes: &[u8], seed: u64) -> usize {
    let Ok(image) = Image::parse(bytes) else {
        return 0;
    };
    let found = Cell::new(0);
    let count = || {
        found.set(found.get() + 1);
        true
    };
    let _ = image.imports(|_| count());
    let _ = image.delay_imports(|_| count(), |_| count());
    let _ = image.exports(|_| count(), |_, _| count());
    let _ = image.resources(64, |_| count());
    let _ = image.il_only();
    let rva = (seed % u64::from(image.image_size.max(1) + 16)) as u32;
    let _ = image.text_at(rva);
    let _ = image.file_span(rva, seed >> 48);
    found.get()
}

#[test]
fn reads_the_sample() {
    let bytes = sample();
    let image = Image::parse(&bytes).expect("the sample is a valid file");
    assert!(image.is_64);
    assert_eq!((image.machine, image.subsystem, image.sections().len()), (0x8664, 3, 2));
    assert_eq!(image.file_span(TEXT, 1).unwrap(), &[0xc3]);

    let mut imports = Vec::new();
    image.imports(|import| {
        imports.push((import.module.to_vec(), import.name.to_vec(), import.ordinal, import.by_ordinal, import.iat_address));
        true
    })
    .unwrap();
    assert_eq!(
        imports,
        [
            (b"kernel32.dll".to_vec(), b"ExitProcess".to_vec(), 0, false, 0x2140),
            (b"kernel32.dll".to_vec(), Vec::new(), 5, true, 0x2148)
        ]
    );

    let mut exports = Vec::new();
    let mut names = Vec::new();
    image
        .exports(
            |export| {
                exports.push((export.ordinal, export.address, export.forwarder.to_vec()));
                true
            },
            |index, name| {
                names.push((index, name.to_vec()));
                true
            },
        )
        .unwrap();
    assert_eq!(exports, [(1, TEXT, Vec::new()), (2, 0x2380, b"ntdll.RtlZeroMemory".to_vec()), (3, 0, Vec::new())]);
    assert_eq!(names, [(0, b"Alpha".to_vec()), (1, b"Beta".to_vec())]);

    let delayed = RefCell::new(Vec::new());
    image
        .delay_imports(
            |descriptor| {
                delayed.borrow_mut().push((descriptor.module.to_vec(), descriptor.module_handle_address, descriptor.iat_address));
                true
            },
            |import| {
                delayed.borrow_mut().push((import.name.to_vec(), u32::from(import.ordinal), import.iat_address));
                true
            },
        )
        .unwrap();
    assert_eq!(*delayed.borrow(), [(b"user32.dll".to_vec(), 0x2490, 0x24a0), (b"MessageBoxW".to_vec(), 0, 0x24a0)]);

    let mut resources = Vec::new();
    image
        .resources(64, |resource| {
            let name = match resource.name {
                Key::Name(bytes) => bytes.to_vec(),
                Key::Id(_) => Vec::new(),
            };
            resources.push((matches!(resource.kind, Key::Id(6)), name, resource.language, resource.entry_address, resource.data_address, resource.size));
            true
        })
        .unwrap();
    assert_eq!(resources, [(true, vec![b'A', 0, b'B', 0], 0x409, 0x2660, 0x2700, 4)]);
    assert!(image.il_only().unwrap());
    assert_eq!(read_everything(&bytes, 0), 10);
}

#[test]
fn a_reader_that_stops_ends_the_read() {
    let bytes = sample();
    let image = Image::parse(&bytes).unwrap();
    let mut seen = 0;
    assert_eq!(
        image.imports(|_| {
            seen += 1;
            false
        }),
        Err(raven_peparse::STOPPED)
    );
    assert_eq!(seen, 1);
}

#[test]
fn refuses_what_breaks_a_rule() {
    let bytes = sample();
    let mut broken = bytes.clone();
    broken[0] = b'X';
    assert_eq!(Image::parse(&broken).unwrap_err(), c"missing DOS signature");
    let mut broken = bytes.clone();
    broken[0x46] = 97;
    assert_eq!(Image::parse(&broken).unwrap_err(), c"invalid PE section count");
    // The export names out of order.
    let mut broken = bytes.clone();
    broken[File::data(0x23a0)] = b'Z';
    let image = Image::parse(&broken).unwrap();
    assert_eq!(image.exports(|_| true, |_, _| true), Err(c"PE export names are empty, duplicated or unsorted"));
    // The import name past the end of its section.
    let mut broken = bytes.clone();
    broken[File::data(0x2100)..File::data(0x2100) + 4].copy_from_slice(&0x2ffeu32.to_le_bytes());
    let image = Image::parse(&broken).unwrap();
    assert!(image.imports(|_| true).is_err());
    // A resource directory that points back at itself.
    let mut broken = bytes.clone();
    broken[File::data(0x2614)..File::data(0x2614) + 4].copy_from_slice(&0x8000_0000u32.to_le_bytes());
    let image = Image::parse(&broken).unwrap();
    assert_eq!(image.resources(64, |_| true), Err(c"unaligned or reused PE resource directory"));
}

/// xorshift64*: the same changes on every run, so a failure names its iteration.
struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
}

#[test]
fn survives_random_changes() {
    let original = sample();
    let mut random = Random(0x9e37_79b9_7f4a_7c15);
    let mut bytes = original.clone();
    let mut read = 0usize;
    for _ in 0..300_000 {
        bytes.copy_from_slice(&original);
        let changes = 1 + random.next() % 6;
        for _ in 0..changes {
            let pick = random.next();
            // Most changes land on the headers and tables, where every byte steers the reader.
            let at = match pick % 4 {
                0 => (pick >> 8) as usize % 0x200,
                1 => File::data(0x2000) + (pick >> 8) as usize % 0x900,
                _ => (pick >> 8) as usize % bytes.len(),
            };
            bytes[at] = match (pick >> 40) % 4 {
                0 => 0,
                1 => 0xff,
                _ => (pick >> 48) as u8,
            };
        }
        read += read_everything(&bytes, random.next());
        let mut widened = bytes.clone();
        widen_il_only(&mut widened);
        let size = (random.next() % 0x200) as u32;
        let _ = read_resources(Directory { address: 0x2600, size }, &bytes[0..size as usize], 0x3000, 64, |_| true, |_, _| ());
        // A file cut short anywhere.
        let cut = (random.next() as usize) % bytes.len();
        read_everything(&bytes[..cut], random.next());
    }
    // The changes leave most files readable, so the walk went deep rather than stopping at the headers.
    assert!(read > 300_000, "only {read} entries read");
}
