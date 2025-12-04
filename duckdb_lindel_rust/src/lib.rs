// duckdb_lindel_rust
// Copyright 2024 Rusty Conover <rusty@conover.me>
// Licensed under the MIT License

use std::ffi::c_void;

// Decode an encoded value and store it in the destination pointer.
#[no_mangle]
pub extern "C" fn perform_decode(
    encoding_type: u8,
    element_bit_width: u8,
    src: *const c_void,
    dest: *mut c_void,
    dest_len: usize,
) {
    macro_rules! decode_and_copy {
        ($dest_type: ty, $src_type:ty, $len:expr) => {{
            unsafe {
                let dest_ptr = dest as *mut $dest_type;
                let function = match encoding_type {
                    0 => lindel::hilbert_decode,
                    1 => lindel::morton_decode,
                    _ => panic!("Invalid encoding type"),
                };
                let values: [$dest_type; $len] = function(*(src as *const $src_type));
                for i in 0..$len {
                    *dest_ptr.add(i) = values[i];
                }
            };
        }};
    }

    match element_bit_width {
        8 => match dest_len {
            1 => decode_and_copy!(u8, u8, 1),
            2 => decode_and_copy!(u8, u16, 2),
            3 => decode_and_copy!(u8, u32, 3),
            4 => decode_and_copy!(u8, u32, 4),
            5 => decode_and_copy!(u8, u64, 5),
            6 => decode_and_copy!(u8, u64, 6),
            7 => decode_and_copy!(u8, u64, 7),
            8 => decode_and_copy!(u8, u64, 8),
            9 => decode_and_copy!(u8, u128, 9),
            10 => decode_and_copy!(u8, u128, 10),
            11 => decode_and_copy!(u8, u128, 11),
            12 => decode_and_copy!(u8, u128, 12),
            13 => decode_and_copy!(u8, u128, 13),
            14 => decode_and_copy!(u8, u128, 14),
            15 => decode_and_copy!(u8, u128, 15),
            16 => decode_and_copy!(u8, u128, 16),
            _ => panic!("Invalid length"),
        },
        16 => match dest_len {
            1 => decode_and_copy!(u16, u16, 1),
            2 => decode_and_copy!(u16, u32, 2),
            3 => decode_and_copy!(u16, u64, 3),
            4 => decode_and_copy!(u16, u64, 4),
            5 => decode_and_copy!(u16, u128, 5),
            6 => decode_and_copy!(u16, u128, 6),
            7 => decode_and_copy!(u16, u128, 7),
            8 => decode_and_copy!(u16, u128, 8),
            _ => panic!("Invalid length"),
        },
        32 => match dest_len {
            1 => decode_and_copy!(u32, u32, 1),
            2 => decode_and_copy!(u32, u64, 2),
            3 => decode_and_copy!(u32, u128, 3),
            4 => decode_and_copy!(u32, u128, 4),
            _ => panic!("Invalid length"),
        },
        64 => match dest_len {
            1 => decode_and_copy!(u64, u64, 1),
            2 => decode_and_copy!(u64, u128, 2),
            _ => panic!("Invalid length"),
        },
        _ => panic!("Invalid element bit width"),
    }
}

// Create a macro to handle the repetitive part
macro_rules! encode_and_store {
    ($function:expr, $array:expr, $type:ty, $result:expr) => {{
        let calculated_result = $function($array);
        let result_ptr = $result as *mut $type;
        unsafe {
            *result_ptr = calculated_result;
        }
    }};
}

macro_rules! generic_encode_u8_var {
    ($func_name:ident, $encoding_expr: expr) => {
        /// # Safety
        ///
        /// This function is unsafe because it dereferences raw pointers.
        #[no_mangle]
        pub unsafe extern "C" fn $func_name(ptr: *const u8, len: usize, result: *mut c_void) -> () {
            let args = unsafe {
                assert!(!ptr.is_null());
                std::slice::from_raw_parts(ptr, len)
            };

            match args.len() {
                1 => encode_and_store!($encoding_expr, [args[0]], u8, result),
                2 => encode_and_store!($encoding_expr, [args[0], args[1]], u16, result),
                3 => encode_and_store!($encoding_expr, [args[0], args[1], args[2]], u32, result),
                4 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3]],
                    u32,
                    result
                ),
                5 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4]],
                    u64,
                    result
                ),
                6 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4], args[5]],
                    u64,
                    result
                ),
                7 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4], args[5], args[6]],
                    u64,
                    result
                ),
                8 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]],
                    u64,
                    result
                ),
                9 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8]
                    ],
                    u128,
                    result
                ),
                10 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9]
                    ],
                    u128,
                    result
                ),
                11 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9], args[10]
                    ],
                    u128,
                    result
                ),
                12 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9], args[10], args[11]
                    ],
                    u128,
                    result
                ),
                13 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9], args[10], args[11], args[12]
                    ],
                    u128,
                    result
                ),
                14 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9], args[10], args[11], args[12], args[13]
                    ],
                    u128,
                    result
                ),
                15 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9], args[10], args[11], args[12], args[13], args[14]
                    ],
                    u128,
                    result
                ),
                16 => encode_and_store!(
                    $encoding_expr,
                    [
                        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                        args[8], args[9], args[10], args[11], args[12], args[13], args[14],
                        args[15]
                    ],
                    u128,
                    result
                ),
                _ => panic!("Invalid length"),
            }
        }
    };
}

generic_encode_u8_var!(hilbert_encode_u8_var, lindel::hilbert_encode);
generic_encode_u8_var!(morton_encode_u8_var, lindel::morton_encode);

macro_rules! generic_encode_u16_var {
    ($func_name:ident, $encoding_expr: expr) => {
        /// # Safety
        ///
        /// This function is unsafe because it dereferences raw pointers.
        #[no_mangle]
        pub unsafe extern "C" fn $func_name(
            ptr: *const u16,
            len: usize,
            result: *mut c_void,
        ) -> () {
            let args = unsafe {
                assert!(!ptr.is_null());
                std::slice::from_raw_parts(ptr, len)
            };

            match args.len() {
                1 => encode_and_store!($encoding_expr, [args[0]], u16, result), // 16
                2 => encode_and_store!($encoding_expr, [args[0], args[1]], u32, result), //32
                3 => encode_and_store!($encoding_expr, [args[0], args[1], args[2]], u64, result), // 48 - 64
                4 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3]],
                    u64,
                    result
                ), // 64 - 64
                5 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4]],
                    u128,
                    result
                ),
                6 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4], args[5]],
                    u128,
                    result
                ),
                7 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4], args[5], args[6]],
                    u128,
                    result
                ),
                8 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]],
                    u128,
                    result
                ),
                _ => panic!("Invalid length"),
            }
        }
    };
}

generic_encode_u16_var!(hilbert_encode_u16_var, lindel::hilbert_encode);
generic_encode_u16_var!(morton_encode_u16_var, lindel::morton_encode);

macro_rules! generic_encode_u32_var {
    ($func_name:ident, $encoding_expr: expr) => {
        /// # Safety
        ///
        /// This function is unsafe because it dereferences raw pointers.
        #[no_mangle]
        pub unsafe extern "C" fn $func_name(
            ptr: *const u32,
            len: usize,
            result: *mut c_void,
        ) -> () {
            let args = unsafe {
                assert!(!ptr.is_null());
                std::slice::from_raw_parts(ptr, len)
            };

            match args.len() {
                1 => encode_and_store!($encoding_expr, [args[0]], u32, result),
                2 => encode_and_store!($encoding_expr, [args[0], args[1]], u64, result),
                3 => encode_and_store!($encoding_expr, [args[0], args[1], args[2]], u128, result),
                4 => encode_and_store!(
                    $encoding_expr,
                    [args[0], args[1], args[2], args[3]],
                    u128,
                    result
                ),
                _ => panic!("Invalid length"),
            }
        }
    };
}

generic_encode_u32_var!(hilbert_encode_u32_var, lindel::hilbert_encode);
generic_encode_u32_var!(morton_encode_u32_var, lindel::morton_encode);

macro_rules! generic_encode_u64_var {
    ($func_name:ident, $encoding_expr: expr) => {
        /// # Safety
        ///
        /// This function is unsafe because it dereferences raw pointers.
        #[no_mangle]
        pub unsafe extern "C" fn $func_name(
            ptr: *const u64,
            len: usize,
            result: *mut c_void,
        ) -> () {
            let args = unsafe {
                assert!(!ptr.is_null());
                std::slice::from_raw_parts(ptr, len)
            };

            match args.len() {
                1 => encode_and_store!($encoding_expr, [args[0]], u64, result),
                2 => encode_and_store!($encoding_expr, [args[0], args[1]], u128, result),
                _ => panic!("Invalid length"),
            }
        }
    };
}

generic_encode_u64_var!(hilbert_encode_u64_var, lindel::hilbert_encode);
generic_encode_u64_var!(morton_encode_u64_var, lindel::morton_encode);

#[cfg(test)]
mod tests {}
