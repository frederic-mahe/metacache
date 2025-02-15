# MetaCache - Metagenomic Classification System

MetaCache maps genomic sequences (short reads, long reads, contigs, etc.) from metagenomic samples to their most likely taxon of origin. It aims to reduce the memory requirement usually associated with k-mer based methods while retaining their speed. MetaCache uses locality sensitive hashing to quickly identify candidate regions within one or multiple reference genomes. A read is then classified based on the similarity to those regions.

For an independend comparison to other tools in terms of classification accuracy see the [LEMMI](https://lemmi.ezlab.org) benchmarking site.

The latest version of MetaCache classifies around 60 Million reads (of length 100) per minute against all complete bacterial, viral and archaea genomes from NCBI RefSeq Release 97 running with 88 threads on a workstation with 2 Intel(R) Xeon(R) Gold 6238 CPUs.


* [MetaCache Github Repository](https://github.com/muellan/metacache)
* [**MetaCacheSpark**: Apache Spark&trade; implementation of MetaCache for big data clusters](https://github.com/jmabuin/MetaCacheSpark)



## Applications


### Mapping Reads From Metagenomic Bacterial Samples

* Quick Start: [Bacterial mapping with NCBI's RefSeq](refseq.md)
* See the [LEMMI](https://lemmi.ezlab.org) benchmarking site for an independend comparison to other tools in terms of classification accuracy, speed and memory consumption.
* The [paper](https://www.doi.org/10.1093/bioinformatics/btx520) that introduced MetaCache.


### [AFS-MetaCache: Food Ingredient Detection & Abundance Analysis...](afs.md)



## Documentation Overview

* [Installation Instructions](https://github.com/muellan/metacache#detailed-installation-instructions)
* [Bacterial Mapping with NCBI's RefSeq](refseq.md)
* [Building Custom Databases](building.md)
* [Using Partitioned Databases](partitioning.md)
* [Output Interpretation, Analysis & Formatting Options](output.md)



#### All Command Line Options

* [for mode `build`](mode_build.txt): build database(s) from reference genomes
* [for mode `query`](mode_query.txt): query reads against databases
* [for mode `merge`](mode_merge.txt): merge results of independent queries
* [for mode `modify`](mode_modify.txt): add reference genomes to database or update taxonomy
* [for mode `info`](mode_info.txt): get information about a database


MetaCache Copyright (C) 2016-2020 [André Müller](https://github.com/muellan) & [Robin Kobus](https://github.com/Funatiq)
This program comes with ABSOLUTELY NO WARRANTY.
This is free software, and you are welcome to redistribute it under certain conditions. See the file 'LICENSE' for details.

