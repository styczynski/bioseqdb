// < begin copyright > 
// Copyright Ryan Marcus 2020
// 
// See root directory of this project for license terms.
// 
// < end copyright > 
 
 
use memmap::MmapOptions;
use rmi_lib::{RMITrainingData, RMITrainingDataIteratorProvider, KeyType, U512};
use byteorder::{LittleEndian, ReadBytesExt};
use std::fs::File;
use std::convert::TryInto;

use rug::{
    Integer
};



pub enum DataType {
    UINT64,
    UINT128,
    UINT32,
    UINT512,
    FLOAT64
}

struct SliceAdapterU64 {
    data: memmap::Mmap,
    length: usize
}

impl RMITrainingDataIteratorProvider for SliceAdapterU64 {
    type InpType = u64;
    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_> {
        Box::new((0..self.length).map(move |i| self.get(i).unwrap()))
    }
    
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        if idx >= self.length { return None; };
        let mi = (u64::from_le_bytes((&self.data[8 + idx * 8..8 + (idx + 1) * 8])
                                    .try_into().unwrap()) >> 0) << 0;
        return Some((mi.into(), idx));
    }
    
    fn key_type(&self) -> KeyType {
        KeyType::U64
    }
    
    fn len(&self) -> usize { self.length }
}


struct SliceAdapterU32 {
    data: memmap::Mmap,
    length: usize
}

impl RMITrainingDataIteratorProvider for SliceAdapterU32 {
    type InpType = u32;
    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_> {
        Box::new((0..self.length).map(move |i| self.get(i).unwrap()))
    }
    
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        if idx >= self.length { return None; };
        let mi = (&self.data[8 + idx * 4..8 + (idx + 1) * 4])
            .read_u32::<LittleEndian>().unwrap().into();
        return Some((mi, idx));
    }
    
    fn key_type(&self) -> KeyType {
        KeyType::U32
    }
    
    fn len(&self) -> usize { self.length }
}

struct SliceAdapterU512 {
    data: memmap::Mmap,
    length: usize
}

impl RMITrainingDataIteratorProvider for SliceAdapterU512 {
    type InpType = U512;
    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_> {
        Box::new((0..self.length).map(move |i| self.get(i).unwrap()))
    }
     
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        if idx >= self.length { return None; };
        // let source: [u8; 64]  = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
       
        // let source = self.data.get(8 + idx * 64..8 + (idx + 1) * 64).unwrap();
         // self.data.get(8 + idx * 64..8 + (idx + 1) * 64).
        let mut source: [u64;8] = [0,0,0,0,0,0,0,0];
        for i_ in 0..8 {
            source[i_] = (&self.data[8 + idx * 64 + i_*8..8 + idx * 64+ (i_+1)*8])
            .read_u64::<LittleEndian>().unwrap().into();
        }

        // if idx <1 || idx== self.length-1 {
        //     println!("index:{} Bits:{:?} Deci:{:?}", idx, source, U512(source));
        // }
        return Some((U512(source), idx));
        // let result: U512 = source.into();
        // return Some((result, idx));
    }
  
    
    fn key_type(&self) -> KeyType {
        KeyType::U512
    }
    
    fn len(&self) -> usize { self.length }
}

struct SliceAdapterF64 {
    data: memmap::Mmap,
    length: usize
}

impl RMITrainingDataIteratorProvider for SliceAdapterF64 {
    type InpType = f64;
    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_> {
        Box::new((0..self.length).map(move |i| self.get(i).unwrap()))
    }
    
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        if idx >= self.length { return None; };
        let mi = (&self.data[8 + idx * 8..8 + (idx + 1) * 8])
            .read_f64::<LittleEndian>().unwrap().into();
        return Some((mi, idx));
    }
    
    fn key_type(&self) -> KeyType {
        KeyType::F64
    }
    
    fn len(&self) -> usize { self.length }
}

struct SliceAdapterU128 {
    data: memmap::Mmap,
    length: usize
}

impl RMITrainingDataIteratorProvider for SliceAdapterU128 {
    type InpType = u128;
    fn cdf_iter(&self) -> Box<dyn Iterator<Item = (Self::InpType, usize)> + '_> {
        Box::new((0..self.length).map(move |i| self.get(i).unwrap()))
    }
    
    fn get(&self, idx: usize) -> Option<(Self::InpType, usize)> {
        if idx >= self.length { return None; };
        let mi = (u128::from_le_bytes((&self.data[8 + idx * 16..8 + (idx + 1) * 16])
                                    .try_into().unwrap()) >> 0) << 0;
        // println!("index:{} Deci:{:?}", idx, mi);
        return Some((mi.into(), idx));
    }
    
    fn key_type(&self) -> KeyType {
        KeyType::U128
    }
    
    fn len(&self) -> usize { self.length }
}

pub enum RMIMMap {
    UINT64(RMITrainingData<u64>),
    UINT32(RMITrainingData<u32>),
    UINT512(RMITrainingData<U512>),
    UINT128(RMITrainingData<u128>),
    // UINT512(RMITrainingData<Integer>),
    FLOAT64(RMITrainingData<f64>)
}

macro_rules! dynamic {
    ($funcname: expr, $data: expr $(, $p: expr )*) => {
        match $data {
            load::RMIMMap::UINT64(mut x) => $funcname(&mut x, $($p),*),
            load::RMIMMap::UINT32(mut x) => $funcname(&mut x, $($p),*),
            load::RMIMMap::UINT128(mut x) => $funcname(&mut x, $($p),*),
            load::RMIMMap::UINT512(mut x) => $funcname(&mut x, $($p),*),
            load::RMIMMap::FLOAT64(mut x) => $funcname(&mut x, $($p),*),
        }
    }
}


impl RMIMMap {
    pub fn soft_copy(&self) -> RMIMMap {
        match self {
            RMIMMap::UINT64(x) => RMIMMap::UINT64(x.soft_copy()),
            RMIMMap::UINT32(x) => RMIMMap::UINT32(x.soft_copy()),
            RMIMMap::UINT128(x) => RMIMMap::UINT128(x.soft_copy()),
            RMIMMap::UINT512(x) => RMIMMap::UINT512(x.soft_copy()),
            RMIMMap::FLOAT64(x) => RMIMMap::FLOAT64(x.soft_copy()),
        }
    }

    pub fn into_u64(self) -> Option<RMITrainingData<u64>> {
        match self {
            RMIMMap::UINT64(x) => Some(x),
            _ => None
        }
    }
}
                

pub fn load_data(filepath: &str,
                 dt: DataType) -> (usize, RMIMMap) {
    let fd = File::open(filepath).unwrap_or_else(|_| {
        panic!("Unable to open data file at {}", filepath)
    });

    let mmap = unsafe { MmapOptions::new().map(&fd).unwrap() };
    let num_items = (&mmap[0..8]).read_u64::<LittleEndian>().unwrap() as usize;

    let rtd = match dt {
        DataType::UINT64 =>
            RMIMMap::UINT64(RMITrainingData::new(Box::new(
                SliceAdapterU64 { data: mmap, length: num_items }
            ))),
        DataType::UINT32 =>
            RMIMMap::UINT32(RMITrainingData::new(Box::new(
                SliceAdapterU32 { data: mmap, length: num_items }
            ))),
        DataType::UINT128 =>
            RMIMMap::UINT128(RMITrainingData::new(Box::new(
                SliceAdapterU128 { data: mmap, length: num_items }
            ))),
        DataType::UINT512 =>
            RMIMMap::UINT512(RMITrainingData::new(Box::new(
                SliceAdapterU512 { data: mmap, length: num_items }
            ))),
        DataType::FLOAT64 =>
            RMIMMap::FLOAT64(RMITrainingData::new(Box::new(
                SliceAdapterF64 { data: mmap, length: num_items }
            )))
    };

    return (num_items, rtd);
}
