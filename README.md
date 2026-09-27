<p align="center"><img src="https://bioseqdb.com/assets/1059dd74d26a3f4b8efcfb12a2e871a12d9323d70fc3963a1ffe7e03ed4805b4/bioseq-mark.svg" alt="BioseqDB" width="180"></p>

# BioseqDB

BioseqDB is a PostgreSQL extension for storing, indexing and searching biological sequences with SQL. Combine genomic matches with relational data using compact sequence types and columnar storage!

Experimental, unsigned alpha packages are provided for ARM64 and x86-64 Linux distributions and MacOS on [SourceForge here](https://sourceforge.net/projects/bioseqdb/).

The docker images depend on the Linux distribution and Postgres major version, so please check which PG version you want.

## Table of contents

- [Docker quick start](#docker-quick-start)
- [Packages and Docker images](#packages-and-docker-images)
- [SQL walkthrough](#sql-walkthrough)
  - [Alpha-version limitations](#alpha-version-limitations)
  - [1. Create a sequence table](#1-create-a-sequence-table)
  - [2. Insert patient data](#2-insert-patient-data)
  - [3. Read metadata (non-seqence) columns](#3-read-metadata-non-seqence-columns)
  - [4. Reading sequences](#4-reading-sequences)
  - [5. Read plan for metadata query](#5-read-plan-for-metadata-query)
  - [6. Alignmment query](#6-alignmment-query)
  - [7. Trigger index rebuild](#7-trigger-index-rebuild)
  - [8. Repeated search](#8-repeated-search)
  - [9. Change algorithms for one call](#9-change-algorithms-for-one-call)
  - [10. Explaining the search (lots of fields!)](#10-explaining-the-search-lots-of-fields)
  - [11. Self-alignment query](#11-self-alignment-query)
  - [12. Compare rows using a self-join](#12-compare-rows-using-a-self-join)
  - [13. Load a FASTA file into another table](#13-load-a-fasta-file-into-another-table)
  - [14. Slices](#14-slices)
  - [15. Every possible alignment field](#15-every-possible-alignment-field)
  - [16. Aggregate metadata](#16-aggregate-metadata)
  - [17. Explain now the query](#17-explain-now-the-query)
  - [18. Update/insert and index rebuild](#18-updateinsert-and-index-rebuild)
  - [19. Index statistics](#19-index-statistics)
  - [20. Try scalar minimizers, scalar chaining and KSW2](#20-try-scalar-minimizers-scalar-chaining-and-ksw2)
  - [21. Combine sequence operations and an edit filter](#21-combine-sequence-operations-and-an-edit-filter)
- [Automatic index updates](#automatic-index-updates)

## Docker quick start

Ubuntu 26.04 + PostgreSQL 19 (prerelease), for amd64 or arm64. Pull the image:

```sh
docker pull styczynski/bioseqdb:0.1.0-alpha.1-pg19-ubuntu26.04
```

Now run the container:

```sh
docker run -d --name bioseqdb-demo -e POSTGRES_PASSWORD=demo-change-me \
  -p 127.0.0.1:55432:5432 styczynski/bioseqdb:0.1.0-alpha.1-pg19-ubuntu26.04
```

Wait for startup:

```sh
until docker exec bioseqdb-demo pg_isready -h 127.0.0.1 -U postgres; do sleep 1; done
```

Now execute client inside  the container:

```sh
docker exec -it -u postgres bioseqdb-demo psql -U postgres -d postgres
```

At the psql prompt:

```sql
SELECT version();
CREATE TABLE demo (id integer, seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO demo
SELECT 1, string_agg(translate(md5(i::text), '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i)::bioseq.seq('dna4', 64)
FROM generate_series(1, 32) AS i;
SELECT id, bioseq.cigar(seq), bioseq.score(seq) FROM demo WHERE public.mmsearch(seq, seq);
```

## Packages and Docker images

Release `0.1.0-alpha.1`. Linux package filenames use `4.2.3`.

Choose the matching distribution and PostgreSQL major.

| Distribution | PostgreSQL | Docker (x86-64 and ARM64) | x86-64 binary | ARM64 binary |
|---|---|---|---|---|
| Ubuntu 26.04 | 17 | [0.1.0-alpha.1-pg17-ubuntu26.04](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg17-ubuntu26.04) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/ubuntu_26.04/x64/da8a3eb94b0d0b59/bioseq-postgresql17_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/ubuntu_26.04/arm64/5e98df6c2402d860/bioseq-postgresql17_4.2.3_arm64.deb) |
| Ubuntu 26.04 | 18 | [0.1.0-alpha.1-pg18-ubuntu26.04](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg18-ubuntu26.04) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/ubuntu_26.04/x64/ab89957bfa9254e9/bioseq-postgresql18_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/ubuntu_26.04/arm64/55f40c2d945bd1ce/bioseq-postgresql18_4.2.3_arm64.deb) |
| Ubuntu 26.04 | 19 (prerelease) | [0.1.0-alpha.1-pg19-ubuntu26.04](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg19-ubuntu26.04) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/ubuntu_26.04/x64/7c4873d62b514070/bioseq-postgresql19_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/ubuntu_26.04/arm64/7ef10e3f8c22a8ec/bioseq-postgresql19_4.2.3_arm64.deb) |
| Ubuntu 26.04 | 20 (development) | [0.1.0-alpha.1-pg20-ubuntu26.04](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg20-ubuntu26.04) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/ubuntu_26.04/x64/9b051bb7bf5e918a/bioseq-postgresql20_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/ubuntu_26.04/arm64/62194f54d7406bf8/bioseq-postgresql20_4.2.3_arm64.deb) |
| Debian 13 | 17 | [0.1.0-alpha.1-pg17-debian13](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg17-debian13) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/debian_13/x64/47f7ef7853641280/bioseq-postgresql17_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/debian_13/arm64/6e1113688ba8be47/bioseq-postgresql17_4.2.3_arm64.deb) |
| Debian 13 | 18 | [0.1.0-alpha.1-pg18-debian13](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg18-debian13) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/debian_13/x64/ebe9cacf5ffe2c36/bioseq-postgresql18_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/debian_13/arm64/97586d8d4c10c6c9/bioseq-postgresql18_4.2.3_arm64.deb) |
| Debian 13 | 19 (prerelease) | [0.1.0-alpha.1-pg19-debian13](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg19-debian13) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/debian_13/x64/22e7fbb4c94321cd/bioseq-postgresql19_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/debian_13/arm64/731e0fd2ead26fce/bioseq-postgresql19_4.2.3_arm64.deb) |
| Debian 13 | 20 (development) | [0.1.0-alpha.1-pg20-debian13](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg20-debian13) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/debian_13/x64/d029365ba27ed97f/bioseq-postgresql20_4.2.3_amd64.deb) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/debian_13/arm64/95931942aa78576e/bioseq-postgresql20_4.2.3_arm64.deb) |
| Arch Linux | 17 | [0.1.0-alpha.1-pg17-arch](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg17-arch) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/arch/x64/7d63bea74a7e08b0/bioseq-postgresql17-4.2.3-1-x86_64.pkg.tar.zst) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/arch/arm64/0217b8904f5d2e2b/bioseq-postgresql17-4.2.3-1-aarch64.pkg.tar.zst) |
| Arch Linux | 18 | [0.1.0-alpha.1-pg18-arch](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg18-arch) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/arch/x64/7f36d14e4eee1699/bioseq-postgresql18-4.2.3-1-x86_64.pkg.tar.zst) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/arch/arm64/f3d628cba9c2b84e/bioseq-postgresql18-4.2.3-1-aarch64.pkg.tar.zst) |
| Arch Linux | 19 (prerelease) | [0.1.0-alpha.1-pg19-arch](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg19-arch) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/x64/f24a28c834556edf/bioseq-postgresql19-4.2.3-1-x86_64.pkg.tar.zst) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/arm64/cd46c53bb556bb34/bioseq-postgresql19-4.2.3-1-aarch64.pkg.tar.zst) |
| Arch Linux | 20 (development) | [0.1.0-alpha.1-pg20-arch](https://hub.docker.com/r/styczynski/bioseqdb/tags?name=0.1.0-alpha.1-pg20-arch) | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/x64/785979a270e4c914/bioseq-postgresql20-4.2.3-1-x86_64.pkg.tar.zst) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/arm64/1d3417bbb4e33c89/bioseq-postgresql20-4.2.3-1-aarch64.pkg.tar.zst) |
| Fedora 44 | 17 | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/fedora_44/x64/033a76a63b030719/bioseq-postgresql17-4.2.3-1.fc44.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/fedora_44/arm64/6e805544bb9bc69d/bioseq-postgresql17-4.2.3-1.fc44.aarch64.rpm) |
| Fedora 44 | 18 | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/fedora_44/x64/f53b5049ea9dc219/bioseq-postgresql18-4.2.3-1.fc44.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/fedora_44/arm64/50e0d29e35a7f952/bioseq-postgresql18-4.2.3-1.fc44.aarch64.rpm) |
| Fedora 44 | 19 (prerelease) | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/fedora_44/x64/bdb1fed13570056f/bioseq-postgresql19-4.2.3-1.fc44.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/fedora_44/arm64/6d52427110d70ad7/bioseq-postgresql19-4.2.3-1.fc44.aarch64.rpm) |
| Fedora 44 | 20 (development) | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/fedora_44/x64/ba12bb76e58161cf/bioseq-postgresql20-4.2.3-1.fc44.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/fedora_44/arm64/38bc2242a3f5a266/bioseq-postgresql20-4.2.3-1.fc44.aarch64.rpm) |
| openSUSE 16.0 | 17 | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/opensuse_16.0/x64/f9b0c8bbca7d30d0/bioseq-postgresql17-4.2.3-1.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/opensuse_16.0/arm64/3bdf77cbddfd703f/bioseq-postgresql17-4.2.3-1.aarch64.rpm) |
| openSUSE 16.0 | 18 | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/opensuse_16.0/x64/3050916230122aed/bioseq-postgresql18-4.2.3-1.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/opensuse_16.0/arm64/ecb32b308b267451/bioseq-postgresql18-4.2.3-1.aarch64.rpm) |
| openSUSE 16.0 | 19 (prerelease) | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/x64/873b945bdfc84a43/bioseq-postgresql19-4.2.3-1.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/arm64/debd034cce662a06/bioseq-postgresql19-4.2.3-1.aarch64.rpm) |
| openSUSE 16.0 | 20 (development) | Not published | [x86-64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/x64/6aa760e75abad2a5/bioseq-postgresql20-4.2.3-1.x86_64.rpm) | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/arm64/baf19e9857557549/bioseq-postgresql20-4.2.3-1.aarch64.rpm) |
| macOS | 17 | Not published | Not published | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg17/macos/arm64/76bedc6d98f03f9b/bioseqdb-0.1.0-alpha.1-pg17-macos-arm64.pkg) |
| macOS | 18 | Not published | Not published | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg18/macos/arm64/657ba47436f713dc/bioseqdb-0.1.0-alpha.1-pg18-macos-arm64.pkg) |
| macOS | 19 (prerelease) | Not published | Not published | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/macos/arm64/300f2135f11e3539/bioseqdb-0.1.0-alpha.1-pg19-macos-arm64.pkg) |
| macOS | 20 (development) | Not published | Not published | [ARM64](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/macos/arm64/54a659966a7afe41/bioseqdb-0.1.0-alpha.1-pg20-macos-arm64.pkg) |

<details>
<summary>PostgreSQL runtime packages and macOS setup</summary>

| Package | Required runtime | Supporting files |
|---|---|---|
| Arch Linux, PG19, arm64 | bioseq-postgresql19-runtime=19beta3-1 | [bioseq-postgresql19-runtime-19beta3-1-aarch64.pkg.tar.zst](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/arm64/cd46c53bb556bb34/bioseq-postgresql19-runtime-19beta3-1-aarch64.pkg.tar.zst); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/arm64/cd46c53bb556bb34/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/arm64/cd46c53bb556bb34/SDK_SETUP.txt) |
| Arch Linux, PG19, x64 | bioseq-postgresql19-runtime=19beta3-1 | [bioseq-postgresql19-runtime-19beta3-1-x86_64.pkg.tar.zst](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/x64/f24a28c834556edf/bioseq-postgresql19-runtime-19beta3-1-x86_64.pkg.tar.zst); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/x64/f24a28c834556edf/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/arch/x64/f24a28c834556edf/SDK_SETUP.txt) |
| Arch Linux, PG20, arm64 | bioseq-postgresql20-runtime=20.0devel20260924-1 | [bioseq-postgresql20-runtime-20.0devel20260924-1-aarch64.pkg.tar.zst](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/arm64/1d3417bbb4e33c89/bioseq-postgresql20-runtime-20.0devel20260924-1-aarch64.pkg.tar.zst); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/arm64/1d3417bbb4e33c89/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/arm64/1d3417bbb4e33c89/SDK_SETUP.txt) |
| Arch Linux, PG20, x64 | bioseq-postgresql20-runtime=20.0devel20260924-1 | [bioseq-postgresql20-runtime-20.0devel20260924-1-x86_64.pkg.tar.zst](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/x64/785979a270e4c914/bioseq-postgresql20-runtime-20.0devel20260924-1-x86_64.pkg.tar.zst); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/x64/785979a270e4c914/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/arch/x64/785979a270e4c914/SDK_SETUP.txt) |
| Fedora 44, PG19, arm64 | bioseq-postgresql19-runtime = 19beta3-1 | [bioseq-postgresql19-runtime-19beta3-1.aarch64.rpm](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/fedora_44/arm64/6d52427110d70ad7/bioseq-postgresql19-runtime-19beta3-1.aarch64.rpm); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/fedora_44/arm64/6d52427110d70ad7/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/fedora_44/arm64/6d52427110d70ad7/SDK_SETUP.txt) |
| Fedora 44, PG20, arm64 | bioseq-postgresql20-runtime = 20.0devel20260924-1 | [bioseq-postgresql20-runtime-20.0devel20260924-1.aarch64.rpm](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/fedora_44/arm64/38bc2242a3f5a266/bioseq-postgresql20-runtime-20.0devel20260924-1.aarch64.rpm); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/fedora_44/arm64/38bc2242a3f5a266/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/fedora_44/arm64/38bc2242a3f5a266/SDK_SETUP.txt) |
| openSUSE 16.0, PG19, arm64 | bioseq-postgresql19-runtime = 19beta3-1 | [bioseq-postgresql19-runtime-19beta3-1.aarch64.rpm](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/arm64/debd034cce662a06/bioseq-postgresql19-runtime-19beta3-1.aarch64.rpm); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/arm64/debd034cce662a06/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/arm64/debd034cce662a06/SDK_SETUP.txt) |
| openSUSE 16.0, PG19, x64 | bioseq-postgresql19-runtime = 19beta3-1 | [bioseq-postgresql19-runtime-19beta3-1.x86_64.rpm](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/x64/873b945bdfc84a43/bioseq-postgresql19-runtime-19beta3-1.x86_64.rpm); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/x64/873b945bdfc84a43/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg19/opensuse_16.0/x64/873b945bdfc84a43/SDK_SETUP.txt) |
| openSUSE 16.0, PG20, arm64 | bioseq-postgresql20-runtime = 20.0devel20260924-1 | [bioseq-postgresql20-runtime-20.0devel20260924-1.aarch64.rpm](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/arm64/baf19e9857557549/bioseq-postgresql20-runtime-20.0devel20260924-1.aarch64.rpm); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/arm64/baf19e9857557549/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/arm64/baf19e9857557549/SDK_SETUP.txt) |
| openSUSE 16.0, PG20, x64 | bioseq-postgresql20-runtime = 20.0devel20260924-1 | [bioseq-postgresql20-runtime-20.0devel20260924-1.x86_64.rpm](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/x64/6aa760e75abad2a5/bioseq-postgresql20-runtime-20.0devel20260924-1.x86_64.rpm); [sdk.json](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/x64/6aa760e75abad2a5/sdk.json); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/opensuse_16.0/x64/6aa760e75abad2a5/SDK_SETUP.txt) |
| macOS, PG17, arm64 | Homebrew PostgreSQL 17 | Homebrew `postgresql@17` |
| macOS, PG18, arm64 | Homebrew PostgreSQL 18 | Homebrew `postgresql@18` |
| macOS, PG19, arm64 | PostgreSQL 19beta3 (Homebrew petere/postgresql); exact keg /opt/homebrew/Cellar/postgresql@19/19beta3 | Homebrew `petere/postgresql/postgresql@19` |
| macOS, PG20, arm64 | PostgreSQL 20devel (BioseqDB SDK df3978c2340d); exact keg /opt/homebrew/Cellar/postgresql@20/20.0devel20260924 | [postgresql@20.rb](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/macos/arm64/54a659966a7afe41/postgresql%4020.rb); [SDK_SETUP.txt](https://downloads.sourceforge.net/project/bioseqdb/0.1.0-alpha.1/pg20/macos/arm64/54a659966a7afe41/SDK_SETUP.txt) |

</details>

[SourceForge release](https://sourceforge.net/projects/bioseqdb/files/0.1.0-alpha.1/) · [Package manifest](https://sourceforge.net/projects/bioseqdb/files/0.1.0-alpha.1/manifest.json/download) · [Docker images](https://hub.docker.com/r/styczynski/bioseqdb/tags)

## SQL walkthrough

Please note that in this example I use sequences longer than >30 bases. Due to how filtering works (also in minimap2) extremally short sequences might not produce any hits.

### Alpha-version limitations

Please note that the current software is an alpha version and still has
numerous issues. The memory checks can be too restrictive. Currently when there's less than 2GiB available BioseqDB reports the following error:
`transient query execution stats disagree with its generated backend`, which does not explain the actual isue.
Because memory cannot  be allocated, the query reports that index cannot be built, so in stats the index access strategy sets the flag as unavilable.
I check the "transient query execution stats" to confirm that what user chose to do is actually done (detects GPU fallbacks, scalar fallbacks and other hidden behaviour).
In the future this error needs to detect that, the problem was memory allocation, so UX is much better

Additionally please `bioseq.index.insert_maintenance.enabled` set to off as currently BioseqDB defaults to grouped-MPF which does not support automatic index update on insert.
You can disable grouped-MPF to  allow automatic index update on insert.

Start a new database from the same `psql` prompt:

```sql
CREATE DATABASE bioseq_walkthrough TEMPLATE template0;
\connect bioseq_walkthrough
SET bioseq.index.insert_maintenance.enabled = off;
```

### 1. Create a sequence table

The four ordinary columns hold some patient metadata. The sequences themselves are DNA4 so "N" is not allowed.
The base unit width is 64 bits (the value is not maximum sequence size like in case of VARCHAR)

`USING bioseq` makes sure that table is stored in columnar format using BioseqDB.

```sql
\set ON_ERROR_STOP on
\pset pager off
CREATE EXTENSION bioseq;

CREATE TABLE samples (
  id integer PRIMARY KEY,
  upload_date date NOT NULL,
  patient_name text NOT NULL,
  age integer NOT NULL,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
```

### 2. Insert patient data

This is the most subjectively ugly way to generate andom sequences.
I use md5 to generate 32-length hex string that then is traslated to valid DNA4 sequence and concatenated 64 times (giving  us 2048 bases).
Then I insert data of 3 patients with sequence sizes (4096 - two copies, 512 - substring, 2048 - full sequence)

```sql
WITH dna AS (
  SELECT string_agg(translate(md5(i::text),
    '0123456789abcdef',
    'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
  FROM generate_series(1, 64) AS g(i)
)
INSERT INTO samples
SELECT 1, DATE '2026-09-01', 'Anna', 34,
  (s || s)::bioseq.seq('dna4', 64) FROM dna
UNION ALL
SELECT 2, DATE '2026-09-02', 'Ben', 52,
  substring(s FROM 257 FOR 512)
    ::bioseq.seq('dna4', 64) FROM dna
UNION ALL
SELECT 3, DATE '2026-09-03', 'Cara', 29,
  s::bioseq.seq('dna4', 64) FROM dna;
```

### 3. Read metadata (non-seqence) columns

We can query the table as normally done in Postgres, but the underlying data is currently stored in columnar format

```sql
SELECT id, upload_date, patient_name, age
FROM samples;
```

### 4. Reading sequences

We combine the filter on metadata with extraction of sequence length and one slicing. This returns Ben and Anna, in that order

`len` reports sequence size, while the slice `[:16]` returns
the first sixteen symbols (note that text cast is optional and the coordinates for sequences always start  at 0, which is different to Postgres array indexing)

```sql
SELECT id, upper(patient_name) AS patient,
  age, bioseq.len(seq) AS bases,
  seq[:16]::text AS prefix
FROM samples
WHERE age BETWEEN 30 AND 60
  AND upload_date >= DATE '2026-09-01'
  AND (patient_name LIKE 'A%'
       OR patient_name LIKE 'B%')
ORDER BY age DESC, id;
```

### 5. Read plan for metadata query

`COSTS OFF` is to hide the cost estimates of the query as they aren't relevant here.

```sql
EXPLAIN (ANALYZE, COSTS OFF)
SELECT id, upload_date, patient_name, age
FROM samples;
```

<details>
<summary>Example plan from the walkthrough</summary>

```text
Custom Scan (BioseqScan) on samples (actual time=0.010..0.015 rows=3 loops=1)
  Bioseq Projected Columns: id, upload_date, patient_name, age
  ->  Cache
        Cache Hits: 0
        Cache Misses: 4
        Cache Evictions: 0
        Cache Writes: 4
        Cache Maximum Size: 60
        Cache Ending Size: 60
        Total Cache Entries: 4
Planning Time: 0.028 ms
Execution Time: 0.031 ms
```

</details>

`BioseqScan` is the scan node thatreads the table. `Projected Columns` lists which columns are required for this query and what the BioseqDB needs to return.

The cache stores decompressed column chunks. Hits and misses count buffer lookups in, the cache.
Writes count buffers added or replaced, evictions count removed entries from the ghunk cache,
and entries count current buffers stored in the cache. Maximum and ending sizes are peak and
current payload bytes.

There is no counter  here used for chunk-skipping (min/max pruning), because naturally there is no filter in the query, so the scan cannot skip chunks.

### 6. Alignmment query

`\gset` saves Ben's DNA in the `psql` variable
`probe`.

`mmsearch` here will try to align each patient sequence against Ben's sequence. This returns
two matches for Anna and one for Ben Because they are exact occurences in this very simplistic case, each CIGAR string is
`512M`.

Requesting `cigar` enables additional pipeline stage for base alignment, because the pipeline needs to know exactly which bases  were subsituted/inserted or deleted. However,
`score` alone does not enable it and can return zero (less exact score is used based only on chaining).

If you encounter `"transient query execution stats disagree with its generated backend"` error here or in the next step, you should have at leawt 2GiB of free memory available

```sql
SELECT seq::text AS probe
FROM samples WHERE id = 2 \gset

SELECT id, patient_name,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    :'probe'::bioseq.seq('dna4', 64));
```

### 7. Trigger index rebuild

This processes three sequence values and returns 3 as the number of sequences in the index. It extracts minimizers
using the current algorithm and k,w, groups their positions, and saves (publishes) the data.

The default uses grouped-MPF format.

```sql
SELECT bioseq.update_index(
  'samples', 'minimizers_per_fragment');
```

### 8. Repeated search

The same three matches remain.

Currently grouped-MPF which was measured  in benchmarks for case of query/target table join doesn't work for single table queries correctly and causes minimizers to be always recomputed.

```sql
SELECT id, bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    :'probe'::bioseq.seq('dna4', 64));
```

### 9. Change algorithms for one call

I wanted to have an example with explicit algorithm settings to show how we can change either the algorithm for specific stage like chaining or the k/w parameters.
You need to pass `aligner_options` to the `mmsearch` function
The codes choose bounded ntHash SIMD minimizers (4), SIMD minimap2
chaining (1), and BlockAligner (1). Each, minimizer window contains ten
consecutive 15-mers. Each of the parameters correspond to the BioseqDB current defaults so the pipeline works  exactly the same as before.

Unfortunately the global GUC settings use strings for enums, while the  `aligner_options` type uses numeric code s (due to my lazyness).

```sql
SELECT id,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30 AND mmsearch(seq,
  :'probe'::bioseq.seq('dna4', 64),
  aligner_options(
    kmer_size => 15, window_size => 10,
    minimizer_algorithm => 4,
    chaining_algorithm => 1,
    base_alignment_algorithm => 1));
```

### 10. Explaining the search (lots of fields!)

I wanted to present entire plan of the query. It contains a lot of statistics and settings to allow me to more easily troubleshoot BioseqDB behaviour

```sql
EXPLAIN (ANALYZE, COSTS OFF)
SELECT id,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30 AND mmsearch(seq,
  :'probe'::bioseq.seq('dna4', 64),
  aligner_options(
    kmer_size => 15, window_size => 10,
    minimizer_algorithm => 4,
    chaining_algorithm => 1,
    base_alignment_algorithm => 1));
```

<details>
<summary>Example plan from the walkthrough</summary>

```text
Result (actual time=1.281..1.295 rows=3 loops=1)
  ->  Custom Scan (BioseqScan) on samples (actual time=1.281..1.294 rows=3 loops=1)
        Bioseq Projected Columns: id, age, seq
        Bioseq Chunk Group Filters: (age >= 30)
        Bioseq Chunk Groups Removed by Filter: 0
        Bioseq Vectorized Filter: (age >= 30)
        Bioseq Index Read Requirement: allow
        Bioseq Recomputed: minimizers 100%
        Bioseq Scheduler: supervisor=local mode=task_parallel workers=6/6 batches=8/8 awaiting=0 queries=8/0 qps=0.000 async_persist_waiting=0 index_memory_reserved=0
        One-to-many alignment:
          Task-parallel [mode=task_parallel, workers=6, index_memory_reserved=0 bytes]:
            Planned Architecture: NEON_BF16
            CPU-Supported Targets: NEON_BF16, NEON, NEON_WITHOUT_AES, EMU128, SCALAR
            Highway Compiled Targets: NEON_BF16, NEON, NEON_WITHOUT_AES, EMU128
            Runnable Targets: NEON_BF16, NEON, NEON_WITHOUT_AES, EMU128
            * Executed Architecture: NEON_BF16
            | QuerySketchStage: Bounded u32 ntHash SIMD Minimizers
              Highway Function: minimizer::bounded_nthash [planned architecture: NEON_BF16]
            | index_stage: pipeline configuration
              [cep.window_len: 0]
              [cep.kmer_len: 0]
              [cep.sequence_len_log2: 63]
              [cep.specialised: fallback]
            | seed_hit_stage: Seed hits + canonical CPU ordering
              max_occ: 500
              query_len: 0
              [reserved: 64]
            | chain_stage: Minimap2 SIMD
              min_chain_score: 40
              min_seed_count: 3
              max_chain_skip: 25
              max_chain_iter: 5000
              bandwidth: 500
              max_gap: 5000
              simd_chaining_policy: bounded_batch_approximate
              [vectorised: true]
              [requires_zero_skip_penalty: false]
              [specialised: fallback]
              Highway Function: chaining::chain_anchors_simd [planned architecture: NEON_BF16]
            | region_stage: Region generation
              query_len: 0
            | primary_secondary_stage: Classify primary/secondary
              mask_level: 0.500000
              mask_len: 2147483647
              sub_diff: 8
            | secondary_filter_stage: Filter secondary regions
              pri_ratio: 0.800000
              min_diff: 30
              best_n: 5
            | align_stage: Block Aligner
              match_score: 2
              mismatch_pen: 4
              gap_open1: 4
              gap_ext1: 2
              block_min_size: 32
              block_max_size: 256
              [vectorised: block-affine]
              [single_affine_only: true]
              [specialised: fallback]
              Highway Function: alignment::block_aligner [planned architecture: NEON_BF16]
            | primary_secondary_stage: Classify primary/secondary
              mask_level: 0.500000
              mask_len: 2147483647
              sub_diff: 8
            | secondary_filter_stage: Filter secondary regions
              pri_ratio: 0.800000
              min_diff: 30
              best_n: 5
            * Submit results [targets=2 persisted_index=0 recomputed=2]
            * Execution statistics [requested_backend=simd_cpu actual_backend=mixed_invalid requested_index_source=persisted actual_index_source=recomputed attempted=2 fully_attested=0 unattested=2 environment_mixed=false persisted_identity_mixed=false identity_conflict=false saturated=false]
            * First unattested [target_row=1 query_ordinal=0]
            * Index outcomes [targets=2 persisted=0 missing_persisted=2 recomputed=2]
            * GPU dispatch [seed=0 chain=0 chain_backtrack=0 coordinate_vote=0 extend=0 backend_declines=0 backend_fallbacks=0]
            GPU Chain Dispatches: 0
            GPU Chain Backtrack Dispatches: 0
            GPU Coordinate Vote Dispatches: 0
            * Host stats [query_sketch_ns=0 target_index_build_ns=0 query_hash_readbacks=0 query_symbol_packing=0 target_symbol_packing=0 alignment_classification=0 cigar_construction=0]
        ->  Cache
              Cache Hits: 0
              Cache Misses: 5
              Cache Evictions: 0
              Cache Writes: 5
              Cache Maximum Size: 16608
              Cache Ending Size: 16608
              Total Cache Entries: 5
Planning Time: 0.241 ms
Execution Time: 1.432 ms
```

</details>

`Result` is just a projection of the `BioseqScan`

The scan reads the identifie, age and sequence. A chunk-group filter
compares the age condition with stored minima and maxima, but couldn't skip any chunk (because we have so few rows, there's only one  chunk).
The vectorized filter tests individual ages in the
remaining batches.

`allow` means that we can either read the index or recompute minimizers from scratch.
The planned share is 100% recomputed - this is the estimate of how many minimziers in index do not have a corresponding minimizers stored. In this case
BioseqDB have buggy const-query implementation which causes index to always be rebuilt which significantly degrades performance For better performance you should use join between query and target tables, so both
of query/target arguments are red  from tables.

`Bioseq Scheduler` reports workers in thread pool, processed/scheduled batches, waiting work and
processed/waiting queries. The local supervisor leaves the query-rate
field at zero.

Runnable targets are the intersection
of compiled and CPU-supported targets (can be used only if built for this architecture and the current hardware supports it).
`QuerySketchStage` lists used minimizer algorithm, and
`Highway Function` lists the C++ functin, name.

Internally BioseqDB is implemented as type-driven heavily-templated code, so CEP means "constant expression parameter".
The zero `cep.window_len`, `cep.kmer_len` and
`query_len` values represent the fact that generic implementaion is used (internally I  have parameter that can allow instantiation of template specifically for k/w parameters or
for specific sequence length, for specific minimizer lengths. `cep.specialised: fallback` with all zeros means that these parameters are runtime, not compile-time optimised constexpr parameters - this is default as enabling instantiation
produces very large binaries the linker msut work with).

`max_occ` limits max counts of minimizers per hash, while `reserved` is a planned anchor-buffer capacity in elements (how many anchors I predict to have - in reality this allocation by default is const, but can grow)

`vectorised` in this case means that the  C++ function can accept batch instead of single element (all the properties in square brackets are compile-time properties of the code itself)

Regions are classified as primary or secondary by their overlap

BlockAligner computes the base-level alignment and CIGAR.

Primary/secondary classification and filtering run again after kase
alignment.

`Submit results` reports two processed targets. Both used recomputed
minimizers and neither used a persisted index

These diagnostics are useful to see what is happening inside the BioseqDB and how it chooses available algorithms.
`attempted` counts how  many target/query jobs were attempted. `fully_attested` and `unattested` and `mixed_invalid` are internally, used special flag with counters that aren't useful at the moment.
(their purpose was to - after connecting additional validation code - check if the result is valid and if it came from the backend that test code expected)

`missing_persisted` is important here as it shows that BioseqDB for queries without join between query and target tables will always use recomputed minimizers for grouped MPF index.
This is currently a major bug, that needs to be addressed.

I won't go into details on the GPU counters as  the most important is the job dispatch counter.
All other were internally used and some of them are just zeroed out currently.

The cache fields have the same meanings as in the first plan. This search
needs sequence data as well as metadata, so the buffers and their sizes
differ

The final two lines give statement planning and xecution time in
milliseconds. They include work outside the alignment stages.

### 11. Self-alignment query

Each  of the three rows produces a hit as natuarally expected sequence should align to itself

```sql
SELECT id,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE mmsearch(seq, seq)
ORDER BY id;
```

### 12. Compare rows using a self-join

As self-alignment is not realistically useful, we can compare various sequences from our dataset using self-join

For target 1 and query 2, the same score occurs at both [256,768) and
[2304,2816). These are different occurrences (hits), even though both have the same cigar
`512M`

`index_read_requirement => 2` is used because otherwise bug in BioseqDB prevents index to be used, which slows down the query.

```sql
SELECT t.id AS target_id, q.id AS query_id,
  bioseq.target_start(t.seq) AS target_start,
  bioseq.target_end(t.seq) AS target_end,
  bioseq.query_start(t.seq) AS query_start,
  bioseq.query_end(t.seq) AS query_end,
  bioseq.score(t.seq) AS score,
  bioseq.cigar(t.seq) AS cigar
FROM samples AS t JOIN samples AS q
  ON mmsearch(t.seq, q.seq, aligner_options(index_read_requirement => 2))
ORDER BY target_id, query_id, target_start,
  query_start;
```

### 13. Load a FASTA file into another table

We can load FASTA file with 3 records into the tables.
`load` will return the number of records in FASTA file, in this case 3. With one text column, one  sequence column and no
explicit parameters, the default `auto` mode keeps each full header
without the leading `>` in the text column.

The header parser for FASTA can accept header format name that supports various databases:

`header_format` parmeter accepts `auto`, `plain`,
`uni_prot`, `uni_ref`, `uni_parc`,
`uni_mes`, `spd`, `ncbi`, `ncbi_gi`,
`ncbi_pdb`, `refseq`, `genbank`, `gisaid`
and `imgt`. Auto can detect which format the, header uses automatically

The last argument of load functionmaps parsed fields to table columns using
`bioseq.column_mapping`, with destination first and source second, so if you want to map cerain fields of the header to the table  column, you can do that!

```sql
COPY (
  SELECT line FROM samples
  CROSS JOIN LATERAL (VALUES
    (0, '>sample_' || id || ' synthetic DNA'),
    (1, seq::text)
  ) AS fasta(part, line)
  ORDER BY id, part
) TO '/tmp/bioseq-walkthrough.fasta';

CREATE TABLE imported (
  header text,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;

SELECT bioseq.load('imported',
  '/tmp/bioseq-walkthrough.fasta');
```

### 14. Slices

The first query returns three headers, lengths and 16bp slices and
the next saves a 256-base slice of the loaded Ben record as
`short_probe`

```sql
SELECT header, bioseq.len(seq) AS bases,
  seq[:16]::text AS prefix
FROM imported ORDER BY header;

SELECT seq[128:384]::text AS short_probe
FROM imported
WHERE header LIKE 'sample_2 %' \gset

SELECT header,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.cigar(seq) AS cigar
FROM imported
WHERE mmsearch(seq,
  :'short_probe'::bioseq.seq('dna4', 64))
ORDER BY header, target_start;
```

### 15. Every possible alignment field

This example shows each possible value we can get from the aligner pipeline: `score` and `dp_score` are aliases for the same value which is computed minimap2 score.
`chain_score` describes the seed chain score.

`block_length` is similar to PAF in minimap2 - total alignment block length i.e length of the expanded CIGAR string.

Here, `num_matches` counts bases covered by `M` and `=` operations in the CIGAR string. `M` can include mismatches.

`edit` counts alignment edits. `mapq` estimates mapping
quality (similar to mapq minimap2), while `divergence` is the score estimated from seeds. `seed_count` counts anchors in  the reported chain.

The Boolean fields report strand orientation, chain classification and CIGAR.

This join requires the compatible persisted target index built in step 7 and as mentioned
before currently due to bugs in implementation of grouped-MPF index this JOIN correctly reads index, while default read policy
ignores it. Moreover as mentioned alignment without join will also ignore index.

```sql
SELECT t.id AS target_id, q.header,
  bioseq.score(t.seq) AS score,
  bioseq.dp_score(t.seq) AS dp_score,
  bioseq.dp_max_score(t.seq) AS dp_max_score,
  bioseq.chain_score(t.seq) AS chain_score,
  bioseq.secondary_chain_score(t.seq)
    AS secondary_chain_score,
  bioseq.query_len(t.seq) AS query_len,
  bioseq.target_len(t.seq) AS target_len,
  bioseq.query_start(t.seq) AS query_start,
  bioseq.query_end(t.seq) AS query_end,
  bioseq.target_start(t.seq) AS target_start,
  bioseq.target_end(t.seq) AS target_end,
  bioseq.num_matches(t.seq) AS num_matches,
  bioseq.block_length(t.seq) AS block_length,
  bioseq.edit(t.seq) AS edits,
  bioseq.mapq(t.seq) AS mapq,
  bioseq.seed_count(t.seq) AS seed_count,
  bioseq.is_reverse(t.seq) AS is_reverse,
  bioseq.is_primary(t.seq) AS is_primary,
  bioseq.has_cigar(t.seq) AS has_cigar,
  bioseq.cigar(t.seq) AS cigar,
  bioseq.divergence(t.seq) AS divergence
FROM samples AS t JOIN imported AS q
  ON mmsearch(t.seq, q.seq,
    aligner_options(index_read_requirement => 2))
ORDER BY score DESC, target_id, q.header,
  target_start, query_start;
```

### 16. Aggregate metadata

This returns two patients, minimum age 34, maximum  age 52, sum 86 and
average 43. The dates span 1-2 September
By default aggregates will filter-out chunks due to min/max pruning.

```sql
SELECT count(*) AS patients,
  min(age) AS youngest, max(age) AS oldest,
  sum(age) AS total_age, avg(age) AS mean_age,
  min(upload_date) AS first_upload,
  max(upload_date) AS last_upload
FROM samples
WHERE age >= 30
  AND upload_date >= DATE '2026-09-01';
```

### 17. Explain now the query



```sql
EXPLAIN (ANALYZE, COSTS OFF)
SELECT count(*) AS patients,
  min(age) AS youngest, max(age) AS oldest,
  sum(age) AS total_age, avg(age) AS mean_age,
  min(upload_date) AS first_upload,
  max(upload_date) AS last_upload
FROM samples
WHERE age >= 30
  AND upload_date >= DATE '2026-09-01';
```

<details>
<summary>Example plan from the walkthrough</summary>

```text
Custom Scan (VectorAggNode) (actual time=0.067..0.067 rows=1 loops=1)
  ->  Custom Scan (BioseqScan) on samples (actual time=0.058..0.063 rows=1 loops=1)
        Bioseq Projected Columns: upload_date, age
        Bioseq Chunk Group Filters: ((age >= 30) AND (upload_date >= '2026-09-01'::date))
        Bioseq Chunk Groups Removed by Filter: 0
        Bioseq Vectorized Filter: ((age >= 30) AND (upload_date >= '2026-09-01'::date))
...
```

</details>

`VectorAggNode` is the BioseqDB vector aggregate. Row nuber doesn't reflect number of rows in the result.

Because number of rows in small the chunk filter didn't filter any chunks. The vectorized filter allowed to evaluate filter on each row in the batch.

### 18. Update/insert and index rebuild

Anna becomes 35 and Daria receives Ben's sequence. There are now four stored sequences.

With insert maintenance off, these writes do not build new, minimizer
indices, so unfortunately you  need to manually rebuild indices.
The two explicit calls prepare MPF and symbol-count indexes.

```sql
UPDATE samples SET age = age + 1
WHERE id = 1
RETURNING id, patient_name, age;

INSERT INTO samples
VALUES (4, DATE '2026-09-04', 'Daria', 41,
  :'probe'::bioseq.seq('dna4', 64))
RETURNING id, patient_name, bioseq.len(seq);

SELECT bioseq.update_index(
  'samples', 'minimizers_per_fragment');
SELECT bioseq.update_index(
  'samples', 'symbol_counts');
```

### 19. Index statistics

MPF and symbol counts have
stored data, while we don't touch MPS in this example.

`storage_size` measures the index relations, while physical rows count internal index records (not patients!)
The update age is the time since the recorded build. The update policy here will return default
`UPDATE_ON_INSERT`

```sql
SELECT index_name, update_policy,
  storage_size, time_since_update,
  bioseq.index_table_row_count(
    'samples', index_name) AS physical_rows
FROM bioseq.list_metadata()
WHERE table_name = 'samples'::regclass
ORDER BY index_name;
```

### 20. Try scalar minimizers, scalar chaining and KSW2

The three algorithm codes are all 0. The query returns five matches.

Adding read requirement 1 forbids persisted target-iadex reads for this call.
That matters because the stored index was  built with a different minimizer
algorithm and otherwise BioseqDB can produce an error

```sql
SELECT id, bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE mmsearch(seq,
  :'probe'::bioseq.seq('dna4', 64),
  aligner_options(
    minimizer_algorithm => 0,
    chaining_algorithm => 0,
    base_alignment_algorithm => 0,
    index_read_requirement => 1))
ORDER BY id;
```

### 21. Combine sequence operations and an edit filter

This returns four alignment rows for Anna, Ben and Daria.
It counts A bases, joins two 8bp slices, reads first letter of sequence, and
reverses or reverse-complements a prefix

The predicate `bioseq.edit(seq) < 5` enables experimental separate stage in aligner pipeline to estimate edit distance before alignement and filter out candidates

```sql
SELECT id,
  bioseq.symbol_count(seq, 'A') AS adenines,
  (seq[:8] OPERATOR(bioseq.||) seq[-8:])
    ::text AS joined_ends,
  seq[0]::text AS first_letter,
  bioseq.reverse(seq[:8])::text AS reversed,
  bioseq.reverse_complement(seq[:8])::text
    AS reverse_complement,
  bioseq.reverse(bioseq.reverse_complement(
    seq[:8]))::text AS complemented,
  bioseq.edit(seq) AS edits,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    :'probe'::bioseq.seq('dna4', 64))
  AND bioseq.edit(seq) < 5
ORDER BY id;
```

## Automatic index updates

Currently grouped-MPF which was measured in benchmarks for case of query/target table join doesn't work for single table queries correctly and causes minimizers to be always recomputed.
To prevent this you need to  change minimizer algorithm (the example below shows incremental automatic updates to index on inserts as well):

```sql
-- Disabled grouped-MPF and change minimizer algorithm to something else not bounded SIMD
SET bioseq.aligner.grouped_soa_generation.enabled = off;
SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';
-- Autmatic index update on each insert:
SET bioseq.index.insert_maintenance.enabled = on;

-- Your INSERT statements go here.
-- No update_index() call is needed for these new rows.
```

This code snippet avoid recomputing minimizer each time. Note that joins without these defaults (grouped-MPF for bounded SIMD) still require `aligner_options(index_read_requirement => 2)` to work correctly.
