EMA: An aligner for barcoded short-read sequencing data
=======================================================
[![Build Status](https://travis-ci.org/arshajii/ema.svg?branch=master)](https://travis-ci.org/arshajii/ema) [![License](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/arshajii/ema/master/LICENSE)

EMA uses a latent variable model to align barcoded short-reads (such as those produced by [10x Genomics](https://www.10xgenomics.com)' sequencing platform). More information is available in [our paper](https://www.biorxiv.org/content/early/2017/11/16/220236). The full experimental setup is available [here](https://github.com/arshajii/ema-paper-data/blob/master/experiments.ipynb).

### Download and Compile
In a nutshell:

```
git clone --recursive https://github.com/arshajii/ema
cd ema
make
```

The `--recursive` flag is needed because EMA uses BWA's C API.

### Usage
```
usage: ./ema <preproc|count|align|help> [options]

preproc: preprocess barcoded FASTQ files
  -w <whitelist path>: specify whitelist [required]
  -n <num buckets>: number of barcode buckets to make [500]
  -h: apply Hamming-2 correction [off]
  -o: <output directory> specify output directory [required]
  -t <threads>: set number of threads [1]
  All other arguments: list of *all* output prefixes generated by count stage

count: performs preliminary barcode count (takes FASTQ via stdin)
  -w <whitelist path>: specify barcode whitelist [required]
  -o <output prefix>: specify output prefix [required]

align: choose best alignments based on barcodes
  -1 <FASTQ1 path>: first (preprocessed and sorted) FASTQ file [required]
  -2 <FASTQ2 path>: second (preprocessed and sorted) FASTQ file [required]
  -s <EMA-FASTQ path>: specify special FASTQ path [none]
  -x: multi-input mode; takes input files after flags and spawns a thread for each
  -r <FASTA path>: indexed reference [required]
  -o <SAM file>: output SAM file [stdout]
  -R <RG string>: full read group string (e.g. '@RG\tID:foo\tSM:bar') [none]
  -d: apply fragment read density optimization
  -p <platform>: sequencing platform (one of '10x', 'tru', 'cpt') [10x]
  -t <threads>: set number of threads [1]

help: print this help message
```

### Input formats
EMA has several input modes:
- `-s <input>`: Input file is a single preprocessed "special" FASTQ generated by the preprocessing steps below.
- `-x`: Input files are listed after flags (as in `ema align -a -b -c <input 1> <input 2> ... <input N>`). Each of these inputs are processed and all results are written to the SAM file specified with `-o`.
- `-1 <first mate>`/`-2 <second mate>`: Input files are standard FASTQs. For interleaved FASTQs, `-2` can be omitted. The only restrictions in this input mode are that read identifiers must end in `:<barcode sequence>` and that the FASTQs must be sorted by barcode. For 10x data, the above two modes are preferred.

### Parallelism
Multithreading can be enabled with `-t <num threads>`. The actual threading mode is dependent on how the input is being read, however:
- `-s`, `-1`/`-2`: Multiple threads are spawned to work on the single input file (or pair of input files).
- `-x`: Threads work on the input files individually.

(Note that, because of this, it never makes sense to spawn more threads than there are input files when using `-x`.)

### End-to-end workflow (10x)
In this guide, we use the following additional tools:
- [pigz](https://github.com/madler/pigz)
- [sambamba](http://lomereiter.github.io/sambamba/)
- [samtools](https://github.com/samtools/samtools)
- [GNU Parallel](https://www.gnu.org/software/parallel/)

We also use a 10x barcode whitelist, which can be found [here](http://ema.csail.mit.edu).

#### Preprocessing
Preprocessing 10x data entails several steps, the first of which is counting barcodes:

```
cd /path/to/gzipped_fastqs/
parallel -j40 --bar 'pigz -c -d {} | ema count -w /path/to/whitelist.txt -o {/.} 2>{/.}.log' ::: *.gz
```

Make sure that only the FASTQs containing the actual reads are included in `*.gz` above (as opposed to sample indices). This will produce `*.ema-ncnt` files, containing the count data.

Now we can do the actual preprocessing, which splits the input into barcode bins (500 by default; specified with `-n`). This preprocessing can be parallelized via `-t`, which specifies how many threads to use:

```
pigz -c -d *.gz | ema preproc -w /path/to/whitelist.txt -n 500 -t 40 -o output_dir *.ema-ncnt 2>&1 | tee preproc.log
```

#### Mapping
First we map each barcode bin with EMA. Here, we'll do this using a combination of GNU Parallel and EMA's internal multithreading, which we found to be optimal due to the runtime/memory trade-off. In the following, for instance, we use 10 jobs each with 4 threads (for 40 total threads). We also pipe EMA's SAM output (stdout by default) to `samtools sort`, which produces a sorted BAM:

```
parallel --bar -j10 "ema align -t 4 -d -r /path/to/ref.fa -s {} | samtools sort -@ 4 -O bam -l 0 -m 4G -o {}.bam -"
```

Lastly, we map the no-barcode bin with BWA:

```
bwa mem -p -t 40 -M -R "@RG\tID:rg1\tSM:sample1" /path/to/ref.fa output_dir/ema-bin-nobc | samtools sort -@ 4 -O bam -l 0 -m 4G -o output_dir/ema-bin-nobc.bam
```

Note that `@RG\tID:rg1\tSM:sample1` is EMA's default read group. If you specify another for EMA, be sure to specify the same for BWA as well (both tools take the full read group string via `-R`).

#### Postprocessing
EMA performs duplicate marking automatically. We mark duplicates on BWA's output with `sambamba markdup`:

```
sambamba markdup -t 40 -p -l 0 output_dir/ema-bin-nobc.bam output_dir/ema-bin-nobc-dupsmarked.bam
rm output_dir/ema-bin-nobc.bam
```

Now we merge all BAMs into a single BAM (might require modifying `ulimit`s, as in `ulimit -n 10000`):

```
sambamba merge -t 40 -p ema_final.bam output_dir/*.bam
```

Now you should have a single, sorted, duplicate-marked BAM `ema_final.bam`.

### Other sequencing platforms
Instructions for preprocessing and running EMA on data from other sequencing platforms can be found [here](https://github.com/arshajii/ema-paper-data/blob/master/experiments.ipynb).

### Output
EMA outputs a standard SAM file with several additional tags:

- `XG`: alignment probability
- `XC`: cloud identifier
- `XA`: alternate high-probability alignments
