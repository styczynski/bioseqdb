// < begin copyright > 
// Copyright Ryan Marcus 2020
// 
// See root directory of this project for license terms.
// 
// < end copyright > 
 

use crate::models::*;
use log::*;
use rug::{
    float::{self, FreeCache, Round},
    ops::{AddAssignRound, AssignRound, MulAssignRound, DivAssignRound},
    Float, Assign, Integer, integer::Order
};

fn slr<T: Iterator<Item = (f64, f64)>>(loc_data: T) -> (f64, f64) {

    // compute the covariance of x and y as well as the variance of x in
    // a single pass.

    let mut mean_x = 0.0;
    let mut mean_y = 0.0;
    let mut c = 0.0;
    let mut n: u64 = 0;
    let mut m2 = 0.0;

    let mut data_size = 0;
    for (x, y) in loc_data {    
        // println!("model X={} Y={}", x, y);
        n += 1;
        let dx = x - mean_x;
        //info!("dx={}", dx);
        mean_x += dx / (n as f64);
        //info!("{} mean_x={}",n, mean_x );
        mean_y += (y - mean_y) / (n as f64);
        c += dx * (y - mean_y);

        let dx2 = x - mean_x;
        m2 += dx * dx2;
        // if n == 1000000 {
        //     info!("{} mean_x={}",n, mean_x );
        //     info!("{} dx2={}",n, dx2 );
        //     info!("{} m2={}",n, m2 );
        // }
        data_size += 1;
    }
    // info!("{} c={} m2={}",n, c , m2);
    // special case when we have 0 or 1 items
    if data_size == 0 {
        return (0.0, 0.0);
    }

    if data_size == 1 {
        return (mean_y, 0.0);
    }


    let cov = c / ((n - 1) as f64);
    let var = m2 / ((n - 1) as f64);
    assert!(var >= 0.0, "variance of model with {} data items was negative", n);

    if var == 0.0 {
        // variance is zero. pick the mean (only) value.
        return (mean_y, 0.0);
    }

    let beta: f64 = cov / var;
    let alpha = mean_y - beta * mean_x;
    info!("model a={} b={}",alpha, beta);
    return (alpha, beta);
}

fn loglinear_slr<T: TrainingKey>(data: &RMITrainingData<T>) -> (f64, f64) {
    // log all of the outputs, omit any item that doesn't have a valid log
    let transformed_data: Vec<(f64, f64)> = data
        .iter()
        .map(|(x, y)| (x.as_float(), (y as f64).ln()))
        .filter(|(_, y)| y.is_finite())
        .collect();

    // TODO this currently creates a copy of the data and then calls
    // slr... we can probably do better by moving the log into the slr.
    return slr(transformed_data.into_iter());
}

pub struct LinearModel {
    params: (f64, f64),
}

impl LinearModel {
    pub fn new<T: TrainingKey>(data: &RMITrainingData<T>) -> LinearModel {
        let params = slr(data.iter()
                         .map(|(inp, offset)| (inp.as_float(), offset as f64)));
        return LinearModel { params };
    }
}

impl Model for LinearModel {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        let (intercept, slope) = self.params;
        return slope.mul_add(inp.as_float(), intercept);
    }

    fn input_type(&self) -> ModelDataType {
        return ModelDataType::Float;
    }
    fn output_type(&self) -> ModelDataType {
        return ModelDataType::Float;
    }

    fn params(&self) -> Vec<ModelParam> {
        return vec![self.params.0.into(), self.params.1.into()];
    }

    fn code(&self) -> String {
        return String::from(
            "
inline double linear(double alpha, double beta, double inp) {
    return std::fma(beta, inp, alpha);
}",
        );
    }

    fn function_name(&self) -> String {
        return String::from("linear");
    }

    fn set_to_constant_model(&mut self, constant: u64) -> bool {
        self.params = (constant as f64, 0.0);
        return true;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_linear1() {
        let md = ModelData::IntKeyToIntPos(vec![(1, 2), (2, 3), (3, 4)]);

        let lin_mod = LinearModel::new(&md);

        assert_eq!(lin_mod.predict_to_int(1.into()), 2);
        assert_eq!(lin_mod.predict_to_int(6.into()), 7);
    }

    #[test]
    fn test_linear_single() {
        let md = ModelData::IntKeyToIntPos(vec![(1, 2)]);

        let lin_mod = LinearModel::new(&md);

        assert_eq!(lin_mod.predict_to_int(1.into()), 2);
    }

    #[test]
    fn test_empty() {
        LinearModel::new(&ModelData::empty());
    }

}




fn slr_big<T: Iterator<Item = (Float, Float)>>(loc_data: T) -> (Float, Float) {

    // compute the covariance of x and y as well as the variance of x in
    // a single pass.

    let mut mean_x = Float::with_val(512,0.0);
    let mut mean_y = Float::with_val(512,0.0);
    let mut c = Float::with_val(512,0.0);
    let mut n: u64 = 0;
    let mut m2 = Float::with_val(512,0.0);

    let mut data_size: u64 = 0;
    for (x, y) in loc_data {
        n += 1;
        //let dx = x - mean_x;
        //info!("model X={} Y={}", x.to_string_radix(10, None), y.to_string_radix(10, None));
        let dx = Float::with_val(512, &x - &mean_x);
        //info!("dx={}", dx.to_string_radix(10, None));
        let tmp_n = Float::with_val(512,n);
        let mut recip_tmp = Float::new(512);
        recip_tmp.assign_round(tmp_n.recip_ref(), Round::Nearest);
        //mean_x += dx / (n as f64);
        let div = Float::with_val(512,&dx * &recip_tmp );
        

        mean_x.add_assign_round( div, Round::Nearest);
        // info!("{} mean_x={}",n, mean_x.to_string_radix(10, None));
        //mean_y += (y - mean_y) / (n as f64);
        let minus = Float::with_val(512, &y - &mean_y );
        // let mut recipTmp = Float::new(512);
        let div = Float::with_val(512, &minus * &recip_tmp);
        mean_y.add_assign_round(div, Round::Nearest);

        //c += dx * (y - mean_y);
        let minus = Float::with_val(512, &y - &mean_y );
        c.add_assign_round(&dx * &minus, Round::Nearest);

        //let dx2 = x - mean_x;
        let dx2 = Float::with_val(512,&x - &mean_x);
        
        //m2 += dx * dx2;
        m2.add_assign_round(&dx * &dx2, Round::Nearest);
        // if n == 1000000 {
        //     info!("{} mean_x={}",n, mean_x.to_string_radix(10, None));
        //     info!("{} dx2={}",n, dx2.to_string_radix(10, None));
        //     info!("{} m2={}",n, m2.to_string_radix(10, None));
        // }
        data_size += 1;
    }
    // info!("{} c={} m2={}",n, c.to_string_radix(10, None), m2.to_string_radix(10, None));
    // special case when we have 0 or 1 items
    if data_size == 0 {
        info!("model a={} b={}", 0, 0);
        return (Float::with_val(512,0), Float::with_val(512,0));
    }

    if data_size == 1 {
        info!("model a={} b={}",mean_y.to_string_radix(10, None), 0);
        return (mean_y, Float::with_val(512,0));
    }

    let tmp_n = Float::with_val(512,n-1);
    //let cov = c / ((n - 1) as f64);
    //let minus =  Float::with_val(512, &tmpN - 1);
    let mut recip_tmp = Float::new(512);
    recip_tmp.assign_round(tmp_n.recip_ref(), Round::Nearest);
    let cov = Float::with_val(512, &c * &recip_tmp);
    //let var = m2 / ((n - 1) as f64);
    // let mut recipTmp = Float::new(512);
    // recipTmp.assign_round(tmpN.recip_ref(), Round::Nearest);
    let var = Float::with_val(512, &m2  * &recip_tmp);
    assert!(var >= 0.0, "variance of model with {} data items was negative", n);

    if var == 0.0 {
        // variance is zero. pick the mean (only) value.
        info!("\nmodel a={} \nb={}",mean_y.to_string_radix(10, None), 0);
        return (mean_y, Float::with_val(512,0));
    }
    let mut recip_tmp = Float::new(512);
    recip_tmp.assign_round(var.recip_ref(), Round::Nearest);
    let beta: Float = Float::with_val(512, &cov * &recip_tmp);
    let alpha =Float::with_val(512, &mean_y - &beta * &mean_x);
    info!("\nmodel a={} \nb={}",alpha.to_string_radix(10, None), beta.to_string_radix(10, None));
    return (alpha, beta);
}

pub struct LinearModelBig {
    params: (Float, Float),
}
impl LinearModelBig {
    pub fn new<T: TrainingKey>(data: &RMITrainingData<T>) -> LinearModelBig {
        let params = slr_big(data.iter()
                         .map(|(inp, offset)| (inp.as_float512(), Float::with_val(512,offset) )));
        return LinearModelBig { params };
    }
}

impl Model for LinearModelBig {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        let (intercept, slope) = &self.params;
        let result = Float::with_val(512,&(inp.as_float512()) *slope);
        return Float::with_val(512, result + intercept).to_f64();
    }

    fn input_type(&self) -> ModelDataType {
        return ModelDataType::Float512;
    }
    fn output_type(&self) -> ModelDataType {
        return ModelDataType::Float512;
    }

    fn params(&self) -> Vec<ModelParam> {
        return vec![ModelParam::Float512(Float::with_val(512,&self.params.0)), ModelParam::Float512(Float::with_val(512,&self.params.1))];
    }

    fn code(&self) -> String {
        return String::from(
            "
inline double linear(cpp_bin_float_512 alpha, cpp_bin_float_512 beta, cpp_bin_float_512 inp) {
    return boost::multiprecision::fma(beta, inp, alpha).convert_to<double>();
}",
        );
    }

    fn function_name(&self) -> String {
        return String::from("linear");
    }

    fn set_to_constant_model(&mut self, constant: u64) -> bool {
        self.params = (Float::with_val(512,constant), Float::with_val(512,0) );
        return true;
    }
}

pub struct LogLinearModel {
    params: (f64, f64),
}

fn exp1(inp: f64) -> f64 {
    let mut x = inp;
    x = 1.0 + x / 64.0;
    x *= x;
    x *= x;
    x *= x;
    x *= x;
    x *= x;
    x *= x;
    return x;
}

impl LogLinearModel {
    pub fn new<T: TrainingKey>(data: &RMITrainingData<T>) -> LogLinearModel {
        return LogLinearModel {
            params: loglinear_slr(&data),
        };
    }
}

impl Model for LogLinearModel {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        let (alpha, beta) = self.params;
        return exp1(beta.mul_add(inp.as_float(), alpha));
    }

    fn input_type(&self) -> ModelDataType {
        return ModelDataType::Float;
    }
    fn output_type(&self) -> ModelDataType {
        return ModelDataType::Float;
    }

    fn params(&self) -> Vec<ModelParam> {
        return vec![self.params.0.into(), self.params.1.into()];
    }

    fn code(&self) -> String {
        return String::from(
            "
inline double loglinear(double alpha, double beta, double inp) {
    return exp1(std::fma(beta, inp, alpha));
}",
        );
    }

    fn function_name(&self) -> String {
        return String::from("loglinear");
    }
    fn standard_functions(&self) -> HashSet<StdFunctions> {
        let mut to_r = HashSet::new();
        to_r.insert(StdFunctions::EXP1);
        return to_r;
    }
}

#[cfg(test)]
mod loglin_tests {
    use super::*;

    #[test]
    fn test_loglinear1() {
        let md = ModelData::IntKeyToIntPos(vec![(2, 2), (3, 4), (4, 16)]);

        let loglin_mod = LogLinearModel::new(&md);

        assert_eq!(loglin_mod.predict_to_int(2.into()), 1);
        assert_eq!(loglin_mod.predict_to_int(4.into()), 13);
    }

    #[test]
    fn test_empty() {
        LogLinearModel::new(&ModelData::empty());
    }
}


pub struct RobustLinearModel {
    params: (f64, f64),
}


impl RobustLinearModel {
    pub fn new<T: TrainingKey>(data: &RMITrainingData<T>) -> RobustLinearModel {
        let total_items = data.len();
        if data.len() == 0 {
            return RobustLinearModel {
                params: (0.0, 0.0)
            };
        }
        
        let bnd = usize::max(1, ((total_items as f64) * 0.0001) as usize);
        assert!(bnd*2+1 < data.len());
        
        let iter = data.iter()
            .skip(bnd)
            .take(data.len() - 2*bnd);

        let robust_params = slr(iter
                                .map(|(inp, offset)| (inp.as_float(), offset as f64)));
        
        return RobustLinearModel {
            params: robust_params
        };
    }
}

impl Model for RobustLinearModel {
    fn predict_to_float(&self, inp: &ModelInput) -> f64 {
        let (alpha, beta) = self.params;
        return beta.mul_add(inp.as_float(), alpha);
    }

    fn input_type(&self) -> ModelDataType {
        return ModelDataType::Float;
    }
    fn output_type(&self) -> ModelDataType {
        return ModelDataType::Float;
    }

    fn params(&self) -> Vec<ModelParam> {
        return vec![self.params.0.into(), self.params.1.into()];
    }

    fn code(&self) -> String {
        return String::from(
            "
inline double linear(double alpha, double beta, double inp) {
    return std::fma(beta, inp, alpha);
}",
        );
    }
    
    fn function_name(&self) -> String {
        return String::from("linear");
    }

    fn set_to_constant_model(&mut self, constant: u64) -> bool {
        self.params = (constant as f64, 0.0);
        return true;
    }
}
