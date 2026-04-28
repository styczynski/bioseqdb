// < begin copyright > 
// Copyright Ryan Marcus 2020
// 
// See root directory of this project for license terms.
// 
// < end copyright > 
 
use crate::models::TrainingKey;
use crate::models::*;
use crate::train::{validate, train_model, train_model_big, TrainedRMI};
use crate::train::lower_bound_correction::LowerBoundCorrection;
use log::*;

fn error_between(v1: u64, v2: u64, max_pred: u64) -> u64 {
    let pred1 = u64::min(v1, max_pred);
    let pred2 = u64::min(v2, max_pred);
    return u64::max(pred1, pred2) - u64::min(pred1, pred2);
}

fn build_models_from<T: TrainingKey>(data: &RMITrainingData<T>,
                                    top_model: &Box<dyn Model>,
                                    model_type: &str,
                                    start_idx: usize, end_idx: usize,
                                    first_model_idx: usize,
                                    num_models: usize) -> Vec<Box<dyn Model>> {

    assert!(end_idx > start_idx,
            "start index was {} but end index was {}",
            start_idx, end_idx);
    assert!(end_idx <= data.len());
    assert!(start_idx <= data.len());

    let dummy_md = RMITrainingData::<T>::empty();
    let mut leaf_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_models as usize);
    let mut second_layer_data = Vec::with_capacity((end_idx - start_idx) / num_models as usize);
    let mut last_target = first_model_idx;
           
    let bounded_it = data.iter()
        .skip(start_idx)
        .take(end_idx - start_idx);
        
    for (x, y) in bounded_it {
        let model_pred = top_model.predict_to_int(&x.to_model_input()) as usize;
        assert!(top_model.needs_bounds_check() || model_pred < first_model_idx + num_models,
                "Top model gave an index of {} which is out of bounds of {}. \
                Subset range: {} to {}",
                model_pred, start_idx + num_models, start_idx, end_idx);
        let target = usize::min(first_model_idx + num_models - 1, model_pred);
        assert!(target >= last_target);
        
        if target > last_target {
            // this is the first datapoint for the next leaf model.
            // train the previous leaf model.
            
            // include the first point of the next leaf node to 
            // support lower bound searches (not required, but reduces error)
            let last_item = second_layer_data.last().copied();
            second_layer_data.push((x, y));
            
            let container = RMITrainingData::new(Box::new(second_layer_data));
            let leaf_model = train_model(model_type, &container);
            leaf_models.push(leaf_model);
            
            
            // leave empty models for any we skipped.
            for _skipped_idx in (last_target+1)..target {
                leaf_models.push(train_model(model_type, &dummy_md));
            }
            assert_eq!(leaf_models.len() + first_model_idx, target);

            second_layer_data = Vec::new();

            // include the last item of this leaf in the next leaf
            // to support lower bound searches.
            if let Some(v) = last_item {
                second_layer_data.push(v);
            }

        }
        
        second_layer_data.push((x, y));
        last_target = target;
    }

    // train the last remaining model
    assert!(! second_layer_data.is_empty());
    let container = RMITrainingData::new(Box::new(second_layer_data));
    let leaf_model = train_model(model_type, &container);
    leaf_models.push(leaf_model);
    assert!(leaf_models.len() <= num_models);
    
    // add models at the end with nothing mapped into them
    for _skipped_idx in (last_target+1)..(first_model_idx + num_models) as usize {
        leaf_models.push(train_model(model_type, &dummy_md));
    }
    assert_eq!(num_models as usize, leaf_models.len());
    return leaf_models;
}

fn build_partial_models_from<T: TrainingKey>(data: &RMITrainingData<T>,
                                    top_model: &Box<dyn Model>,
                                    model_type: &str,
                                    start_idx: usize, end_idx: usize,
                                    first_model_idx: usize,
                                    num_models: usize,
                                    top_model_offset: u64 ) -> Vec<Box<dyn Model>> {

    assert!(end_idx > start_idx,
            "start index was {} but end index was {}",
            start_idx, end_idx);
    assert!(end_idx <= data.len());
    assert!(start_idx <= data.len());

    let dummy_md = RMITrainingData::<T>::empty();
    let mut leaf_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_models as usize);
    let mut second_layer_data = Vec::with_capacity((end_idx - start_idx) / num_models as usize);
    let mut last_target = first_model_idx;
           
    let bounded_it = data.iter()
        .skip(start_idx)
        .take(end_idx - start_idx);
        
    for (x, y) in bounded_it {
        // let model_pred = top_model.predict_to_int(&x.to_model_input()) as usize + top_model_offset as usize;
        let model_pred = top_model.predict_to_int(&x.to_model_input()) as usize;
        assert!(top_model.needs_bounds_check() || model_pred < first_model_idx + num_models,
                "Top model gave an index of {} which is out of bounds of {}. \
                Subset range: {} to {}",
                model_pred, start_idx + num_models, start_idx, end_idx);
        let target = usize::min(first_model_idx + num_models - 1, model_pred);
        assert!(target >= last_target);
        
        if target > last_target {
            // println!("Model pred: {} start:{} end:{}", target, first_model_idx,first_model_idx + num_models);
            // this is the first datapoint for the next leaf model.
            // train the previous leaf model.
            
            // include the first point of the next leaf node to 
            // support lower bound searches (not required, but reduces error)
            let last_item = second_layer_data.last().copied();
            second_layer_data.push((x, y));
            
            let container = RMITrainingData::new(Box::new(second_layer_data));
            let leaf_model = train_model(model_type, &container);
            leaf_models.push(leaf_model);
            
            
            // leave empty models for any we skipped.
            for _skipped_idx in (last_target+1)..target {
                leaf_models.push(train_model(model_type, &dummy_md));
            }
            assert_eq!(leaf_models.len() + first_model_idx, target);

            second_layer_data = Vec::new();

            // include the last item of this leaf in the next leaf
            // to support lower bound searches.
            if let Some(v) = last_item {
                second_layer_data.push(v);
            }

        }
        
        second_layer_data.push((x, y));
        last_target = target;
    }

    // train the last remaining model
    assert!(! second_layer_data.is_empty());
    let container = RMITrainingData::new(Box::new(second_layer_data));
    let leaf_model = train_model(model_type, &container);
    leaf_models.push(leaf_model);
    assert!(leaf_models.len() <= num_models);
    
    // add models at the end with nothing mapped into them
    for _skipped_idx in (last_target+1)..(first_model_idx + num_models) as usize {
        leaf_models.push(train_model(model_type, &dummy_md));
    }
    assert_eq!(num_models as usize, leaf_models.len());
    return leaf_models;
}
fn build_3layer_models_from<T: TrainingKey>(data: &RMITrainingData<T>,
                                    top_model: &Box<dyn Model>,
                                    sec_models: &Vec<Box<dyn Model>>,
                                    second_model_type: &str,
                                    third_model_type: &str,
                                    start_idx: usize, end_idx: usize,
                                    first_model_idx: usize,
                                    num_second_models: usize,
                                    num_third_models: usize
                                ) -> Vec<Box<dyn Model>> {

    assert!(end_idx > start_idx,
            "start index was {} but end index was {}",
            start_idx, end_idx);
    assert!(end_idx <= data.len());
    assert!(start_idx <= data.len());
    let num_rows = data.len();
    println!("Num rows in build3layer {}", num_rows);
    let dummy_md = RMITrainingData::<T>::empty();
    let mut second_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_second_models as usize);
    let mut third_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_third_models);
    // let mut third_layer_data = Vec::with_capacity((end_idx - start_idx) / num_third_models as usize);
    let mut last_target = first_model_idx;
           
    

    let batch_size = num_third_models;

    for memory_batch in (0..(num_third_models)).step_by(batch_size){
        let mut data_of_batch = vec![Vec::with_capacity((end_idx - start_idx) / num_third_models as usize);batch_size ];

        let bounded_it = data.iter()
            .skip(start_idx)
            .take(end_idx - start_idx);

        for (x, y) in bounded_it {
            let model_pred_1 = top_model.predict_to_int(&x.to_model_input()) as usize;
            let mut pred_sec_layer = u64::min(num_second_models as u64 - 1, model_pred_1 as u64) as usize;
            let model_pred = sec_models[pred_sec_layer].predict_to_int(&x.to_model_input()) as usize;
            assert!(top_model.needs_bounds_check() || model_pred < first_model_idx + num_third_models,
                    "Top model gave an index of {} which is out of bounds of {}. \
                    Subset range: {} to {}",
                    model_pred, start_idx + num_second_models, start_idx, end_idx);
            let model_pred = u64::min((pred_sec_layer  as u64 +1)*((num_third_models/num_second_models) as u64) - 1, model_pred as u64) as usize;
            let target = u64::max((pred_sec_layer  as u64 )*((num_third_models/num_second_models) as u64), model_pred as u64) as usize;
            // let target = usize::min(first_model_idx + num_third_models - 1, model_pred);
            
            if (target >= memory_batch && target < memory_batch + batch_size){
                data_of_batch[target - memory_batch].push((x,y));

            }

            assert!(target >= last_target, "Last target was {}, Current target was {}",
            last_target, target);
           
        }
        for i in 0..batch_size {
            if memory_batch + i >= ( num_third_models) {
                break;
            }
            if data_of_batch[i].len() != 0 {
                let mut container = RMITrainingData::new(Box::new(data_of_batch.get(i).cloned().unwrap() ));
                let leaf_model = train_model(third_model_type, &container);
                third_models.push(leaf_model);
            }
            else{
                third_models.push(train_model(third_model_type, &dummy_md));
            }
            
        }
    }
   
    assert_eq!(num_third_models as usize, third_models.len());
    return third_models;
}

fn build_naive_3layer_models_from<T: TrainingKey>(data: &RMITrainingData<T>,
                                    top_model: &Box<dyn Model>,
                                    sec_models: &Vec<Box<dyn Model>>,
                                    second_model_type: &str,
                                    third_model_type: &str,
                                    start_idx: usize, end_idx: usize,
                                    first_model_idx: usize,
                                    num_second_models: usize,
                                    num_third_models: usize
                                ) -> Vec<Box<dyn Model>> {

    assert!(end_idx > start_idx,
            "start index was {} but end index was {}",
            start_idx, end_idx);
    assert!(end_idx <= data.len());
    assert!(start_idx <= data.len());
    let num_rows = data.len();
    println!("Num rows in build3layer {}", num_rows);
    let dummy_md = RMITrainingData::<T>::empty();
    let mut second_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_second_models as usize);
    let mut third_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_third_models);
    // let mut third_layer_data = Vec::with_capacity((end_idx - start_idx) / num_third_models as usize);
    let mut last_target = first_model_idx;
           
    

    let batch_size = num_third_models;

    for memory_batch in (0..(num_third_models)).step_by(batch_size){
        let mut data_of_batch = vec![Vec::with_capacity((end_idx - start_idx) / num_third_models as usize);batch_size ];

        let bounded_it = data.iter()
            .skip(start_idx)
            .take(end_idx - start_idx);

        for (x, y) in bounded_it {
            let model_pred_1 = top_model.predict_to_int(&x.to_model_input()) as usize;
            let mut pred_sec_layer = u64::min(num_second_models as u64 - 1, model_pred_1 as u64) as usize;
            let model_pred = sec_models[pred_sec_layer].predict_to_int(&x.to_model_input()) as usize;
            assert!(top_model.needs_bounds_check() || model_pred < first_model_idx + num_third_models,
                    "Top model gave an index of {} which is out of bounds of {}. \
                    Subset range: {} to {}",
                    model_pred, start_idx + num_second_models, start_idx, end_idx);
            // let model_pred = u64::min((pred_sec_layer  as u64 +1)*((num_third_models/num_second_models) as u64) - 1, model_pred as u64) as usize;
            let target = usize::min(first_model_idx + num_third_models - 1, model_pred);
            
            if (target >= memory_batch && target < memory_batch + batch_size){
                data_of_batch[target - memory_batch].push((x,y));

            }

            assert!(target >= last_target, "Last target was {}, Current target was {}",
            last_target, target);
           
        }

        let mut last_key = 0;
        for i in 0..batch_size {
            if memory_batch + i >= ( num_third_models) {
                break;
            }
            if data_of_batch[i].len() != 0 {
                last_key = data_of_batch[i][data_of_batch[i].len()-1].1;
                let mut container = RMITrainingData::new(Box::new(data_of_batch.get(i).cloned().unwrap() ));
                let leaf_model = train_model(third_model_type, &container);
                third_models.push(leaf_model);
            }
            else{
                let mut leaf_model = train_model(third_model_type, &dummy_md);
                leaf_model.set_to_constant_model(last_key as u64);
                third_models.push(leaf_model);
            }
            
        }
    }
   
    assert_eq!(num_third_models as usize, third_models.len());
    return third_models;
}

fn build_partial_3layer_models_from<T: TrainingKey>(data: &RMITrainingData<T>,
                                    top_model: &Box<dyn Model>,
                                    model_type: &str,
                                    model_type_partial: &str,
                                    start_idx: usize, end_idx: usize,
                                    first_model_idx: usize,
                                    num_models: usize) -> (Vec<Box<dyn Model>>, Vec<(usize,usize)>,Vec<LowerBoundCorrection<T>>, Vec<Box<dyn Model>>, usize) {

    assert!(end_idx > start_idx,
            "start index was {} but end index was {}",
            start_idx, end_idx);
    assert!(end_idx <= data.len());
    assert!(start_idx <= data.len());

    let dummy_md = RMITrainingData::<T>::empty();
    let mut leaf_models: Vec<Box<dyn Model>>
                             = Vec::with_capacity(num_models as usize);
    let mut partial_3rd_models: Vec<Box<dyn Model>> =  Vec::new();                         
    let mut partial_3rd_idx: Vec<(usize,usize)> = Vec::new();
    let mut partial_3rd_lb_corrs: Vec<LowerBoundCorrection<T>> = Vec::new();
    let mut second_layer_data: Vec<(T, usize)> = Vec::with_capacity((end_idx - start_idx) / num_models as usize);
    let mut last_target = first_model_idx;
           
    let bounded_it = data.iter()
        .skip(start_idx)
        .take(end_idx - start_idx);
    
    let mut third_layer_num: usize = 0;

    let make_partial_threshold = 1000;
    let average_partial_model_num = 20;

    for (x, y) in bounded_it {
        let model_pred = top_model.predict_to_int(&x.to_model_input()) as usize;
        assert!(top_model.needs_bounds_check() || model_pred < first_model_idx + num_models,
                "Top model gave an index of {} which is out of bounds of {}. \
                Subset range: {} to {}",
                model_pred, start_idx + num_models, start_idx, end_idx);
        let target = usize::min(first_model_idx + num_models - 1, model_pred);
        assert!(target >= last_target);
        if target > last_target {
            // this is the first datapoint for the next leaf model.
            // train the previous leaf model.
            // include the first point of the next leaf node to 
            // support lower bound searches (not required, but reduces error)
            let last_item = second_layer_data.last().copied();
            second_layer_data.push((x, y));
            let mut container = RMITrainingData::new(Box::new(second_layer_data));
            if container.len() > make_partial_threshold {
                let curr_third_layer_num = (container.len() as f64 / average_partial_model_num as f64).round() as u64;
                // let curr_third_layer_num = 65536 as u64;
                // container.set_offset(container.get(0).1 - (third_layer_num * 100) );
                let start_y = container.get(0).1;
                let end_y = container.get(container.len()-1 ).1;

                container.set_offset(container.get(0).1);
                container.set_scale( (curr_third_layer_num-1) as f64 / (end_y-start_y) as f64);
                
                let leaf_model = train_model(model_type, &container);
                // let leaf_model = train_model("pwl42", &container);

                // check if  only single partial model is used, and exclude

                partial_3rd_idx.append(&mut vec![(third_layer_num as usize, curr_third_layer_num as usize)]);
                container.set_offset(0);
                container.set_scale(1.0);
            
                let mut curr_partial_3rd_model = build_partial_models_from(&container, &leaf_model, model_type_partial,
                                    0, container.len(), 0,
                                    curr_third_layer_num as usize, third_layer_num as u64);

                let lb_corrections = LowerBoundCorrection::new(
                                        |x| leaf_model.predict_to_int(&x.to_model_input()) , curr_third_layer_num, &container
                                       );
                for idx in 0..(curr_third_layer_num as usize) {
                    assert_eq!(lb_corrections.first_key(idx).is_none(),
                    lb_corrections.last_key(idx).is_none());
                    // println!("lbcorr, next index:{}", lb_corrections.next_index(idx));
                    // println!("Partial:{} lbcorr, upper bound:{}",partial_3rd_lb_corrs.len(), lb_corrections.next_index(idx));
                    if lb_corrections.last_key(idx).is_none() {
                    // model is empty!
                        let mut upper_bound = lb_corrections.next_index(idx);
                        // next index is 0 if first and single model is used in 3rd layer
                        // no need to use partial layer in this case, need fix
                        if lb_corrections.first_non_empty_model() == 0 && lb_corrections.first_non_empty_model() == lb_corrections.last_non_empty_model() {
                            upper_bound = end_y+1;
                        }
                        // println!("Partial:{} lbcorr, upper bound:{}",partial_3rd_lb_corrs.len(), upper_bound);
                        if !curr_partial_3rd_model[idx].set_to_constant_model(upper_bound as u64) {
                            assert!(false);
                        }
                    }
                }
                // println!("{} First elem y: {} Last elem y: {}, lbcor lastkey: {}",curr_third_layer_num, container.get(0).1,container.get(container.len()-1).1, lb_corrections.last_key(last_model).unwrap().as_uint()  );
                leaf_models.push(leaf_model);
                partial_3rd_lb_corrs.append(&mut vec![lb_corrections]);
                partial_3rd_models.append(&mut curr_partial_3rd_model);
                third_layer_num += curr_third_layer_num as usize;
                assert!(partial_3rd_models.len() == third_layer_num);
            }
            else{
                let leaf_model = train_model(model_type, &container);
                partial_3rd_idx.append(&mut vec![(0,0)] );
                leaf_models.push(leaf_model);
            }
            // leave empty models for any we skipped.
            for _skipped_idx in (last_target+1)..target {
                leaf_models.push(train_model(model_type, &dummy_md));
                partial_3rd_idx.append(&mut vec![(0,0)] );
            }
            assert_eq!(leaf_models.len() + first_model_idx, target);

            second_layer_data = Vec::new();

            // include the last item of this leaf in the next leaf
            // to support lower bound searches.
            if let Some(v) = last_item {
                second_layer_data.push(v);
            }
        }
        second_layer_data.push((x, y));
        last_target = target;
    }

    // train the last remaining model
    assert!(! second_layer_data.is_empty());
    let mut container = RMITrainingData::new(Box::new(second_layer_data));
    if container.len() > make_partial_threshold {
        let curr_third_layer_num = (container.len() as f64 / average_partial_model_num as f64).round() as usize;
        // container.set_offset(container.get(0).1 - (third_layer_num * 100) );
        let start_y = container.get(0).1;
        let end_y = container.get(container.len()-1 ).1;

        container.set_offset(container.get(0).1);
        container.set_scale( (curr_third_layer_num-1) as f64 / (end_y-start_y) as f64);
        
        let leaf_model = train_model(model_type, &container);
        partial_3rd_idx.append(&mut vec![(third_layer_num as usize, curr_third_layer_num as usize)]);
        
        container.set_offset(0);
        container.set_scale(1.0);
        // build partial 3 layer model with calculated number of models to build
        let mut curr_partial_3rd_model = build_partial_models_from(&container, &leaf_model, model_type_partial,
            0, container.len(), 0,
            curr_third_layer_num as usize, third_layer_num as u64);
        // Do lowerboundcorrection
        let lb_corrections = LowerBoundCorrection::new(
                        |x| leaf_model.predict_to_int(&x.to_model_input()) , curr_third_layer_num as u64, &container
                    );
        // set empty models to constant model
        for idx in 0..(curr_third_layer_num as usize) {
            assert_eq!(lb_corrections.first_key(idx).is_none(),
            lb_corrections.last_key(idx).is_none());
            if lb_corrections.last_key(idx).is_none() {
                // model is empty!
                let mut upper_bound = lb_corrections.next_index(idx);
                // if data is all inside single and first model of partial models, upper bound should be end_y + 1
                // lowerbound have next set to 0 in this case
                if lb_corrections.first_non_empty_model() == 0 && lb_corrections.first_non_empty_model() == lb_corrections.last_non_empty_model() {
                    upper_bound = end_y+1;
                }
                // if lb_corrections.last_non_empty_model() < idx as u64 {
                //     upper_bound = data.len();
                // }
                if !curr_partial_3rd_model[idx].set_to_constant_model(upper_bound as u64) {
                    assert!(false);
                }
            }
        }
        // println!("{} First elem y: {} Last elem y: {}, lbcor lastkey: {}",curr_third_layer_num, container.get(0).1,container.get(container.len()-1).1, lb_corrections.last_key(last_model).unwrap().as_uint() );
        leaf_models.push(leaf_model);
        partial_3rd_lb_corrs.append(&mut vec![lb_corrections]);
        partial_3rd_models.append(&mut curr_partial_3rd_model);
        
        third_layer_num += curr_third_layer_num;
        assert!(partial_3rd_models.len() == third_layer_num);
    }
    else{
        let leaf_model = train_model(model_type, &container);
        partial_3rd_idx.append(&mut vec![(0,0)] );
        leaf_models.push(leaf_model);
    }
    // let leaf_model = train_model(model_type, &container);
    // leaf_models.push(leaf_model);
    // partial_3rd_idx.append((0,0));
    assert!(leaf_models.len() <= num_models);
    
    // add models at the end with nothing mapped into them
    for _skipped_idx in (last_target+1)..(first_model_idx + num_models) as usize {
        leaf_models.push(train_model(model_type, &dummy_md));
        partial_3rd_idx.append(&mut vec![(0,0)]);
    }
    assert_eq!(num_models as usize, leaf_models.len());
    return (leaf_models , partial_3rd_idx, partial_3rd_lb_corrs, partial_3rd_models, third_layer_num);
}
pub fn train_two_layer<T: TrainingKey>(md_container: &mut RMITrainingData<T>,
                                      layer1_model: &str, layer2_model: &str,
                                      num_leaf_models: u64) -> TrainedRMI {
    validate(&[String::from(layer1_model), String::from(layer2_model)]);

    let num_rows = md_container.len();

    println!("Training top-level {} model layer", layer1_model);
    md_container.set_scale(num_leaf_models as f64 / num_rows as f64);
    let top_model = train_model(layer1_model, &md_container);

    // Check monotonicity if in debug mode
    #[cfg(debug_assertions)]
    {
        let mut last_pred = 0;
        for (x, _y) in md_container.iter_model_input() {
            let prediction = top_model.predict_to_int(&x);
            debug_assert!(prediction >= last_pred,
                          "Top model {} was non-monotonic on input {:?}",
                          layer1_model, x);
            last_pred = prediction;
        }
        trace!("Top model was monotonic.");
    }

    println!("Training second-level {} model layer (num models = {})",
          layer2_model, num_leaf_models);
    md_container.set_scale(1.0);

    // find a prediction boundary near the middle
    let midpoint_model = num_leaf_models / 2;
    let split_idx = md_container.lower_bound_by(|x| {
        let model_idx = top_model.predict_to_int(&x.0.to_model_input());
        let model_target = u64::min(num_leaf_models - 1, model_idx);
        return model_target.cmp(&midpoint_model);
    });

    // make sure the split point that we got is valid
    if split_idx > 0 && split_idx < md_container.len() {
        let key_at = top_model.predict_to_int(&md_container.get_key(split_idx)
                                              .to_model_input());
        let key_pr = top_model.predict_to_int(&md_container.get_key(split_idx - 1)
                                              .to_model_input());
        assert!(key_at > key_pr);
    }

    let mut leaf_models = if split_idx >= md_container.len() {
        build_models_from(&md_container, &top_model, layer2_model,
                          0, md_container.len(), 0,
                          num_leaf_models as usize)
    } else {
        let split_idx_target = u64::min(num_leaf_models - 1,
                                        top_model.predict_to_int(
                                            &md_container.get_key(split_idx)
                                                .to_model_input()))
            as usize;

        let first_half_models = split_idx_target as usize;
        let second_half_models = num_leaf_models as usize - split_idx_target as usize;

        let (mut hf1, mut hf2)
            = rayon::join(|| build_models_from(&md_container, &top_model, layer2_model,
                                               0, split_idx,
                                               0,
                                               first_half_models),
                          || build_models_from(&md_container, &top_model, layer2_model,
                                               split_idx + 1, md_container.len(),
                                               split_idx_target,
                                               second_half_models));

        let mut leaf_models = Vec::new();
        leaf_models.append(&mut hf1);
        leaf_models.append(&mut hf2);
        leaf_models
    };

    println!("Computing lower bound stats...");
    let lb_corrections = LowerBoundCorrection::new(
        |x| top_model.predict_to_int(&x.to_model_input()), num_leaf_models, md_container
    );

    println!("Fixing empty models...");
    let mut empty_models = 0;
    // replace any empty model with a model that returns the correct constant
    // (for LB predictions), if the underlying model supports it.
    let mut could_not_replace = false;

    // for idx in 0..(num_leaf_models as usize)-1 {
    for idx in 0..(num_leaf_models as usize) {
        assert_eq!(lb_corrections.first_key(idx).is_none(),
                   lb_corrections.last_key(idx).is_none());

        if lb_corrections.last_key(idx).is_none() {
            // model is empty!
            empty_models+= 1;
            // upper_bound is set to next leaf index, if only empty models exists from next leaf models, 
            let upper_bound = lb_corrections.next_index(idx);
            if !leaf_models[idx].set_to_constant_model(upper_bound as u64) {
                could_not_replace = true;
            }
        }
    }
    println!("Number of Empty models {} out of {}",empty_models, num_leaf_models);
    if could_not_replace {
        warn!("Some empty models could not be replaced with constants, \
               negative lookup performance may be poor.");
    }
    
    
    println!("Computing last level errors...");
    // evaluate model, compute last level errors

    let mut max_min_gap = vec![[0, u64::MAX] ; num_leaf_models as usize];

    let mut last_layer_max_l1s = vec![(0, 0) ; num_leaf_models as usize];
    for (x, y) in md_container.iter_model_input() {
        let leaf_idx = top_model.predict_to_int(&x);
        let target = u64::min(num_leaf_models - 1, leaf_idx) as usize;
        
        let pred = leaf_models[target].predict_to_int(&x);

        if ( (y as u64) > max_min_gap[target as usize][0]){
            max_min_gap[target as usize][0] = y as u64;
        }
        if ( (y as u64) < max_min_gap[target as usize][1]){
            max_min_gap[target as usize][1] = y as u64;
        }

        let err = error_between(pred, y as u64, md_container.len() as u64);

        let cur_val = last_layer_max_l1s[target];
        
        last_layer_max_l1s[target] = (cur_val.0 + 1, u64::max(err, cur_val.1));
    }    

    let mut sum:f64 = 0.0; 
    let mut avg:f64 = 0.0; 
    for i__ in 0..num_leaf_models as usize {
        sum +=  (max_min_gap[i__][0] - max_min_gap[i__][1]) as f64 ;

        // if (i__ % 10000 == 1){
        //     println!("{}: Gap Value:{} ",i__, max_min_gap[i__][0] - max_min_gap[i__][1]);
        // }
    }
    avg = sum / num_leaf_models as f64;
    println!("Average gap: {}",avg);
    // for lower bound searches, we need to make sure that:
    //   (1) a query for the first key in the next leaf minus one 
    //       includes the key in the next leaf. (upper error)
    //   (2) a query for the last key in the previous leaf plus one
    //       includes the first key after the previous leaf (lower error)
    //       (normally, the first key after the previous leaf is the first
    //        key in this leaf, but not in the case where this leaf has no keys)
    let mut large_corrections = 0;
    for leaf_idx in 0..num_leaf_models as usize {
        let curr_err = last_layer_max_l1s[leaf_idx].1;

        let mut upper_flag = false;
        let mut lower_flag = false;
        let upper_error = {
            let (idx_of_next, key_of_next) = lb_corrections.next(leaf_idx);
            let pred = leaf_models[leaf_idx].predict_to_int(
                &key_of_next.minus_epsilon().to_model_input()
            );
            upper_flag= pred>idx_of_next as u64;
            error_between(pred, idx_of_next as u64 + 1, md_container.len() as u64)
        };
        
        let lower_error = {
            let first_key_before = lb_corrections.prev_key(leaf_idx);

            let prev_idx = if leaf_idx == 0 { 0 } else { leaf_idx - 1 };
            let first_idx = lb_corrections.next_index(prev_idx);

            let pred = leaf_models[leaf_idx].predict_to_int(
                &first_key_before.plus_epsilon().to_model_input()
            );
            lower_flag = pred > first_idx as u64;
            error_between(pred, first_idx as u64, md_container.len() as u64)
        };
        
        
        let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap()
            + lb_corrections.longest_run(leaf_idx);

        

        let num_items_in_leaf = last_layer_max_l1s[leaf_idx].0;
        last_layer_max_l1s[leaf_idx] = (num_items_in_leaf, new_err);
        if new_err - curr_err > 512 && num_items_in_leaf > 100 {
            large_corrections += 1;
        }
    }

    if large_corrections > 1 {
        println!("Of {} models, {} needed large lower bound corrections.",
        num_leaf_models, large_corrections);
        trace!("Of {} models, {} needed large lower bound corrections.",
              num_leaf_models, large_corrections);
    }
                        
    println!("Evaluating two-layer RMI...");
    let mut new_last_layer_max_l1s = vec![(0, 0) ;num_leaf_models as usize];
    let mut idx_ = 0;
    for (num_key, err) in &last_layer_max_l1s {
        new_last_layer_max_l1s[idx_].0 = *num_key;
        new_last_layer_max_l1s[idx_].1 = *err;
        // new_last_layer_max_l1s[idx_].1 = (*err & 0x7fffffff) + ((*err >> 32) & 0x3fffffff);
        idx_ += 1;
    }
    let (m_idx, m_err) = new_last_layer_max_l1s
        .iter().enumerate()
        .max_by_key(|(_idx, &x)| x.1 ).unwrap();
    
    let model_max_error = m_err.1;
    let model_max_error_idx = m_idx;

    let model_avg_error: f64 = new_last_layer_max_l1s
        .iter().map(|(n, err)| n * err).sum::<u64>() as f64 / num_rows as f64;

    let model_avg_l2_error: f64 = new_last_layer_max_l1s
        .iter()
        .map(|(n, err)| ((n*err) as f64).powf(2.0) / num_rows as f64).sum::<f64>();
    
    let model_avg_log2_error: f64 = new_last_layer_max_l1s
        .iter().map(|(n, err)| (*n as f64)*((2*err + 2) as f64).log2()).sum::<f64>() / num_rows as f64;

    let model_max_log2_error: f64 = (model_max_error as f64).log2();
    

    let final_errors = last_layer_max_l1s.into_iter()
        .map(|(_n, err)| err).collect();
    


    return TrainedRMI {
        num_rmi_rows: md_container.len(),
        num_data_rows: md_container.len(),
        model_avg_error,
        model_avg_l2_error,
        model_avg_log2_error,
        model_max_error,
        model_max_error_idx,
        model_max_log2_error,
        last_layer_max_l1s: final_errors,
        third_layer_max_l1s: vec![],
        rmi: vec![vec![top_model], leaf_models],
        models: format!("{},{}", layer1_model, layer2_model),
        branching_factor: num_leaf_models,
        cache_fix: None,
        build_time: 0
    };

}

pub fn train_three_layer<T: TrainingKey>(md_container: &mut RMITrainingData<T>,
                                        layer1_model: &str, layer2_model: &str,
                                        layer3_model: &str,
                                        num_leaf_models: u64) -> TrainedRMI {
    validate(&[String::from(layer1_model), String::from(layer2_model),  String::from(layer3_model)]);

    let num_rows = md_container.len();
    
    let second_model_num = (num_leaf_models as f64).sqrt() as u64;
    let third_model_num = (num_leaf_models as f64).sqrt() as u64;
    assert!(num_leaf_models==second_model_num*third_model_num);

    println!("Training top-level {} model layer", layer1_model);
    md_container.set_scale(second_model_num as f64 / num_rows as f64);
    let top_model = train_model(layer1_model, &md_container);

    // Check monotonicity if in debug mode
    #[cfg(debug_assertions)]
    {
        let mut last_pred = 0;
        for (x, _y) in md_container.iter_model_input() {
            let prediction = top_model.predict_to_int(&x);
            debug_assert!(prediction >= last_pred,
                        "Top model {} was non-monotonic on input {:?}",
                        layer1_model, x);
            last_pred = prediction;
        }
        trace!("Top model was monotonic.");
    }

    println!("Training second-level {} model layer (num models = {})",
            layer2_model, second_model_num);
    md_container.set_scale( (num_leaf_models) as f64 / num_rows as f64);
    // md_container.set_scale(1.0);
    // find a prediction boundary near the middle
    let midpoint_model = second_model_num / 2;
    let split_idx = md_container.lower_bound_by(|x| {
        let model_idx = top_model.predict_to_int(&x.0.to_model_input());
        let model_target = u64::min(second_model_num - 1, model_idx);
        return model_target.cmp(&midpoint_model);
    });

    // make sure the split point that we got is valid
    if split_idx > 0 && split_idx < md_container.len() {
        let key_at = top_model.predict_to_int(&md_container.get_key(split_idx)
                .to_model_input());
        let key_pr = top_model.predict_to_int(&md_container.get_key(split_idx - 1)
                .to_model_input());
        assert!(key_at > key_pr);
    }

    // let (mut sec_models,mut leaf_models) = build_3layer_models_from(&md_container, &top_model, layer2_model, layer3_model,
    //                     0, md_container.len(), 0,
    //                     second_model_num as usize, third_model_num as usize);
    
    let first_idx_target = 0;
    // top_model.predict_to_int(
    //     &md_container.get_key(0)
    //         .to_model_input()) as usize;
    let (mut sec_models) = 
    if split_idx >= md_container.len() {
        build_models_from(&md_container, &top_model, layer2_model,
            0, md_container.len(), first_idx_target,
            second_model_num as usize)
    } else {
        
        let split_idx_target = u64::min(second_model_num - 1,
                                        top_model.predict_to_int(
                                            &md_container.get_key(split_idx)
                                                .to_model_input()))
            as usize;
        let first_half_models = split_idx_target as usize;
        let second_half_models = second_model_num as usize - split_idx_target as usize;
        println!("split idx target:{} split_idx:{} first_half:{} first_idx_target:{}", split_idx_target, split_idx, first_half_models, first_idx_target);

        let (mut hf1, mut hf2)
            = rayon::join(|| build_models_from(&md_container, &top_model, layer2_model,
                            0, split_idx, first_idx_target,
                            first_half_models),
                          || build_models_from(&md_container, &top_model, layer2_model,
                            split_idx + 1, md_container.len(), split_idx_target,
                            second_half_models));

        let mut sec_models = Vec::new();
        sec_models.append(&mut hf1);
        sec_models.append(&mut hf2);
        sec_models
    };
    
    

    // println!("[2nd layer]Computing lower bound stats...");
    let lb_corrections_top = LowerBoundCorrection::new(
     |x| top_model.predict_to_int(&x.to_model_input()), second_model_num, md_container
    );

    // println!("[2nd layer]Fixing empty models...");
    // replace any empty model with a model that returns the correct constant
    // (for LB predictions), if the underlying model supports it.
    let mut could_not_replace = false;
    for idx in 0..(second_model_num as usize)-1 {
         assert_eq!(lb_corrections_top.first_key(idx).is_none(),
         lb_corrections_top.last_key(idx).is_none());

        if lb_corrections_top.last_key(idx).is_none() {
        // model is empty!
            let upper_bound = lb_corrections_top.next_index(idx);
            if !sec_models[idx].set_to_constant_model(upper_bound as u64) {
                could_not_replace = true;
            }
         }
    }

    if could_not_replace {
        warn!("[2nd layer]Some empty models could not be replaced with constants, \
            negative lookup performance may be poor.");
    }

    md_container.set_scale(1.0);

    let mut leaf_models = build_3layer_models_from(&md_container, &top_model, &sec_models, layer2_model, layer3_model,
        0, md_container.len(), 0,
        second_model_num as usize, (num_leaf_models) as usize);
    
    println!("[3rd layer]Computing lower bound stats...");
    let lb_corrections = LowerBoundCorrection::new(
     |x| {
        let second_idx = top_model.predict_to_int(&x.to_model_input());
        let  pred_sec_layer = u64::min(second_model_num - 1, second_idx) as usize;
        let  pred_third_layer = sec_models[pred_sec_layer].predict_to_int(&x.to_model_input()) as u64;
        return pred_third_layer;
        }, num_leaf_models, md_container
    );

    println!("[3rd layer]Fixing empty models...");
    // replace any empty model with a model that returns the correct constant
    // (for LB predictions), if the underlying model supports it.
    let mut could_not_replace = false;
    for idx in 0..((num_leaf_models) as usize) {
         assert_eq!(lb_corrections.first_key(idx).is_none(),
         lb_corrections.last_key(idx).is_none());

        if lb_corrections.last_key(idx).is_none() {
        // model is empty!
            let upper_bound = lb_corrections.next_index(idx);
            if !leaf_models[idx].set_to_constant_model(upper_bound as u64) {
                could_not_replace = true;
            }
         }
    }

    if could_not_replace {
        warn!("[3rd layer]Some empty models could not be replaced with constants, \
            negative lookup performance may be poor.");
    }


    println!("Computing last level errors...");
    // evaluate model, compute last level errors
    //set back to original scale
    // md_container.set_scale(1.0);

    let mut max_min_gap = vec![[0, u64::MAX] ; num_leaf_models as usize];

    let mut last_layer_max_l1s = vec![(0, 0) ; num_leaf_models as usize];
    for (x, y) in md_container.iter_model_input() {
        let second_idx = top_model.predict_to_int(&x);
        let  pred_sec_layer = u64::min(second_model_num - 1, second_idx) as usize;

        let  pred_third_layer = sec_models[pred_sec_layer].predict_to_int(&x) as usize;
        // let mut target = pred_third_layer + pred_sec_layer* (third_model_num as usize);
        // let mut target = pred_third_layer as usize;

        //let target = u64::min(num_leaf_models - 1, pred_third_layer as u64) as usize;
        let target = u64::min((pred_sec_layer as u64 +1)*third_model_num - 1, pred_third_layer as u64) as usize;
        let target = u64::max((pred_sec_layer  as u64)*third_model_num, target as u64) as usize;
        let pred = leaf_models[target].predict_to_int(&x);

        // if ( x.as_int() > max_min_gap[target as usize][0]){
        //     max_min_gap[target as usize][0] = x.as_int();
        // }
        // if ( x.as_int() < max_min_gap[target as usize][1]){
        //     max_min_gap[target as usize][1] = x.as_int();
        // }
        if ( (y as u64) > max_min_gap[target as usize][0]){
            max_min_gap[target as usize][0] = y as u64;
        }
        if ( (y as u64) < max_min_gap[target as usize][1]){
            max_min_gap[target as usize][1] = y as u64;
        }

        // if ( y % 2000000 == 1 ){
        //     println!("pred:{} y:{} x:{}, sec model:{} third model:{}",pred, y, x.as_float(), pred_sec_layer, pred_third_layer );
        // }
        let err = error_between(pred, y as u64, md_container.len() as u64);

        let cur_val = last_layer_max_l1s[target];
        last_layer_max_l1s[target] = (cur_val.0 + 1, u64::max(err, cur_val.1));
    }    

    let mut sum:f64 = 0.0; 
    let mut avg:f64 = 0.0; 
    for i__ in 0..num_leaf_models as usize {
        sum +=  (max_min_gap[i__][0] - max_min_gap[i__][1]) as f64 ;
        // if (i__ % 10000 == 1){
        //     println!("{}: Gap Value:{} ",i__, max_min_gap[i__][0] - max_min_gap[i__][1]);
        // }
    }
    avg = sum / num_leaf_models as f64;
    println!("Average gap: {}",avg);
    // for lower bound searches, we need to make sure that:
    //   (1) a query for the first key in the next leaf minus one 
    //       includes the key in the next leaf. (upper error)
    //   (2) a query for the last key in the previous leaf plus one
    //       includes the first key after the previous leaf (lower error)
    //       (normally, the first key after the previous leaf is the first
    //        key in this leaf, but not in the case where this leaf has no keys)
    println!("Computing error value...");
    let mut large_corrections = 0;
    for leaf_idx in 0..num_leaf_models as usize {


        let curr_err = last_layer_max_l1s[leaf_idx].1;
    // println!("model idx:{} Err:{}",leaf_idx, curr_err);

        let upper_error = {
            let (idx_of_next, key_of_next) = lb_corrections.next(leaf_idx);
            let pred = leaf_models[leaf_idx].predict_to_int(
            &key_of_next.minus_epsilon().to_model_input()
            );
            error_between(pred, idx_of_next as u64 + 1, md_container.len() as u64)
        };

        let lower_error = {
            let first_key_before = lb_corrections.prev_key(leaf_idx);

            let prev_idx = if leaf_idx == 0 { 0 } else { leaf_idx - 1 };
            let first_idx = lb_corrections.next_index(prev_idx);

            let pred = leaf_models[leaf_idx].predict_to_int(
            &first_key_before.plus_epsilon().to_model_input()
            );
            error_between(pred, first_idx as u64, md_container.len() as u64)
        };


        let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap()
             + lb_corrections.longest_run(leaf_idx);

        let num_items_in_leaf = last_layer_max_l1s[leaf_idx].0;
        last_layer_max_l1s[leaf_idx] = (num_items_in_leaf, new_err);

        if new_err - curr_err > 512 && num_items_in_leaf > 100 {
            large_corrections += 1;
        }
    }

    if large_corrections > 1 {
        trace!("Of {} models, {} needed large lower bound corrections.",
            num_leaf_models, large_corrections);
    }

    trace!("Evaluating two-layer RMI...");
    let (m_idx, m_err) = last_layer_max_l1s
        .iter().enumerate()
        .max_by_key(|(_idx, &x)| x.1).unwrap();

    let model_max_error = m_err.1;
    let model_max_error_idx = m_idx;

    let model_avg_error: f64 = last_layer_max_l1s
      .iter().map(|(n, err)| n * err).sum::<u64>() as f64 / num_rows as f64;

    let model_avg_l2_error: f64 = last_layer_max_l1s
        .iter()
        .map(|(n, err)| ((n*err) as f64).powf(2.0) / num_rows as f64).sum::<f64>();

    let model_avg_log2_error: f64 = last_layer_max_l1s
        .iter().map(|(n, err)| (*n as f64)*((2*err + 2) as f64).log2()).sum::<f64>() / num_rows as f64;

    let model_max_log2_error: f64 = (model_max_error as f64).log2();

    let final_errors = last_layer_max_l1s.into_iter()
         .map(|(_n, err)| err).collect();

    return TrainedRMI {
        num_rmi_rows: md_container.len(),
        num_data_rows: md_container.len(),
        model_avg_error,
        model_avg_l2_error,
        model_avg_log2_error,
        model_max_error,
        model_max_error_idx,
        model_max_log2_error,
        last_layer_max_l1s: final_errors,
        third_layer_max_l1s: vec![],
        rmi: vec![vec![top_model], sec_models, leaf_models],
        models: format!("{},{},{}", layer1_model, layer2_model, layer3_model),
        branching_factor: num_leaf_models,
        cache_fix: None,
        build_time: 0
    };

}

pub fn train_naive_three_layer<T: TrainingKey>(md_container: &mut RMITrainingData<T>,
    layer1_model: &str, layer2_model: &str,
    layer3_model: &str,
    num_leaf_models: u64) -> TrainedRMI {
validate(&[String::from(layer1_model), String::from(layer2_model),  String::from(layer3_model)]);

let num_rows = md_container.len();

let second_model_num = (num_leaf_models as f64).sqrt() as u64;
let third_model_num = (num_leaf_models as f64).sqrt() as u64;
assert!(num_leaf_models==second_model_num*third_model_num);

println!("Training top-level {} model layer", layer1_model);
md_container.set_scale(second_model_num as f64 / num_rows as f64);
let top_model = train_model(layer1_model, &md_container);

// Check monotonicity if in debug mode
#[cfg(debug_assertions)]
{
let mut last_pred = 0;
for (x, _y) in md_container.iter_model_input() {
let prediction = top_model.predict_to_int(&x);
debug_assert!(prediction >= last_pred,
"Top model {} was non-monotonic on input {:?}",
layer1_model, x);
last_pred = prediction;
}
trace!("Top model was monotonic.");
}

println!("Training second-level {} model layer (num models = {})",
layer2_model, second_model_num);
md_container.set_scale( (num_leaf_models) as f64 / num_rows as f64);
// md_container.set_scale(1.0);
// find a prediction boundary near the middle
let midpoint_model = second_model_num / 2;
let split_idx = md_container.lower_bound_by(|x| {
let model_idx = top_model.predict_to_int(&x.0.to_model_input());
let model_target = u64::min(second_model_num - 1, model_idx);
return model_target.cmp(&midpoint_model);
});

// make sure the split point that we got is valid
if split_idx > 0 && split_idx < md_container.len() {
let key_at = top_model.predict_to_int(&md_container.get_key(split_idx)
.to_model_input());
let key_pr = top_model.predict_to_int(&md_container.get_key(split_idx - 1)
.to_model_input());
assert!(key_at > key_pr);
}

// let (mut sec_models,mut leaf_models) = build_3layer_models_from(&md_container, &top_model, layer2_model, layer3_model,
//                     0, md_container.len(), 0,
//                     second_model_num as usize, third_model_num as usize);

let first_idx_target = 0;
// top_model.predict_to_int(
//     &md_container.get_key(0)
//         .to_model_input()) as usize;
let (mut sec_models) = 
if split_idx >= md_container.len() {
build_models_from(&md_container, &top_model, layer2_model,
0, md_container.len(), first_idx_target,
second_model_num as usize)
} else {

let split_idx_target = u64::min(second_model_num - 1,
    top_model.predict_to_int(
        &md_container.get_key(split_idx)
            .to_model_input()))
as usize;
let first_half_models = split_idx_target as usize;
let second_half_models = second_model_num as usize - split_idx_target as usize;
println!("split idx target:{} split_idx:{} first_half:{} first_idx_target:{}", split_idx_target, split_idx, first_half_models, first_idx_target);

let (mut hf1, mut hf2)
= rayon::join(|| build_models_from(&md_container, &top_model, layer2_model,
0, split_idx, first_idx_target,
first_half_models),
|| build_models_from(&md_container, &top_model, layer2_model,
split_idx + 1, md_container.len(), split_idx_target,
second_half_models));

let mut sec_models = Vec::new();
sec_models.append(&mut hf1);
sec_models.append(&mut hf2);
sec_models
};



println!("[2nd layer]Computing lower bound stats...");
let lb_corrections_top = LowerBoundCorrection::new(
|x| top_model.predict_to_int(&x.to_model_input()), second_model_num, md_container
);

println!("[2nd layer]Fixing empty models...");
// replace any empty model with a model that returns the correct constant
// (for LB predictions), if the underlying model supports it.
let mut could_not_replace = false;
for idx in 0..(second_model_num as usize)-1 {
assert_eq!(lb_corrections_top.first_key(idx).is_none(),
lb_corrections_top.last_key(idx).is_none());

if lb_corrections_top.last_key(idx).is_none() {
// model is empty!
let upper_bound = lb_corrections_top.next_index(idx);
if !sec_models[idx].set_to_constant_model(upper_bound as u64) {
could_not_replace = true;
}
}
}

if could_not_replace {
warn!("[2nd layer]Some empty models could not be replaced with constants, \
negative lookup performance may be poor.");
}

md_container.set_scale(1.0);

let mut leaf_models = build_naive_3layer_models_from(&md_container, &top_model, &sec_models, layer2_model, layer3_model,
                                                    0, md_container.len(), 0,
                                                    second_model_num as usize, (num_leaf_models) as usize);

println!("[3rd layer]Computing lower bound stats...");
let lb_corrections = LowerBoundCorrection::new(
    |x| {
    let second_idx = top_model.predict_to_int(&x.to_model_input());
    let  pred_sec_layer = u64::min(second_model_num - 1, second_idx) as usize;
    let  pred_third_layer = sec_models[pred_sec_layer].predict_to_int(&x.to_model_input()) as u64;
    return pred_third_layer;
    }, num_leaf_models, md_container
);

println!("[3rd layer]Fixing empty models...");
// replace any empty model with a model that returns the correct constant
// (for LB predictions), if the underlying model supports it.
let mut could_not_replace = false;
for idx in 0..((num_leaf_models) as usize) {
assert_eq!(lb_corrections.first_key(idx).is_none(),
lb_corrections.last_key(idx).is_none());

if lb_corrections.last_key(idx).is_none() {
// model is empty!
let upper_bound = lb_corrections.next_index(idx);
if !leaf_models[idx].set_to_constant_model(upper_bound as u64) {
could_not_replace = true;
}
}
}

if could_not_replace {
warn!("[3rd layer]Some empty models could not be replaced with constants, \
negative lookup performance may be poor.");
}


println!("Computing last level errors...");
// evaluate model, compute last level errors
//set back to original scale
// md_container.set_scale(1.0);

let mut max_min_gap = vec![[0, u64::MAX] ; num_leaf_models as usize];

let mut last_layer_max_l1s = vec![(0, 0) ; num_leaf_models as usize];
for (x, y) in md_container.iter_model_input() {
let second_idx = top_model.predict_to_int(&x);
let  pred_sec_layer = u64::min(second_model_num - 1, second_idx) as usize;

let  pred_third_layer = sec_models[pred_sec_layer].predict_to_int(&x) as usize;
// let mut target = pred_third_layer + pred_sec_layer* (third_model_num as usize);
// let mut target = pred_third_layer as usize;

let target = u64::min(num_leaf_models - 1, pred_third_layer as u64) as usize;
// let target = u64::min((pred_sec_layer as u64 +1)*third_model_num - 1, pred_third_layer as u64) as usize;
// let target = u64::max((pred_sec_layer  as u64)*third_model_num, target as u64) as usize;
let pred = leaf_models[target].predict_to_int(&x);

// if ( x.as_int() > max_min_gap[target as usize][0]){
//     max_min_gap[target as usize][0] = x.as_int();
// }
// if ( x.as_int() < max_min_gap[target as usize][1]){
//     max_min_gap[target as usize][1] = x.as_int();
// }
if ( (y as u64) > max_min_gap[target as usize][0]){
max_min_gap[target as usize][0] = y as u64;
}
if ( (y as u64) < max_min_gap[target as usize][1]){
max_min_gap[target as usize][1] = y as u64;
}

// if ( y % 2000000 == 1 ){
//     println!("pred:{} y:{} x:{}, sec model:{} third model:{}",pred, y, x.as_float(), pred_sec_layer, pred_third_layer );
// }
let err = error_between(pred, y as u64, md_container.len() as u64);

let cur_val = last_layer_max_l1s[target];
last_layer_max_l1s[target] = (cur_val.0 + 1, u64::max(err, cur_val.1));
}    

let mut sum:f64 = 0.0; 
let mut avg:f64 = 0.0; 
for i__ in 0..num_leaf_models as usize {
sum +=  (max_min_gap[i__][0] - max_min_gap[i__][1]) as f64 ;
// if (i__ % 10000 == 1){
//     println!("{}: Gap Value:{} ",i__, max_min_gap[i__][0] - max_min_gap[i__][1]);
// }
}
avg = sum / num_leaf_models as f64;
println!("Average gap: {}",avg);
// for lower bound searches, we need to make sure that:
//   (1) a query for the first key in the next leaf minus one 
//       includes the key in the next leaf. (upper error)
//   (2) a query for the last key in the previous leaf plus one
//       includes the first key after the previous leaf (lower error)
//       (normally, the first key after the previous leaf is the first
//        key in this leaf, but not in the case where this leaf has no keys)
println!("Computing error value...");
let mut large_corrections = 0;
for leaf_idx in 0..num_leaf_models as usize {


let curr_err = last_layer_max_l1s[leaf_idx].1;
// println!("model idx:{} Err:{}",leaf_idx, curr_err);

let upper_error = {
let (idx_of_next, key_of_next) = lb_corrections.next(leaf_idx);
let pred = leaf_models[leaf_idx].predict_to_int(
&key_of_next.minus_epsilon().to_model_input()
);
error_between(pred, idx_of_next as u64 + 1, md_container.len() as u64)
};

let lower_error = {
let first_key_before = lb_corrections.prev_key(leaf_idx);

let prev_idx = if leaf_idx == 0 { 0 } else { leaf_idx - 1 };
let first_idx = lb_corrections.next_index(prev_idx);

let pred = leaf_models[leaf_idx].predict_to_int(
&first_key_before.plus_epsilon().to_model_input()
);
error_between(pred, first_idx as u64, md_container.len() as u64)
};


let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap()
+ lb_corrections.longest_run(leaf_idx);

let num_items_in_leaf = last_layer_max_l1s[leaf_idx].0;
last_layer_max_l1s[leaf_idx] = (num_items_in_leaf, new_err);

if new_err - curr_err > 512 && num_items_in_leaf > 100 {
large_corrections += 1;
}
}

if large_corrections > 1 {
trace!("Of {} models, {} needed large lower bound corrections.",
num_leaf_models, large_corrections);
}

trace!("Evaluating two-layer RMI...");
let (m_idx, m_err) = last_layer_max_l1s
.iter().enumerate()
.max_by_key(|(_idx, &x)| x.1).unwrap();

let model_max_error = m_err.1;
let model_max_error_idx = m_idx;

let model_avg_error: f64 = last_layer_max_l1s
.iter().map(|(n, err)| n * err).sum::<u64>() as f64 / num_rows as f64;

let model_avg_l2_error: f64 = last_layer_max_l1s
.iter()
.map(|(n, err)| ((n*err) as f64).powf(2.0) / num_rows as f64).sum::<f64>();

let model_avg_log2_error: f64 = last_layer_max_l1s
.iter().map(|(n, err)| (*n as f64)*((2*err + 2) as f64).log2()).sum::<f64>() / num_rows as f64;

let model_max_log2_error: f64 = (model_max_error as f64).log2();

let final_errors = last_layer_max_l1s.into_iter()
.map(|(_n, err)| err).collect();

return TrainedRMI {
num_rmi_rows: md_container.len(),
num_data_rows: md_container.len(),
model_avg_error,
model_avg_l2_error,
model_avg_log2_error,
model_max_error,
model_max_error_idx,
model_max_log2_error,
last_layer_max_l1s: final_errors,
third_layer_max_l1s: vec![0],
rmi: vec![vec![top_model], sec_models, leaf_models],
models: format!("{},{},{}", layer1_model, layer2_model, layer3_model),
branching_factor: num_leaf_models,
cache_fix: None,
build_time: 0
};

}



pub fn train_partial_three_layer<T: TrainingKey>(md_container: &mut RMITrainingData<T>,
                                        layer1_model: &str, layer2_model: &str,
                                        layer3_model: &str,
                                        num_leaf_models: u64) -> TrainedRMI {
    validate(&[String::from(layer1_model), String::from(layer2_model),  String::from(layer3_model)]);

    let num_rows = md_container.len();
    
    let second_model_num = num_leaf_models;

    println!("Training top-level {} model layer", layer1_model);
    md_container.set_scale(second_model_num as f64 / num_rows as f64);
    let top_model = train_model(layer1_model, &md_container);

    // Check monotonicity if in debug mode
    #[cfg(debug_assertions)]
    {
        let mut last_pred = 0;
        for (x, _y) in md_container.iter_model_input() {
            let prediction = top_model.predict_to_int(&x);
            debug_assert!(prediction >= last_pred,
                        "Top model {} was non-monotonic on input {:?}",
                        layer1_model, x);
            last_pred = prediction;
        }
        trace!("Top model was monotonic.");
    }

    println!("Training second-level {} model layer (num models = {})",
            layer2_model, second_model_num);
    // md_container.set_scale( (num_leaf_models) as f64 / num_rows as f64);
    md_container.set_scale(1.0);
    let first_idx_target = 0;
    let (mut sec_models, mut partial_3rd_idx, mut partial_3rd_lb_corrs, mut partial_3rd_models, third_layer_num) = 
        build_partial_3layer_models_from(&md_container, &top_model, layer2_model,layer3_model,
            0, md_container.len(), first_idx_target,
            second_model_num as usize);

    println!("[2nd layer]Computing lower bound stats...");
    let lb_corrections = LowerBoundCorrection::new(
     |x| top_model.predict_to_int(&x.to_model_input()), second_model_num, md_container
    );

    println!("[2nd layer]Fixing empty models...");
    // replace any empty model with a model that returns the correct constant
    // (for LB predictions), if the underlying model supports it.
    let mut could_not_replace = false;
    for idx in 0..(second_model_num as usize) {
         assert_eq!(lb_corrections.first_key(idx).is_none(),
         lb_corrections.last_key(idx).is_none());

        if lb_corrections.last_key(idx).is_none() {
        // model is empty!
            let upper_bound = lb_corrections.next_index(idx);
            if !sec_models[idx].set_to_constant_model(upper_bound as u64) {
                could_not_replace = true;
            }
         }
    }
    if could_not_replace {
        warn!("[2nd layer]Some empty models could not be replaced with constants, \
            negative lookup performance may be poor.");
        assert!(false);
    }
    println!("Computing last level errors...");
    // evaluate model, compute last level errors

    // let mut max_min_gap: Vec<(u64, u64)> = vec![[0, u64::MAX] ; num_leaf_models as usize];

    let mut last_layer_max_l1s = vec![(0, 0) ; num_leaf_models as usize];
    let mut third_layer_max_l1s = vec![(0,0) ; third_layer_num as usize];

    for (x, y) in md_container.iter_model_input() {
        let leaf_idx = top_model.predict_to_int(&x);
        let target = u64::min(num_leaf_models - 1, leaf_idx) as usize;
        
        let mut pred;
        if (partial_3rd_idx[target] == (0,0)){
            pred = sec_models[target].predict_to_int(&x);
            let err = error_between(pred, y as u64, md_container.len() as u64);
            let cur_val = last_layer_max_l1s[target];
            // last_layer_max_l1s[target] = (cur_val.0 + 1, (u64::max(err, cur_val.1)<<32)>>32 );
            // using 32bit should be enough to express error value
            // if not cannot build partial 3 layer, increase model number to lower error 
            // assert!((u64::max(err, cur_val.1)<<32)>>32 == u64::max(err, cur_val.1));

            // cur_val stores minimum error (can be minus value) and maximum error (can be minus also)
            // first 32bit is for minimum err, next 32bit for maximum error
            // first bit is used for sign, 1 for minus, 0 for plus

            let mut min_err = (cur_val.1  >> 32)& 0x000000003fffffffu64 ;//left most bit is used for partial model representation
            let mut min_flag = ((cur_val.1  >> 32)& 0x0000000040000000u64)>>30 ;
            let mut max_err = (cur_val.1 )& 0x000000007fffffffu64 ;
            let mut max_flag = ((cur_val.1 )& 0x0000000080000000u64)>>31 ;
            
            if pred > y as u64 { // err is minus err
                if min_err < err || min_flag==0 {
                    min_err = err;
                    min_flag = 1;
                }
                if max_err > err && max_flag==1 {
                    max_err = err;
                    max_flag = 1;
                }
            }else{ // pred is smaller than y , plus err
                if min_err > err && min_flag ==0 {
                    min_err = err;
                    min_flag = 0;
                }
                if max_err < err || max_flag ==1 {
                    max_err = err;
                    max_flag = 0;
                }
            }
            
            last_layer_max_l1s[target] = (cur_val.0 + 1, min_flag<<62| min_err<<32 | max_flag<<31 | max_err );
            assert!(last_layer_max_l1s[target].1>>63 == 0); // flag for partial model should be 0
        }
        else{
            //partial_3rd_idx.0 have start of 3rd model list, partial_3rd_idx.1 has number of models 
            let mut target_third = sec_models[target].predict_to_int(&x) + partial_3rd_idx[target].0 as u64;
            target_third = u64::min((partial_3rd_idx[target].0 + partial_3rd_idx[target].1 - 1) as u64, target_third );
            target_third = u64::max(partial_3rd_idx[target].0 as u64, target_third);
            pred = partial_3rd_models[target_third as usize].predict_to_int(&x);
            
            let cur_val = last_layer_max_l1s[target];
            // put number of cumulative partial models in 32 most significant bits, number of models in 32 least significant bits
            // first bit is set as 1 if partial model is used
            assert!(partial_3rd_idx[target].0 as u64 <= 0x000000007fffffffu64);
            assert!(partial_3rd_idx[target].1 as u64<= 0x00000000ffffffffu64);
            last_layer_max_l1s[target] = (cur_val.0 + 1, ((partial_3rd_idx[target].0 as u64 | 0x0000000080000000u64)<<32)|partial_3rd_idx[target].1 as u64);
            
            let err = error_between(pred, y as u64, md_container.len() as u64);
            let cur_val = third_layer_max_l1s[target_third as usize];
            // third_layer_max_l1s[target_third as usize ] = (cur_val.0 +1 , u64::max(err, cur_val.1) );
            
            let mut min_err = (cur_val.1  >> 32)& 0x000000003fffffffu64 ;//left most bit is used for partial model representation
            let mut min_flag = ((cur_val.1  >> 32)& 0x0000000040000000u64)>>30 ;
            let mut max_err = (cur_val.1 )& 0x000000007fffffffu64 ;
            let mut max_flag = ((cur_val.1 )& 0x0000000080000000u64)>>31 ;
            
            if pred > y as u64{ // err is minus err
                if min_err < err || min_flag==0 {
                    min_err = err;
                    min_flag = 1;
                }
                if max_err > err && max_flag==1 {
                    max_err = err;
                    max_flag = 1;
                }
            }else{ // pred is smaller than y , plus err
                if min_err > err && min_flag ==0 {
                    min_err = err;
                    min_flag = 0;
                }
                if max_err < err || max_flag ==1 {
                    max_err = err;
                    max_flag = 0;
                }
            }
            third_layer_max_l1s[target_third as usize ] = (cur_val.0 + 1, min_flag<<62| min_err<<32 | max_flag<<31 | max_err );
        }

        // if ( (y as u64) > max_min_gap[target as usize][0]){
        //     max_min_gap[target as usize][0] = y as u64;
        // }
        // if ( (y as u64) < max_min_gap[target as usize][1]){
        //     max_min_gap[target as usize][1] = y as u64;
        // }
        
    }    

    let mut sum:f64 = 0.0; 
    let mut avg:f64 = 0.0; 
    // for i__ in 0..num_leaf_models as usize {
    //     sum +=  (max_min_gap[i__][0] - max_min_gap[i__][1]) as f64 ;
    // }
    // avg = sum / (num_leaf_models+third_layer_num as u64 -partial_3rd_lb_corrs.len() as u64) as f64;
    // println!("Average gap: {}",avg);
    
    // for lower bound searches, we need to make sure that:
    //   (1) a query for the first key in the next leaf minus one 
    //       includes the key in the next leaf. (upper error)
    //   (2) a query for the last key in the previous leaf plus one
    //       includes the first key after the previous leaf (lower error)
    //       (normally, the first key after the previous leaf is the first
    //        key in this leaf, but not in the case where this leaf has no keys)
    let mut large_corrections = 0;
    let mut partial_third_num = 0;
    let report_error_threshold = 100000;
    for leaf_idx in 0..num_leaf_models as usize {
        if (partial_3rd_idx[leaf_idx] != (0,0)){
            for third_idx in 0..partial_3rd_idx[leaf_idx].1{
                let curr_err = third_layer_max_l1s[third_idx+ partial_3rd_idx[leaf_idx].0].1;

                let mut min_err = (curr_err >> 32)& 0x000000003fffffffu64 ;//left most bit is used for partial model representation
                let mut min_flag = ((curr_err  >> 32)& 0x0000000040000000u64)>>30 ;
                let mut max_err =curr_err& 0x000000007fffffffu64 ;
                let mut max_flag = (curr_err& 0x0000000080000000u64)>>31 ;


                let mut upper_flag = false;
                let mut lower_flag = false;
                let upper_error = {
                    // if third_idx == partial_3rd_idx[leaf_idx].1-1 {
                    if third_idx >= partial_3rd_lb_corrs[partial_third_num].last_non_empty_model() as usize {
                        let (idx_of_next, key_of_next) = lb_corrections.next(leaf_idx);
                        let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                            &key_of_next.minus_epsilon().to_model_input()
                        );
                        
                        upper_flag= pred>idx_of_next as u64;
                        error_between(pred, idx_of_next as u64 + 1, md_container.len() as u64)
                    }else{
                        let (idx_of_next, key_of_next) = partial_3rd_lb_corrs[partial_third_num].next(third_idx);
                        let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                            &key_of_next.minus_epsilon().to_model_input()
                        );
                        
                        upper_flag= pred>idx_of_next as u64;
                        error_between(pred, idx_of_next as u64 + 1, md_container.len() as u64)
                    }
                };
                let lower_error = {
                    let mut first_key_before ;
                    let mut prev_idx ;
                    let mut first_idx ;
                    // if empty models 
                    if third_idx <= partial_3rd_lb_corrs[partial_third_num].first_non_empty_model() as usize {
                        first_key_before = lb_corrections.prev_key(leaf_idx);
                        // prev_idx = if third_idx == 0 { 0 } else { third_idx - 1 };
                        // first_idx = partial_3rd_lb_corrs[partial_third_num].next_index(prev_idx);
                        prev_idx = if leaf_idx == 0 { 0 } else {  leaf_idx - 1  };
                        first_idx = lb_corrections.next_index(prev_idx);
                        if leaf_idx == 0 {
                            prev_idx = if third_idx == 0 { 0 } else { third_idx - 1 };
                            first_idx = partial_3rd_lb_corrs[partial_third_num].next_index(prev_idx);
                        }

                    }else{
                        first_key_before = partial_3rd_lb_corrs[partial_third_num].prev_key(third_idx);
                        prev_idx = if third_idx == 0 { 0 } else { third_idx - 1 };
                        first_idx = partial_3rd_lb_corrs[partial_third_num].next_index(prev_idx);
                        if partial_3rd_lb_corrs[partial_third_num].first_non_empty_model() == 0 && partial_3rd_lb_corrs[partial_third_num].first_non_empty_model() == partial_3rd_lb_corrs[partial_third_num].last_non_empty_model() {
                            prev_idx = if leaf_idx == 0 { 0 } else {  leaf_idx - 1  };
                            first_idx = lb_corrections.next_index(prev_idx);    
                        }
                    }
                    
                    let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                        &first_key_before.plus_epsilon().to_model_input()
                    );
                    
                    lower_flag = pred > first_idx as u64;
                    error_between(pred, first_idx as u64, md_container.len() as u64)
                };

                if upper_error > report_error_threshold {
                    if third_idx >= partial_3rd_lb_corrs[partial_third_num].last_non_empty_model() as usize {
                        let (idx_of_next, key_of_next) = lb_corrections.next(leaf_idx);
                        let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                            &key_of_next.minus_epsilon().to_model_input()
                        );
                        println!("[upper-last]pred: {} idxnext:{} keyofnext:{} upper_error:{}", pred, idx_of_next, &key_of_next.as_uint(), upper_error);
                        let (idx_of_next_, key_of_next_) = partial_3rd_lb_corrs[partial_third_num].next(third_idx);
                        println!("[third models upper-last]pred: {} idxnext:{} keyofnext:{}",pred,idx_of_next_, &key_of_next_.as_uint());
                    }
                    else{
                        let (idx_of_next, key_of_next) = partial_3rd_lb_corrs[partial_third_num].next(third_idx);
                        let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                        &key_of_next.minus_epsilon().to_model_input()
                        );
                        println!("[upper]pred: {} idxnext:{} keyofnext:{} upper_error:{}", pred, idx_of_next, &key_of_next.as_uint(), upper_error);
                    }
                    
                }
                if lower_error > report_error_threshold {
                    let mut first_key_before ;
                    let mut prev_idx ;
                    let mut first_idx ;

                    if third_idx <= partial_3rd_lb_corrs[partial_third_num].first_non_empty_model() as usize {
                        first_key_before = lb_corrections.prev_key(leaf_idx);
                        // prev_idx = if third_idx == 0 { 0 } else { third_idx - 1 };
                        // first_idx = partial_3rd_lb_corrs[partial_third_num].next_index(prev_idx);
                        prev_idx = if leaf_idx == 0 { 0 } else {  leaf_idx - 1  };
                        first_idx = lb_corrections.next_index(prev_idx);
                        if leaf_idx == 0 {
                            prev_idx = if third_idx == 0 { 0 } else { third_idx - 1 };
                            first_idx = partial_3rd_lb_corrs[partial_third_num].next_index(prev_idx);
                        }
                        let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                            &first_key_before.plus_epsilon().to_model_input()
                        );
                        println!("[lower-last]pred: {} first_idx:{} first_key_before:{} prev_idx:{} lower_error:{}", pred, first_idx, &first_key_before.as_uint(), prev_idx, lower_error);
                    }else{
                        first_key_before = partial_3rd_lb_corrs[partial_third_num].prev_key(third_idx);
                        prev_idx = if third_idx == 0 { 0 } else { third_idx - 1 };
                        // first_idx is index of first key which goes into next model
                        first_idx = partial_3rd_lb_corrs[partial_third_num].next_index(prev_idx);
                        if partial_3rd_lb_corrs[partial_third_num].first_non_empty_model() == 0 && partial_3rd_lb_corrs[partial_third_num].first_non_empty_model() == partial_3rd_lb_corrs[partial_third_num].last_non_empty_model() {
                            prev_idx = if leaf_idx == 0 { 0 } else {  leaf_idx - 1  };
                            first_idx = lb_corrections.next_index(prev_idx);    
                        }
                        let pred = partial_3rd_models[third_idx+partial_3rd_idx[leaf_idx].0].predict_to_int(
                            &first_key_before.plus_epsilon().to_model_input()
                        );
                        println!("[lower]pred: {} first_idx:{} first_key_before:{} prev_key:{} prev_idx:{} lower_error:{}", pred, first_idx, &first_key_before.as_uint(),partial_3rd_lb_corrs[partial_third_num].prev(third_idx), prev_idx, lower_error);
                    }

                }
                

                // let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap()
                //     + partial_3rd_lb_corrs[partial_third_num].longest_run(third_idx);
                
                if upper_flag { // err is minus err
                    if min_err < upper_error || min_flag==0 {
                        min_err = upper_error;
                        min_flag = 1;
                    }
                    if max_err > upper_error && max_flag==1 {
                        max_err = upper_error;
                        max_flag = 1;
                    }
                }else{ // pred is smaller than y , plus err
                    if min_err > upper_error && min_flag ==0 {
                        min_err = upper_error;
                        min_flag = 0;
                    }
                    if max_err < upper_error || max_flag ==1 {
                        max_err = upper_error;
                        max_flag = 0;
                    }
                }

                if lower_flag { // err is minus err
                    if min_err < lower_error || min_flag==0 {
                        min_err = lower_error;
                        min_flag = 1;
                    }
                    if max_err > lower_error && max_flag==1 {
                        max_err = lower_error;
                        max_flag = 1;
                    }
                }else{ // pred is smaller than y , plus err
                    if min_err > lower_error && min_flag ==0 {
                        min_err = lower_error;
                        min_flag = 0;
                    }
                    if max_err < lower_error || max_flag ==1 {
                        max_err = lower_error;
                        max_flag = 0;
                    }
                }
                let num_items_in_leaf = third_layer_max_l1s[third_idx+ partial_3rd_idx[leaf_idx].0].0;

                // let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap();
                // if new_err > report_error_threshold {
                //     if third_idx>1 {
                //         println!("[FYI] Front model elem num:{}", third_layer_max_l1s[third_idx-1+ partial_3rd_idx[leaf_idx].0].0);
                //     }
                    
                //     println!("leaf[{}/{}] partial_model:{}/{} elem num:{}/{} new_err:{} curr err:{} first non empty:{} last non empty:{} partial_third_num:{}\n",
                //     leaf_idx,num_leaf_models, third_idx, partial_3rd_idx[leaf_idx].1-1, third_layer_max_l1s[third_idx+ partial_3rd_idx[leaf_idx].0].0,last_layer_max_l1s[leaf_idx].0,  new_err, curr_err, partial_3rd_lb_corrs[partial_third_num].first_non_empty_model(),partial_3rd_lb_corrs[partial_third_num].last_non_empty_model() ,partial_third_num);
                // }
                // if new_err - curr_err > 512 && num_items_in_leaf > 100 {
                //     large_corrections += 1;
                // }
                // assert!(curr_err<=0x7fffffff);
                // assert!(new_err<=0xffffffff);
                // third_layer_max_l1s[third_idx+ partial_3rd_idx[leaf_idx].0] = (num_items_in_leaf, new_err | curr_err<<32 );

                third_layer_max_l1s[third_idx+ partial_3rd_idx[leaf_idx].0] = (num_items_in_leaf, min_flag<<62| min_err<<32 | max_flag<<31 | max_err );
                // third_layer_max_l1s[third_idx+ partial_3rd_idx[leaf_idx].0] = (num_items_in_leaf, upper_error | lower_error<<32 );
                
                
            }
            partial_third_num += 1;
            
            continue;
        }

        let curr_err = last_layer_max_l1s[leaf_idx].1;
        // println!("model idx:{} Err:{}",leaf_idx, curr_err);
        let mut min_err = (curr_err >> 32)& 0x000000003fffffffu64 ;//left most bit is used for partial model representation
        let mut min_flag = ((curr_err  >> 32)& 0x0000000040000000u64)>>30 ;
        let mut max_err =curr_err& 0x000000007fffffffu64 ;
        let mut max_flag = (curr_err& 0x0000000080000000u64)>>31 ;

        let mut upper_flag = false;
        let mut lower_flag = false;

        let upper_error = {
            let (idx_of_next, key_of_next) = lb_corrections.next(leaf_idx);
            let pred = sec_models[leaf_idx].predict_to_int(
                &key_of_next.minus_epsilon().to_model_input()
            );
            upper_flag = pred > idx_of_next as u64;
            error_between(pred, idx_of_next as u64 + 1, md_container.len() as u64)
        };
        
        let lower_error = {
            let first_key_before = lb_corrections.prev_key(leaf_idx);

            let prev_idx = if leaf_idx == 0 { 0 } else { leaf_idx - 1 };
            let first_idx = lb_corrections.next_index(prev_idx);

            let pred = sec_models[leaf_idx].predict_to_int(
                &first_key_before.plus_epsilon().to_model_input()
            );
            lower_flag = pred > first_idx as u64;
            error_between(pred, first_idx as u64, md_container.len() as u64)
        };
          
        
        // let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap()
        //     + lb_corrections.longest_run(leaf_idx);
        if upper_flag { // err is minus err
            if min_err < upper_error || min_flag==0 {
                min_err = upper_error;
                min_flag = 1;
            }
            if max_err > upper_error && max_flag==1 {
                max_err = upper_error;
                max_flag = 1;
            }
        }else{ // pred is smaller than y , plus err
            if min_err > upper_error && min_flag ==0 {
                min_err = upper_error;
                min_flag = 0;
            }
            if max_err < upper_error || max_flag ==1 {
                max_err = upper_error;
                max_flag = 0;
            }
        }

        if lower_flag { // err is minus err
            if min_err < lower_error || min_flag==0 {
                min_err = lower_error;
                min_flag = 1;
            }
            if max_err > lower_error && max_flag==1 {
                max_err = lower_error;
                max_flag = 1;
            }
        }else{ // pred is smaller than y , plus err
            if min_err > lower_error && min_flag ==0 {
                min_err = lower_error;
                min_flag = 0;
            }
            if max_err < lower_error || max_flag ==1 {
                max_err = lower_error;
                max_flag = 0;
            }
        }
        let num_items_in_leaf = last_layer_max_l1s[leaf_idx].0;
        
        // let new_err = *(&[curr_err, upper_error, lower_error]).iter().max().unwrap();
        // assert!(curr_err<=0x7fffffff);
        // assert!(new_err<=0xffffffff);
        // last_layer_max_l1s[leaf_idx] = (num_items_in_leaf, new_err | curr_err<<32);
        // if new_err - curr_err > 512 && num_items_in_leaf > 100 {
        //     large_corrections += 1;
        // }

        last_layer_max_l1s[leaf_idx] = (num_items_in_leaf,  min_flag<<62| min_err<<32 | max_flag<<31 | max_err);

        
    }


    if large_corrections > 1 {
        trace!("Of {} models, {} needed large lower bound corrections.",
            num_leaf_models, large_corrections);
    }

    trace!("Evaluating Second layer of RMI...");

    // print total leaf-models, partial models, 
    // println!("Total Partial model num: {}, Leaf of partial model num: {}",third_layer_num, partial_3rd_lb_corrs.len());
    println!("[INFO] Number of leaf and partial models: {}, leaf: {}, partial: {}, leaf models that have partial models:{}",(num_leaf_models as usize +third_layer_num as usize  ), num_leaf_models, third_layer_num, partial_3rd_lb_corrs.len() );
    
    // let mut new_last_layer_max_l1s = vec![(0, 0) ; (num_leaf_models as usize +third_layer_num as usize -partial_3rd_lb_corrs.len() as usize ) as usize];

    // let mut idx_ = 0;
    // for (num_key, err) in &last_layer_max_l1s {
    //     if err & 0x8000000000000000u64 != 0 {
    //         continue;
    //     }
    //     else{
    //         if *num_key != 0{
    //             // println!("[2nd layer]num: {} err:{}", *num_key, *err);
    //         }
    //         new_last_layer_max_l1s[idx_].0 = *num_key;
    //         // new_last_layer_max_l1s[idx_].1 = *err & 0xffffffff;
    //         new_last_layer_max_l1s[idx_].1 = (*err & 0x7fffffff) + ((*err >> 32) & 0x3fffffff);
    //         idx_ += 1;
    //         continue
    //     }
    // }
    
    // println!("Partial start at idx:{}",idx_);
    // for (num_key_3, err_3) in &third_layer_max_l1s{
    //     new_last_layer_max_l1s[idx_].0 = *num_key_3;
    //     // new_last_layer_max_l1s[idx_].1 = *err_3 & 0xffffffff;
    //     new_last_layer_max_l1s[idx_].1 = (*err_3 & 0x7fffffff) + ((*err_3 >> 32) & 0x3fffffff);
    //     if *num_key_3 != 0{
    //         // println!("[3rd layer]num: {} err:{}", *num_key_3, *err_3);
    //     }
    //     idx_+= 1;
    // }

    // let (m_idx, m_err) = new_last_layer_max_l1s
    //     .iter().enumerate()
    //     .max_by_key(|(_idx, &x)| x.1).unwrap();

    // let model_max_error = m_err.1;
    // let model_max_error_idx = m_idx;

    // let model_avg_error: f64 = new_last_layer_max_l1s
    //   .iter().map(|(n, err)| n * err).sum::<u64>() as f64 / num_rows as f64;

    // let model_avg_l2_error: f64 = new_last_layer_max_l1s
    //     .iter()
    //     .map(|(n, err)| ((n*err) as f64).powf(2.0) / num_rows as f64).sum::<f64>();

    // // let model_avg_log2_error: f64 = new_last_layer_max_l1s
    // //     .iter().map(|(n, err)| (*n as f64)*((2*err + 2) as f64).log2()).sum::<f64>() / num_rows as f64;
    // let model_avg_log2_error: f64 = new_last_layer_max_l1s
    //     .iter().map(|(n, err)| (*n as f64)*((err + 2) as f64).log2()).sum::<f64>() / num_rows as f64;

    // let model_max_log2_error: f64 = (model_max_error as f64).log2();

    let (m_idx, m_err) = last_layer_max_l1s
        .iter().enumerate()
        .max_by_key(|(_idx, &x)| (x.1 & 0x7fffffff + ((x.1 >> 32) & 0x3fffffff) )).unwrap();

    let model_max_error = (m_err.1 & 0x7fffffff) + ((m_err.1 >> 32) & 0x3fffffff);
    let model_max_error_idx = m_idx;

    let model_avg_error: f64 = last_layer_max_l1s
      .iter().map(|(n, err)| n * ( (err& 0x7fffffff) + ((*err >> 32) & 0x3fffffff)) ).sum::<u64>() as f64 / num_rows as f64;

    let model_avg_l2_error: f64 = last_layer_max_l1s
        .iter()
        .map(|(n, err)| ( ((err& 0x7fffffff) + ((*err >> 32) & 0x3fffffff)) as f64).powf(2.0) / num_rows as f64).sum::<f64>();

    // let model_avg_log2_error: f64 = new_last_layer_max_l1s
    //     .iter().map(|(n, err)| (*n as f64)*((2*err + 2) as f64).log2()).sum::<f64>() / num_rows as f64;
    let model_avg_log2_error: f64 = last_layer_max_l1s
        .iter().map(|(n, err)| (*n as f64)*((( (err& 0x7fffffff) + ((*err >> 32) & 0x3fffffff)) + 2) as f64).log2()).sum::<f64>() / num_rows as f64;

    let model_max_log2_error: f64 = (model_max_error as f64).log2();

    let final_errors = last_layer_max_l1s.into_iter()
         .map(|(_n, err)| err).collect();

    let final_third_errors = third_layer_max_l1s.into_iter()
         .map(|(_n, err)| err).collect();

    let mut rmi;
    if third_layer_num > 0 {
        rmi = vec![vec![top_model], partial_3rd_models, sec_models];
    }
    else{
        let dummy_model = train_model("pwl", &md_container);
        rmi = vec![vec![top_model],vec![dummy_model], sec_models];
    }

    return TrainedRMI {
        num_rmi_rows: md_container.len(),
        num_data_rows: md_container.len(),
        model_avg_error,
        model_avg_l2_error,
        model_avg_log2_error,
        model_max_error,
        model_max_error_idx,
        model_max_log2_error,
        last_layer_max_l1s: final_errors,
        third_layer_max_l1s: final_third_errors,
        rmi: rmi,
        models: format!("{},{},{}", layer1_model, layer3_model, layer2_model),
        branching_factor: num_leaf_models,
        cache_fix: None,
        build_time: 0
    };

}

