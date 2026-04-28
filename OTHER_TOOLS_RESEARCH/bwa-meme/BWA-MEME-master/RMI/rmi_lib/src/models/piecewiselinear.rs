// < begin copyright > 
// Copyright Ryan Marcus 2020
// 
// See root directory of this project for license terms.
// 
// < end copyright > 
 

use crate::models::*;
use log::*;

pub struct PiecewiselinearModel {
    params: u64,
}

impl PiecewiselinearModel {
    pub fn new<T: TrainingKey>(data: &RMITrainingData<T>, params: u64) -> PiecewiselinearModel {
        return PiecewiselinearModel { params };
    }
}

impl Model for PiecewiselinearModel {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        let kmer = self.params;

        return (inp.as_int() >> (64-kmer)) as f64;
        // return (inp.as_int128() >> (128-kmer)) as f64; // 96 for >> in load, int128 to load uint128 key
    }

    fn input_type(&self) -> ModelDataType {
        return ModelDataType::Int;
    }
    fn output_type(&self) -> ModelDataType {
        return ModelDataType::Int;
    }

    fn params(&self) -> Vec<ModelParam> {
        return vec![self.params.into()];
    }

    fn code(&self) -> String {
        return format!(
            "
inline uint64_t pwl(uint64_t kmer, uint64_t inp) {{

    return inp >> (64-kmer);
}}",
        );
    
//         return String::from(
//             "
// inline uint64_t pwl(uint64_t inp) {

//     return inp >> (64-{});
// }",
//         );
    }

    fn function_name(&self) -> String {
        return String::from("pwl");
    }

    fn set_to_constant_model(&mut self, constant: u64) -> bool {
        return false;
    }
}


pub struct PiecewiselinearModel_partial {
    params: (u64, u64)
    
}

impl PiecewiselinearModel_partial {
    pub fn new<T: TrainingKey>(data: &RMITrainingData<T>, up_kmer: u64, curr_kmer: u64) -> PiecewiselinearModel_partial {
        let params = (up_kmer , curr_kmer );
        return PiecewiselinearModel_partial { params };
    }
}

impl Model for PiecewiselinearModel_partial {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        let (up_kmer,curr_kmer) = self.params;

        return ( (inp.as_int() << up_kmer ) >> (64-curr_kmer  + up_kmer)) as f64;
    }

    fn input_type(&self) -> ModelDataType {
        return ModelDataType::Int;
    }
    fn output_type(&self) -> ModelDataType {
        return ModelDataType::Int;
    }

    fn params(&self) -> Vec<ModelParam> {
        return vec![self.params.0.into(),self.params.1.into()];
    }

    fn code(&self) -> String {
        return format!(
            "
inline uint64_t pwl_partial(uint64_t up_kmer,uint64_t curr_kmer, uint64_t inp) {{

    return (inp<<up_kmer) >> (64+up_kmer-kmer);
}}",
        );
    
//         return String::from(
//             "
// inline uint64_t pwl(uint64_t inp) {

//     return inp >> (64-{});
// }",
//         );
    }

    fn function_name(&self) -> String {
        return String::from("pwl_partial");
    }

    fn set_to_constant_model(&mut self, constant: u64) -> bool {
        assert!(false);
        return false;
    }
}