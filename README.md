# Shopping Sessions

Native C++20 entry for the [Databricks Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). It writes both required Parquet tables with Arrow C++. The image downloads a pinned Arrow distribution at build time. The executable runs offline and uses no Python interpreter.

## Build and run

Build the image from the repository root.

```sh
docker build --platform linux/amd64 -t shopping-sessions .
mkdir -p output scratch
docker run --rm --network none --read-only --user 1000:1000 --cpus 8 --memory 24g \
  --tmpfs /tmp:size=64m \
  -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e NCPU=8 \
  -v "$PWD/data:/data:ro" -v "$PWD/output:/output" -v "$PWD/scratch:/scratch" \
  shopping-sessions
```

The mount directories for output and scratch must be writable by UID 1000. The executable reads the input layout described in the track rules and writes `output/user_daily/*.parquet` and `output/campaign_daily/*.parquet`. Set `PROFILE=1` to log stage timings to stderr. The default native build targets x86-64-v3 (AVX2). Use `make ARCH_FLAGS=-march=x86-64` for older local x86-64 CPUs.

## Run the official checks

For a native Linux build, install a C++20 compiler, GNU Make, curl and unzip. Set `DUCKDB` to the path to a DuckDB 1.4.2 CLI executable or put `duckdb` on PATH. Download the track's starter, generator and dev answer from the track page.

```sh
make
make check-fixtures FIXTURES=/path/to/starter-duckdb/fixtures
/path/to/generate --seed 1 --events 1000000 --clicks 250000 --output dev
make check-dev DEV_DATA=dev DEV_ANSWER=/path/to/dev-answer
```

The checks compare rows in both directions, including duplicate multiplicity and nullable categories.

## Implementation

- The CSV parser preserves field bytes, strictly checks quoting and accepts only LF as a record boundary. Input is memory mapped and split at line boundaries for parallel reads.
- Union-find resolves linked users across all files. Each component keeps its byte-wise smallest ID, including IDs that occur only in links.
- Event IDs are hash-sharded and deduplicated with full string comparisons. Syntactically malformed copies are excluded before earliest-ingest selection. Validation and the lateness cutoff apply to the selected copy.
- Events and clicks are routed by canonical person, sorted by timestamp and processed together. Session days come from UTC start times. Daily categories use summed purchase prices and byte-wise tie-breaking.
- Each click defines an interval of eligible session starts. Per-campaign interval coverage counts each assisting campaign once per session without materializing a click-to-session join. Attribution uses the latest eligible click and the byte-wise smallest click ID at equal timestamps.
- Workers write user Parquet files independently. Campaign totals are reduced across workers and written as a separate Parquet table.

## Submit

Put the contents of this directory at the root of a public GitHub repository and submit the commit on the track page. The source package contains the root Dockerfile and build scripts. Keep generated inputs, benchmark outputs and downloaded dependencies outside Git.
