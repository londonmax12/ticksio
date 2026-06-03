/* ticksio side of the format benchmark. Generates a synthetic trade stream,
 * times write / insert / read for COMPRESSION_NONE and COMPRESSION_ZSTD, dumps
 * the identical rows to CSV (for the Parquet/Feather side), and writes the
 * ticks rows of bench/results.csv. Not part of the CMake build.
 *
 *   write    = bulk: one ticks_add_data(all N) then close.
 *   insert   = streaming: many ticks_add_data(batch) then close. Swept across
 *              batch sizes (1k / 10k / 100k) because insert throughput is a
 *              function of batch granularity, not a single point.
 *   read     = decode the whole file back, in two modes:
 *                scan        — iterate every record into one reused struct and
 *                              discard (bounded memory, no N-sized output).
 *                materialize — decode into three N-sized int64 output arrays
 *                              (head-to-head with the columnar formats' decode
 *                              into numpy arrays).
 *
 * Every metric is reported best-of-REPS (min wall-clock): cold-run jitter
 * dominates a single shot, and the steady-state number is the honest one. The
 * Python side uses the same best-of-REPS rule on the identical rows.
 *
 * Caveat best-of-REPS does NOT fix: the machine's baseline write speed drifts
 * BETWEEN process invocations (thermal + OS disk-writeback contention), and
 * min-over-reps inside one process cannot see that. Measured empirically at
 * ~20% run-to-run CV on zstd write, unchanged by REPS=3 vs REPS=10. So:
 *   --no-csv          skip the ~120 MB out.csv write (its writeback bleeds into
 *                     the NEXT invocation's flush). Only the Parquet/Feather
 *                     side needs out.csv; standalone/A-B timing does not.
 *   BENCH_SETTLE_MS=K Sleep(K) between reps (outside the timed region) so the
 *                     prior rep's writeback drains before the next is timed.
 * To compare an optimization against a baseline, do NOT diff a stored number
 * from another session — run both interleaved in one session via bench/ab.py,
 * which cancels the drift in the paired delta.
 *
 * Output is the tidy long form `format,n,metric,variant,value`:
 *   size       variant=""                       value=on-disk bytes
 *   write      variant=""                       value=best-of seconds (to disk, incl close)
 *   encode     variant=""                       value=best-of seconds (add_data only, no close)
 *   insert     variant=<batch size>             value=best-of seconds
 *   read       variant=scan|materialize         value=best-of seconds
 * `encode` is ticks-only — the low-noise CPU/RAM number to optimize against;
 * `write` is the to-disk number kept for the head-to-head vs the other formats.
 * The Python side appends rows in the same shape (size/write/insert/read only).
 *
 * Usage:  bench.exe [N] [REPS] [--no-csv]   (defaults: N=5,000,000  REPS=3)
 *
 * Build:
 *   gcc -O2 -Isrc/c/include bench/bench.c build/c/libticksio.a \
 *       build/c/_deps/zstd-build/lib/libzstd.a -o bench/bench.exe
 */
#include "ticksio/ticksio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <windows.h>

static double now_s(void) {
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* Inter-rep settle (outside any timed region): let the previous rep's OS disk
 * writeback drain before the next rep is timed, so min-over-reps isn't taken
 * over reps that contend with each other's flush. 0 = off (default). */
static unsigned g_settle_ms = 0;
static void settle(void){ if(g_settle_ms) Sleep(g_settle_ms); }

/* Pin to a single performance (P) core and raise priority. This machine is a
 * hybrid Lunar Lake part (P-cores + LP E-cores, Thread Director migration); an
 * unpinned single-threaded bench that lands on a P-core one run and an E-core
 * the next swings throughput ~1.5-2x, a large bursty source of run-to-run
 * variance that best-of-N cannot remove. Pin to one core (the lowest-numbered
 * of the highest EfficiencyClass) so every rep runs on the same fast core, no
 * migration. Set BENCH_NO_PIN=1 to disable. Best-effort: failures are silent. */
static void pin_perf_core(void){
    if(getenv("BENCH_NO_PIN")) return;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    DWORD len=0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if(!len) return;
    BYTE* buf=malloc(len);
    if(!buf) return;
    DWORD_PTR perf_mask=0; BYTE best=0; int found=0;
    if(GetLogicalProcessorInformationEx(RelationProcessorCore,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)){
        for(BYTE* p=buf; p<buf+len; ){
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* e=(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)p;
            if(e->Relationship==RelationProcessorCore){
                BYTE eff=e->Processor.EfficiencyClass;
                DWORD_PTR m=(DWORD_PTR)e->Processor.GroupMask[0].Mask;
                if(!found || eff>best){ best=eff; perf_mask=m; found=1; }   /* new top class */
                else if(eff==best){ perf_mask|=m; }                          /* same class */
            }
            p+=e->Size;
        }
    }
    free(buf);
    if(found && perf_mask){
        DWORD_PTR one = perf_mask & (~perf_mask + 1);  /* lowest set bit = one P-core */
        if(SetThreadAffinityMask(GetCurrentThread(), one))
            fprintf(stderr,"[bench] pinned to P-core mask 0x%llx (priority HIGH)\n",
                    (unsigned long long)one);
    }
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t xrng(void){ uint64_t x=rng_state; x^=x<<13; x^=x>>7; x^=x<<17; rng_state=x; return x; }
static uint32_t urange(uint32_t n){ return (uint32_t)(xrng()%n); }

/* Insert (streaming) batch sizes swept to draw the granularity curve. Must match
 * BATCHES in bench_py.py so the two sides report the same points. */
static const uint64_t INSERT_BATCHES[] = { 1000, 10000, 100000 };
#define N_INSERT_BATCHES (sizeof(INSERT_BATCHES)/sizeof(INSERT_BATCHES[0]))

/* One write of the whole stream. batch==0 -> bulk (single ticks_add_data);
 * batch>0 -> streaming in fixed-size chunks. Returns 0 on success and reports
 * two split times:
 *   *out_total  = whole call incl ticks_close (the to-disk number — but its
 *                 fclose carries the Defender on-access scan + physical
 *                 writeback, the dominant run-to-run noise on this machine).
 *   *out_encode = open + add_data loop only, EXCLUDING ticks_close. All chunk
 *                 bytes are produced and fwritten (to the stdio buffer / OS page
 *                 cache, RAM-speed) inside add_data; the disk flush and the
 *                 close-time index/header rewrite are not. So this isolates the
 *                 encode/compress CPU work that optimizations actually target,
 *                 with the bursty disk/Defender variance kept out of the timer. */
static int build_once(const char* path, compression_type_e c, trade_data_t* t,
                      uint64_t n, uint64_t batch, double* out_total, double* out_encode) {
    ticks_header_t h; memset(&h,0,sizeof(h));
    strcpy(h.ticker,"AAPL"); strcpy(h.currency,"USD"); strcpy(h.country,"US");
    h.asset_class=ASSET_CLASS_STOCK; h.schema_id=SCHEMA_TRADE;
    h.price_scale=-2; h.volume_scale=0; h.compression_type=c;

    double t0=now_s();
    ticks_file_t* w=NULL;
    ticks_status_e s=ticks_new_file(path,&h,&w);
    if(s!=TICKS_OK||!w){ fprintf(stderr,"new_file: %s\n",ticks_status_to_string(s)); return 1; }
    if(batch){
        for(uint64_t i=0;i<n;i+=batch){
            uint64_t b=(n-i<batch)?(n-i):batch;
            s=ticks_add_data(w,t+i,b);
            if(s!=TICKS_OK){ fprintf(stderr,"add: %s\n",ticks_status_to_string(s)); return 1; }
        }
    } else {
        s=ticks_add_data(w,t,n);
        if(s!=TICKS_OK){ fprintf(stderr,"add: %s\n",ticks_status_to_string(s)); return 1; }
    }
    double t_enc=now_s()-t0;                 /* encode = everything before close */
    s=ticks_close(w);
    if(s!=TICKS_OK){ fprintf(stderr,"close: %s\n",ticks_status_to_string(s)); return 1; }
    *out_total=now_s()-t0;
    if(out_encode) *out_encode=t_enc;
    return 0;
}

/* Best-of-reps build time (min), for both the to-disk total and the encode-only
 * split, each minimized independently across reps. Each rep overwrites the same
 * path with the identical, deterministic bytes; the first rep warms the OS file
 * cache so the min is a steady-state number, not a cold one. */
static int build_best(const char* path, compression_type_e c, trade_data_t* t,
                      uint64_t n, uint64_t batch, int reps, double* out_total, double* out_encode) {
    double best_t=1e300, best_e=1e300;
    for(int r=0;r<reps;r++){
        double tot,enc;
        if(build_once(path,c,t,n,batch,&tot,&enc)) return 1;
        if(tot<best_t) best_t=tot;
        if(enc<best_e) best_e=enc;
        settle();
    }
    *out_total=best_t;
    if(out_encode) *out_encode=best_e;
    return 0;
}

/* read mode "scan": iterate every record into one reused struct and discard. */
static int scan_once(const char* path, double* out_s, uint64_t* out_count){
    double t0=now_s();
    ticks_file_t* r=NULL;
    if(ticks_open_read(path,&r)!=TICKS_OK){ fprintf(stderr,"open %s\n",path); return 1; }
    ticks_header_t h; ticks_get_header(r,&h);
    ticks_iterator_t* it=NULL;
    ticks_iterator_create(r,(int64_t)h.min_timestamp,(int64_t)h.max_timestamp+1,&it);
    uint64_t c=0; trade_data_t rec;
    while(ticks_iterator_next(it,&rec)==TICKS_OK) c++;
    ticks_iterator_destroy(it);
    ticks_close(r);
    *out_s=now_s()-t0; *out_count=c;
    return 0;
}

/* read mode "materialize": decode into three N-sized int64 output arrays, the
 * head-to-head analog of the columnar formats decoding into numpy arrays. The
 * allocation of the output buffers is part of the cost, as it is for numpy.
 * Uses ticks_read_columns, which decodes each column straight into its array
 * (no per-record struct, no row->column transpose) — the apples-to-apples match
 * for combine_chunks + to_numpy on the columnar formats. */
static int materialize_once(const char* path, uint64_t n, double* out_s, uint64_t* out_count){
    double t0=now_s();
    int64_t* ts=malloc(n*sizeof(int64_t));
    int64_t* px=malloc(n*sizeof(int64_t));
    int64_t* vol=malloc(n*sizeof(int64_t));
    if(!ts||!px||!vol){ fprintf(stderr,"oom materialize\n"); free(ts);free(px);free(vol); return 1; }
    ticks_file_t* r=NULL;
    if(ticks_open_read(path,&r)!=TICKS_OK){ fprintf(stderr,"open %s\n",path); free(ts);free(px);free(vol); return 1; }
    ticks_header_t h; ticks_get_header(r,&h);
    int64_t* cols[3]={ts,px,vol};
    uint64_t c=0;
    ticks_status_e st=ticks_read_columns(r,(int64_t)h.min_timestamp,(int64_t)h.max_timestamp+1,
                                         cols,3,n,&c);
    ticks_close(r);
    free(ts); free(px); free(vol);
    if(st!=TICKS_OK){ fprintf(stderr,"read_columns: %s\n",ticks_status_to_string(st)); return 1; }
    *out_s=now_s()-t0; *out_count=c;
    return 0;
}

static int scan_best(const char* path, int reps, double* out_s, uint64_t* out_count){
    double best=1e300; uint64_t cnt=0;
    for(int r=0;r<reps;r++){ double s; if(scan_once(path,&s,&cnt)) return 1; if(s<best) best=s; settle(); }
    *out_s=best; *out_count=cnt; return 0;
}
static int materialize_best(const char* path, uint64_t n, int reps, double* out_s, uint64_t* out_count){
    double best=1e300; uint64_t cnt=0;
    for(int r=0;r<reps;r++){ double s; if(materialize_once(path,n,&s,&cnt)) return 1; if(s<best) best=s; settle(); }
    *out_s=best; *out_count=cnt; return 0;
}

static long fsize(const char* p){ FILE* f=fopen(p,"rb"); if(!f)return -1; fseek(f,0,SEEK_END); long s=ftell(f); fclose(f); return s; }

int main(int argc, char** argv){
    /* Positional N and REPS, plus an order-independent --no-csv flag. */
    uint64_t n=5000000ULL; int reps=3; int no_csv=0; int pos=0;
    for(int a=1;a<argc;a++){
        if(strcmp(argv[a],"--no-csv")==0){ no_csv=1; continue; }
        if(pos==0){ n=strtoull(argv[a],NULL,10); pos=1; }
        else if(pos==1){ reps=atoi(argv[a]); pos=2; }
    }
    if(reps<1) reps=1;
    const char* sm=getenv("BENCH_SETTLE_MS"); if(sm){ int v=atoi(sm); if(v>0) g_settle_ms=(unsigned)v; }
    pin_perf_core();
    trade_data_t* ticks=malloc(n*sizeof(trade_data_t));
    if(!ticks){ fprintf(stderr,"oom\n"); return 1; }

    uint64_t ts=1700000000000ULL; int64_t px=18950;
    for(uint64_t i=0;i<n;i++){
        uint32_t r=urange(100);
        uint64_t gap=(r<30)?0:(r<90)?(1+urange(8)):(10+urange(200));
        ts+=gap;
        int32_t step; uint32_t pr=urange(100);
        if(pr<55)step=0; else if(pr<90)step=(int32_t)urange(3)-1;
        else if(pr<99)step=(int32_t)urange(7)-3; else step=(int32_t)urange(41)-20;
        px+=step; if(px<100)px=100;
        uint64_t vol; uint32_t vr=urange(100);
        if(vr<70)vol=100; else if(vr<90)vol=(uint64_t)(1+urange(9))*100;
        else if(vr<99)vol=(uint64_t)(1+urange(50))*100; else vol=(uint64_t)(1+urange(5000));
        ticks[i].ms_since_epoch=ts; ticks[i].price=(uint64_t)px; ticks[i].volume=vol;
    }

    struct { const char* name; const char* path; compression_type_e c; } cfg[]={
        { "ticks none", "bench/out_none.ticks", COMPRESSION_NONE },
        { "ticks zstd", "bench/out_zstd.ticks", COMPRESSION_ZSTD },
    };

    FILE* res=fopen("bench/results.csv","wb");
    fputs("format,n,metric,variant,value\n",res);

    for(int k=0;k<2;k++){
        const char* name=cfg[k].name; const char* path=cfg[k].path;

        double w_s,enc_s;                                            /* bulk write + encode split */
        if(build_best(path,cfg[k].c,ticks,n,0,reps,&w_s,&enc_s)) return 1;
        long sz=fsize(path);

        double ins_s[N_INSERT_BATCHES];                              /* insert sweep */
        for(size_t bi=0;bi<N_INSERT_BATCHES;bi++)
            if(build_best(path,cfg[k].c,ticks,n,INSERT_BATCHES[bi],reps,&ins_s[bi],NULL)) return 1;

        double scan_s,mat_s; uint64_t c1,c2;                         /* two read modes */
        if(scan_best(path,reps,&scan_s,&c1)) return 1;
        if(materialize_best(path,n,reps,&mat_s,&c2)) return 1;
        if(c1!=n||c2!=n){
            fprintf(stderr,"FAIL %s read %llu/%llu != %llu\n",name,
                    (unsigned long long)c1,(unsigned long long)c2,(unsigned long long)n);
            return 1;
        }

        fprintf(res,"%s,%" PRIu64 ",size,,%ld\n",name,n,sz);
        fprintf(res,"%s,%" PRIu64 ",write,,%.6f\n",name,n,w_s);
        fprintf(res,"%s,%" PRIu64 ",encode,,%.6f\n",name,n,enc_s);
        for(size_t bi=0;bi<N_INSERT_BATCHES;bi++)
            fprintf(res,"%s,%" PRIu64 ",insert,%" PRIu64 ",%.6f\n",name,n,INSERT_BATCHES[bi],ins_s[bi]);
        fprintf(res,"%s,%" PRIu64 ",read,scan,%.6f\n",name,n,scan_s);
        fprintf(res,"%s,%" PRIu64 ",read,materialize,%.6f\n",name,n,mat_s);

        printf("%-12s size=%9ld  write=%.3fs (encode=%.3fs)  insert[1k/10k/100k]=%.3f/%.3f/%.3f  "
               "read[scan/mat]=%.3f/%.3f  (%llu ok)\n",
               name,sz,w_s,enc_s,ins_s[0],ins_s[1],ins_s[2],scan_s,mat_s,(unsigned long long)n);
    }
    fclose(res);

    /* identical rows for the Parquet/Feather side. Skipped under --no-csv: only
     * bench_py.py consumes it, and its ~120 MB writeback contends with the next
     * invocation's flush, so standalone/A-B timing leaves it off. */
    if(!no_csv){
        FILE* csv=fopen("bench/out.csv","wb");
        fputs("timestamp,price,volume\n",csv);
        for(uint64_t i=0;i<n;i++)
            fprintf(csv,"%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",ticks[i].ms_since_epoch,ticks[i].price,ticks[i].volume);
        fclose(csv);
    }

    free(ticks);
    return 0;
}
