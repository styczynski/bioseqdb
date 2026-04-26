// < begin copyright > 
// Copyright Ryan Marcus 2020
// 
// See root directory of this project for license terms.
// 
// < end copyright > 


mod balanced_radix;
mod cubic_spline;
mod histogram;
mod linear;
mod piecewiselinear;
mod linear_spline;
mod normal;
mod radix;
mod stdlib;
mod utils;

pub use balanced_radix::BalancedRadixModel;
pub use cubic_spline::CubicSplineModel;
pub use histogram::EquidepthHistogramModel;
pub use linear::LinearModel;
pub use linear::LinearModelBig;
pub use linear::RobustLinearModel;
pub use linear::LogLinearModel;
pub use linear_spline::LinearSplineModel;

pub use piecewiselinear::PiecewiselinearModel;
pub use piecewiselinear::PiecewiselinearModel_partial;

pub use normal::LogNormalModel;
pub use normal::NormalModel;
pub use radix::RadixModel;
pub use radix::RadixTable;
pub use stdlib::StdFunctions;

use std::cmp::Ordering;
use std::collections::HashSet;
use std::sync::Arc;
use std::io::Write;
use byteorder::{WriteBytesExt, LittleEndian};

use rug::{
    float::{self, FreeCache, Round},
    ops::{AddAssignRound, AssignRound, MulAssignRound},
    Float, Assign, Integer, integer::Order
};

use log::*;

use uint::*;

construct_uint! {
	pub struct U512(8);
}


#[derive(Clone, Copy)]
pub enum KeyType {
    U32, U64, F64, U128, U512, F512
}

impl KeyType {
    pub fn c_type(&self) -> &'static str {
        match self {
            KeyType::U32 => "uint32_t",
            KeyType::U64 => "uint64_t",
            KeyType::F64 => "double",
            KeyType::F512 => "cpp_bin_float_512",
            KeyType::U128 => "__uint128_t",
            KeyType::U512 => "uint512_t"
        }
    }

    pub fn to_model_data_type(self) -> ModelDataType {
        match self {
            KeyType::U32 => ModelDataType::Int,
            KeyType::U64 => ModelDataType::Int,
            KeyType::U128 => ModelDataType::Int128,
            KeyType::U512 => ModelDataType::Int512,
            KeyType::F64 => ModelDataType::Float,
            KeyType::F512 =>ModelDataType::Float512
        }
    }
}

// struct wrap_inegeter{
//     elem: Integer,
// }

// impl wrap_inegeter{
//     fn new() -> Self {
//         wrap_inegeter{
//             elem: Integer::from(0),
//         }
//     }
// }

// impl Copy for wrap_inegeter {}
// impl Clone for wrap_inegeter {
//     fn clone(&self) -> Self{
//         wrap_inegeter{
//             elem: Integer::from(*self)
//         }
//     }
// }

#[derive(Copy, Clone, Eq, PartialEq, Hash, Debug)]
pub struct uint512(pub [u64; 8]);


pub trait TrainingKey: PartialEq + Copy + Send + Sync + std::fmt::Debug + 'static {
    fn minus_epsilon(&self) -> Self;
    fn zero_value() -> Self;
    fn plus_epsilon(&self) -> Self;
    fn max_value() -> Self;

    fn as_float(&self) -> f64;
    fn as_float512(&self) -> Float;
    fn as_uint(&self) -> u64;
    fn as_uint128(&self) -> u128;
    
    fn to_model_input(&self) -> ModelInput;
}

impl TrainingKey for u64 {
    fn minus_epsilon(&self) -> Self { *self - 1 }
    fn zero_value() -> Self { 0 }
    fn plus_epsilon(&self) -> Self { *self + 1 }
    fn max_value() -> Self { std::u64::MAX }

    fn as_float(&self) -> f64 { *self as f64 }
    fn as_float512(&self) -> Float { Float::with_val(512,*self) }
    fn as_uint(&self) -> u64 { *self }
    fn as_uint128(&self) -> u128 { *self as u128 }
    
    fn to_model_input(&self) -> ModelInput { (*self).into() }
}

impl TrainingKey for u128 {
    fn minus_epsilon(&self) -> Self { *self - 1 }
    fn zero_value() -> Self { 0 }
    fn plus_epsilon(&self) -> Self { *self + 1 }
    fn max_value() -> Self { std::u128::MAX }

    fn as_float(&self) -> f64 { *self as f64 }
    fn as_float512(&self) -> Float { Float::with_val(512,*self) }
    fn as_uint(&self) -> u64 { *self as u64 }
    fn as_uint128(&self) -> u128 { *self  }
    
    fn to_model_input(&self) -> ModelInput { (*self).into() }
}

impl TrainingKey for u32 {
    fn minus_epsilon(&self) -> Self { *self - 1 }
    fn zero_value() -> Self { 0 }
    fn plus_epsilon(&self) -> Self { *self + 1 }
    fn max_value() -> Self { std::u32::MAX }

    fn as_float(&self) -> f64 { *self as f64 }
    fn as_float512(&self) -> Float { Float::with_val(512,*self) }
    fn as_uint(&self) -> u64 { *self as u64 }
    fn as_uint128(&self) -> u128 { *self as u128 }

    fn to_model_input(&self) -> ModelInput { (*self).into() }
}

impl TrainingKey for U512 {
    fn minus_epsilon(&self) -> Self { *self - 1 }
    fn zero_value() -> Self { U512::from(0) }
    fn plus_epsilon(&self) -> Self {  *self + 1 }
    fn max_value() -> Self {U512::MAX }

    fn as_float(&self) -> f64 { (*self).low_u64() as f64 }
    fn as_float512(&self) -> Float { 
        // let mut target = vec![0u8; 64];
        // let target: [u8;64] = (*self).into();
        let mut target= [0u8; 64];
        (*self).to_little_endian(&mut target);
        // target = (*self).into();
        let result = Float::with_val(512,Integer::from_digits(&target, Order::LsfLe));
        // println!("Input: {:?} Float: {:?} Restored {:?}",(*self), result, result.to_integer().unwrap().to_digits::<u64>(Order::MsfBe) );
        result
    }
    fn as_uint(&self) -> u64 { (*self).low_u64() }
    fn as_uint128(&self) -> u128 { assert!(false);(*self).low_u64() as u128 }

    fn to_model_input(&self) -> ModelInput { (*self).into() }
}

// impl TrainingKey for String {
//     fn minus_epsilon(&self) -> Self { (Integer::from_str_radix(*self,16) - 1).to_string_radix(16) }
//     fn zero_value() -> Self { "0" }
//     fn plus_epsilon(&self) -> Self { (Integer::from_str_radix(*self,16) + 1).to_string_radix(16) }
//     fn max_value() -> Self { "1"*512 }

//     fn as_float(&self) -> f64 { (Integer::from_str_radix(*self,16)).to_f64() }
//     fn as_float512(&self) -> Float { Float::with_val(512,Integer::from_str_radix(*self,16)) }
//     fn as_uint(&self) -> u64 { (Integer::from_str_radix(*self,16)).to_u64_wrapping() }
    
//     fn to_model_input(&self) -> ModelInput { Integer::from_str_radix(*self,16) }
// }

impl TrainingKey for f64 {
    fn minus_epsilon(&self) -> Self { *self - std::f64::EPSILON }
    fn zero_value() -> Self { 0.0 }
    fn plus_epsilon(&self) -> Self { *self + std::f64::EPSILON }
    fn max_value() -> Self { std::f64::MAX }

    fn as_float(&self) -> f64 { *self }
    fn as_float512(&self) -> Float { Float::with_val(512,*self) }
    fn as_uint(&self) -> u64 { *self as u64 }
    fn as_uint128(&self) -> u128 { *self as u128 }
    
    fn to_model_input(&self) -> ModelInput { (*self).into() }
}

pub trait RMITrainingDataIteratorProvider: Send + Sync {
    type InpType: TrainingKey;
    
    fn len(&self) -> usize;
    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_>;
    fn key_type(&self) -> KeyType;
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        return Some(self.cdf_iter().nth(idx).unwrap());
    }
   
}

impl <K: TrainingKey> RMITrainingDataIteratorProvider for Vec<(K, usize)> {
    type InpType = K;
    fn len(&self) -> usize {
        return Vec::len(&self);
    }

    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_> {
        return Box::new(self.iter()
                        .cloned()
                        .map(|(key, offset)| (key.into(), offset)));
    }

    fn key_type(&self) -> KeyType { return KeyType::U64; }
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        self.as_slice().get(idx).map(|(key, offset)| ((*key).into(), *offset))
    }
}


struct FixDupsIter<K, T: Iterator<Item=(K, usize)>> {
    iter: T,
    last_item: Option<(K, usize)>
}

impl <K, T: Iterator<Item=(K, usize)>> FixDupsIter<K, T> {
    fn new(iter: T) -> FixDupsIter<K, T> {
        return FixDupsIter { iter: iter, last_item: None };
    }
}

impl <K, T> Iterator for FixDupsIter<K, T> where
    T: Iterator<Item=(K, usize)>,
    K: TrainingKey {
    type Item = (K, usize);

    fn next(&mut self) -> Option<Self::Item> {
        match self.last_item {
            None => {
                match self.iter.next() {
                    None => { return None },
                    Some(itm) => {
                        self.last_item = Some(itm);
                        return Some(itm);
                    }
                }
            },
            Some(last) => {
                match self.iter.next() {
                    Some(nxt) => {
                        if nxt.0 == last.0 {
                            Some((nxt.0, last.1))
                        } else {
                            self.last_item = Some(nxt);
                            return Some(nxt);
                        }
                    }
                    None => { self.last_item.take() }
                }
            }
        }
    }
}

struct DedupIter<K, T: Iterator<Item=(K, usize)>> {
    iter: T,
    last_item: Option<(K, usize)>
}

impl <K, T: Iterator<Item=(K, usize)>> DedupIter<K, T> {
    fn new(iter: T) -> DedupIter<K, T> {
        return DedupIter { iter: iter, last_item: None };
    }
}

impl <K, T> Iterator for DedupIter<K, T> where
    T: Iterator<Item=(K, usize)>,
    K: TrainingKey {
    type Item = (K, usize);

    fn next(&mut self) -> Option<Self::Item> {
        match self.last_item {
            None => {
                match self.iter.next() {
                    None => { return None; }
                    Some(nxt) => {
                        self.last_item = Some(nxt);
                        return Some(nxt)
                    }
                }
            },
            Some(last) => {
                loop {
                    match self.iter.next() {
                        Some(nxt) => {
                            if nxt.0 == last.0 {
                                continue;
                            } else {
                                self.last_item = Some(nxt);
                                return Some(nxt);
                            }
                        }
                        None => { return None; }
                    }
                }
            }
        }
    }
}

pub struct RMITrainingData<T> {
    iterable: Arc<Box<dyn RMITrainingDataIteratorProvider<InpType=T>>>,
    scale: f64,
    offset: usize
}

macro_rules! map_scale {
    ($self: expr, $inp: expr) => {{
        let sf = ($self).scale;
        let of = ($self).offset;
        let use_sf = (sf - 1.0).abs() > std::f64::EPSILON;
        ($inp).map(move |(key, offset)| {
                if use_sf {
                    (key, ( (offset-of) as f64 * sf) as usize)
                } else {
                    (key, offset-of )
                }
            })
    }}
}

impl <T: TrainingKey> RMITrainingData<T> {
    pub fn new(iterable: Box<dyn RMITrainingDataIteratorProvider<InpType=T>>)
               -> RMITrainingData<T> {
        return RMITrainingData { iterable: Arc::new(iterable), scale: 1.0, offset: 0 };
    }

    pub fn empty() -> RMITrainingData<T> {
        return RMITrainingData::<T>::new(Box::new(vec![]));
    }

    pub fn len(&self) -> usize { return self.iterable.len(); }

    pub fn set_scale(&mut self, scale: f64) {
        self.scale = scale;
    }

    pub fn set_offset(&mut self, offset: usize) {
        self.offset = offset;
    }

    pub fn get(&self, idx: usize) -> (T, usize) {
        return map_scale!(self, self.iterable.get(idx)).unwrap();
    }

    pub fn get_key(&self, idx: usize) -> T {
        return map_scale!(self, self.iterable.get(idx)).unwrap().0
    }

    pub fn iter(&self) -> impl Iterator<Item = (T, usize)> + '_ {
        map_scale!(self, FixDupsIter::new(self.iterable.cdf_iter()))
    }

    pub fn iter_model_input(&self) -> impl Iterator<Item = (ModelInput, usize)> + '_ {
        return map_scale!(self, FixDupsIter::new(self.iterable.cdf_iter()))
            .map(|(k, o)| (k.to_model_input(), o));
    }


    pub fn iter_unique(&self) -> impl Iterator<Item = (T, usize)> + '_ {
        map_scale!(self, DedupIter::new(self.iterable.cdf_iter()))
    }


    // Code adapted from superslice,
    // https://docs.rs/superslice/1.0.0/src/superslice/lib.rs.html
    // which was copyright 2017 Alkis Evlogimenos under the Apache License.
    pub fn lower_bound_by<F>(&self, f: F) -> usize
    where F: Fn((T, usize)) -> Ordering {
        let mut size = self.len();
        if size == 0 { return 0; }
        
        let mut base = 0usize;
        while size > 1 {
            let half = size / 2;
            let mid = base + half;
            let cmp = f(self.get(mid));
            base = if cmp == Ordering::Less { mid } else { base };
            size -= half;
        }
        let cmp = f(self.get(base));
        base + (cmp == Ordering::Less) as usize
    }
    
    pub fn soft_copy(&self) -> RMITrainingData<T> {
        return RMITrainingData {
            scale: self.scale,
            offset: self.offset,
            iterable: Arc::clone(&self.iterable)
        };
    }
}

/*struct RMITrainingDataIteratorProviderWrapper {
    orig: Arc<Box<dyn RMITrainingDataIteratorProvider>>
}

impl RMITrainingDataIteratorProvider for RMITrainingDataIteratorProviderWrapper {

}*/


/*impl PartialEq for ModelInput {
    fn eq(&self, other: &Self) -> bool {
        match self {
            ModelInput::Int(x) => {
                match other {
                    ModelInput::Int(y) => x == y,
                    ModelInput::Float(_) => false
                }
            }

            ModelInput::Float(x) => {
                match other {
                    ModelInput::Int(_) => false,
                    ModelInput::Float(y) => x == y // exact equality is intentional
                }
            }
        }
    }
}

impl Eq for ModelInput { }

impl PartialOrd for ModelInput {
    fn partial_cmp(&self, other: &ModelInput) -> Option<Ordering> {
        match self {
            ModelInput::Int(x) => {
                match other {
                    ModelInput::Int(y) => x.partial_cmp(y),
                    ModelInput::Float(_) => None
                }
            }
            ModelInput::Float(x) => {
                match other {
                    ModelInput::Int(_) => None,
                    ModelInput::Float(y) => x.partial_cmp(y) 
                }
            }
        }
    }
}*/



#[derive(Clone, Copy, Debug)]
pub enum ModelInput {
    Int(u64),
    Int128(u128),
    Float(f64),
    UINT512(U512),
}

impl PartialEq for ModelInput {
    fn eq(&self, other: &Self) -> bool {
        match self {
            ModelInput::Int(x) => {
                match other {
                    ModelInput::Int(y) => x == y,
                    ModelInput::Int128(y) => false,
                    ModelInput::Float(_) => false,
                    ModelInput::UINT512(_) => false
                }
            }
            ModelInput::Int128(x) => {
                match other {
                    ModelInput::Int(y) => false,
                    ModelInput::Int128(y) => x == y,
                    ModelInput::Float(_) => false,
                    ModelInput::UINT512(_) => false
                }
            }
            ModelInput::Float(x) => {
                match other {
                    ModelInput::Int(_) => false,
                    ModelInput::Int128(y) => false,
                    ModelInput::Float(y) => x == y, // exact equality is intentional
                    ModelInput::UINT512(_) => false
                }
            }
            ModelInput::UINT512(x) => {
                match other {
                    ModelInput::Int(_) => false,
                    ModelInput::Int128(y) => false,
                    ModelInput::Float(_) => false, 
                    ModelInput::UINT512(y) => x == y // exact equality is intentional
                }
            }
        }
    }
}

impl Eq for ModelInput { }

impl PartialOrd for ModelInput {
    fn partial_cmp(&self, other: &ModelInput) -> Option<Ordering> {
        match self {
            ModelInput::Int(x) => {
                match other {
                    ModelInput::Int(y) => x.partial_cmp(y),
                    ModelInput::Int128(_) => None,
                    ModelInput::Float(_) => None,
                    ModelInput::UINT512(_) => None
                }
            }
            ModelInput::Int128(x) => {
                match other {
                    ModelInput::Int(_) => None,
                    ModelInput::Int128(y) => x.partial_cmp(y),
                    ModelInput::Float(_) => None,
                    ModelInput::UINT512(_) => None
                }
            }
            ModelInput::Float(x) => {
                match other {
                    ModelInput::Int(_) => None,
                    ModelInput::Int128(_) => None,
                    ModelInput::Float(y) => x.partial_cmp(y),
                    ModelInput::UINT512(_) => None
                }
            }
            ModelInput::UINT512(x) => {
                match other {
                    ModelInput::Int(_) => None,
                    ModelInput::Int128(_) => None,
                    ModelInput::Float(_) => None,
                    ModelInput::UINT512(y) => x.partial_cmp(y)
                }
            }
        }
    }
}



impl ModelInput {
    pub fn as_float(&self) -> f64 {
        return match self {
            ModelInput::Int(x) => *x as f64,
            ModelInput::Int128(x) => *x as f64,
            ModelInput::Float(x) => *x,
            ModelInput::UINT512(x) => (*x).low_u64() as f64,
        };
    }
    pub fn as_float512(&self) -> Float {
        return match self {
            ModelInput::Int(x) => Float::with_val(512,*x),
            ModelInput::Int128(x) => Float::with_val(512,*x),
            ModelInput::Float(x) => Float::with_val(512,*x),
            ModelInput::UINT512(x) => {
                let mut target = vec![0u8; 64];
                (*x).to_big_endian(&mut target);
                Float::with_val(512,Integer::from_digits( &target, Order::Msf))
            },
        };
    }
    pub fn as_int(&self) -> u64 {
        return match self {
            ModelInput::Int(x) => *x,
            ModelInput::Int128(x) => *x as u64,
            ModelInput::Float(x) => *x as u64,
            ModelInput::UINT512(x) => (*x).low_u64(),
        };
    }
    pub fn as_int128(&self) -> u128 {
        return match self {
            ModelInput::Int(x) => *x as u128,
            ModelInput::Int128(x) => *x as u128,
            ModelInput::Float(x) => *x as u128,
            ModelInput::UINT512(x) => (*x).low_u64() as u128,
        };
    }
    pub fn max_value(&self) -> ModelInput {
        return match self {
            ModelInput::Int(_) => std::u64::MAX.into(),
            ModelInput::Int128(_) => std::u128::MAX.into(),
            ModelInput::Float(_) => std::f64::MAX.into(),
            ModelInput::UINT512(_) => U512::MAX.into(),
        };
    }

    pub fn min_value(&self) -> ModelInput {
        return match self {
            ModelInput::Int(_) => 0.into(),
            ModelInput::Int128(_) => 0.into(),
            ModelInput::Float(_) => std::f64::MIN.into(),
            ModelInput::UINT512(_) => 0.into(),
        };
    }

    pub fn minus_epsilon(&self) -> ModelInput {
        return match self {
            ModelInput::Int(x) => if *x > 0 { (x - 1).into() } else { 0.into() }
            ModelInput::Int128(x) => if *x > 0 { (x - 1).into() } else { 0.into() }
            ModelInput::Float(x) => (x - std::f64::EPSILON).into(),
            ModelInput::UINT512(x) =>  if *x > U512::from(0) { (x - U512::from(1)).into() } else { U512::from(0).into() },
        };
    }

    pub fn plus_epsilon(&self) -> ModelInput {
        return match self {
            ModelInput::Int(x) => if *x < std::u64::MAX {
                (x + 1).into()
            } else {
                std::u64::MAX.into()
            }
            ModelInput::Int128(x) => if *x < std::u128::MAX {
                (x + 1).into()
            } else {
                std::u128::MAX.into()
            }
            ModelInput::Float(x) => (x + std::f64::EPSILON).into(),
            ModelInput::UINT512(x) =>  if *x < U512::MAX {
                (x + 1).into()
            } else {
                U512::MAX.into()
            }
        };
    }
}

impl From<u64> for ModelInput {
    fn from(i: u64) -> Self {
        ModelInput::Int(i)
    }
}

impl From<u128> for ModelInput {
    fn from(i: u128) -> Self {
        ModelInput::Int128(i)
    }
}

impl From<u32> for ModelInput {
    fn from(i: u32) -> Self {
        ModelInput::Int(i as u64)
    }
}

impl From<i32> for ModelInput {
    fn from(i: i32) -> Self {
        assert!(i >= 0);
        ModelInput::Int(i as u64)
    }
}


impl From<f64> for ModelInput {
    fn from(f: f64) -> Self {
        ModelInput::Float(f)
    }
}

impl From<U512> for ModelInput {
    fn from(f: U512) -> Self {
        ModelInput::UINT512(f)
    }
}

pub enum ModelDataType {
    Int,
    Int128,
    Int512,
    Float,
    Float512
    
}

impl ModelDataType {
    pub fn c_type(&self) -> &'static str {
        match self {
            ModelDataType::Int => "uint64_t",
            ModelDataType::Int128 => "__uint128_t",
            ModelDataType::Int512 => "uint512_t",
            ModelDataType::Float => "double",
            ModelDataType::Float512 => "cpp_bin_float_512",
        }
    }
}

#[derive(Debug, Clone)]
pub enum ModelParam {
    Int(u64),
    Int128(u128),
    Float(f64),
    Float512(Float),
    Int512(Integer),
    ShortArray(Vec<u16>),
    IntArray(Vec<u64>),
    Int128Array(Vec<u128>),
    Int32Array(Vec<u32>),
    Int512Array(Vec<Integer>),
    FloatArray(Vec<f64>),
}

impl ModelParam {
    // size in bytes
    pub fn size(&self) -> usize {
        match self {
            ModelParam::Int(_) => 8,
            ModelParam::Int128(_) => 16,
            ModelParam::Float(_) => 8,
            ModelParam::Float512(_) => 66,
            ModelParam::Int512(_) => 64,
            ModelParam::ShortArray(a) => 2 * a.len(),
            ModelParam::IntArray(a) => 8 * a.len(),
            ModelParam::Int128Array(a) => 16 * a.len(),
            ModelParam::Int32Array(a) => 4 * a.len(),
            ModelParam::Int512Array(a) => 64 * a.len(),
            ModelParam::FloatArray(a) => 8 * a.len(),
        }
    }

    pub fn c_type(&self) -> &'static str {
        match self {
            ModelParam::Int(_) => "uint64_t",
            ModelParam::Int128(_) => "__uint128_t",
            ModelParam::Float(_) => "double",
            ModelParam::Float512(_) => "cpp_bin_float_512",
            ModelParam::Int512(_) => "uint512_t",
            ModelParam::ShortArray(_) => "short",
            ModelParam::IntArray(_) => "uint64_t",
            ModelParam::Int128Array(_) => "__uint128_t",
            ModelParam::Int32Array(_) => "uint32_t",
            ModelParam::Int512Array(_) => "uint512_t",
            ModelParam::FloatArray(_) => "double",
        }
    }

    pub fn is_array(&self) -> bool {
        match self {
            ModelParam::Int(_) => false,
            ModelParam::Int128(_) => false,
            ModelParam::Float(_) => false,
            ModelParam::Float512(_) => false,
            ModelParam::Int512(_) => false,
            ModelParam::ShortArray(_) => true,
            ModelParam::IntArray(_) => true,
            ModelParam::Int128Array(_) => true,
            ModelParam::Int32Array(_) => true,
            ModelParam::Int512Array(_) => true,
            ModelParam::FloatArray(_) => true
        }
    }

    pub fn c_type_mod(&self) -> &'static str {
        match self {
            ModelParam::Int(_) => "",
            ModelParam::Int128(_) => "",
            ModelParam::Float(_) => "",
            ModelParam::Float512(_) => "",
            ModelParam::Int512(_) => "",
            ModelParam::ShortArray(_) => "[]",
            ModelParam::IntArray(_) => "[]",
            ModelParam::Int128Array(_) => "[]",
            ModelParam::Int32Array(_) => "[]",
            ModelParam::Int512Array(_) => "[]",
            ModelParam::FloatArray(_) => "[]",
        }
    }

    pub fn c_val(&self) -> String {
        match self {
            ModelParam::Int(v) => format!("{}UL", v),
            ModelParam::Int128(v) => format!("{}", v),
            ModelParam::Float(v) => {
                let mut tmp = format!("{:.}", v);
                if !tmp.contains('.') {
                    tmp.push_str(".0");
                }
                return tmp;
            },
            ModelParam::Float512(v) => {
                let mut tmp = format!("{:.}", v);
                if !tmp.contains('.') {
                    tmp.push_str(".0");
                }
                return tmp;
            },
            ModelParam::Int512(v) => format!("{}UL", v),
            ModelParam::ShortArray(arr) => {
                let itms: Vec<String> = arr.iter().map(|i| format!("{}", i)).collect();
                return format!("{{ {} }}", itms.join(", "));
            },
            ModelParam::IntArray(arr) => {
                let itms: Vec<String> = arr.iter().map(|i| format!("{}UL", i)).collect();
                return format!("{{ {} }}", itms.join(", "));
            },
            ModelParam::Int128Array(arr) => {
                let itms: Vec<String> = arr.iter().map(|i| format!("{}", i)).collect();
                return format!("{{ {} }}", itms.join(", "));
            },
            ModelParam::Int32Array(arr) => {
                let itms: Vec<String> = arr.iter().map(|i| format!("{}UL", i)).collect();
                return format!("{{ {} }}", itms.join(", "));
            },
            ModelParam::Int512Array(arr) => {
                let itms: Vec<String> = arr.iter().map(|i| format!("{}UL", i)).collect();
                return format!("{{ {} }}", itms.join(", "));
            },
            ModelParam::FloatArray(arr) => {
                let itms: Vec<String> = arr
                    .iter()
                    .map(|i| format!("{:.}", i))
                    .map(|s| if !s.contains('.') { s + ".0" } else { s })
                    .collect();
                return format!("{{ {} }}", itms.join(", "));
            }
        }
    }

    /* useful for debugging floating point issues
    pub fn as_bits(&self) -> u64 {
        return match self {
            ModelParam::Int(v) => *v,
            ModelParam::Float(v) => v.to_bits(),
            ModelParam::ShortArray(_) => panic!("Cannot treat a short array parameter as a float"),
            ModelParam::IntArray(_) => panic!("Cannot treat an int array parameter as a float"),
            ModelParam::FloatArray(_) => panic!("Cannot treat an float array parameter as a float"),
        };
    }*/

    pub fn is_same_type(&self, other: &ModelParam) -> bool {
        return std::mem::discriminant(self) == std::mem::discriminant(other);
    }

    pub fn write_to<T: Write>(&self, target: &mut T) -> Result<(), std::io::Error> {
        match self {
            ModelParam::Int(v) => target.write_u64::<LittleEndian>(*v),
            ModelParam::Int128(v) => target.write_u128::<LittleEndian>(*v),
            ModelParam::Float(v) => target.write_f64::<LittleEndian>(*v),
            ModelParam::Float512(v) => {
                let hexstring = v.to_string_radix(2,Some(512));
                let  tmp_string = format!("{:0>512}", hexstring);
                // println!("Write String:\n{} \n{:?}", hexstring, v);
                let  _exponent= 0;
                let mut str = String::from("");
                let mut write_exp = 0;
                let mut count_write = 0;
                let mut dot_pos:i16 = 0;
                let mut is_minus:u16 = 0;
                // str.push('!');
                for (_i,a) in tmp_string.chars().enumerate() {
                    
                    if a == '.' {
                        //write dot pos for case when there is no e for exponenet value
                        dot_pos = (_i-1 ) as i16;
                        dot_pos = dot_pos - is_minus as i16;
                        continue;
                    }
                    if a == '@' {
                        //@ is exponent value
                        continue;
                    }
                    if write_exp ==1 {
                        
                        let z = i16::from_str_radix(&tmp_string[_i..], 10).unwrap();
                        // println!("exponent:{}, from_str_radix{}", &tmp_string[_i..] , z  );
                        let _r1 = target.write_i16::<LittleEndian>(z);
                        break;
                    }
                    if a == '-' && write_exp == 0 {
                        //in case number is minus
                        is_minus = 1;
                        continue;
                    }
                    if a == 'e' {
                        //@ is exponent value
                        write_exp =1 ;
                        continue;
                    }
                    
                    str.push(a);
                    if str.len() == 16 {
                        count_write+=1;
                        let z = u16::from_str_radix(&str, 2).unwrap();
                        // println!("str:{} z:{} z_b:{:0>16b}",str,z,z);
                        let mut _r1 = target.write_u16::<LittleEndian>(z);
                        str = String::from("");
                    }
                }
                if write_exp == 0{
                    // let z = 0;
                    // let _r1 = target.write_i16::<LittleEndian>(z);
                    let _r1 = target.write_i16::<LittleEndian>(dot_pos);
                }

                let _r1 = target.write_u16::<LittleEndian>(is_minus);

                assert!(count_write == 32);

                Ok(())
            },
            //ModelParam::Int512(v) => target.write_u64::<LittleEndian>(*v),
            //ModelParam::Int512(v) => target.write_all::<LittleEndian>(*v.to_digits::<u32>(Order::MsfLe)),
            ModelParam::Int512(v) => {
                let mut TmpString = format!("{:0>32}", v.to_string_radix(16));
                assert_eq!(TmpString.len(),32);
                for a in TmpString.chars() {
                    let z = u16::from_str_radix(&a.to_string(), 16).unwrap();
                    target.write_u16::<LittleEndian>(z)?;
                }

                Ok(())
            },
            ModelParam::ShortArray(arr) => {
                for v in arr {
                    target.write_u16::<LittleEndian>(*v)?;
                }

                Ok(())
            },
            
            ModelParam::IntArray(arr) => {
                for v in arr {
                    target.write_u64::<LittleEndian>(*v)?;
                }

                Ok(())
            },
            
            ModelParam::Int128Array(arr) => {
                for v in arr {
                    target.write_u128::<LittleEndian>(*v)?;
                }

                Ok(())
            },

            ModelParam::Int32Array(arr) => {
                for v in arr {
                    target.write_u32::<LittleEndian>(*v)?;
                }

                Ok(())
            },
            ModelParam::Int512Array(arr) => {
                for v in arr {
                    let mut TmpString = format!("{:0>32}", v.to_string_radix(16));
                    assert_eq!(TmpString.len(),32);
                    for a in TmpString.chars() {
                        let z = u16::from_str_radix(&a.to_string(), 16).unwrap();
                        target.write_u16::<LittleEndian>(z)?;
                    }
                }

                Ok(())
            },
            ModelParam::FloatArray(arr) => {
                for v in arr {
                    target.write_f64::<LittleEndian>(*v)?;
                }

                Ok(())

            }

        }
    }
    
    pub fn as_float(&self) -> f64 {
        match self {
            ModelParam::Int(v) => *v as f64,
            ModelParam::Int128(v) => *v as f64,
            ModelParam::Float(v) => *v,
            ModelParam::Float512(v) => v.to_f64(),
            ModelParam::Int512(_) => panic!("Cannot treat a rug::Integer parameter as a float"),
            ModelParam::ShortArray(_) => panic!("Cannot treat a short array parameter as a float"),
            ModelParam::IntArray(_) => panic!("Cannot treat an int array parameter as a float"),
            ModelParam::Int128Array(_) => panic!("Cannot treat an int128 array parameter as a float"),
            ModelParam::Int32Array(_) => panic!("Cannot treat an int32 array parameter as a float"),
            ModelParam::Int512Array(_) => panic!("Cannot treat an int512 array parameter as a float"),
            ModelParam::FloatArray(_) => panic!("Cannot treat an float array parameter as a float"),
        }
    }

    pub fn len(&self) -> usize {
        match self {
            ModelParam::Int(_) => 1,
            ModelParam::Int128(_) => 1,
            ModelParam::Float(_) => 1,
            ModelParam::Float512(_) => 1,
            ModelParam::Int512(_) => 1,
            ModelParam::ShortArray(p) => p.len(),
            ModelParam::IntArray(p) => p.len(),
            ModelParam::Int128Array(p) => p.len(),
            ModelParam::Int32Array(p) => p.len(),
            ModelParam::Int512Array(p) => p.len(),
            ModelParam::FloatArray(p) => p.len()
        }
    }
}

impl From<usize> for ModelParam {
    fn from(i: usize) -> Self {
        ModelParam::Int(i as u64)
    }
}

impl From<u64> for ModelParam {
    fn from(i: u64) -> Self {
        ModelParam::Int(i)
    }
}
impl From<u128> for ModelParam {
    fn from(i: u128) -> Self {
        ModelParam::Int128(i)
    }
}

impl From<Integer> for ModelParam {
    fn from(i: Integer) -> Self {
        ModelParam::Int512(i)
    }
}
impl From<u8> for ModelParam {
    fn from(i: u8) -> Self {
        ModelParam::Int(u64::from(i))
    }
}

impl From<f64> for ModelParam {
    fn from(f: f64) -> Self {
        ModelParam::Float(f)
    }
}

impl From<Float> for ModelParam {
    fn from(f: Float) -> Self {
        ModelParam::Float512(f)
    }
}


impl From<Vec<u16>> for ModelParam {
    fn from(f: Vec<u16>) -> Self {
        ModelParam::ShortArray(f)
    }
}

impl From<Vec<u64>> for ModelParam {
    fn from(f: Vec<u64>) -> Self {
        ModelParam::IntArray(f)
    }
}

impl From<Vec<u128>> for ModelParam {
    fn from(f: Vec<u128>) -> Self {
        ModelParam::Int128Array(f)
    }
}

impl From<Vec<u32>> for ModelParam {
    fn from(f: Vec<u32>) -> Self {
        ModelParam::Int32Array(f)
    }
}

impl From<Vec<Integer>> for ModelParam {
    fn from(f: Vec<Integer>) -> Self {
        ModelParam::Int512Array(f)
    }
}

impl From<Vec<f64>> for ModelParam {
    fn from(f: Vec<f64>) -> Self {
        ModelParam::FloatArray(f)
    }
}

pub enum ModelRestriction {
    None,
    MustBeTop,
    MustBeBottom,
}

pub trait Model: Sync + Send {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        return self.predict_to_int(inp) as f64;
    }

    fn predict_to_int(&self, inp: &ModelInput) -> u64 {
        return f64::max(0.0, self.predict_to_float(inp).floor()) as u64;
    }

    fn input_type(&self) -> ModelDataType;
    fn output_type(&self) -> ModelDataType;

    fn params(&self) -> Vec<ModelParam>;

    fn code(&self) -> String;
    fn function_name(&self) -> String;

    fn standard_functions(&self) -> HashSet<StdFunctions> {
        return HashSet::new();
    }

    fn needs_bounds_check(&self) -> bool {
        return true;
    }
    fn restriction(&self) -> ModelRestriction {
        return ModelRestriction::None;
    }
    fn error_bound(&self) -> Option<u64> {
        return None;
    }

    fn set_to_constant_model(&mut self, _constant: u64) -> bool {
        return false;
    }
}

pub trait ModelBig: Sync + Send {
    fn predict_to_float(&self, inp: &ModelInput) -> Float {
        return Float::with_val(512, self.predict_to_int(inp) );
    }

    fn predict_to_int(&self, inp: &ModelInput) -> u64 {
        return f64::max(0.0, self.predict_to_float(inp).to_f64_round(Round::Down)) as u64;
    }

    fn input_type(&self) -> ModelDataType;
    fn output_type(&self) -> ModelDataType;

    fn params(&self) -> Vec<ModelParam>;

    fn code(&self) -> String;
    fn function_name(&self) -> String;

    fn standard_functions(&self) -> HashSet<StdFunctions> {
        return HashSet::new();
    }

    fn needs_bounds_check(&self) -> bool {
        return true;
    }
    fn restriction(&self) -> ModelRestriction {
        return ModelRestriction::None;
    }
    fn error_bound(&self) -> Option<u64> {
        return None;
    }

    fn set_to_constant_model(&mut self, _constant: u64) -> bool {
        return false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_scale() {
        let mut v = ModelData::IntKeyToIntPos(vec![(0, 0), (1, 1), (3, 2), (100, 3)]);

        v.scale_targets_to(50, 4);

        let results = v.as_int_int();
        assert_eq!(results[0].1, 0);
        assert_eq!(results[1].1, 12);
        assert_eq!(results[2].1, 25);
        assert_eq!(results[3].1, 37);
    }

    #[test]
    fn test_iter() {
        let data = vec![(0, 1), (1, 2), (3, 3), (100, 4)];

        let v = ModelData::IntKeyToIntPos(data.clone());

        let iterated: Vec<(u64, u64)> = v.iter_uint_uint().collect();
        assert_eq!(data, iterated);
    }
}
