// duckdb_lindel_rust
// Copyright 2024 Rusty Conover <rusty@conover.me>
// Licensed under the MIT License
//
// Every function exported here returns `true` on success and `false` when it
// is given an argument combination it does not support. None of them panic:
// unwinding across an `extern "C"` boundary would abort the host process, so
// the C++ caller is responsible for turning `false` into a DuckDB exception.
//
// Results and sources are accessed with unaligned reads and writes because
// DuckDB's `uhugeint_t` is only guaranteed 8-byte alignment while Rust's
// `u128` requires 16.

use std::ffi::c_void;

/// Decode one encoded key into `dest_len` coordinates of `element_bit_width`
/// bits each.
///
/// `encoding_type` is 0 for Hilbert and 1 for Morton. `src` must point at an
/// unsigned integer of the width the encoder produced for that combination and
/// `dest` at space for `dest_len` unsigned integers of `element_bit_width`.
///
/// # Safety
///
/// `src` and `dest` must be valid for the reads and writes described above.
#[no_mangle]
pub unsafe extern "C" fn perform_decode(
    encoding_type: u8,
    element_bit_width: u8,
    src: *const c_void,
    dest: *mut c_void,
    dest_len: usize,
) -> bool {
    if src.is_null() || dest.is_null() {
        return false;
    }

    macro_rules! decode_and_copy {
        ($dest_type:ty, $src_type:ty, $len:expr) => {{
            let key = (src as *const $src_type).read_unaligned();
            let values: [$dest_type; $len] = match encoding_type {
                0 => lindel::hilbert_decode(key),
                1 => lindel::morton_decode(key),
                _ => return false,
            };
            let dest_ptr = dest as *mut $dest_type;
            for (i, value) in values.iter().enumerate() {
                dest_ptr.add(i).write_unaligned(*value);
            }
            true
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
            _ => false,
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
            _ => false,
        },
        32 => match dest_len {
            1 => decode_and_copy!(u32, u32, 1),
            2 => decode_and_copy!(u32, u64, 2),
            3 => decode_and_copy!(u32, u128, 3),
            4 => decode_and_copy!(u32, u128, 4),
            _ => false,
        },
        64 => match dest_len {
            1 => decode_and_copy!(u64, u64, 1),
            2 => decode_and_copy!(u64, u128, 2),
            _ => false,
        },
        _ => false,
    }
}

// Encode `$args` (a slice) as an `[_; $n]` array with `$function` and store the
// key as `$out` at `$result`.
macro_rules! encode_and_store {
    ($function:path, $args:expr, $result:expr, $n:literal, $out:ty) => {{
        match <[_; $n]>::try_from($args) {
            Ok(array) => {
                let key: $out = $function(array);
                ($result as *mut $out).write_unaligned(key);
                true
            }
            Err(_) => false,
        }
    }};
}

// Generate one exported encoder for coordinates of type `$elem`.
//
// Each `$n => $out` pair maps a coordinate count to the key type it produces;
// any other count is rejected.
macro_rules! generic_encode {
    ($func_name:ident, $function:path, $elem:ty, { $($n:literal => $out:ty),+ $(,)? }) => {
        /// Encode `len` coordinates read from `ptr` and write the key to `result`.
        ///
        /// Returns `false` (and writes nothing) for an unsupported `len` or a
        /// null pointer.
        ///
        /// # Safety
        ///
        /// `ptr` must be valid for `len` reads and `result` must have room for
        /// the key type that `len` maps to.
        #[no_mangle]
        pub unsafe extern "C" fn $func_name(
            ptr: *const $elem,
            len: usize,
            result: *mut c_void,
        ) -> bool {
            if ptr.is_null() || result.is_null() {
                return false;
            }
            let args = std::slice::from_raw_parts(ptr, len);
            match len {
                $($n => encode_and_store!($function, args, result, $n, $out),)+
                _ => false,
            }
        }
    };
}

macro_rules! encoders_u8 {
    ($func_name:ident, $function:path) => {
        generic_encode!($func_name, $function, u8, {
            1 => u8, 2 => u16, 3 => u32, 4 => u32,
            5 => u64, 6 => u64, 7 => u64, 8 => u64,
            9 => u128, 10 => u128, 11 => u128, 12 => u128,
            13 => u128, 14 => u128, 15 => u128, 16 => u128,
        });
    };
}

macro_rules! encoders_u16 {
    ($func_name:ident, $function:path) => {
        generic_encode!($func_name, $function, u16, {
            1 => u16, 2 => u32, 3 => u64, 4 => u64,
            5 => u128, 6 => u128, 7 => u128, 8 => u128,
        });
    };
}

macro_rules! encoders_u32 {
    ($func_name:ident, $function:path) => {
        generic_encode!($func_name, $function, u32, {
            1 => u32, 2 => u64, 3 => u128, 4 => u128,
        });
    };
}

macro_rules! encoders_u64 {
    ($func_name:ident, $function:path) => {
        generic_encode!($func_name, $function, u64, { 1 => u64, 2 => u128 });
    };
}

encoders_u8!(hilbert_encode_u8_var, lindel::hilbert_encode);
encoders_u8!(morton_encode_u8_var, lindel::morton_encode);
encoders_u16!(hilbert_encode_u16_var, lindel::hilbert_encode);
encoders_u16!(morton_encode_u16_var, lindel::morton_encode);
encoders_u32!(hilbert_encode_u32_var, lindel::hilbert_encode);
encoders_u32!(morton_encode_u32_var, lindel::morton_encode);
encoders_u64!(hilbert_encode_u64_var, lindel::hilbert_encode);
encoders_u64!(morton_encode_u64_var, lindel::morton_encode);

#[cfg(test)]
mod tests {
    use super::*;

    // Encode `input` with `encoder`, decode it with `perform_decode` and
    // check the coordinates survive the roundtrip.
    macro_rules! roundtrip {
        ($encoder:ident, $encoding:expr, $elem:ty, $key:ty, $bits:expr, $input:expr) => {{
            let input: Vec<$elem> = $input;
            let mut key: $key = 0;
            assert!(unsafe {
                $encoder(input.as_ptr(), input.len(), &mut key as *mut $key as *mut c_void)
            });
            let mut output: Vec<$elem> = vec![0; input.len()];
            assert!(unsafe {
                perform_decode(
                    $encoding,
                    $bits,
                    &key as *const $key as *const c_void,
                    output.as_mut_ptr() as *mut c_void,
                    output.len(),
                )
            });
            assert_eq!(input, output);
        }};
    }

    #[test]
    fn roundtrips_every_width() {
        for (encoding, h) in [(0u8, true), (1u8, false)] {
            if h {
                roundtrip!(hilbert_encode_u8_var, encoding, u8, u128, 8, (1..=16).collect());
                roundtrip!(hilbert_encode_u16_var, encoding, u16, u64, 16, vec![1, 65535, 7]);
                roundtrip!(hilbert_encode_u32_var, encoding, u32, u128, 32, vec![u32::MAX, 0, 9, 1]);
                roundtrip!(hilbert_encode_u64_var, encoding, u64, u128, 64, vec![u64::MAX, 3]);
            } else {
                roundtrip!(morton_encode_u8_var, encoding, u8, u128, 8, (1..=16).collect());
                roundtrip!(morton_encode_u16_var, encoding, u16, u64, 16, vec![1, 65535, 7]);
                roundtrip!(morton_encode_u32_var, encoding, u32, u128, 32, vec![u32::MAX, 0, 9, 1]);
                roundtrip!(morton_encode_u64_var, encoding, u64, u128, 64, vec![u64::MAX, 3]);
            }
        }
    }

    #[test]
    fn rejects_unsupported_arguments_without_panicking() {
        let input = [0u64; 3];
        let mut key = 0u128;
        let out = &mut key as *mut u128 as *mut c_void;
        assert!(!unsafe { hilbert_encode_u64_var(input.as_ptr(), 3, out) });
        assert!(!unsafe { morton_encode_u64_var(std::ptr::null(), 1, out) });

        let mut dest = [0u64; 3];
        let src = &key as *const u128 as *const c_void;
        let dest_ptr = dest.as_mut_ptr() as *mut c_void;
        assert!(!unsafe { perform_decode(0, 64, src, dest_ptr, 3) });
        assert!(!unsafe { perform_decode(2, 64, src, dest_ptr, 1) });
        assert!(!unsafe { perform_decode(0, 12, src, dest_ptr, 1) });
    }

    #[test]
    fn writes_keys_to_unaligned_destinations() {
        let input = [u64::MAX, 1];
        let mut buffer = [0u8; 24];
        let unaligned = unsafe { buffer.as_mut_ptr().add(1) } as *mut c_void;
        assert!(unsafe { hilbert_encode_u64_var(input.as_ptr(), 2, unaligned) });
        let mut output = [0u64; 2];
        assert!(unsafe {
            perform_decode(0, 64, unaligned as *const c_void, output.as_mut_ptr() as *mut c_void, 2)
        });
        assert_eq!(input, output);
    }
}
