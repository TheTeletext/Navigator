// This file is providing the malloc, realloc, and free functions
// (each with the `jpegxr_custom_` prefix added) for C, delegating to
// the `std::alloc` crate. Since in Rust, we also have to pass in the
// size when deallocating, we use a little trick to keep track of it:
// Every allocation is 4 bytes longer than the C code asks for, and the
// first 4 bytes store the allocation size, so we can read it for deallocation.
// The pointer returned to C points to just after these 4 bytes.

use std::alloc::Layout;

#[no_mangle]
unsafe fn malloc(bytes: usize) -> *mut u8 {
    let modified_size = bytes + 4;
    let info_ptr = std::alloc::alloc(Layout::from_size_align(modified_size, 4).unwrap());
    if info_ptr.is_null() {
        return info_ptr;
    }

    (info_ptr as *mut u32).write_unaligned(modified_size as u32);

    info_ptr.add(4)
}

#[no_mangle]
unsafe fn calloc(num: usize, size: usize) -> *mut u8 {
    let modified_size = (num * size) + 4;
    let info_ptr = std::alloc::alloc_zeroed(Layout::from_size_align(modified_size, 4).unwrap());
    if info_ptr.is_null() {
        return info_ptr;
    }

    (info_ptr as *mut u32).write_unaligned(modified_size as u32);

    info_ptr.add(4)
}

#[no_mangle]
unsafe fn realloc(ptr: *mut u8, bytes: usize) -> *mut u8 {
    if ptr.is_null() {
        return malloc(bytes);
    }

    let info_ptr = ptr.sub(4);
    let old_size = (info_ptr as *mut u32).read_unaligned();

    let new_size = bytes + 4;
    let new_ptr = std::alloc::realloc(
        info_ptr,
        Layout::from_size_align(old_size as usize, 4).unwrap(),
        new_size,
    );

    (new_ptr as *mut u32).write_unaligned(new_size as u32);
    new_ptr.add(4)
}

#[no_mangle]
unsafe fn free(ptr: *mut u8) {
    if !ptr.is_null() {
        let info_ptr = ptr.sub(4);
        let modified_size = (info_ptr as *mut u32).read_unaligned();
        std::alloc::dealloc(
            info_ptr,
            Layout::from_size_align(modified_size as usize, 4).unwrap(),
        );
    }
}
