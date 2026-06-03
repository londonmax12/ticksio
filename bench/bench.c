/* ticksio side of the format benchmark. Generates a synthetic trade stream,
 * times write / insert / read for COMPRESSION_NONE and COMPRESSION_ZSTD, dumps
 * the identical rows to CSV (for the Parquet/Feather side), and writes the
 * ticks rows of bench/results.csv. Not part of the CMake build.
 *
 *   write  = bulk: one ticks_add_data(all N) then close
 *   insert = streaming: many small ticks_add_data(batch) then close
 *   read   = full scan back through the range iterator
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

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t xrng(void){ uint64_t x=rng_state; x^=x<<13; x^=x>>7; x^=x<<17; rng_state=x; return x; }
static uint32_t urange(uint32_t n){ return (uint32_t)(xrng()%n); }

#define INSERT_BATCH 1000

static int build(const char* path, compression_type_e c, trade_data_t* t, uint64_t n,
                 int streaming, double* out_s) {
    ticks_header_t h; memset(&h,0,sizeof(h));
    strcpy(h.ticker,"AAPL"); strcpy(h.currency,"USD"); strcpy(h.country,"US");
    h.asset_class=ASSET_CLASS_STOCK; h.schema_id=SCHEMA_TRADE;
    h.price_scale=-2; h.volume_scale=0; h.compression_type=c;

    double t0=now_s();
    ticks_file_t* w=NULL;
    ticks_status_e s=ticks_new_file(path,&h,&w);
    if(s!=TICKS_OK||!w){ fprintf(stderr,"new_file: %s\n",ticks_status_to_string(s)); return 1; }
    if(streaming){
        for(uint64_t i=0;i<n;i+=INSERT_BATCH){
            uint64_t b=(n-i<INSERT_BATCH)?(n-i):INSERT_BATCH;
            s=ticks_add_data(w,t+i,b);
            if(s!=TICKS_OK){ fprintf(stderr,"add: %s\n",ticks_status_to_string(s)); return 1; }
        }
    } else {
        s=ticks_add_data(w,t,n);
        if(s!=TICKS_OK){ fprintf(stderr,"add: %s\n",ticks_status_to_string(s)); return 1; }
    }
    s=ticks_close(w);
    if(s!=TICKS_OK){ fprintf(stderr,"close: %s\n",ticks_status_to_string(s)); return 1; }
    *out_s=now_s()-t0;
    return 0;
}

static int scan(const char* path, double* out_s, uint64_t* out_count){
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

static long fsize(const char* p){ FILE* f=fopen(p,"rb"); if(!f)return -1; fseek(f,0,SEEK_END); long s=ftell(f); fclose(f); return s; }

int main(int argc, char** argv){
    uint64_t n=(argc>1)?strtoull(argv[1],NULL,10):5000000ULL;
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
    fputs("format,n,size_bytes,write_s,insert_s,read_s\n",res);

    for(int k=0;k<2;k++){
        double w_s,i_s,r_s; uint64_t cnt;
        if(build(cfg[k].path,cfg[k].c,ticks,n,0,&w_s)) return 1;          /* bulk write */
        long sz=fsize(cfg[k].path);
        if(build(cfg[k].path,cfg[k].c,ticks,n,1,&i_s)) return 1;          /* streaming insert */
        if(scan(cfg[k].path,&r_s,&cnt)) return 1;                          /* full read */
        if(cnt!=n){ fprintf(stderr,"FAIL %s read %llu != %llu\n",cfg[k].name,(unsigned long long)cnt,(unsigned long long)n); return 1; }
        fprintf(res,"%s,%" PRIu64 ",%ld,%.6f,%.6f,%.6f\n",cfg[k].name,n,sz,w_s,i_s,r_s);
        printf("%-12s size=%9ld  write=%.3fs insert=%.3fs read=%.3fs  (%llu ok)\n",
               cfg[k].name,sz,w_s,i_s,r_s,(unsigned long long)cnt);
    }
    fclose(res);

    /* identical rows for the Parquet/Feather side */
    FILE* csv=fopen("bench/out.csv","wb");
    fputs("timestamp,price,volume\n",csv);
    for(uint64_t i=0;i<n;i++)
        fprintf(csv,"%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",ticks[i].ms_since_epoch,ticks[i].price,ticks[i].volume);
    fclose(csv);

    free(ticks);
    return 0;
}
