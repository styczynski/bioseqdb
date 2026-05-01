//! Experiments with parallel processing
//!
//! The provided functions focus on the possibility of
//! returning results while the parser proceeds. Sequences are processesd in
//! batches (`RecordSet`) because sending across channels has a performance
//! impact. FASTA/FASTQ records can be accessed in both the 'worker' function and
//! (after processing) a function running in the main thread.
//!
//! # Search first occurrence of a sequence pattern
//!
//! ```no_run
//! use seq_io::fastq::{Reader,Record};
//! use seq_io::parallel::read_parallel;
//!
//! let reader = Reader::from_path("seqs.fastq").unwrap();
//!
//! read_parallel(reader, 4, 2, |record_set| {
//!     // this function does the heavy work
//!     for (i, record) in record_set.into_iter().enumerate() {
//!         // this is not very efficient code, just for demonstration
//!         if let Some(pos) = record.seq().windows(3).position(|s| s == b"AAA") {
//!             return Some((i, pos));
//!         }
//!     }
//!     None
//! }, |record_sets| {
//!     // This function runs in the main thread. It provides a streaming iterator over
//!     // record sets and the corresponding return values from the worker function
//!     // (not necessarily in the same order as in the file)
//!     while let Some(result) = record_sets.next() {
//!         let (record_set, found) = result.unwrap();
//!         if let Some((i, pos)) = found {
//!             let record = record_set.into_iter().nth(i).unwrap();
//!             println!("Found AAA in record {} at position {}", record.id().unwrap(), pos);
//!              // this will also stop the worker threads, although with some delay
//!             return;
//!         }
//!     }
//! });
//! ```
//!
//! # Per-record processsing
//! The `parallel_fasta` / `parallel_fastq` functions are designed to efficiently pass
//! results for **each record** to the main thread without having to care about record sets.
//! This example filters sequences by the occurrence of a pattern:
//!
//! ```no_run
//! use seq_io::fastq::{Reader,Record};
//! use seq_io::parallel::parallel_fastq;
//! use std::fs::File;
//! use std::io::BufWriter;
//!
//! let reader = Reader::from_path("seqs.fastq").unwrap();
//! let mut writer = BufWriter::new(File::create("filtered.fastq").unwrap());
//!
//! parallel_fastq(reader, 4, 2,
//!     |record, found| { // runs in worker
//!         *found = record.seq().windows(3).position(|s| s == b"AAA").is_some();
//!     },
//!     |record, found| { // runs in main thread
//!         if *found {
//!             record.write(&mut writer).unwrap();
//!         }
//!         // Some(value) will stop the reader, and the value will be returned.
//!         // In the case of never stopping, we need to give the compiler a hint about the
//!         // type parameter, thus the special 'turbofish' notation is needed,
//!         // hoping on progress here: https://github.com/rust-lang/rust/issues/27336
//!         None::<()>
//! }).unwrap();
//! ```

use std::io;

use std::marker::PhantomData;
use std::sync::mpsc;

pub trait Reader {
    type DataSet: Send;
    type Err: Send;
    fn fill_data(&mut self, record: &mut Self::DataSet) -> Option<Result<(), Self::Err>>;
}

pub fn read_parallel<R, O, W, F, Out>(
    reader: R,
    n_threads: u32,
    queue_len: usize,
    work: W,
    func: F,
) -> Out
where
    R: Reader + Send,
    R::DataSet: Default,
    O: Default + Send,
    W: Send + Sync + Fn(&mut R::DataSet) -> O,
    F: FnMut(&mut ParallelRecordsets<R::DataSet, R::Err, O>) -> Out,
{
    read_parallel_init::<_, (), _, (), _, _, (), _, _, Out>(
        n_threads,
        queue_len,
        || Ok::<_, ()>(reader),
        || Ok::<_, ()>(R::DataSet::default()),
        work,
        func,
    )
    .unwrap()
}

/// This function allows initiating the reader and datasets using a closure.
/// This is more flexible and allows readers not to be `Send`
pub fn read_parallel_init<R, E, Ri, Er, O, Di, Ed, W, F, Out>(
    n_threads: u32,
    queue_len: usize,
    reader_init: Ri,
    mut dataset_init: Di,
    work: W,
    func: F,
) -> Result<Out, E>
where
    R: Reader,
    Ri: Send + FnOnce() -> Result<R, Er>,
    Er: Send,
    E: From<Er> + From<Ed>,
    O: Send,
    Di: Send + Sync + FnMut() -> Result<R::DataSet, Ed>,
    W: Send + Sync + Fn(&mut R::DataSet) -> O,
    F: FnOnce(&mut ParallelRecordsets<R::DataSet, R::Err, O>) -> Out,
{
    let (done_send, done_recv) = mpsc::sync_channel(queue_len);
    let (empty_send, empty_recv): (mpsc::SyncSender<R::DataSet>, _) = mpsc::sync_channel(queue_len);

    crossbeam_utils::thread::scope(|scope| {
        let handle = scope.spawn::<_, Result<(), Er>>(move |_| {
            let mut reader = reader_init()?;

            let mut pool = scoped_threadpool::Pool::new(n_threads);

            pool.scoped(|pool_scope| {
                let work = &work;

                loop {
                    // recycle an old DataSet sent back after use
                    let mut data = if let Ok(r) = empty_recv.recv() {
                        r
                    } else {
                        // ParallelRecordsets dropped -> stop
                        return;
                    };

                    let done_send = done_send.clone();

                    if let Some(res) = reader.fill_data(&mut data) {
                        match res {
                            Ok(_) => {
                                // expensive work carried out by func()
                                pool_scope.execute(move || {
                                    let out = work(&mut data);

                                    done_send.send(Some(Ok((data, out)))).ok();
                                });
                            }
                            Err(e) => {
                                done_send.send(Some(Err(e))).ok();
                                break;
                            }
                        }
                    } else {
                        break;
                    }
                }

                pool_scope.join_all();

                done_send.send(None).ok();
            });
            Ok(())
        });

        for _ in 0..queue_len {
            if empty_send.send(dataset_init()?).is_err() {
                break;
            }
        }

        let mut rsets = ParallelRecordsets {
            empty_send,
            done_recv,
            current_recordset: dataset_init()?,
        };

        let out = func(&mut rsets);
        ::std::mem::drop(rsets);

        handle.join().unwrap()?;
        Ok(out)
    })
    .unwrap()
}

pub struct ParallelRecordsets<R, E, O>
where
    R: Send,
    E: Send,
    O: Send,
{
    empty_send: mpsc::SyncSender<R>,
    done_recv: mpsc::Receiver<Option<Result<(R, O), E>>>,
    current_recordset: R,
}

impl<R, E, O> ParallelRecordsets<R, E, O>
where
    R: Send,
    E: Send,
    O: Send,
{
    #[allow(clippy::should_implement_trait)]
    #[inline]
    pub fn next(&mut self) -> Option<Result<(&mut R, O), E>> {
        self.done_recv.recv().unwrap().map(move |result| {
            match result {
                Ok((r, o)) => {
                    let prev_rset = ::std::mem::replace(&mut self.current_recordset, r);
                    self.empty_send.send(prev_rset).ok(); // error: channel closed is not a problem, happens after calling stop()
                    Ok((&mut self.current_recordset, o))
                }
                Err(e) => Err(e),
            }
        })
    }
}

#[macro_export]
macro_rules! parallel_record_impl {
    ($name:ident, $name_init:ident, $io_r:tt, $rdr:ty, $dataset:ty, $record:ty, $err:ty) => {
        /// Function reading records in a different thread.
        /// processing them in another worker thread
        /// and finally returning the results to the main thread.
        ///
        /// The output is passed around between threads, allowing
        /// allocations to be 'recycled'. This also means, that the
        /// data must implement `Default`, and data handled to the 'work'
        /// function will receive 'old' data from earlier records which
        /// has to be overwritten.
        pub fn $name<$io_r, D, W, F, Out>(
            reader: $rdr,
            n_threads: u32,
            queue_len: usize,
            work: W,
            mut func: F,
        ) -> Result<Option<Out>, $err>
        where
            $io_r: io::Read + Send,
            D: Default + Send,
            W: Send + Sync + Fn($record, &mut D),
            F: FnMut($record, &mut D) -> Option<Out>,
        {
            $name_init(
                n_threads,
                queue_len,
                || Ok::<_, $err>(reader),
                || Ok::<_, $err>(D::default()),
                || Ok::<_, $err>(()),
                |record, record_out, _| work(record, record_out),
                |record, record_out, _| func(record, record_out),
            )
        }

        /// More customisable function doing per-record processing with
        /// closures for initialization and moer options.
        ///
        /// The reader is lazily initialized in a closure (`reader_init`) and therefore does not
        /// need to implement `Send`. There is also an initializer for the output data
        /// for each record, therefore the type is not required to implement.
        /// `Default` (`record_data_init`). Finally, each record set can have
        /// its own data (kind of thread local data, but actually passed around
        /// with the record set) (`rset_data_init`).
        pub fn $name_init<Ri, E, $io_r, Er, Di, D, Ed, Si, S, Es, W, F, Out>(
            n_threads: u32,
            queue_len: usize,
            reader_init: Ri,
            record_data_init: Di,
            rset_data_init: Si,
            work: W,
            mut func: F,
        ) -> Result<Option<Out>, E>
        where
            $io_r: io::Read,
            Ri: Send + FnOnce() -> Result<$rdr, Er>,
            Er: Send,
            Ed: Send,
            E: From<$err> + From<Er> + From<Ed> + From<Es>,
            Di: Fn() -> Result<D, Ed> + Send + Sync,
            D: Send,
            Si: Fn() -> Result<S, Es> + Send + Sync,
            S: Send,
            W: Send + Sync + Fn($record, &mut D, &mut S),
            F: FnMut($record, &mut D, &mut S) -> Option<Out>,
        {
            $crate::parallel::read_parallel_init::<_, E, _, _, _, _, Es, _, _, _>(
                n_threads,
                queue_len,
                || reader_init().map($crate::parallel::ReusableReader::<$rdr, (Vec<D>, S)>::new),
                || rset_data_init().map(|d| (<$dataset>::default(), (vec![], d))),
                |&mut (ref mut recordset, (ref mut out, ref mut rset_data))| {
                    let mut record_iter = recordset.into_iter();
                    //let &mut (ref mut out, ref mut rset_data): &mut (Vec<D>, Option<S>) = &mut d.1;
                    for mut d in out.iter_mut().zip(&mut record_iter) {
                        work(d.1, &mut d.0, rset_data);
                    }
                    for record in record_iter {
                        out.push(record_data_init()?);
                        work(record, out.last_mut().unwrap(), rset_data);
                    }
                    Ok::<_, Ed>(())
                },
                |records| {
                    while let Some(result) = records.next() {
                        let (r, res) = result?;
                        res?;
                        let &mut (ref records, (ref mut out, ref mut rset_data)) = r;
                        for x in records.into_iter().zip(out.iter_mut()) {
                            if let Some(out) = func(x.0, x.1, rset_data) {
                                return Ok(Some(out));
                            }
                        }
                    }
                    Ok(None)
                },
            )?
        }
    };
}

parallel_record_impl!(
    parallel_fasta,
    parallel_fasta_init,
    R,
    fasta::Reader<R>,
    fasta::RecordSet,
    fasta::RefRecord,
    fasta::Error
);

parallel_record_impl!(
    parallel_fastq,
    parallel_fastq_init,
    R,
    fastq::Reader<R>,
    fastq::RecordSet,
    fastq::RefRecord,
    fastq::Error
);

/// Wrapper for `parallel::Reader` instances allowing
/// the output to be reused in order to save allocations.
/// Used by `parallel_fasta`/`parallel_fastq`
///
/// ```no_run
/// use seq_io::fastq::{Reader,Record,RecordSet};
/// use seq_io::parallel::{read_parallel,ReusableReader};
///
/// let inner = Reader::from_path("seqs.fastq").unwrap();
/// let reader = ReusableReader::new(inner);
///
/// read_parallel(reader, 4, 2, |&mut (ref record_set, ref mut out): &mut (RecordSet, Vec<bool>)| {
///     out.clear();
///     for record in record_set {
///         let found = record.seq().windows(3).position(|s| s == b"AAA").is_some();
///         out.push(found);
///     }
/// }, |record_sets| {
///     while let Some(result) = record_sets.next() {
///         let &(ref record_set, ref out) = &*result.unwrap().0;
///         for (record, found) in record_set.into_iter().zip(out) {
///             // ...
///         }
///     }
/// });
/// ```
pub struct ReusableReader<P, O>(P, PhantomData<O>);

impl<P, O> ReusableReader<P, O> {
    #[inline]
    pub fn new(p: P) -> ReusableReader<P, O> {
        ReusableReader(p, PhantomData)
    }
}

impl<P, O> Reader for ReusableReader<P, O>
where
    P: Reader,
    O: Send,
{
    type DataSet = (P::DataSet, O);
    type Err = P::Err;

    #[inline]
    fn fill_data(&mut self, data: &mut Self::DataSet) -> Option<Result<(), P::Err>> {
        self.0.fill_data(&mut data.0)
    }
}

/// Using this function currently does not work due to a
/// [compiler bug](https://github.com/rust-lang/rust/issues/42950).
///
/// `parallel_fasta`/`parallel_fastq` provide the same functionality for now
/// (implemented using `parallel_record_impl` macro)
pub fn parallel_records<R, O, W, F, Out>(
    parser: R,
    n_threads: u32,
    queue_len: usize,
    work: W,
    mut func: F,
) -> Result<Option<Out>, R::Err>
where
    R: Reader + Send,
    for<'a> &'a R::DataSet: IntoIterator,
    R::DataSet: Default,
    O: Default + Send,
    W: Send + Sync,
    W: Fn(<&R::DataSet as IntoIterator>::Item, &mut O),
    F: FnMut(<&R::DataSet as IntoIterator>::Item, &O) -> Option<Out>,
{
    let reader = ReusableReader(parser, PhantomData);

    read_parallel(
        reader,
        n_threads,
        queue_len,
        |d| {
            let mut iter = d.0.into_iter();
            let out: &mut Vec<O> = &mut d.1;
            for x in out.iter_mut().zip(&mut iter) {
                work(x.1, x.0);
            }
            for i in iter {
                out.push(O::default());
                work(i, out.last_mut().unwrap())
            }
        },
        |records| {
            while let Some(result) = records.next() {
                let (r, _) = result?;
                for x in r.0.into_iter().zip(&r.1) {
                    if let Some(out) = func(x.0, x.1) {
                        return Ok(Some(out));
                    }
                }
            }
            Ok(None)
        },
    )
}

// trait impls

use super::fasta;

impl<R, P> Reader for fasta::Reader<R, P>
where
    R: io::Read,
    P: super::policy::BufPolicy + Send,
{
    type DataSet = fasta::RecordSet;
    type Err = fasta::Error;

    #[inline]
    fn fill_data(&mut self, rset: &mut fasta::RecordSet) -> Option<Result<(), fasta::Error>> {
        self.read_record_set(rset)
    }
}

use super::fastq;

impl<R, P> Reader for fastq::Reader<R, P>
where
    R: io::Read,
    P: super::policy::BufPolicy + Send,
{
    type DataSet = fastq::RecordSet;
    type Err = fastq::Error;

    #[inline]
    fn fill_data(&mut self, rset: &mut fastq::RecordSet) -> Option<Result<(), fastq::Error>> {
        self.read_record_set(rset)
    }
}
