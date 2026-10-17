// filmbench: one dataset, one set of questions, four layouts.
//   file/csv (re-parsed per query)  file/indexed (index built once)  PostgreSQL  openGauss
//   The report decomposes C = T + S + eps: S is read from the statement log (the same
//   execution, no parse or plan); EXPLAIN (ANALYZE) is a second execution, scaling only.
//   Build:  g++ -std=c++17 -O2 -I/usr/include/postgresql filmbench.cpp -o filmbench -lpq
// ============================================================================

// the file side reads data with stdio only
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>

#include <time.h>

#include </usr/include/postgresql/libpq-fe.h>

// ============================================================================
//  1.  clocks and timers
// ============================================================================
static const double g_ticksPerMs = 1e6;      // clock_gettime counts nanoseconds

// Monotonic clock, nanoseconds. A system-clock reset cannot disturb a measurement.
static long long now_ticks()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Timer for one operation: constructed at the start, ms() at the end.
class Timer
{
public:
    Timer() : start_(now_ticks()) {}
    double ms() const { return (double)(now_ticks() - start_) / g_ticksPerMs; }
private:
    long long start_;
};

// ============================================================================
//  2.  small helpers
// ============================================================================

struct Result
{
    double minMs, medMs, maxMs;
    int    runs;
};

// Summarise repeated runs: median, the mean of the two middle samples when the count is even.
static Result summarise(std::vector<double> samples)
{
    Result r;
    r.runs = (int)samples.size();
    std::sort(samples.begin(), samples.end());
    r.minMs = samples.front();
    r.maxMs = samples.back();

    size_t n = samples.size();
    if (n % 2 == 1)
        r.medMs = samples[n / 2];
    else
        r.medMs = 0.5 * (samples[n / 2 - 1] + samples[n / 2]);

    return r;
}

// ASCII lower-casing.
static std::string to_lower(const std::string& s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

// Substring scan without allocating: building a string per row would measure malloc.
static bool contains(const char* hay, size_t hayLen, const char* needle, size_t needleLen)
{
    if (needleLen == 0) return true;
    if (needleLen > hayLen) return false;
    const char* last = hay + (hayLen - needleLen);
    for (const char* p = hay; p <= last; ++p)
        if (*p == *needle && memcmp(p, needle, needleLen) == 0) return true;
    return false;
}

// Case-blind substring scan.
static bool containsIgnoreCase(const char* hay, size_t hayLen,
                               const char* needle, size_t needleLen)
{
    if (needleLen == 0) return true;
    if (needleLen > hayLen) return false;
    const char* last = hay + (hayLen - needleLen);
    for (const char* p = hay; p <= last; ++p) {
        size_t k = 0;
        while (k < needleLen &&
               tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
            ++k;
        if (k == needleLen) return true;
    }
    return false;
}

// ============================================================================
//  3.  reading the data
// ============================================================================

// Reads CSV row by row: quoted fields, commas inside quotes, two quotes for one.
// No embedded newlines (this data has none); an empty unquoted field is NULL.
class CsvReader
{
public:
    explicit CsvReader(FILE* f) : f_(f) {}

    // an empty unquoted field is NULL
    bool nextRow(std::vector<std::string>& fields, std::vector<char>& isNull)
    {
        fields.clear();
        isNull.clear();

        int c = getc(f_);
        if (c == EOF) return false;

        for (;;) {
            std::string field;
            bool null = false;

            if (c == '"') { // a quoted field
                for (;;) {
                    c = getc(f_);
                    if (c == EOF) break;
                    if (c == '"') {
            // Two quotes are one quote; after a single quote the next character decides the end.
                        int d = getc(f_);
                        if (d == '"') { field += '"'; c = d; continue; }
                        c = d;
                        break;
                    }
                    field += (char)c;
                }
            } else {    // an unquoted field
            // empty and unquoted means NULL.
                if (c == ',' || c == '\n' || c == '\r' || c == EOF) {
                    null = true;
                } else {
                    while (c != ',' && c != '\n' && c != '\r' && c != EOF) {
                        field += (char)c;
                        c = getc(f_);
                    }
                }
            }

            fields.push_back(field);
            isNull.push_back(null ? 1 : 0);

            if (c == ',') { c = getc(f_); continue; }   // more fields: hand c to the next round
        // CRLF: do not eat the first character of the next row.
            if (c == '\r') { int d = getc(f_); if (d != '\n' && d != EOF) ungetc(d, f_); }
            break;
        }
        return true;
    }

    private:
        FILE* f_;
};

// --- the packed in-memory record -------------------------------------------

// Packed, no alignment padding: the indexed layout scans the array as one block, so the
// stride must be fixed. The record is 113 bytes; the 9 reserved bytes are read by no
// question. Drop them and the record becomes 104 bytes, changing every indexed scan.
constexpr size_t RECORD_SIZE = 113;

#pragma pack(push, 1)
struct MovieRecord
{
    int32_t movieid;

    unsigned char reserved[9];      // year / runtime / hasRuntime at export

    char    country[4];      // two-letter country code + terminator
    char    title[96];       // the longest real title is 81 bytes
};
#pragma pack(pop)

// Pin the record size so the layout cannot drift by accident.
static_assert(sizeof(MovieRecord) == RECORD_SIZE,
              "record size fixes the stride of the indexed scan; every file/indexed "
              "number in the report was measured with this layout");

// The three indexes, all built before timing starts: byId for lookups, byTitle for
// prefix and exact matches, byToken for full text.
struct MovieTable
{
    std::vector<MovieRecord> rows;
    std::unordered_map<int, int> byId;        // movieid -> row number
    std::vector<int>             byTitle;     // row numbers, sorted by title
    std::unordered_map<std::string, std::vector<int> > byToken;

    static std::string field(const char* p, size_t n)
    {
        return std::string(p, strnlen(p, n));
    }

    // Reads movies.csv and builds the indexes. The field order must match export:
    // movieid, title, country, year, runtime.
    bool load(const std::string& path)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;

        CsvReader reader(f);
        std::vector<std::string> fields;
        std::vector<char> isNull;

        while (reader.nextRow(fields, isNull)) {
            if (fields.size() < 5) continue;
            if (fields[1].size() >= sizeof(MovieRecord::title)) {
                fprintf(stderr, "title too long for the record layout: %s\n",
                        fields[1].c_str());
                fclose(f);
                return false;
            }
            if (fields[2].size() >= sizeof(MovieRecord::country)) {
                fprintf(stderr, "country too long for the record layout: %s\n",
                        fields[2].c_str());
                fclose(f);
                return false;
            }

            MovieRecord rec;
            memset(&rec, 0, sizeof rec);
            rec.movieid    = atoi(fields[0].c_str());
            memcpy(rec.country, fields[2].data(),
                   std::min(fields[2].size(), sizeof rec.country - 1));
            memcpy(rec.title, fields[1].data(), fields[1].size());
            rows.push_back(rec);
        }
        fclose(f);

        // The three index structures are built once, before any timer starts.
        size_t n = rows.size();
        // Reserve buckets to avoid rehashing.
        byId.reserve(n * 2);
        // The copy into id is not decoration: MovieRecord is packed, so the field sits at
        // an odd offset, and operator[] takes its key by reference -- binding that reference
        // to a misaligned int is undefined behaviour, and UBSan says so.
        for (size_t i = 0; i < n; ++i) {
            int id = rows[i].movieid;
            byId[id] = (int)i;
        }

        byTitle.resize(n);
        for (size_t i = 0; i < n; ++i) byTitle[i] = (int)i;
        std::sort(byTitle.begin(), byTitle.end(),
                  [this](int a, int b) {
                      return strcmp(rows[a].title, rows[b].title) < 0;
                  });

        for (size_t i = 0; i < n; ++i) {
            const char* t = rows[i].title;
            std::string token;
            // Plain string tokenising with no stemmer: "fox" does not find "Foxes", which is exactly
            // why the file side answers T6.1 more narrowly. Bytes >= 0x80 count as word characters,
            // otherwise a non-ASCII title is chopped into pieces.
            for (const char* p = t; ; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c && (isalnum(c) || c >= 0x80)) token += (char)tolower(c);
                else {
                    // Not a word character: harvest the word. The trailing \0 is what closes the last one.
                    if (!token.empty()) { byToken[token].push_back((int)i); token.clear(); }
                    if (!c) break;
                }
            }
        }
        return true;
    }
};

// ============================================================================
//  4.  the database side (libpq)
// ============================================================================

// The database side. PostgreSQL and openGauss both go through libpq: same queries, same
// memory budget, and only the connection string differs.
class Database
{
public:
    Database() : conn_(nullptr) {}

    ~Database() { if (conn_) PQfinish(conn_); }

    // Connect. main exits if this fails.
    bool connect(const std::string& dsn)
    {
        conn_ = PQconnectdb(dsn.c_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            fprintf(stderr, "connection failed: %s\n", PQerrorMessage(conn_));
            return false;
        }
        return true;
    }

    // Run a statement whose result is not wanted: SET, BEGIN, ROLLBACK, DECLARE CURSOR.
    void exec(const std::string& sql)
    {
        PGresult* r = PQexec(conn_, sql.c_str());
        if (r) PQclear(r);
    }

    PGconn* handle() const { return conn_; }

    // Give both engines the same memory budget. The GUC names differ; unknown ones are skipped.
    void alignSettings()
    {
        const char* settings[] = {
            "SET work_mem = '64MB'",
            "SET effective_cache_size = '4GB'",
            "SET synchronous_commit = on",
            "SET max_parallel_workers_per_gather = 0",   // PostgreSQL only
            "SET query_dop = 1",                          // openGauss only
            "SET jit = off",                              // PostgreSQL only
            "SET client_encoding = 'UTF8'"
        };
        runAll(settings);
    }

    // Turn the statement log on and print the marker. The report's decomposition reads S
    // from this log: the same execution, including write cost. EXPLAIN (ANALYZE) is
    // a second execution, used only for scaling.
    void enableStatementLogging()
    {
        const char* settings[] = {
            "SET log_min_duration_statement = 0",   // PostgreSQL
            "SET log_duration = on"                 // openGauss
        };
        runAll(settings);

        // The marker locates this run's statements in a log that others also write to.
        PGresult* r = PQexec(conn_, "SELECT 'filmbench-mark'");
        if (r) PQclear(r);
    }

    // T6.3: keep the index, forbid the planner to use it - proof that T6.2's gain is the index.
    void forbidIndexScans()
    {
        const char* settings[] = {
            "SET enable_indexscan = off",
            "SET enable_bitmapscan = off",
            "SET enable_indexonlyscan = off"    // PostgreSQL only
        };
        runAll(settings, true);
    }

    // Run SQL and return its answer: a single numeric cell if there is one, else the row
    // count; an UPDATE returns the number of rows changed.
    long long run(const std::string& sql)
    {
        PGresult* r = PQexec(conn_, sql.c_str());
        ExecStatusType status = PQresultStatus(r);
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
            fprintf(stderr, "query failed: %s\n%s\n", sql.c_str(), PQerrorMessage(conn_));
            PQclear(r);
            exit(3);
        }

        // The field count must be checked, or a multi-column result yields its first column.
        long long answer = (long long)PQntuples(r);
        if (status == PGRES_COMMAND_OK) {
            const char* changed = PQcmdTuples(r);      // how many rows changed
            if (changed && *changed) answer = strtoll(changed, nullptr, 10);
        } else if (answer == 1 && PQnfields(r) == 1 && !PQgetisnull(r, 0, 0)) {
            const char* v = PQgetvalue(r, 0, 0);
            char* end = nullptr;
            long long n = strtoll(v, &end, 10);
            if (end && end != v && *end == '\0') answer = n;   // it is a plain number
        }

        PQclear(r);
        return answer;
    }

    // Execution time from EXPLAIN (ANALYZE): a second execution, no planning, scaling only.
    // The caller samples ten times and takes the median.
    bool serverTime(const std::string& sql, double& ms)
    {
        std::string explain = "EXPLAIN (ANALYZE) " + sql;
        PGresult* r = PQexec(conn_, explain.c_str());
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return false; }

        ms = -1.0;
        for (int i = 0; i < PQntuples(r); ++i) {
            const char* line = PQgetvalue(r, i, 0);
            const char* hit = strstr(line, "Execution Time:");
            if (!hit) hit = strstr(line, "Total runtime:");
            if (hit) {
                hit = strchr(hit, ':') + 1;
                ms = atof(hit);
            }
        }
        PQclear(r);
        return ms >= 0;
    }

private:
    // Run the statements one by one. warn=false: cross-engine compatibility, failure expected;
    // warn=true: this setting matters, so say so when the engine refuses it.
    template <size_t N>
    void runAll(const char* const (&sql)[N], bool warn = false)
    {
        for (size_t i = 0; i < N; ++i) {
            PGresult* r = PQexec(conn_, sql[i]);
            if (warn && r && PQresultStatus(r) != PGRES_COMMAND_OK)
                fprintf(stderr, "setting rejected (%s): %s\n", sql[i], PQerrorMessage(conn_));
            if (r) PQclear(r);
        }
    }

    PGconn* conn_;
};

// ============================================================================
//  5.  the operations
// ============================================================================

// Film queries hit movies by default; the scaling run points --table at the large table.
static std::string g_moviesTable = "movies";

// Double the single quotes in a SQL literal. Unescaped, the string closes early.
static std::string quoteOne(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'') out += "''";
        else              out += s[i];
    }
    return out;
}

// Build a SQL string literal.
static std::string quoteLiteral(const std::string& s)
{
    return "'" + quoteOne(s) + "'";
}

// Wrap a term in the wildcards LIKE needs.
static std::string quoteLike(const std::string& s, const char* prefix, const char* suffix)
{
    return "'" + std::string(prefix) + quoteOne(s) + suffix + "'";
}

// The SQL for the nine questions. The file side answers them by other means.
// op: scan/scani/prefix/exact/lookup/ftsearch/join/groupby/update.
static std::string sqlFor(const std::string& op, const std::string& term, int key)
{
    const std::string T = g_moviesTable;
    if (op == "scan")    return "SELECT count(*) FROM " + T + " WHERE title LIKE " + quoteLike(term, "%", "%");
    if (op == "scani")   return "SELECT count(*) FROM " + T + " WHERE title ILIKE " + quoteLike(term, "%", "%");
    if (op == "prefix")  return "SELECT count(*) FROM " + T + " WHERE title LIKE " + quoteLike(term, "", "%");
    if (op == "exact")   return "SELECT count(*) FROM " + T + " WHERE title = " + quoteLiteral(term);
    if (op == "lookup")  return "SELECT count(*) FROM " + T + " WHERE movieid = " + std::to_string(key);
    if (op == "ftsearch")return "SELECT count(*) FROM " + T + " WHERE to_tsvector('english', title) "
                                "@@ to_tsquery('english', " + quoteLiteral(term) + ")";
    if (op == "join")    return "SELECT count(*) FROM " + T + " m "
                                "JOIN credits c ON c.movieid = m.movieid "
                                "JOIN people  p ON p.peopleid = c.peopleid "
                                "WHERE m.title LIKE " + quoteLike(term, "%", "%");
    if (op == "groupby") return "SELECT country, count(*) FROM " + T + " GROUP BY country";
    if (op == "update")  return "UPDATE people SET first_name = replace(first_name,'T','Ttt'), "
                                "surname = replace(surname,'T','Ttt')";
    return "";
}

// File-side match kind: 0 substring, 1 case-blind substring, 2 prefix, 3 exact.
static int matchKind(const std::string& op)
{
    if (op == "scani")  return 1;   // case-blind substring
    if (op == "prefix") return 2;   // starts with
    if (op == "exact")  return 3;   // equal
    return 0;                       // case-sensitive substring
}

// --- writing CSV -----------------------------------------------------------

// Write a CSV field: NULL as empty, otherwise quoted, with quotes doubled.
static void csvWriteField(FILE* f, const char* value, bool isNull)
{
    if (isNull) return;                     // NULL -> nothing between the two commas
    fputc('"', f);
    for (const char* p = value; *p; ++p) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

// --- the file side ---------------------------------------------------------

// The file side. csv: re-reads the file per query; indexed: loads and indexes once.
class FileSide
{
public:
    FileSide(const std::string& dir, const std::string& layout)
        : dir_(dir), layout_(layout) {}

    // indexed: load movies, people and credits at start-up.
    // csv: only check that movies.csv exists; the reading happens in each query.
    bool load()
    {
        if (layout_ == "indexed") {
            if (!table_.load(path("movies.csv"))) return false;
            loadPeopleAndCredits();     // this layout may preload
            return true;
        }

        // The csv layout re-reads movies.csv per query, so probe it once here:
        // a wrong path then fails at start-up, not after twenty runs that each "return 0".
        FILE* probe = fopen(path("movies.csv").c_str(), "rb");
        if (!probe) {
            fprintf(stderr, "cannot open %s\n", path("movies.csv").c_str());
            return false;
        }
        fclose(probe);
        return true;
    }

    // Dispatch on the operation. noop touches no data: it measures the instrument floor;
    // the indexed layout refuses update outright.
    long long run(const std::string& op, const std::string& term, int key)
    {
        if (op == "scan" || op == "scani" || op == "prefix" || op == "exact")
            return search(term, matchKind(op));
        if (op == "lookup")    return lookup(key);
        if (op == "ftsearch")  return fullTextSearch(term);
        if (op == "join")      return joinCount(term);
        if (op == "groupby")   return groupByCountry();
        // The empty path: no work at all, for the instrument's own cost (18 ns in Section 5).
        if (op == "noop")      return 0;
        if (op == "update") {
            // The indexed layout read movies.csv once at start-up, so rewriting
            // people.csv would mean nothing; refuse rather than return a fake time.
            if (layout_ == "indexed") {
                fprintf(stderr, "update is not supported on the indexed layout\n");
                exit(2);
            }
            return rewritePeopleFile();
        }
        return 0;
    }

private:
    std::string dir_, layout_;
    MovieTable  table_;

    // The two small tables the join needs: indexed keeps them, csv re-reads them every call.
    std::vector<std::pair<int, int> > credits_;      // (movieid, peopleid)
    std::unordered_map<int, char>     peopleIds_;    // peopleid -> 1

    std::string path(const char* name) const { return dir_ + "/" + name; }

    // --- search ----------------------------------------------------------------

    // T1-T4: title search. indexed uses its index where it can, else scans memory; csv re-reads.
    long long search(const std::string& term, int kind)
    {
        if (layout_ == "indexed") {
            if (kind == 2) return prefixFromIndex(term);
            if (kind == 3) return exactFromIndex(term);
            return scanMemory(term, kind);
        }
        return scanCsv(term, kind);
    }

    // The four title match kinds.
    static bool matches(const char* text, size_t len, const std::string& term, int kind)
    {
        switch (kind) {
            case 1:  return containsIgnoreCase(text, len, term.data(), term.size());
            case 2:  return len >= term.size() && memcmp(text, term.data(), term.size()) == 0;
            case 3:  return len == term.size() && memcmp(text, term.data(), len) == 0;
            default: return contains(text, len, term.data(), term.size());
        }
    }

    // Re-read movies.csv each time: the plain CSV's main cost.
    long long scanCsv(const std::string& term, int kind)
    {
        FILE* f = fopen(path("movies.csv").c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open movies.csv\n"); exit(2); }

        CsvReader reader(f);
        std::vector<std::string> fields;
        std::vector<char> isNull;
        long long hits = 0;

        while (reader.nextRow(fields, isNull)) {
            if (fields.size() < 5) continue;
            if (matches(fields[1].data(), fields[1].size(), term, kind)) ++hits;
        }
        fclose(f);
        return hits;
    }

    // Scan the fixed-length records loaded at start-up.
    long long scanMemory(const std::string& term, int kind)
    {
        long long hits = 0;
        for (size_t i = 0; i < table_.rows.size(); ++i) {
            const char* t = table_.rows[i].title;
            if (matches(t, strnlen(t, sizeof table_.rows[i].title), term, kind)) ++hits;
        }
        return hits;
    }

    // T3: prefix match. Titles are sorted: binary-search the interval start, then count in order.
    long long prefixFromIndex(const std::string& term)
    {
        std::vector<int>& order = table_.byTitle;
        size_t lo = 0, hi = order.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (strncmp(table_.rows[order[mid]].title, term.c_str(), term.size()) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        // The interval is contiguous, so the first mismatch ends the count.
        long long hits = 0;
        for (size_t i = lo; i < order.size(); ++i) {
            if (strncmp(table_.rows[order[i]].title, term.c_str(), term.size()) != 0) break;
            ++hits;
        }
        return hits;
    }

    // T4: exact match. Same search as prefix, but the whole title is compared.
    long long exactFromIndex(const std::string& term)
    {
        std::vector<int>& order = table_.byTitle;
        size_t lo = 0, hi = order.size();
        while (lo < hi) {                       // first row >= term
            size_t mid = (lo + hi) / 2;
            if (strcmp(table_.rows[order[mid]].title, term.c_str()) < 0) lo = mid + 1;
            else hi = mid;
        }
        long long hits = 0;
        for (size_t i = lo; i < order.size(); ++i) {
            if (strcmp(table_.rows[order[i]].title, term.c_str()) != 0) break;
            ++hits;
        }
        return hits;
    }

    // --- lookups ---------------------------------------------------------------
    // T5: look up by movieid. indexed hashes; csv reads in order and stops at the hit.
    // movieid 97 sits near the start of the file, so the csv layout is fast here too.
    long long lookup(int key)
    {
        if (layout_ == "indexed") {
            return table_.byId.count(key) ? 1 : 0;
        }
        FILE* f = fopen(path("movies.csv").c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path("movies.csv").c_str()); exit(2); }
        CsvReader reader(f);
        std::vector<std::string> fields;
        std::vector<char> isNull;
        long long found = 0;
        while (reader.nextRow(fields, isNull))
            if (!fields.empty() && atoi(fields[0].c_str()) == key) { found = 1; break; }
        fclose(f);
        return found;
    }

    // --- full text -------------------------------------------------------------
    // The file side full text: whole lower-case words only, no stemmer.
    // So "fox" does not find "Foxes".
    static bool hasToken(const char* text, size_t len, const std::string& want)
    {
        size_t start = 0;
        bool inside = false;
            // Walk to the end of the word, then compare length first and content after.
        for (size_t i = 0; i <= len; ++i) {
            unsigned char c = (i < len) ? (unsigned char)text[i] : 0;
            bool isWord = c && (isalnum(c) || c >= 0x80);
            if (isWord) {
                if (!inside) { inside = true; start = i; }   // a word starts here
            } else if (inside) {
                inside = false;                              // the word ends here
                if (i - start == want.size()) {
                    size_t k = 0;
                    while (k < want.size() &&
                           tolower((unsigned char)text[start + k]) == want[k]) ++k;
                    if (k == want.size()) return true;
                }
            }
        }
        return false;
    }

    // T6.1 on the file side. indexed consults its token table; csv tokenises every row.
    long long fullTextSearch(const std::string& term)
    {
        std::string want = to_lower(term);
        if (layout_ == "indexed") {
            std::unordered_map<std::string, std::vector<int> >::iterator it =
                table_.byToken.find(want);
            return it == table_.byToken.end() ? 0 : (long long)it->second.size();
        }

        long long hits = 0;
        FILE* f = fopen(path("movies.csv").c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path("movies.csv").c_str()); exit(2); }
        CsvReader reader(f);
        std::vector<std::string> fields;
        std::vector<char> isNull;
        while (reader.nextRow(fields, isNull))
            if (fields.size() >= 2 &&
                hasToken(fields[1].data(), fields[1].size(), want)) ++hits;
        fclose(f);
        return hits;
    }

    // --- join ------------------------------------------------------------------
    // T7: the join, file-style. Three passes: matching films, people that exist, credits.
    // indexed reads people and credits at start-up; csv re-reads them per call.
    long long joinCount(const std::string& term)
    {
        // The films whose titles match the term.
        std::unordered_map<int, char> wantedMovie;
        if (layout_ == "indexed") {
            for (size_t i = 0; i < table_.rows.size(); ++i) {
                const MovieRecord& r = table_.rows[i];
                if (contains(r.title, strnlen(r.title, sizeof r.title),
                             term.data(), term.size())) {
                    int id = r.movieid;         // packed field: do not bind a reference to it
                    wantedMovie[id] = 1;
                }
            }
        } else {
            readMovieIdsMatching(term, wantedMovie);
        }

        // The indexed layout has these two tables already; otherwise read now, and the cost counts.
        if (layout_ != "indexed") loadPeopleAndCredits();

        long long hits = 0;
        for (size_t i = 0; i < credits_.size(); ++i)
            if (wantedMovie.count(credits_[i].first) && peopleIds_.count(credits_[i].second))
                ++hits;
        return hits;
    }

    // Read people.csv and credits.csv. A missing file exits:
    // an empty set cannot be returned, because 0 is also a legal answer.
    void loadPeopleAndCredits()
    {
        peopleIds_.clear();
        credits_.clear();

        FILE* fp = fopen(path("people.csv").c_str(), "rb");
        if (!fp) { fprintf(stderr, "cannot open %s\n", path("people.csv").c_str()); exit(2); }
        {
            CsvReader reader(fp);
            std::vector<std::string> fields;
            std::vector<char> isNull;
            while (reader.nextRow(fields, isNull))
                if (!fields.empty()) peopleIds_[atoi(fields[0].c_str())] = 1;
            fclose(fp);
        }

        FILE* fc = fopen(path("credits.csv").c_str(), "rb");
        if (!fc) { fprintf(stderr, "cannot open %s\n", path("credits.csv").c_str()); exit(2); }
        {
            CsvReader reader(fc);
            std::vector<std::string> fields;
            std::vector<char> isNull;
            while (reader.nextRow(fields, isNull))
                if (fields.size() >= 2)
                    credits_.push_back(std::make_pair(atoi(fields[0].c_str()),
                                                      atoi(fields[1].c_str())));
            fclose(fc);
        }
    }

    // Scan movies.csv for the movieids whose titles contain the term.
    void readMovieIdsMatching(const std::string& term, std::unordered_map<int, char>& out)
    {
        FILE* f = fopen(path("movies.csv").c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path("movies.csv").c_str()); exit(2); }
        CsvReader reader(f);
        std::vector<std::string> fields;
        std::vector<char> isNull;
        while (reader.nextRow(fields, isNull))
            if (fields.size() >= 2 &&
                contains(fields[1].data(), fields[1].size(), term.data(), term.size()))
                out[atoi(fields[0].c_str())] = 1;
        fclose(f);
    }

    // --- group by --------------------------------------------------------------

    // T8: count the countries. The file side groups by bytes; NULL exports as an empty field,
    // and this data has no NULL-versus-empty-string distinction, so it matches the database.
    long long groupByCountry()
    {
        std::unordered_map<std::string, long long> groups;

        if (layout_ == "indexed") {
            for (size_t i = 0; i < table_.rows.size(); ++i) {
                const MovieRecord& r = table_.rows[i];
                ++groups[MovieTable::field(r.country, sizeof r.country)];
            }
            return (long long)groups.size();
        }

        FILE* f = fopen(path("movies.csv").c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path("movies.csv").c_str()); exit(2); }
        CsvReader reader(f);
        std::vector<std::string> fields;
        std::vector<char> isNull;
        while (reader.nextRow(fields, isNull))
            if (fields.size() >= 5) ++groups[fields[2]];
        fclose(f);
        return (long long)groups.size();
    }

    // T9 on the file side: read people.csv, replace T, write a new file. The original stays,
    // so every repetition starts from the same bytes - the database repeats by ROLLBACK,
    // the file by not overwriting. No WAL, no transaction, no constraints: the least work.
    long long rewritePeopleFile()
    {
        const std::string src = path("people.csv");

        FILE* in = fopen(src.c_str(), "rb");
        FILE* out = fopen(path("people.new.csv").c_str(), "wb");
        if (!in || !out) { fprintf(stderr, "update: cannot open %s\n", src.c_str()); exit(2); }

        CsvReader reader(in);
        std::vector<std::string> fields;
        std::vector<char> isNull;
        long long rows = 0;

        while (reader.nextRow(fields, isNull)) {
            if (fields.size() < 6) continue;

            if (!isNull[1]) fields[1] = replaceT(fields[1]);
            fields[2] = replaceT(fields[2]);

            for (size_t i = 0; i < 6; ++i) {
                if (i) fputc(',', out);
                csvWriteField(out, fields[i].c_str(), isNull[i] != 0);
            }
            fputc('\n', out);
            ++rows;
        }

        fclose(in);
        fflush(out);
        fclose(out);

        // Move the new file into place. POSIX rename replaces atomically; the safety rests on it.
        const std::string dst = path("people.rewritten.csv");
        if (rename(path("people.new.csv").c_str(), dst.c_str()) != 0)
            fprintf(stderr, "update: could not rename people.new.csv into place; "
                            "the rewritten copy is left as people.new.csv\n");
        return rows;
    }

    // The same as SQL replace(x, 'T', 'Ttt').
    static std::string replaceT(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == 'T') out += "Ttt";
            else out += s[i];
        }
        return out;
    }
};

// ============================================================================
//  6.  export, so the file side reads the same bytes as the database
// ============================================================================
// Export a table as CSV. The file side reads the bytes the database produced, which is
// what makes the four layouts comparable. A cursor keeps the result set out of memory.
static bool exportTable(Database& db, const std::string& table, const std::string& sql,
                        const std::string& outPath)
{
    FILE* out = fopen(outPath.c_str(), "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return false; }

    PGresult* r = PQexec(db.handle(), ("DECLARE cur NO SCROLL CURSOR FOR " + sql).c_str());
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "%s\n", PQerrorMessage(db.handle()));
        PQclear(r);
        fclose(out);
        return false;
    }
    PQclear(r);

    long long total = 0;
    for (;;) {
        r = PQexec(db.handle(), "FETCH FORWARD 20000 FROM cur");
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); break; }
        int n = PQntuples(r);
        if (n == 0) { PQclear(r); break; }

        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < PQnfields(r); ++j) {
                if (j) fputc(',', out);
                bool isNull = PQgetisnull(r, i, j) != 0;
                csvWriteField(out, isNull ? "" : PQgetvalue(r, i, j), isNull);
            }
            fputc('\n', out);
        }
        total += n;
        PQclear(r);
    }
    PQexec(db.handle(), "CLOSE cur");
    fclose(out);
    printf("  %-8s %8lld rows -> %s\n", table.c_str(), total, outPath.c_str());
    return true;
}

// ============================================================================
//  7.  the command line
// ============================================================================

// A small argument parser: --name value; switches carry no value.
struct Options
{
    std::map<std::string, std::string> values;

    std::string get(const std::string& key, const std::string& fallback = "") const
    {
        std::map<std::string, std::string>::const_iterator it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    int getInt(const std::string& key, int fallback) const
    {
        std::string v = get(key);
        return v.empty() ? fallback : atoi(v.c_str());
    }
    bool has(const std::string& key) const { return values.count(key) > 0; }
};

static void printUsage()
{
    printf(
        "filmbench - one program, four ways of answering the same question\n"
        "\n"
        "  filmbench ping    --dsn DSN\n"
        "  filmbench export  --dsn DSN --data DIR\n"
        "  filmbench file    --data DIR --layout csv|indexed --op OP [options]\n"
        "  filmbench db      --dsn DSN --op OP [options]\n"
        "  filmbench verify  rows.csv   (checks C = T + S + eps; exits 1 on a violation)\n"
        "\n"
        "  layouts:    csv (parse movies.csv per query)\n"
        "              indexed (parse movies.csv once, then use in-memory indexes)\n"
        "  operations: scan scani prefix exact lookup ftsearch join groupby update\n"
        "              noop (file side: the empty path, to measure the harness itself)\n"
        "  options:    --term T --key K --runs N --warmup N --label NAME\n"
        "              --tag TEXT (free label stored in the CSV) --out CSV\n"
        "              --table NAME (aim the movie queries at another table)\n"
        "              --log-cmd CMD | --log-file PATH   (where to read the log)\n"
        "              --match TEXT --transfer MS\n"
        "                                   (S from that log; writes the tidy row)\n"
        "              --log-server-timing (turn the session's statement log on and\n"
        "                                   print the marker an external reader uses;\n"
        "                                   it does not change how S is measured)\n"
        "              --no-index (forbid the planner from using any index)\n");
}

// Time one operation. update runs inside BEGIN/ROLLBACK so the data survives.
static double timeOnce(FileSide* file, Database* db, const std::string& op,
                       const std::string& term, int key, long long& answer)
{
    Timer t;

    if (file) {
        answer = file->run(op, term, key);
    } else if (op == "update") {
        // EXPLAIN (ANALYZE) performs the update, so it is wrapped and rolled back;
        // otherwise every run lengthens the names (T -> Ttt -> Tttt).
        db->exec("BEGIN");
        answer = db->run(sqlFor(op, term, key));
        db->exec("ROLLBACK");
    } else {
        answer = db->run(sqlFor(op, term, key));
    }

    return t.ms();
}

// Warm up first, then measure. The answer is the one the last run produced.
static Result measure(FileSide* file, Database* db, const std::string& op,
                      const std::string& term, int key, long long& answer,
                      int warmup, int runs)
{
    for (int i = 0; i < warmup; ++i)
        timeOnce(file, db, op, term, key, answer);

    std::vector<double> samples;
    for (int i = 0; i < runs; ++i)
        samples.push_back(timeOnce(file, db, op, term, key, answer));
    return summarise(samples);
}

// Append a result row, writing a header when the file is new. Ordinary rows and the tidy
// rows that verify reads use different headers. A failed write does not abort the run.
static void appendCsv(const std::string& path, const std::string& row,
                      const char* header =
                          "system,op,layout,term,tag,answer,runs,min_ms,median_ms,max_ms,explain_ms")
{
    if (path.empty()) return;

    bool fresh = true;
    FILE* probe = fopen(path.c_str(), "rb");
    if (probe) { fresh = (fgetc(probe) == EOF); fclose(probe); }

    FILE* f = fopen(path.c_str(), "ab");
    if (!f) return;
    if (fresh) fprintf(f, "%s\n", header);
    fputs(row.c_str(), f);
    fclose(f);
}

// Read every line of the log, from a file or from a command the caller supplied:
// the program assumes neither docker nor the log location.
static void logLines(const std::string& path, const std::string& cmd,
                     std::vector<std::string>& lines)
{
    FILE* f = nullptr;
    if (!cmd.empty()) {
        f = popen(cmd.c_str(), "r");
        if (!f) { fprintf(stderr, "cannot run %s\n", cmd.c_str()); exit(2); }
    } else {
        f = fopen(path.c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(2); }
    }
    char buf[8192];
    while (fgets(buf, sizeof buf, f)) lines.push_back(buf);
    if (!cmd.empty()) pclose(f); else fclose(f);
}

// S from the engine log: the S the report decomposes, measured in the same execution as C.
// Only lines after the last filmbench-mark, skipping the harness's own SET and EXPLAIN;
// match identifies the statement under test (%night% or movieid = 97).
// Even counts take the mean of the middle two, as summarise() does.
static double logMedian(const std::string& path, const std::string& cmd,
                        const std::string& match)
{
    std::vector<std::string> lines;
    logLines(path, cmd, lines);

    size_t start = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find("filmbench-mark") != std::string::npos) start = i + 1;

    std::vector<double> samples;
    for (size_t i = start; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.find("duration:") == std::string::npos) continue;
        if (l.find("EXPLAIN") != std::string::npos) continue;
        // Drop only the harness's own SET lines: UPDATE ... SET x has those characters too.
        size_t st = l.find("statement: ");
        if (st != std::string::npos && l.compare(st + 11, 4, "SET ") == 0) continue;
        if (!match.empty() && l.find(match) == std::string::npos) continue;

        size_t at = l.find("duration:");
        samples.push_back(atof(l.c_str() + at + 9));
    }
    if (samples.empty()) return -1.0;
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    return (n % 2) ? samples[n / 2] : (samples[n / 2 - 1] + samples[n / 2]) / 2.0;
}

// Check C = T + S + eps. The input needs client_ms, transfer_ms and server_ms.
// Reads must fall inside the tolerance; update is skipped: its residual is the commit.
static int verifyRows(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    char buf[4096];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 2; }

    std::vector<std::string> cols;
    {
        std::string line(buf), cur;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ',' || line[i] == '\n') {
                cur.erase(std::remove(cur.begin(), cur.end(), '\r'), cur.end());
                if (!cur.empty() || i < line.size()) cols.push_back(cur);
                cur.clear();
            } else cur += line[i];
        }
    }
    int ci = -1, ti = -1, si = -1, oi = -1;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == "client_ms")   ci = (int)i;
        if (cols[i] == "transfer_ms") ti = (int)i;
        if (cols[i] == "server_ms")   si = (int)i;
        if (cols[i] == "op")          oi = (int)i;
    }
    if (ci < 0 || ti < 0 || si < 0) {
        fprintf(stderr, "%s needs client_ms, transfer_ms and server_ms\n", path.c_str());
        fclose(f);
        return 2;
    }

    int rows = 0, bad = 0;
    while (fgets(buf, sizeof buf, f)) {
        std::string line(buf), cur;
        std::vector<std::string> v;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ',' || line[i] == '\n') {
                cur.erase(std::remove(cur.begin(), cur.end(), '\r'), cur.end());
                v.push_back(cur);
                cur.clear();
            } else cur += line[i];
        }
        int need = ci; if (ti > need) need = ti; if (si > need) need = si;
        if ((int)v.size() <= need) continue;
        if (oi >= 0 && (int)v.size() > oi && v[oi] == "update") continue;
        double c = atof(v[ci].c_str()), t = atof(v[ti].c_str()), sv = atof(v[si].c_str());
        if (sv <= 0) continue;
        double e = c - t - sv;
        // The tolerance cannot be a constant: everything outside S -- the round trip, the
        // statement log write -- grows with the query, and a tighter bound than this one
        // flagged honest rows on a loaded machine. 0.25 ms or 5% of the row, whichever is
        // larger, still catches a row whose S came from another execution: that error is the
        // size of an execution, not a few percent.
        double tol = 0.05 * c;
        if (tol < 0.25) tol = 0.25;
        ++rows;
        if (e > tol || e < -tol) {
            ++bad;
            printf("  residual %+.4f ms: %s %s\n", e,
                   v.size() > 0 ? v[0].c_str() : "?",
                   v.size() > 1 ? v[1].c_str() : "?");
        }
    }
    fclose(f);
    printf("%d rows checked, %d outside the tolerance (0.25 ms or 5%%, whichever is larger)\n",
           rows, bad);
    return bad ? 1 : 0;
}

// Entry point: ping / export / file / db / verify.
int main(int argc, char** argv)
{
    if (argc < 2) { printUsage(); return 1; }
    if (std::string(argv[1]) == "verify")
        return verifyRows(argc > 2 ? argv[2] : "rows.csv");

    std::string command = argv[1];

    Options opt;
    // Most options are --name value pairs; a few switches carry no value.
    for (int i = 2; i < argc; ) {
        std::string key = argv[i];
        while (!key.empty() && key[0] == '-') key.erase(key.begin());
        // These are switches. Miss one and it swallows the next option as its value.
        if (key == "log-server-timing" || key == "no-index") {
            opt.values[key] = "1"; i += 1; continue;
        }
        if (i + 1 < argc) opt.values[key] = argv[i + 1];
        i += 2;
    }

    const std::string op    = opt.get("op", "scan");
    const std::string term  = opt.get("term");
    const int         runs  = opt.getInt("runs", 20);
    const int         warmup= opt.getInt("warmup", 3);
    const int         key   = opt.getInt("key", 1);
    const std::string label = opt.get("label", command);
    const std::string outCsv= opt.get("out");
    if (opt.has("table")) g_moviesTable = opt.get("table");

    // ------ ping ---------------------------------------------------
    // The empty round trip: T in C = T + S + eps.
    if (command == "ping") {
        Database db;
        if (!db.connect(opt.get("dsn"))) return 2;
        db.alignSettings();
        if (opt.has("log-server-timing")) db.enableStatementLogging();
        if (opt.has("no-index")) db.forbidIndexScans();

        for (int i = 0; i < warmup; ++i) db.run("SELECT 1");

        std::vector<double> samples;
        for (int i = 0; i < runs; ++i) {
            Timer t;
            db.run("SELECT 1");
            samples.push_back(t.ms());
        }
        Result r = summarise(samples);
        printf("%-12s ping      min %7.3f ms   median %7.3f ms   max %7.3f ms  (%d runs)\n",
               label.c_str(), r.minMs, r.medMs, r.maxMs, r.runs);
        return 0;
    }

    // ------ export --------------------------------------
    // export: the CSVs the file side reads.
    // --table exports the scaling table, still into movies.csv: it overwrites the baseline.
    if (command == "export") {
        Database db;
        if (!db.connect(opt.get("dsn"))) return 2;
        db.exec("SET client_encoding = 'UTF8'");
        db.exec("BEGIN");

        std::string dir = opt.get("data", "data");

        if (opt.has("table")) {
            std::string name = opt.get("table");
            bool ok = exportTable(db, name,
                                  "SELECT movieid, title, country, year_released, runtime FROM " +
                                  name + " ORDER BY movieid",
                                  dir + "/movies.csv");
            db.exec("COMMIT");
            return ok ? 0 : 2;
        }

        bool ok = exportTable(db, "movies",
                              "SELECT movieid, title, country, year_released, runtime "
                              "FROM movies ORDER BY movieid",
                              dir + "/movies.csv");
        ok = exportTable(db, "people",
                         "SELECT peopleid, first_name, surname, born, died, gender "
                         "FROM people ORDER BY peopleid", dir + "/people.csv") && ok;
        ok = exportTable(db, "credits",
                         "SELECT movieid, peopleid, credited_as FROM credits "
                         "ORDER BY movieid, peopleid, credited_as", dir + "/credits.csv") && ok;

        db.exec("COMMIT");
        return ok ? 0 : 2;
    }

    // ------- file --------------------------------------------
    // file: one file layout. Timing covers the query only;
    // start-up loading and index building are not part of it.
    if (command == "file") {
        const std::string layout = opt.get("layout", "csv");
        if (layout != "csv" && layout != "indexed") {
            fprintf(stderr, "unknown layout '%s': --layout accepts csv or indexed\n",
                    layout.c_str());
            return 2;
        }

        FileSide file(opt.get("data", "data"), layout);
        if (!file.load()) { fprintf(stderr, "cannot load the data files\n"); return 2; }

        long long answer = 0;
        Result r = measure(&file, nullptr, op, term, key, answer, warmup, runs);

        printf("%-12s %-9s %-8s answer=%-8lld min %9.6f ms   median %9.6f ms   max %9.6f ms  (%d runs)\n",
               label.c_str(), op.c_str(), layout.c_str(),
               answer, r.minMs, r.medMs, r.maxMs, r.runs);

        char row[512];
        snprintf(row, sizeof row, "file,%s,%s,%s,%s,%lld,%d,%.7f,%.7f,%.7f,-1\n",
                 op.c_str(), layout.c_str(), term.c_str(),
                 opt.get("tag").c_str(), answer, r.runs, r.minMs, r.medMs, r.maxMs);
        appendCsv(outCsv, row);
        return 0;
    }

    // -------- db -------------------------------------------------
    // db: one database engine. median_ms is the caller's clock C.
    // With --log-cmd/--log-file, S comes from the log; otherwise explain_ms comes from EXPLAIN.
    if (command == "db") {
        Database db;
        if (!db.connect(opt.get("dsn"))) return 2;
        db.alignSettings();
        if (opt.has("log-server-timing")) db.enableStatementLogging();
        if (opt.has("no-index")) db.forbidIndexScans();

        long long answer = 0;
        Result r = measure(nullptr, &db, op, term, key, answer, warmup, runs);

        // Two sources for S: the log (same execution, write cost included) or EXPLAIN (scaling only).
        if (opt.has("log-file") || opt.has("log-cmd")) {
            std::string blob = opt.get("log-file");
            std::string cmd  = opt.get("log-cmd");
            double logS = logMedian(blob, cmd, opt.get("match", term));
            if (logS < 0) {
                fprintf(stderr, "no duration for %s after the last filmbench-mark\n",
                        opt.get("match", term).c_str());
                return 4;
            }
            printf("%-12s %-9s sql      answer=%-8lld min %8.3f ms   median %8.3f ms"
                   "   max %8.3f ms   server %8.4f ms  (%d runs)\n",
                   label.c_str(), op.c_str(), answer, r.minMs, r.medMs, r.maxMs,
                   logS, r.runs);

            char tidy[512];
            snprintf(tidy, sizeof tidy, "%s,%s,%s,sql,%.7f,%.7f,%.7f,%lld\n",
                     label.c_str(), op.c_str(), term.c_str(), r.medMs, logS,
                     atof(opt.get("transfer", "0").c_str()), answer);
            appendCsv(outCsv, tidy,
                      "system,op,term,layout,client_ms,server_ms,transfer_ms,answer");
            return 0;
        }

        std::string sql = sqlFor(op, term, key);
        double explainMedian = -1.0;
        {
            std::vector<double> explainSamples;
            for (int i = 0; i < 10; ++i) {
                double ms = -1;
                if (op == "update") {
                    db.exec("BEGIN");
                    if (db.serverTime(sql, ms)) explainSamples.push_back(ms);
                    db.exec("ROLLBACK");
                } else if (db.serverTime(sql, ms)) {
                    explainSamples.push_back(ms);
                }
            }
            if (!explainSamples.empty()) explainMedian = summarise(explainSamples).medMs;
        }

        printf("%-12s %-9s sql      answer=%-8lld min %8.3f ms   median %8.3f ms   max %8.3f ms"
               "   explain %8.4f ms  (%d runs)\n",
               label.c_str(), op.c_str(), answer, r.minMs, r.medMs, r.maxMs,
               explainMedian, r.runs);

        char row[512];
        snprintf(row, sizeof row, "%s,%s,sql,%s,%s,%lld,%d,%.7f,%.7f,%.7f,%.7f\n",
                 label.c_str(), op.c_str(), term.c_str(), opt.get("tag").c_str(),
                 answer, r.runs, r.minMs, r.medMs, r.maxMs, explainMedian);
        appendCsv(outCsv, row);
        return 0;
    }

    printUsage();
    return 1;
}