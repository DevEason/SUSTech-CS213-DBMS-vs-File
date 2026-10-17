# Is a Database Worth It?

### A micro-benchmark comparing a plain file, an indexed file, PostgreSQL, and openGauss.

**SUSTech CS213 · Principles of Database Systems (H) · Fall 2026**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-18.6-336791.svg)](https://www.postgresql.org/)
[![openGauss](https://img.shields.io/badge/openGauss-7.0.0--RC3-B03A3A.svg)](https://opengauss.org/)

> On data that fits in memory, what does a DBMS actually buy you over a hand-written index?

This is a course project that asks one question nine ways. The **deliverable is
`report.pdf`** — page 1 is the answer, the rest is the evidence. The methodology, the
dual-clock timing model and the engineering trade-offs are all in there; this file only
gets you running.

---

## 📂 Repository

| File | Description |
|---|---|
| `report.pdf` | **The report.** Start here. |
| `filmbench.cpp` | The whole experiment: both file layouts and both databases, one C++ file (~1,340 lines). |
| `filmdb-load.sql` | The dataset: 10 tables, 76,131 rows, plain SQL for either engine. |
| `project1.pdf` | The assignment this answers. |
| `figures/*.png` | The three pictures below, rendered from the report's figures. |
| `README.md` | This file. |

There are no scripts. The CSVs the file side reads are exported straight out of the
database by `filmbench` itself, so both sides answer from byte-identical inputs.

## 📊 The answer on one page

![Every question, every system, on one clock](figures/chart-scoreboard.png)

*Every question, every system, on the clock they share. A hollow marker is a faster
but wrong answer: the file side never stems a word, so it misses "Foxes". The full
argument is Section 11 of the report.*

## 🧩 What you need

| | |
|---|---|
| WSL 2 + Ubuntu 24.04 | the client and both engines run **here** |
| `sudo apt install g++ libpq-dev` | g++ ≥ 13, libpq 16 |
| PostgreSQL, as a **Linux container** | 18.6 here; any recent version runs the harness |
| openGauss, as a **Linux container** (optional) | 7.0.0-RC3; needed only for Section 9 |

**The databases must be Linux containers inside WSL, not Windows installs.** A
PostgreSQL installed on Windows with the EDB installer is a different server: WSL
cannot reach it on `127.0.0.1`, and if it could, every number would include a 13–18 ms
trip across the Windows boundary. See what you actually have:

```bash
docker ps --format '{{.Names}}\t{{.Ports}}'
```

### Ports are yours, not mine

This project published PostgreSQL on **5434** and openGauss on **5432** so both could
run on one machine — those are not defaults. Yours are whatever `docker ps` printed.
`$DSN` is the only place it appears:

```bash
# PostgreSQL on 5432, openGauss on 5433 -- substitute what docker ps showed you
export DSN="host=127.0.0.1 port=5432 dbname=filmdb user=postgres password=YOURS"
```

`filmdb-load.sql` creates the tables, not the database. If yours has no `filmdb`,
create it first, connecting to the maintenance database (`dbname=postgres`):

```bash
# PostgreSQL
psql "$ADMIN_DSN" -c 'create database filmdb'

# openGauss: it defaults to A compatibility, where '' IS NULL, and this dataset has
# empty strings in NOT NULL columns. It must be created PG-compatible:
psql "$ADMIN_DSN" -c "create database filmdb with dbcompatibility 'PG'"
```

The loader checks this itself and stops with that instruction if the mode is wrong.

### No openGauss? You can still run almost all of it

openGauss is only needed for the PostgreSQL-vs-openGauss comparison (Section 9). Every
other section runs against PostgreSQL alone: skip the openGauss lines, or pass a
PostgreSQL `$DSN` and read the rows as one engine's results. The harness never assumes
two servers — it opens one connection per process.

If you do want it, any openGauss 7 image works; the harness talks to it through libpq
and needs nothing else.

## 🚀 Quick start

![The workflow](figures/pipeline.png)

*The client runs inside WSL beside the containers, both engines are loaded from the same
SQL file and checked identical before anything is timed, and the two checks bracket the
measurement.*

Run everything **inside WSL**, next to your containers.

### 1. Build, load, export

```bash
sudo apt install g++ libpq-dev
g++ -std=c++17 -O2 -I/usr/include/postgresql filmbench.cpp -o filmbench -lpq

# point DSN at whichever engine you are loading (repeat for the other)
export DSN="host=127.0.0.1 port=5434 dbname=filmdb user=postgres password=YOURS"

psql "$DSN" -f filmdb-load.sql          # ~2-4 s
mkdir -p /root/filmdata
./filmbench export --dsn "$DSN" --data /root/filmdata
```

Keep the CSVs on the Linux filesystem. The same files read through `/mnt/c/...` are
about nine times slower — enough to turn the T1 scan from 1.2 ms into 25 ms.

### 2. Ask one question four ways

```bash
./filmbench file --data /root/filmdata --layout csv     --op scan --term night   # plain file
./filmbench file --data /root/filmdata --layout indexed --op scan --term night   # indexed file
./filmbench db --dsn "$DSN" --op scan --term night                               # PostgreSQL
```

Each prints the answer and the median of 20 runs, after 3 discarded warm-ups.

### 3. Reproduce the full matrix

To get the caller's wait `C` **and** the engine's own duration `S` in one row, ask for
the statement log as well. `T` is the number `ping` prints — copy it into `--transfer`.

```bash
# T: one empty round trip. Run this **per engine** — T is a property of the link.
./filmbench ping --dsn "$DSN" --runs 20
# ping  min 0.07 ms  median 0.10 ms  max 0.21 ms
#                                             ^^^^ your T: put your own number in --transfer

# one measurement: C, S, T and the answer, as one tidy row
./filmbench db --dsn "$DSN" --op scan --term night --runs 20 --warmup 3 \
    --label postgresql --transfer 0.10 --match '%night%' \
    --log-server-timing --log-cmd 'docker logs postgresql 2>&1' --out rows.csv

# the two clocks must describe the same execution
./filmbench verify rows.csv
# 20 rows checked, 0 outside the tolerance (0.25 ms or 5%, whichever is larger)
```

On openGauss the log command is
`--log-cmd "docker exec opengauss bash -lc 'cat /var/lib/opengauss/data/pg_log/*.log'"`
— the container name is whatever `docker ps` called yours.
Repeat for each `--op`: `scan scani prefix exact lookup ftsearch join groupby update`,
with `--key 97` for `lookup`.

**T6 needs an index.** T6.1 is the command above with `--op ftsearch --term fox`; T6.2
is the same after the index exists; T6.3 forbids the planner to use it.

```bash
psql "$DSN" -c "CREATE INDEX ix_movies_ft ON movies USING gin (to_tsvector('english', title))"
psql "$DSN" -c "ANALYZE movies;"

# T6.2: the step-3 command with --op ftsearch --term fox --match 'to_tsquery'
# (--transfer is your own T again, not the 0.10 of the example)
./filmbench db --dsn "$DSN" --op ftsearch --term fox --runs 20 --warmup 3 \
    --label postgresql --transfer 0.10 --match 'to_tsquery' \
    --log-server-timing --log-cmd 'docker logs postgresql 2>&1' --out rows.csv

# T6.3: the same line with --no-index added
./filmbench db --dsn "$DSN" --op ftsearch --term fox --runs 20 --warmup 3 \
    --label postgresql --transfer 0.10 --match 'to_tsquery' \
    --log-server-timing --log-cmd 'docker logs postgresql 2>&1' --no-index --out rows.csv
```

Every command that writes `--out rows.csv` above produces the same eight columns, so
they can share one file; `verify` reads the header and checks them together.

## 🧠 The measurement model

Three numbers, all defined and justified in Section 2 of the report:

| | | |
|---|---|---|
| `C` | what the caller waited | a monotonic clock around one query |
| `S` | what the server spent | the engine's own statement log, same execution |
| `T` | the empty round trip | one `SELECT 1` |
| `ε` | the residual | `C − T − S` — parsing, planning, protocol |

![What the caller's wait is made of](figures/chart-twoclocks.png)

*One query, split into the three numbers: the round trip `T`, the server's own execution
`S`, and the residual `ε` that neither timer covers.*

Every comparison between systems uses `C`, because it is the one clock all four share.
`S` comes from the statement log (`--log-server-timing`); the second channel,
`EXPLAIN (ANALYZE)`, is a separate execution and is used only for the scaling section —
it is what column `explain_ms` holds when `--log-cmd` is absent.

## ⚠️ Known quirks (not bugs)

- **The file side finds 1 film for "fox"; the databases find 3.** The C++ index matches
  whole words and has no stemmer, so it never sees *Foxes*. Faster, and narrower — that
  is what Section 6 is about.
- **`--op update --layout indexed` exits with status 2.** That layout stopped reading
  `movies.csv` at start-up, so rewriting `people.csv` would measure nothing. It refuses
  rather than report a meaningless time.
- **`ALTER SYSTEM SET work_mem` fails on openGauss.** That one is session-level there,
  and `filmbench` sets it per session for both engines. Only `shared_buffers` is
  server-side.

## 🛠 Troubleshooting

**Every query takes 13–18 ms.** You are running the client from Windows. Move it inside
WSL.

**File-side numbers are ten times too slow.** The CSVs are on the Windows mount. Move
them to the Linux filesystem.

**`no duration for ... after the last filmbench-mark`.** The log command returned a log
without this run: wrong path, or the statement log is off. Keep `--log-server-timing`
on — that is what writes the marker.

## ⚙️ CLI reference

| option | meaning |
|---|---|
| `--op` | `scan scani prefix exact lookup ftsearch join groupby update` |
| `--layout` | `csv` or `indexed` |
| `--term`, `--key` | the parameter of the question |
| `--data DIR` | where the CSVs are (`/root/filmdata`) |
| `--dsn` | connection string |
| `--runs`, `--warmup` | default 20 and 3 |
| `--out FILE` | append the row to a CSV |
| `--label`, `--tag` | how the row is named |
| `--log-server-timing` | turn the session's statement log on and print the marker |
| `--log-cmd CMD` | the command that reads the log; `S` comes from it |
| `--log-file PATH` | the same, from a file |
| `--match TEXT` | what identifies this statement in the log |
| `--transfer MS` | `T`, so the row is complete |
| `--no-index` | forbid the planner to use any index |
| `--table` | query another table (the scaling run) |

Commands: `ping` · `export` · `file` · `db` · `verify`.
