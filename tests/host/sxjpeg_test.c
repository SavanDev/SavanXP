/* Stage-level checks use the actual private implementation, as the MIDI
 * operator tests do; no testing-only functions enter the public SDK. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
static unsigned allocations, releases, fail_allocation;
static size_t last_allocation_size;
static void *test_alloc(size_t size) { ++allocations; last_allocation_size=size; return fail_allocation ? NULL : malloc(size); }
static void test_free(void *ptr) { if(ptr) ++releases; free(ptr); }
#define malloc test_alloc
#define free test_free
#include "../../subsystems/posix/sdk/v1/runtime/sximg.c"
#undef malloc
#undef free
#include "sxjpeg_fixtures.h"
#include "sxjpeg_color_fixtures.h"
#include "sxjpeg_reduced_fixtures.h"
#include "sxjpeg_progressive_fixtures.h"

static int failures;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n",msg,__LINE__); ++failures; } } while (0)

static int metadata_equal(const struct sx_image_metadata *a, const struct sx_image_metadata *b)
{
    return a->format==b->format && a->source_width==b->source_width && a->source_height==b->source_height &&
           a->width==b->width && a->height==b->height && a->scale_denom==b->scale_denom;
}

static void stages(void)
{
    struct jpeg_huffman h;
    unsigned char counts[16]={1,1,1}, symbols[3]={0,1,2};
    CHECK(jpeg_huffman_build(&h,counts,symbols,3,0),"canonical Huffman construction");
    const unsigned char bytes[]={0x5b};
    struct jpeg_reader r={bytes,sizeof(bytes),0,0,0};
    for (unsigned i=0;i<3;++i) { unsigned s=99; CHECK(jpeg_symbol(&r,&h,&s) && s==i,"MSB-first canonical symbols 0/10/110"); }
    CHECK(jpeg_align(&r),"all-ones padding accepted");
    counts[0]=2; counts[1]=counts[2]=0;
    CHECK(!jpeg_huffman_build(&h,counts,symbols,2,0),"all-ones code is forbidden");
    counts[0]=3;
    CHECK(!jpeg_huffman_build(&h,counts,symbols,3,0),"oversubscribed table refused");
    memset(counts,0,sizeof(counts)); counts[15]=1;
    CHECK(jpeg_huffman_build(&h,counts,symbols,1,0),"16-bit Huffman code accepted");
    const unsigned char zeroes[]={0,0}; r=(struct jpeg_reader){zeroes,2,0,0,0};
    unsigned symbol=99;
    CHECK(jpeg_symbol(&r,&h,&symbol) && symbol==0,"16-bit Huffman code decoded");
    r=(struct jpeg_reader){zeroes,1,0,0,0};
    CHECK(!jpeg_symbol(&r,&h,&symbol),"truncated long code refused");
    memset(counts,0,sizeof(counts)); counts[1]=2;
    unsigned char duplicate[]={0,0};
    CHECK(!jpeg_huffman_build(&h,counts,duplicate,2,0),"duplicate values refused");
    unsigned char bad[]={12,0x10,0x0b};
    counts[1]=1;
    CHECK(!jpeg_huffman_build(&h,counts,bad,1,0),"8-bit DC category limit");
    // EOB1 (0x10) is a progressive AC symbol: the table is legal but marked,
    // and a sequential scan refuses it (see malformed()).
    CHECK(jpeg_huffman_build(&h,counts,bad+1,1,1) && h.eob_runs,"EOB run symbol accepted and marked");
    const unsigned char ordinary[]={0x01};
    CHECK(jpeg_huffman_build(&h,counts,ordinary,1,1) && !h.eob_runs,"ordinary AC table carries no EOB runs");
    CHECK(!jpeg_huffman_build(&h,counts,bad+2,1,1),"8-bit AC category limit");
    for (unsigned bits=0;bits<8;++bits) {
        unsigned char byte=(unsigned char)(bits<<5); int32_t value=99;
        r=(struct jpeg_reader){&byte,1,0,0,0};
        CHECK(jpeg_receive(&r,3,&value) && value==(bits<4 ? (int)bits-7 : (int)bits),"JPEG signed amplitude extension");
    }
    int32_t value=99; r=(struct jpeg_reader){NULL,0,0,0,0};
    CHECK(jpeg_receive(&r,0,&value) && value==0 && r.pos==0,"zero-bit DC difference");
    const unsigned char stuffed[]={0xff,0,0x80}; uint32_t read=0;
    r=(struct jpeg_reader){stuffed,3,0,0,0};
    CHECK(jpeg_bits(&r,8,&read) && read==255 && r.pos==2,"FF00 byte stuffing");
    CHECK(jpeg_bits(&r,1,&read) && read==1 && !jpeg_align(&r),"zero padding refused");
    const unsigned char marker[]={0xff,0xd9}; r=(struct jpeg_reader){marker,2,0,0,0};
    CHECK(!jpeg_bits(&r,1,&read),"marker cannot become entropy bits");
    struct jpeg_state state={0}; unsigned char quant[65]={0};
    for (unsigned i=1;i<65;++i) quant[i]=(unsigned char)i;
    CHECK(jpeg_quant_tables(&state,quant,sizeof(quant)) && state.quant[0][0]==1 && state.quant[0][1]==2 &&
          state.quant[0][8]==3 && state.quant[0][16]==4 && state.quant[0][63]==64,"DQT zig-zag to natural positions");
    quant[32]=0; CHECK(!jpeg_quant_tables(&state,quant,sizeof(quant)),"zero quantizer refused");
    unsigned char wide[129]={0x10};
    for (unsigned i=0;i<64;++i) { wide[1+2*i]=0x12; wide[2+2*i]=0x34; }
    CHECK(jpeg_quant_tables(&state,wide,sizeof(wide)) && state.quant[0][27]==0x1234 && state.quant_wide[0],"16-bit big-endian quantizers");
    CHECK(!jpeg_quant_tables(&state,wide,sizeof(wide)-1),"truncated wide quantizer refused");
    struct jpeg_huffman dc,ac; unsigned char dc_counts[16]={1}, dc_symbols[1]={0};
    unsigned char ac_counts[16]={0,2}, ac_symbols[2]={0x11,0};
    CHECK(jpeg_huffman_build(&dc,dc_counts,dc_symbols,1,0) && jpeg_huffman_build(&ac,ac_counts,ac_symbols,2,1),"coefficient test tables");
    const unsigned char coeff[]={0x17}; int32_t predictor=3, block[64];
    r=(struct jpeg_reader){coeff,1,0,0,0};
    CHECK(jpeg_block(&r,&dc,&ac,&predictor,block) && block[0]==3 && block[8]==1,"DC predictor, AC run, receive and EOB");
    unsigned nonzero=0; for(unsigned i=0;i<64;++i) nonzero+=block[i]!=0;
    CHECK(nonzero==2,"EOB initializes remaining coefficients");
    // Bound zero runs, nonzero runs and the accumulated DC before dequantization.
    unsigned char zrl_symbols[1]={0xf0}, one_count[16]={1};
    CHECK(jpeg_huffman_build(&ac,one_count,zrl_symbols,1,1),"ZRL test table");
    r=(struct jpeg_reader){zeroes,2,0,0,0}; predictor=0;
    CHECK(!jpeg_block(&r,&dc,&ac,&predictor,block),"four ZRLs overflow a block and are refused");
    unsigned char run_symbols[1]={0xf1}, run_data[1]={0x2a};
    CHECK(jpeg_huffman_build(&ac,one_count,run_symbols,1,1),"AC run test table");
    r=(struct jpeg_reader){run_data,1,0,0,0}; predictor=0;
    CHECK(!jpeg_block(&r,&dc,&ac,&predictor,block),"nonzero AC run beyond coefficient 63 is refused");
    unsigned char dc_large[1]={11}, dc_data[2]={0x7f,0xf0};
    CHECK(jpeg_huffman_build(&dc,one_count,dc_large,1,0),"large DC test table");
    r=(struct jpeg_reader){dc_data,2,0,0,0}; predictor=0;
    CHECK(!jpeg_block(&r,&dc,&ac,&predictor,block),"out-of-range accumulated DC is refused");
    memset(block,0,sizeof(block)); block[0]=3; block[8]=1;
    for(unsigned i=0;i<64;++i) state.quant[0][i]=3;
    jpeg_dequantize(block,state.quant[0]); CHECK(block[0]==9 && block[8]==3,"coefficient dequantization");
    memset(block,0,sizeof(block)); unsigned char pixels[64];
    for (int dc_value=-1024;dc_value<=1023;++dc_value) {
        block[0]=dc_value; jpeg_idct(block,pixels);
        int expected=(int)round((double)dc_value/8.0)+128;
        if(expected<0) expected=0;
        if(expected>255) expected=255;
        for(unsigned i=0;i<64;++i) CHECK(pixels[i]==expected,"DC-only IDCT, level shift and clamp");
    }
    // Compare a separate floating-point definition with the integer transform.
    const double pi=3.14159265358979323846;
    for (unsigned test=0;test<128;++test) {
        for(unsigned i=0;i<64;++i) block[i]=(int32_t)((test*31+i*17+i*i)%101)-50;
        jpeg_idct(block,pixels);
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
            double sum=0;
            for(unsigned v=0;v<8;++v) for(unsigned u=0;u<8;++u)
                sum+=(u?sqrt(1.0):sqrt(0.5))*(v?sqrt(1.0):sqrt(0.5))*block[v*8+u]*
                     cos((2*x+1)*u*pi/16)*cos((2*y+1)*v*pi/16)/4;
            int expected=(int)round(sum)+128; if(expected<0) expected=0;
        if(expected>255) expected=255;
            CHECK(abs((int)pixels[y*8+x]-expected)<=1,"integer IDCT within one level of independent definition");
        }
    }
    for(unsigned i=0;i<64;++i) block[i]=(i&1)?1023:-1024;
    for(unsigned i=0;i<64;++i) state.quant[0][i]=65535;
    jpeg_dequantize(block,state.quant[0]); jpeg_idct(block,pixels); // sanitizer overflow boundary
}

static unsigned char fixture[32768]; static size_t used; static unsigned pending, bit_count;
static void byte(unsigned v) { fixture[used++]=(unsigned char)v; }
static void mark(unsigned v) { byte(255); byte(v); }
static void segment(unsigned marker,const unsigned char *body,size_t length) {
    mark(marker); byte((unsigned)((length+2)>>8)); byte((unsigned)(length+2));
    memcpy(fixture+used,body,length); used+=length;
}
static void bits(unsigned value,unsigned count) {
    while(count--) { pending=(pending<<1)|((value>>count)&1); if(++bit_count==8) {
        byte(pending); if((pending&255)==255) byte(0); pending=bit_count=0;
    }}
}
static void pad(void) { if(bit_count) bits((1u<<(8-bit_count))-1u,8-bit_count); }
static size_t minimal(unsigned width,unsigned height,unsigned restart) {
    used=pending=bit_count=0; mark(0xd8);
    unsigned char q[65]={0}; memset(q+1,1,64); segment(0xdb,q,sizeof(q));
    unsigned char h[36]={0}; h[1]=1; h[18]=0x10; h[19]=1; segment(0xc4,h,sizeof(h));
    unsigned char f[9]={8,(unsigned char)(height>>8),(unsigned char)height,(unsigned char)(width>>8),(unsigned char)width,1,7,0x11,0};
    segment(0xc0,f,sizeof(f));
    if(restart) { unsigned char r[2]={(unsigned char)(restart>>8),(unsigned char)restart}; segment(0xdd,r,2); }
    const unsigned char scan[6]={1,7,0,0,63,0}; segment(0xda,scan,6);
    unsigned blocks=((width+7)/8)*((height+7)/8), rst=0;
    for(unsigned i=0;i<blocks;++i) { if(i && restart && i%restart==0) { pad(); mark(0xd0+(rst++&7)); } bits(0,2); }
    pad(); mark(0xd9); return used;
}
static size_t find_marker(unsigned marker) {
    for(size_t i=2;i+1<used;++i) if(fixture[i]==255 && fixture[i+1]==marker) return i;
    CHECK(0,"fixture marker exists"); return 0;
}
static void error_expected(int expected, int header_error) {
    const struct sx_image_options full={0,0,0,1}; // Keep these error assertions at full resolution.
    uint32_t pixels[258]; for(unsigned i=0;i<258;++i) pixels[i]=0xdeadbeef;
    struct sx_image_metadata info={9,9,9,9,9,9}, zero={0};
    int result=sx_jpeg_info(fixture,used,&full,&info);
    CHECK(result==(header_error?expected:SX_IMAGE_OK),"specific info result");
    if(header_error) CHECK(metadata_equal(&info,&zero),"info cleared on error");
    CHECK(sx_jpeg_decode_into(fixture,used,&full,pixels+1,256,&info)==expected && metadata_equal(&info,&zero),"specific failure and cleared decode metadata");
    CHECK(pixels[0]==0xdeadbeef && pixels[257]==0xdeadbeef,"error destination guards");
    CHECK(sx_image_info(fixture,used,&full,&info)==(header_error?expected:SX_IMAGE_OK),"common info forwards JPEG error");
    CHECK(sx_image_decode_into(fixture,used,&full,pixels+1,256,&info)==expected && metadata_equal(&info,&zero),"common decode forwards JPEG error");
    if(header_error) for(unsigned i=0;i<258;++i) CHECK(pixels[i]==0xdeadbeef,"header rejection precedes writes");
}

static void fixtures(void) {
    const struct { const unsigned char *jpeg,*pixels; size_t size; unsigned w,h; } refs[]={
#define REF(name,w,h) {jpeg_##name,expected_##name,sizeof(jpeg_##name),w,h}
        REF(black,8,8),REF(white,8,8),REF(tiny,3,5),REF(gradient,9,9),REF(grain,17,13),REF(restart,17,13)
#undef REF
    };
    for(unsigned n=0;n<sizeof(refs)/sizeof(refs[0]);++n) {
        const unsigned char *data=refs[n].jpeg; size_t size=refs[n].size;
        unsigned w=refs[n].w,h=refs[n].h;
        uint32_t pixels[258]; for(unsigned i=0;i<258;++i) pixels[i]=0xdeadbeef;
        struct sx_image_metadata info={0}, decoded={0};
        CHECK(sx_jpeg_info(data,size,NULL,&info)==0 && info.width==w && info.height==h && info.source_width==w &&
              info.source_height==h && info.scale_denom==1 && info.format==SX_IMAGE_FORMAT_JPEG,"reference metadata");
        CHECK(sx_jpeg_decode_into(data,size,NULL,pixels+1,w*h,&decoded)==0 && metadata_equal(&info,&decoded),"reference JPEG decoded");
        unsigned max_error=0;
        for(unsigned i=0;i<w*h;++i) {
            unsigned gray=pixels[i+1]&255; unsigned delta=(unsigned)abs((int)gray-refs[n].pixels[i]);
            if(delta>max_error) max_error=delta;
            CHECK((pixels[i+1]>>24)==0 && ((pixels[i+1]>>8)&255)==gray && ((pixels[i+1]>>16)&255)==gray,"opaque grayscale RGB packing");
        }
        printf("reference %u: maximum channel difference %u\n",n,max_error);
        CHECK(max_error<=1,"reference tolerance is one intensity level");
        CHECK(pixels[0]==0xdeadbeef && pixels[w*h+1]==0xdeadbeef,"reference exact buffer bounds");
        uint32_t common[256];
        CHECK(sx_image_info(data,size,NULL,&decoded)==0 && metadata_equal(&info,&decoded),"common JPEG metadata");
        CHECK(sx_image_decode_into(data,size,NULL,common,w*h,NULL)==0 && !memcmp(common,pixels+1,w*h*4),"common JPEG pixels identical");
        memcpy(fixture,data,size); used=size;
        for(size_t length=0;length<size;++length) {
            struct sx_image_metadata partial={9,9,9,9,9,9}, zero={0};
            CHECK(sx_jpeg_decode_into(data,length,NULL,pixels+1,w*h,&partial)<0 && metadata_equal(&partial,&zero),"every truncated prefix refused");
            CHECK(pixels[0]==0xdeadbeef && pixels[w*h+1]==0xdeadbeef,"truncation buffer bounds");
        }
        CHECK(sx_jpeg_decode_into(data,size,NULL,common,w*h-1,&decoded)==SX_IMAGE_ERROR_BUFFER,"short destination refused");
        struct sx_image_options exact={w,h,w*h,1};
        CHECK(sx_jpeg_decode_into(data,size,&exact,common,w*h,&decoded)==0,"exact limits accepted");
        for(unsigned denom=2;denom<=8;denom*=2) { exact.scale_denom=denom;
            CHECK(sx_jpeg_info(data,size,&exact,&decoded)==0 && decoded.width==(w+denom-1)/denom &&
                  decoded.height==(h+denom-1)/denom && decoded.scale_denom==denom &&
                  sx_image_info(data,size,&exact,&decoded)==0,"explicit reduction plan"); }
        exact=(struct sx_image_options){w-1,h,w*h,0};
        CHECK(sx_jpeg_decode_into(data,size,&exact,common,w*h,&decoded)==0 && decoded.scale_denom==2,"auto reduces oversized output");
    }
}

static void color_fixtures(void) {
    const struct { const unsigned char *jpeg,*pixels; size_t size; unsigned w,h; const char *name; } refs[]={
#define REF(name,w,h) {jpeg_color_##name,expected_color_##name,sizeof(jpeg_color_##name),w,h,#name}
        REF(y444,9,13),REF(y422,17,9),REF(y420,17,19),REF(y440,9,17),REF(separate,17,19),
        REF(mixed,17,19),REF(pair,17,9),REF(rgb,13,9),REF(rgb_separate,17,13),REF(rgb_ids,9,9),
        REF(no_marker,9,9),REF(adobe_ycc,9,13),REF(both,9,9),REF(rgb422,17,9)
#undef REF
    };
    for(unsigned n=0;n<sizeof(refs)/sizeof(refs[0]);++n) {
        const unsigned char *data=refs[n].jpeg; size_t size=refs[n].size;
        unsigned w=refs[n].w,h=refs[n].h, count=w*h; uint32_t pixels[325],common[323];
        for(unsigned i=0;i<325;++i) pixels[i]=0xdeadbeef;
        struct sx_image_metadata info={0},decoded={0},zero={0};
        unsigned before=allocations, freed=releases;
        CHECK(sx_jpeg_info(data,size,NULL,&info)==0 && info.width==w && info.height==h &&
              info.source_width==w && info.source_height==h && info.format==SX_IMAGE_FORMAT_JPEG &&
              info.scale_denom==1,"colour reference metadata");
        CHECK(allocations==before,"colour info allocates nothing");
        CHECK(sx_jpeg_decode_into(data,size,NULL,pixels+1,count,&decoded)==0 && metadata_equal(&info,&decoded),"colour reference decoded");
        CHECK(allocations==before+1 && releases==freed+1,"one bounded colour workspace released");
        unsigned max_error=0;
        for(unsigned i=0;i<count;++i) {
            CHECK((pixels[i+1]>>24)==0,"opaque RGB packing");
            for(unsigned c=0;c<3;++c) {
                unsigned value=(pixels[i+1]>>(16-8*c))&255;
                unsigned delta=(unsigned)abs((int)value-refs[n].pixels[3*i+c]);
                if(delta>max_error) max_error=delta;
            }
        }
        printf("colour %s: maximum channel difference %u\n",refs[n].name,max_error);
        // A one-level component IDCT difference plus the YCbCr matrix can change RGB by three levels.
        CHECK(max_error<=3,"colour reference tolerance is three RGB levels");
        CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef,"colour exact buffer guards");
        CHECK(sx_image_info(data,size,NULL,&decoded)==0 && metadata_equal(&info,&decoded),"common colour plan");
        CHECK(sx_image_decode_into(data,size,NULL,common,count,NULL)==0 && !memcmp(common,pixels+1,count*4),"common colour pixels");
        before=allocations;
        CHECK(sx_jpeg_decode_into(data,size,NULL,common,count-1,&decoded)==SX_IMAGE_ERROR_BUFFER &&
              metadata_equal(&decoded,&zero) && allocations==before,"colour capacity checked before allocation");
        struct sx_image_options options={w,h,count,1};
        CHECK(sx_jpeg_decode_into(data,size,&options,common,count,NULL)==0,"colour exact limits");
        options.max_pixels=count-1;
        CHECK(sx_jpeg_decode_into(data,size,&options,common,count,NULL)==SX_IMAGE_ERROR_LIMIT,"colour output budget");
        for(unsigned scale=2;scale<=8;scale*=2) {
            options=(struct sx_image_options){0,0,0,scale};
            CHECK(sx_jpeg_decode_into(data,size,&options,common,count,&decoded)==0 && decoded.width==(w+scale-1)/scale &&
                  decoded.height==(h+scale-1)/scale && decoded.scale_denom==scale,"colour reduction");
        }
        for(size_t length=0;length<size;++length) {
            pixels[0]=pixels[count+1]=0xdeadbeef; decoded=info; before=allocations; freed=releases;
            CHECK(sx_jpeg_decode_into(data,length,NULL,pixels+1,count,&decoded)<0 && metadata_equal(&decoded,&zero),"every colour truncated prefix rejected");
            CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef,"truncated colour guards");
            CHECK(allocations-before==releases-freed,"truncated colour workspace released");
        }
        fail_allocation=1; decoded=info;
        for(unsigned i=0;i<325;++i) pixels[i]=0xdeadbeef;
        CHECK(sx_jpeg_decode_into(data,size,NULL,pixels+1,count,&decoded)==SX_IMAGE_ERROR_DECODE &&
              metadata_equal(&decoded,&zero),"colour allocation failure contract");
        for(unsigned i=0;i<325;++i) CHECK(pixels[i]==0xdeadbeef,"allocation failure precedes writes");
        fail_allocation=0;
    }
}

static void dc_bits(int difference) {
    unsigned magnitude=(unsigned)abs(difference), category=0;
    while(magnitude) { ++category; magnitude>>=1; }
    bits(category,4);
    if(category) bits((unsigned)(difference<0 ? difference+(1<<category)-1 : difference),category);
    bits(0,1); // AC EOB.
}

/* Independent constant-block encoder exercises every combination of H/V 1/2,
 * including uncommon component ratios, partial MCUs and two-component scans.
 * Quantizers and restart intervals change at each scan boundary. */
static void color_synthetic(const unsigned factors[3],unsigned mode,unsigned app) {
    unsigned hmax=1,vmax=1;
    for(unsigned c=0;c<3;++c) { if((factors[c]>>4)>hmax) hmax=factors[c]>>4;
        if((factors[c]&15)>vmax) vmax=factors[c]&15; }
    used=pending=bit_count=0; mark(0xd8);
    unsigned char adobe[12]={'A','d','o','b','e',0,100,0,0,0,0,0};
    adobe[11]=(unsigned char)(app==1);
    if(app<3) segment(0xee,adobe,12);
    if(app==2) { const unsigned char jfif[14]={'J','F','I','F',0,1,2,0,0,1,0,1,0,0}; segment(0xe0,jfif,14); }
    unsigned char huff[47]={0}; huff[4]=12;
    for(unsigned i=0;i<12;++i) huff[17+i]=(unsigned char)i;
    huff[29]=0x10; huff[30]=1; segment(0xc4,huff,sizeof(huff));
    unsigned char frame[15]={8,0,19,0,17,3,1,0,0,2,0,0,3,0,0};
    const unsigned char rgb[3]={'R','G','B'};
    for(unsigned c=0;c<3;++c) {frame[6+3*c]=app==3?rgb[c]:(unsigned char)(c+1); frame[7+3*c]=(unsigned char)factors[c];}
    segment(0xc0,frame,sizeof(frame));
    unsigned scans=mode==0?1:mode==1?3:2;
    for(unsigned s=0;s<scans;++s) {
        unsigned q=1u<<s; unsigned char quant[65]={0}; memset(quant+1,(int)q,64); segment(0xdb,quant,65);
        segment(0xc4,huff,sizeof(huff)); // Table redefinition also works after entropy data.
        unsigned char dri[2]={0,(unsigned char)(s+1)}; segment(0xdd,dri,2);
        unsigned select[3]={0,1,2},count=3;
        if(mode==1) {count=1; select[0]=(s+2)%3;}
        if(mode==2) {count=s==0?2:1; select[0]=s==0?0:1; select[1]=2;}
        unsigned char scan[10]={0}; scan[0]=(unsigned char)count;
        for(unsigned i=0;i<count;++i) scan[1+2*i]=frame[6+3*select[i]];
        scan[2+2*count]=63; segment(0xda,scan,4+2*count);
        unsigned c=select[0],cw=(17*(factors[c]>>4)+hmax-1)/hmax,ch=(19*(factors[c]&15)+vmax-1)/vmax;
        unsigned across=count>1?(17+8*hmax-1)/(8*hmax):(cw+7)/8;
        unsigned down=count>1?(19+8*vmax-1)/(8*vmax):(ch+7)/8;
        int predictor[3]={0}; unsigned restart=0;
        for(unsigned m=0;m<across*down;++m) {
            if(m && m%(s+1)==0) {pad(); mark(0xd0+(restart++&7)); memset(predictor,0,sizeof(predictor));}
            for(unsigned i=0;i<count;++i) {
                c=select[i]; unsigned h=count>1?factors[c]>>4:1, v=count>1?factors[c]&15:1;
                for(unsigned by=0;by<v;++by) for(unsigned bx=0;bx<h;++bx) {
                    int dc=(int)(8*c+8*((m%across)*h+bx+(m/across)*v+by))*8/(int)q;
                    dc_bits(dc-predictor[c]); predictor[c]=dc;
                }
            }
        }
        pad();
    }
    mark(0xd9);
}

static void color_scans(void) {
    uint32_t pixels[325]; struct sx_image_metadata info;
    CHECK(jpeg_ycbcr(128,128,128)==0x808080 && jpeg_ycbcr(0,128,128)==0 &&
          jpeg_ycbcr(255,128,128)==0xffffff,"neutral YCbCr conversion");
    CHECK(jpeg_ycbcr(76,85,255)==0xfe0000 && jpeg_ycbcr(150,44,21)==0x00ff01 &&
          jpeg_ycbcr(29,255,107)==0x0000fe,"JFIF primary conversion and clipping");
    for(unsigned combination=0;combination<64;++combination) {
        unsigned factors[3],blocks=0;
        for(unsigned c=0;c<3;++c) {unsigned f=(combination>>(2*c))&3;
            factors[c]=((1+(f>>1))<<4)| (1+(f&1)); blocks+=(factors[c]>>4)*(factors[c]&15);}
        for(unsigned mode=0;mode<3;++mode) for(unsigned app=0;app<5;++app) {
            color_synthetic(factors,mode,app);
            pixels[0]=pixels[324]=0xdeadbeef;
            if(mode==0 && blocks>10) {
                CHECK(sx_jpeg_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_JPEG_SAMPLING,"interleaved MCU ten-block ceiling");
                continue;
            }
            for(unsigned d=1;d<=8;d*=2) {
                unsigned w=(17+d-1)/d,h=(19+d-1)/d,block_size=8/d;
                struct sx_image_options options={0,0,0,d}; pixels[0]=pixels[w*h+1]=0xdeadbeef;
                CHECK(sx_image_decode_into(fixture,used,&options,pixels+1,w*h,&info)==0,"synthetic colour scan at every scale");
                unsigned hmax=1,vmax=1;
                for(unsigned c=0;c<3;++c) {if((factors[c]>>4)>hmax) hmax=factors[c]>>4;
                    if((factors[c]&15)>vmax) vmax=factors[c]&15;}
                for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
                    unsigned value[3];
                    for(unsigned c=0;c<3;++c) value[c]=128+8*c+8*((x*(factors[c]>>4)/hmax)/block_size+(y*(factors[c]&15)/vmax)/block_size);
                    uint32_t expected=(app==0 || app==3)?(value[0]<<16)|(value[1]<<8)|value[2]:jpeg_ycbcr(value[0],value[1],value[2]);
                    CHECK(pixels[1+y*w+x]==expected,"exact scaled component placement, predictor resets, table snapshots and replication");
                }
                CHECK(pixels[0]==0xdeadbeef && pixels[w*h+1]==0xdeadbeef,"synthetic colour guards at reduced extent");
            }
        }
    }
}

static size_t later_marker(unsigned marker,unsigned occurrence) {
    for(size_t i=2;i+1<used;++i) if(fixture[i]==255 && fixture[i+1]==marker && occurrence--==0) return i;
    CHECK(0,"later fixture marker exists"); return 0;
}

static void color_malformed(void) {
    const unsigned factors[3]={0x22,0x11,0x11};
    // These images exceed error_expected's destination, so use a fitting 8x8 frame.
#define COLOR(mode,app) do { color_synthetic(factors,mode,app); size_t f=find_marker(0xc0); \
    fixture[f+6]=8; fixture[f+8]=8; } while(0)
    COLOR(0,0); fixture[find_marker(0xc0)+13]=1; error_expected(SX_IMAGE_ERROR_HEADER,1); // Repeated frame ID.
    COLOR(0,0); fixture[find_marker(0xda)+7]=1; error_expected(SX_IMAGE_ERROR_HEADER,1); // Repeated scan selector.
    COLOR(0,0); fixture[find_marker(0xda)+5]=2; fixture[find_marker(0xda)+7]=1;
    error_expected(SX_IMAGE_ERROR_HEADER,1); // Interleaved order must follow SOF.
    COLOR(0,0); fixture[find_marker(0xda)+9]=99; error_expected(SX_IMAGE_ERROR_HEADER,1);
    COLOR(0,0); fixture[find_marker(0xda)+10]=0x22; error_expected(SX_IMAGE_ERROR_HEADER,1); // Missing selected tables.
    COLOR(0,0); fixture[find_marker(0xc0)+18]=1; error_expected(SX_IMAGE_ERROR_HEADER,1); // Missing Cr quantizer.
    COLOR(0,0); fixture[find_marker(0xee)+15]=2; error_expected(SX_IMAGE_ERROR_JPEG_COLOR,1);
    COLOR(0,0); fixture[find_marker(0xee)+15]=255; error_expected(SX_IMAGE_ERROR_JPEG_COLOR,1);
    COLOR(0,0); size_t at=find_marker(0xee); fixture[at+3]=13;
    memmove(fixture+at+15,fixture+at+16,used-at-16); --used; error_expected(SX_IMAGE_ERROR_HEADER,1);
    COLOR(0,2); at=find_marker(0xe0); fixture[at+16]=1; fixture[at+17]=1;
    error_expected(SX_IMAGE_ERROR_HEADER,1); // Thumbnail extent exceeds APP0.
#undef COLOR
    uint32_t pixels[325]; struct sx_image_metadata info,zero={0};
    for(unsigned fault=0;fault<11;++fault) {
        color_synthetic(factors,1,0);
        size_t scan=later_marker(0xda,1), third=later_marker(0xda,2);
        switch(fault) {
        case 0: used=scan; mark(0xd9); break; // Only one component supplied.
        case 1: fixture[scan+5]=3; break; // Component repeated from first scan.
        case 2: fixture[scan+5]=99; break;
        case 3: fixture[scan+6]=0x11; break; // Missing Huffman tables in later scan.
        case 4: fixture[scan+7]=1; break; // Nonsequential later scan.
        case 5: fixture[later_marker(0xd0,1)+1]=0xd3; break;
        case 6: used=third; mark(0xd9); break; // Missing last component.
        case 7: at=later_marker(0xdb,1); fixture[at+5]=0; break; // Malformed replacement quantizer.
        case 8: at=later_marker(0xc4,2); fixture[at+4]=0x30; break;
        case 9: fixture[later_marker(0xdd,1)+3]=3; break; // Truncated DRI body.
        case 10: used-=2; {const unsigned char adobe[12]={'A','d','o','b','e',0,100,0,0,0,0,1};
            segment(0xee,adobe,12); mark(0xd9);} break; // Contradictory colour hints.
        }
        unsigned before=allocations,freed=releases; pixels[0]=pixels[324]=0xdeadbeef;
        CHECK(sx_jpeg_info(fixture,used,NULL,&info)==0,"late colour error cannot be detected by info");
        CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,323,&info)==SX_IMAGE_ERROR_DECODE &&
              metadata_equal(&info,&zero),"invalid later scan is decode failure");
        CHECK(pixels[0]==0xdeadbeef && pixels[324]==0xdeadbeef,"late colour error guards");
        CHECK(allocations-before==releases-freed,"late colour error workspace released");
        for(unsigned d=2;d<=8;d*=2) {
            struct sx_image_options options={0,0,0,d}; unsigned count=((17+d-1)/d)*((19+d-1)/d);
            pixels[0]=pixels[count+1]=0xdeadbeef; before=allocations; freed=releases;
            CHECK(sx_image_decode_into(fixture,used,&options,pixels+1,count,&info)==SX_IMAGE_ERROR_DECODE &&
                  metadata_equal(&info,&zero),"reduced decode still validates every later scan");
            CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef &&
                  allocations-before==releases-freed,"reduced late error guards and release");
        }
    }
    // SOF1 supports the same full colour path without changing SOF0 output.
    memcpy(fixture,jpeg_color_separate,sizeof(jpeg_color_separate)); used=sizeof(jpeg_color_separate);
    fixture[find_marker(0xc0)+1]=0xc1;
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,323,NULL)==0,"SOF1 colour and separate scans");
    uint32_t baseline[323];
    CHECK(sx_jpeg_decode_into(jpeg_color_separate,sizeof(jpeg_color_separate),NULL,baseline,323,NULL)==0 &&
          !memcmp(baseline,pixels+1,sizeof(baseline)),"SOF0 and SOF1 colour agree");
    uint32_t seed=0x8943;
    for(unsigned i=0;i<3000;++i) {
        const unsigned char *original=i%2?jpeg_color_separate:jpeg_color_mixed;
        size_t size=i%2?sizeof(jpeg_color_separate):sizeof(jpeg_color_mixed);
        memcpy(fixture,original,size); used=size; seed=seed*1664525u+1013904223u;
        size_t at_mutation=seed%size; seed=seed*1664525u+1013904223u;
        fixture[at_mutation]^=(unsigned char)(1u<<(seed%8));
        pixels[0]=pixels[324]=0xdeadbeef;
        unsigned before=allocations,freed=releases;
        const struct sx_image_options full={0,0,0,1};
        int result=sx_image_decode_into(fixture,used,&full,pixels+1,323,&info);
        CHECK(pixels[0]==0xdeadbeef && pixels[324]==0xdeadbeef,"mutated colour guards");
        CHECK(allocations-before==releases-freed,"mutated colour workspace released");
        if(result) CHECK(metadata_equal(&info,&zero),"mutated colour failure metadata cleared");
        else CHECK(info.width*info.height<=323 && info.scale_denom==1,"mutated colour success dimensions bounded");
    }
}

static void reduced_stages(void) {
    const double pi=3.14159265358979323846;
    int32_t block[64]={0}; unsigned char pixels[66];
    for(unsigned d=2;d<=8;d*=2) {
        unsigned n=8/d;
        for(int dc=-1024;dc<=1023;++dc) {
            block[0]=dc; memset(pixels,0xa5,sizeof(pixels)); jpeg_idct_scaled(block,pixels+1,d);
            int expected=(int)round(dc/8.0)+128; if(expected<0) expected=0; if(expected>255) expected=255;
            for(unsigned i=0;i<n*n;++i) CHECK(pixels[i+1]==expected,"reduced constant block rounds and clamps exactly");
            CHECK(pixels[0]==0xa5 && pixels[n*n+1]==0xa5,"scaled IDCT writes only N*N samples");
        }
        double basis[8][8]={0};
        for(unsigned x=0;x<n;++x) for(unsigned u=0;u<8;++u)
            for(unsigned i=0;i<d;++i) basis[x][u]+=(u?1:sqrt(.5))*cos((2*(x*d+i)+1)*u*pi/16)/d;
        for(unsigned test=0;test<128;++test) {
            for(unsigned i=0;i<64;++i) block[i]=(int32_t)((test*31+i*17+i*i)%401)-200;
            memset(pixels,0xa5,sizeof(pixels)); jpeg_idct_scaled(block,pixels+1,d);
            for(unsigned y=0;y<n;++y) for(unsigned x=0;x<n;++x) {
                double sum=0;
                for(unsigned v=0;v<8;++v) for(unsigned u=0;u<8;++u) sum+=block[v*8+u]*basis[x][u]*basis[y][v]/4;
                int expected=(int)round(sum)+128; if(expected<0) expected=0; if(expected>255) expected=255;
                CHECK(abs((int)pixels[1+y*n+x]-expected)<=1,"reduced IDCT matches unclipped spatial averages within one level");
            }
            CHECK(pixels[0]==0xa5 && pixels[n*n+1]==0xa5,"nonconstant reduced IDCT guards");
        }
        memset(block,0,sizeof(block));
        // Averaging annihilates these frequencies; skipping arbitrary AC terms does not.
        block[4]=1000; block[32]=-1000; jpeg_idct_scaled(block,pixels,d);
        for(unsigned i=0;i<n*n;++i) CHECK(pixels[i]==128,"cancelled high frequency terms");
        for(unsigned i=0;i<64;++i) block[i]=((i&1)?1023:-1024)*65535;
        jpeg_idct_scaled(block,pixels,d); // UBSan checks wide products/sums at every reduced scale.
        memset(block,0,sizeof(block));
    }
}

static void reduced_fixtures(void) {
    const struct {const unsigned char *jpeg; size_t size; unsigned w,h; const uint32_t *expected[3];} refs[]={
#define REF(name,w,h) {jpeg_##name,sizeof(jpeg_##name),w,h,{expected_reduced_##name##_2,expected_reduced_##name##_4,expected_reduced_##name##_8}}
        REF(black,8,8),REF(white,8,8),REF(tiny,3,5),REF(gradient,9,9),REF(grain,17,13),REF(restart,17,13),
        REF(color_y444,9,13),REF(color_y422,17,9),REF(color_y420,17,19),REF(color_y440,9,17),
        REF(color_separate,17,19),REF(color_mixed,17,19),REF(color_pair,17,9),REF(color_rgb,13,9),
        REF(color_rgb_separate,17,13),REF(color_rgb_ids,9,9),REF(color_no_marker,9,9),
        REF(color_adobe_ycc,9,13),REF(color_both,9,9),REF(color_rgb422,17,9)
#undef REF
    };
    unsigned max_gray=0,max_color=0;
    for(unsigned r=0;r<sizeof(refs)/sizeof(refs[0]);++r) for(unsigned d=2,index=0;d<=8;d*=2,++index) {
        unsigned w=(refs[r].w+d-1)/d,h=(refs[r].h+d-1)/d,count=w*h;
        uint32_t pixels[325],common[323]; for(unsigned i=0;i<325;++i) pixels[i]=0xdeadbeef;
        struct sx_image_options options={0,0,0,d};
        struct sx_image_metadata info={0},decoded={0},wanted={SX_IMAGE_FORMAT_JPEG,refs[r].w,refs[r].h,w,h,d},zero={0};
        unsigned before=allocations,freed=releases;
        CHECK(sx_jpeg_info(refs[r].jpeg,refs[r].size,&options,&info)==0 && metadata_equal(&info,&wanted) &&
              allocations==before,"reduced info exact metadata, no allocation");
        CHECK(sx_jpeg_decode_into(refs[r].jpeg,refs[r].size,&options,pixels+1,count,&decoded)==0 &&
              metadata_equal(&decoded,&info),"reduced reference decoded");
        CHECK(allocations-before==releases-freed,"reduced workspace released");
        if(r<6) CHECK(allocations==before,"reduced greyscale has no allocation");
        else CHECK(allocations==before+1 && last_allocation_size<=3*count,"colour workspace bounded by reduced output");
        for(unsigned i=0;i<count;++i) {
            CHECK((pixels[i+1]>>24)==0,"reduced opaque packing");
            for(unsigned shift=0;shift<=16;shift+=8) {
                unsigned delta=(unsigned)abs((int)((pixels[i+1]>>shift)&255)-(int)((refs[r].expected[index][i]>>shift)&255));
                CHECK(delta<=(r<6?1u:3u),"matched scaled reference tolerance: greyscale one, RGB three");
                if(r<6) {if(delta>max_gray) max_gray=delta;} else if(delta>max_color) max_color=delta;
            }
        }
        CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef,"reduced exact buffer guards");
        CHECK(sx_image_info(refs[r].jpeg,refs[r].size,&options,&decoded)==0 && metadata_equal(&decoded,&wanted) &&
              sx_image_decode_into(refs[r].jpeg,refs[r].size,&options,common,count,NULL)==0 &&
              !memcmp(common,pixels+1,count*4),"common reduced metadata and pixels");
        before=allocations;
        CHECK(sx_jpeg_decode_into(refs[r].jpeg,refs[r].size,&options,pixels+1,count-1,&decoded)==SX_IMAGE_ERROR_BUFFER &&
              metadata_equal(&decoded,&zero) && allocations==before,"reduced short buffer precedes allocation");
        if(r>=6) {
            for(unsigned i=0;i<325;++i) pixels[i]=0xdeadbeef;
            fail_allocation=1;
            CHECK(sx_image_decode_into(refs[r].jpeg,refs[r].size,&options,pixels+1,count,&decoded)==SX_IMAGE_ERROR_DECODE &&
                  metadata_equal(&decoded,&zero),"reduced allocation failure resets metadata");
            fail_allocation=0;
            for(unsigned i=0;i<325;++i) CHECK(pixels[i]==0xdeadbeef,"reduced allocation failure precedes writes");
        }
        for(size_t length=0;length<refs[r].size;++length) {
            pixels[0]=pixels[count+1]=0xdeadbeef; before=allocations; freed=releases;
            CHECK(sx_jpeg_decode_into(refs[r].jpeg,length,&options,pixels+1,count,&decoded)<0 &&
                  metadata_equal(&decoded,&zero),"all reduced truncated prefixes fail even at DC-only scale");
            CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef &&
                  allocations-before==releases-freed,"reduced truncation guards and release");
        }
    }
    printf("60 reduced references: maximum difference greyscale %u, colour %u\n",max_gray,max_color);
}

static void scale_plans(void) {
    const struct {struct sx_image_options options; unsigned d;} plans[]={
        {{0,0,0,0},1},{{16,0,0,0},2},{{0,18,0,0},2},{{0,0,322,0},2},
        {{8,0,0,0},4},{{0,9,0,0},4},{{0,0,89,0},4},{{4,0,0,0},8},{{0,4,0,0},8},{{0,0,24,0},8},
        {{2,0,0,0},0},{{0,2,0,0},0},{{0,0,8,0},0},{{8,0,0,2},0},{{4,0,0,4},0},{{0,0,8,8},0},
        {{9,10,90,2},2},{{5,5,25,4},4},{{3,3,9,8},8},{{9,9,90,0},4},{{9,10,25,0},4},
        {{UINT32_MAX,UINT32_MAX,UINT32_MAX,0},1}
    };
    for(unsigned i=0;i<sizeof(plans)/sizeof(plans[0]);++i) {
        uint32_t pixels[325],explicit_pixels[323]; for(unsigned j=0;j<325;++j) pixels[j]=0xdeadbeef;
        struct sx_image_metadata info={0},decoded={0},zero={0};
        int expected=plans[i].d?SX_IMAGE_OK:SX_IMAGE_ERROR_LIMIT;
        unsigned before=allocations;
        CHECK(sx_image_info(jpeg_color_y420,sizeof(jpeg_color_y420),&plans[i].options,&info)==expected &&
              allocations==before,"automatic scale considers all three limits without allocation");
        if(!plans[i].d) {
            CHECK(metadata_equal(&info,&zero) && sx_image_decode_into(jpeg_color_y420,sizeof(jpeg_color_y420),
                  &plans[i].options,pixels+1,323,&decoded)==expected && metadata_equal(&decoded,&zero) && allocations==before,
                  "unfitting exact/automatic scale fails before allocation");
            for(unsigned j=0;j<325;++j) CHECK(pixels[j]==0xdeadbeef,"limit failure has no pixel writes");
            continue;
        }
        unsigned d=plans[i].d,w=(17+d-1)/d,h=(19+d-1)/d;
        struct sx_image_metadata wanted={SX_IMAGE_FORMAT_JPEG,17,19,w,h,d};
        CHECK(metadata_equal(&info,&wanted),"automatic chooses smallest fitting scale");
        CHECK(sx_image_decode_into(jpeg_color_y420,sizeof(jpeg_color_y420),&plans[i].options,pixels+1,w*h,&decoded)==0 &&
              metadata_equal(&info,&decoded),"automatic info and decode agree");
        struct sx_image_options exact={0,0,0,d};
        CHECK(sx_jpeg_decode_into(jpeg_color_y420,sizeof(jpeg_color_y420),&exact,explicit_pixels,w*h,NULL)==0 &&
              !memcmp(explicit_pixels,pixels+1,w*h*4),"automatic pixels equal explicit scale");
        const size_t expected_bytes=d==1?503:d==2?140:d==4?43:17;
        CHECK(last_allocation_size==expected_bytes,"known reduced planes allocation, no full-sized workspace");
        CHECK(pixels[0]==0xdeadbeef && pixels[w*h+1]==0xdeadbeef,"automatic output guards");
    }
    struct sx_image_metadata info,zero={0}; uint32_t pixels[4098];
    for(unsigned dimension=1;dimension<=32768;dimension=dimension==1?4097:32768) {
        minimal(dimension,1,7); unsigned d=dimension==1?1:dimension==4097?2:8;
        CHECK(sx_jpeg_info(fixture,used,NULL,&info)==0 && info.source_width==dimension &&
              info.width==(dimension+d-1)/d && info.height==1 && info.scale_denom==d,"large sources reduce below global output ceiling");
        unsigned count=(dimension+d-1)/d; pixels[0]=pixels[count+1]=0xdeadbeef;
        CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,count,NULL)==0,"large source entropy and restarts decoded at reduced scale");
        for(unsigned x=0;x<count;++x) CHECK(pixels[x+1]==0x808080,"large reduced constant pixels");
        CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef,"large source reduced guards");
        if(dimension==32768) break;
    }
    minimal(32769,1,0);
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_LIMIT && metadata_equal(&info,&zero),"no scale fits global width ceiling");
    minimal(1,65535,0);
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_LIMIT,"large height cannot fit even at 1/8");
    minimal(8,8,0); size_t at=find_marker(0xc0); fixture[at+5]=fixture[at+7]=0x80; fixture[at+6]=fixture[at+8]=0;
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==0 && info.width==4096 && info.height==4096 &&
          info.scale_denom==8,"largest output area planned from 32768-square source without allocation");
}

static void reduced_errors(void) {
    uint32_t pixels[18]; struct sx_image_metadata info,zero={0};
    // Even the 1x1 IDCT must read and validate AC entropy, stuffing, padding and EOI.
    for(unsigned fault=0;fault<4;++fault) for(unsigned d=2;d<=8;d*=2) {
        minimal(8,8,0); size_t start=find_marker(0xda)+10;
        if(fault==0) fixture[start]|=0x40; // Invalid AC code after valid DC.
        if(fault==1) fixture[start]=0xff; // Unstuffed FF.
        if(fault==2) fixture[start]&=0xfe; // Zero padding bit.
        if(fault==3) used-=2; // No EOI.
        struct sx_image_options options={0,0,0,d}; unsigned count=(8/d)*(8/d);
        for(unsigned i=0;i<18;++i) pixels[i]=0xdeadbeef;
        CHECK(sx_jpeg_decode_into(fixture,used,&options,pixels+1,count,&info)==SX_IMAGE_ERROR_DECODE &&
              metadata_equal(&info,&zero),"reduction does not hide corrupt discarded AC or scan termination");
        CHECK(pixels[0]==0xdeadbeef && pixels[count+1]==0xdeadbeef,"corrupt reduced greyscale guards");
    }
    minimal(1,1,0); struct sx_image_options exact={1,1,1,8};
    CHECK(sx_jpeg_decode_into(fixture,used,&exact,pixels,1,&info)==0 && info.width==1 && info.height==1 &&
          info.scale_denom==8 && pixels[0]==0x808080,"explicit 1/8 remains exact even for a one-pixel source");
    uint32_t seed=0x5754;
    for(unsigned i=0;i<1500;++i) {
        const unsigned char *data=i%2?jpeg_color_separate:jpeg_grain;
        size_t size=i%2?sizeof(jpeg_color_separate):sizeof(jpeg_grain); memcpy(fixture,data,size); used=size;
        seed=seed*1664525u+1013904223u; size_t at=seed%size;
        seed=seed*1664525u+1013904223u; fixture[at]^=(unsigned char)(1u<<(seed%8));
        struct sx_image_options options={4,4,16,i%4==3?0:2u<<(i%4)};
        pixels[0]=pixels[17]=0xdeadbeef; unsigned before=allocations,freed=releases;
        int result=sx_image_decode_into(fixture,used,&options,pixels+1,16,&info);
        CHECK(pixels[0]==0xdeadbeef && pixels[17]==0xdeadbeef && allocations-before==releases-freed,"mutated reduced guards and release");
        if(result) CHECK(metadata_equal(&info,&zero),"mutated reduced error metadata");
        else CHECK(info.width<=4 && info.height<=4 && info.width*info.height<=16 &&
                   (info.scale_denom==1 || info.scale_denom==2 || info.scale_denom==4 || info.scale_denom==8) &&
                   info.width==(info.source_width+info.scale_denom-1)/info.scale_denom &&
                   info.height==(info.source_height+info.scale_denom-1)/info.scale_denom &&
                   (!options.scale_denom || info.scale_denom==options.scale_denom),"mutated reduced success honours dimensions, limits and exact denominator");
    }
}

static void malformed(void) {
    minimal(8,8,0); CHECK(sx_jpeg_info(NULL,0,NULL,NULL)==SX_IMAGE_ERROR_ARGUMENT,"null info arguments");
    struct sx_image_metadata info={0}; uint32_t pixels[4098];
    CHECK(sx_jpeg_info(NULL,0,NULL,&info)==SX_IMAGE_ERROR_ARGUMENT && sx_jpeg_decode_into(fixture,used,NULL,NULL,64,&info)==SX_IMAGE_ERROR_ARGUMENT,"null data and pixels");
    struct sx_image_options options={0,0,0,3};
    CHECK(sx_jpeg_info(fixture,used,&options,&info)==SX_IMAGE_ERROR_ARGUMENT,"invalid denominator");
    options=(struct sx_image_options){0,0,63,1};
    CHECK(sx_jpeg_info(fixture,used,&options,&info)==SX_IMAGE_ERROR_LIMIT,"pixel budget");
    options=(struct sx_image_options){0,7,0,1};
    CHECK(sx_jpeg_info(fixture,used,&options,&info)==SX_IMAGE_ERROR_LIMIT,"height limit");
    const struct {unsigned marker; int error;} unsupported[]={
        {0xc6,SX_IMAGE_ERROR_JPEG_PROGRESSIVE},
        {0xc9,SX_IMAGE_ERROR_JPEG_ARITHMETIC},{0xca,SX_IMAGE_ERROR_JPEG_ARITHMETIC},
        {0xc3,SX_IMAGE_ERROR_JPEG_LOSSLESS},{0xc7,SX_IMAGE_ERROR_JPEG_LOSSLESS}
    };
    for(unsigned i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
        minimal(8,8,0); fixture[find_marker(0xc0)+1]=(unsigned char)unsupported[i].marker;
        error_expected(unsupported[i].error,1);
    }
    minimal(8,8,0); fixture[find_marker(0xc0)+4]=12; error_expected(SX_IMAGE_ERROR_JPEG_PRECISION,1);
    // SOF2 is decoded now; a sequential scan (Ss=0, Se=63) under it is invalid.
    minimal(8,8,0); fixture[find_marker(0xc0)+1]=0xc2; error_expected(SX_IMAGE_ERROR_HEADER,1);
    minimal(8,8,0); { size_t at=find_marker(0xc0); fixture[at+1]=0xc2; fixture[at+4]=12; }
    error_expected(SX_IMAGE_ERROR_JPEG_PRECISION,1);
    // A sequential scan must not select an AC table holding EOB runs.
    minimal(8,8,0); fixture[find_marker(0xc4)+39]=0x10; error_expected(SX_IMAGE_ERROR_HEADER,1);
    // Actual three- and four-component frames, not merely a corrupted length.
    for(unsigned components=3;components<=4;++components) {
        minimal(8,8,0); size_t at=find_marker(0xc0), tail=at+13;
        memmove(fixture+tail+(components-1)*3,fixture+tail,used-tail); used+=(components-1)*3;
        fixture[at+3]=(unsigned char)(8+3*components); fixture[at+9]=(unsigned char)components;
        for(unsigned c=1;c<components;++c) { fixture[at+10+3*c]=(unsigned char)c; fixture[at+11+3*c]=0x11; fixture[at+12+3*c]=0; }
        if (components == 3) error_expected(SX_IMAGE_ERROR_DECODE,0); // Two components have no scan.
        else error_expected(SX_IMAGE_ERROR_JPEG_COLOR,1); // CMYK/YCCK remains unsupported.
    }
    for(unsigned sampling=0;sampling<4;++sampling) {
        minimal(8,8,0); fixture[find_marker(0xc0)+11]=(unsigned char)(sampling==0?0:sampling==1?0x10:sampling==2?0x31:0x14);
        error_expected(SX_IMAGE_ERROR_JPEG_SAMPLING,1);
    }
    for(unsigned sampling=0;sampling<4;++sampling) {
        minimal(9,9,0); fixture[find_marker(0xc0)+11]=(unsigned char)((1+(sampling>>1))*16+1+(sampling&1));
        CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,81,NULL)==0,"single-component sampling 1/2 has one block per MCU");
    }
    for(unsigned i=0;i<10;++i) {
        minimal(8,8,0);
        switch(i) {
            case 0: fixture[find_marker(0xdb)+4]=4; break; // bad quantizer ID
            case 1: fixture[find_marker(0xdb)+5]=0; break; // zero quantizer
            case 2: fixture[find_marker(0xc4)+4]=0x20; break; // bad table class
            case 3: fixture[find_marker(0xc4)+5]=2; break; // all-ones Huffman code
            case 4: fixture[find_marker(0xc4)+21]=12; break; // DC category
            case 5: fixture[find_marker(0xc0)+12]=3; break; // missing quantizer
            case 6: fixture[find_marker(0xda)+5]=8; break; // unknown component
            case 7: fixture[find_marker(0xda)+6]=0x11; break; // missing tables
            case 8: fixture[find_marker(0xda)+8]=62; break; // incomplete sequential scan
            case 9: fixture[find_marker(0xda)+9]=1; break; // successive approximation
        }
        error_expected(SX_IMAGE_ERROR_HEADER,1);
    }
    for(unsigned i=0;i<3;++i) {
        minimal(8,8,0); size_t at=find_marker(0xc0);
        if(i==0) fixture[at+7]=fixture[at+8]=0; // zero width
        if(i==1) fixture[at+7]=0x10,fixture[at+8]=1; // width 4097
        if(i==2) fixture[at+5]=0x10,fixture[at+6]=1; // height 4097
        error_expected(i==0?SX_IMAGE_ERROR_HEADER:SX_IMAGE_ERROR_LIMIT,1);
    }
    minimal(8,8,0); fixture[find_marker(0xdb)+2]=0xff; error_expected(SX_IMAGE_ERROR_HEADER,1);
    minimal(8,8,0); fixture[find_marker(0xdb)+3]=1; error_expected(SX_IMAGE_ERROR_HEADER,1);
    minimal(8,8,0); fixture[used-3]&=0xfe; error_expected(SX_IMAGE_ERROR_DECODE,0); // bad pad bit
    minimal(8,8,0); fixture[used-3]=0xff; error_expected(SX_IMAGE_ERROR_DECODE,0); // unstuffed marker
    minimal(8,8,0); fixture[used-1]=0xd0; error_expected(SX_IMAGE_ERROR_DECODE,0); // missing EOI
    minimal(16,8,1); fixture[find_marker(0xd0)+1]=0xd1; error_expected(SX_IMAGE_ERROR_DECODE,0);
    minimal(16,8,1); size_t rst=find_marker(0xd0); memmove(fixture+rst,fixture+rst+2,used-rst-2); used-=2;
    error_expected(SX_IMAGE_ERROR_DECODE,0);
    minimal(16,8,1); fixture[find_marker(0xdd)+4]=fixture[find_marker(0xdd)+5]=0;
    error_expected(SX_IMAGE_ERROR_DECODE,0); // restart without DRI
    minimal(129,9,1); CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,129*9,NULL)==0,"restart numbering wraps modulo eight");
    for(unsigned i=0;i<129*9;++i) CHECK(pixels[i+1]==0x808080,"restart output and predictor reset");
    // Extended sequential grayscale uses 16-bit DQT and selectors 0..3.
    minimal(9,9,0);
    size_t q_at=find_marker(0xdb), sof_at=find_marker(0xc0), scan_at=find_marker(0xda);
    unsigned char enlarged[32768];
    memcpy(enlarged,fixture,q_at+4); enlarged[q_at+2]=0; enlarged[q_at+3]=131;
    enlarged[q_at+4]=0x13;
    for(unsigned i=0;i<64;++i) { enlarged[q_at+5+2*i]=0; enlarged[q_at+6+2*i]=1; }
    memcpy(enlarged+q_at+133,fixture+q_at+69,used-q_at-69); used+=64;
    memcpy(fixture,enlarged,used); sof_at+=64; scan_at+=64;
    fixture[sof_at+1]=0xc1; fixture[sof_at+12]=3;
    size_t h_at=find_marker(0xc4); fixture[h_at+4]=3; fixture[h_at+22]=0x13; fixture[scan_at+6]=0x33;
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,81,NULL)==0,"SOF1 wide quantizer and table ID 3 decode");
    for(unsigned i=0;i<81;++i) CHECK(pixels[i+1]==0x808080,"SOF1 grayscale pixels");
    fixture[sof_at+1]=0xc0;
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_HEADER,"SOF0 rejects extended tables");
    minimal(8,8,0); used-=2;
    const unsigned char comment[3]={'o','w','n'}; segment(0xfe,comment,3);
    const unsigned char app[4]={1,2,3,4}; segment(0xe1,app,4);
    unsigned char q_body[65]={0}; for(unsigned i=1;i<65;++i) q_body[i]=1;
    segment(0xdb,q_body,65); mark(0xd9);
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,64,NULL)==0,"tables and APP/COM between scan and EOI accepted");
    minimal(8,8,0); size_t extra=find_marker(0xdb);
    // A duplicate frame is invalid, but repeated tables before the scan are valid.
    unsigned char copy[69]; memcpy(copy,fixture+extra,69);
    memmove(fixture+extra+69,fixture+extra,used-extra); memcpy(fixture+extra,copy,69); used+=69;
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,64,NULL)==0,"table redefinition before SOS accepted");
    minimal(8,8,0); sof_at=find_marker(0xc0); unsigned char frame_copy[13]; memcpy(frame_copy,fixture+sof_at,13);
    memmove(fixture+sof_at+13,fixture+sof_at,used-sof_at); memcpy(fixture+sof_at,frame_copy,13); used+=13;
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_HEADER,"duplicate frame refused");
    // Mutations supplement targeted assertions and exercise bounded decoding.
    const unsigned char *original=jpeg_grain; size_t size=sizeof(jpeg_grain);
    uint32_t seed=0x9135;
    for(unsigned i=0;i<2000;++i) {
        memcpy(fixture,original,size); used=size; seed=seed*1664525u+1013904223u;
        size_t at=seed%size; seed=seed*1664525u+1013904223u; fixture[at]^=(unsigned char)(1u<<(seed%8));
        for(unsigned k=0;k<258;++k) pixels[k]=0xdeadbeef;
        const struct sx_image_options full={0,0,0,1};
        int result=sx_image_decode_into(fixture,size,&full,pixels+1,256,&info);
        CHECK(pixels[0]==0xdeadbeef && pixels[257]==0xdeadbeef,"mutated input output guards");
        struct sx_image_metadata zero={0};
        if(result) CHECK(metadata_equal(&info,&zero),"mutated failure metadata cleared");
        else CHECK(info.width*info.height<=256 && info.scale_denom==1,"mutated success dimensions bounded");
    }
    CHECK(!strcmp(sx_image_error_string(SX_IMAGE_ERROR_JPEG_PROGRESSIVE),"Differential progressive JPEG is unsupported"),"differential progressive diagnostic");
    CHECK(!strcmp(sx_image_error_string(SX_IMAGE_ERROR_JPEG_ARITHMETIC),"Arithmetic JPEG is unsupported"),"arithmetic diagnostic");
    CHECK(!strcmp(sx_image_error_string(SX_IMAGE_ERROR_JPEG_LOSSLESS),"Lossless JPEG is unsupported"),"lossless diagnostic");
}

/* --- Progressive JPEG (I7) --- */

/* Stage checks with a hand-built AC table: 000 EOB0, 001 (0,1), 010 (1,1),
 * 011 EOB1, 100 ZRL. Bits are written MSB first into `bytes`. */
static void progressive_stages(void)
{
    struct jpeg_huffman h; struct jpeg_state st; memset(&st,0,sizeof(st));
    unsigned char counts[16]={0}, symbols[5]={0x00,0x01,0x11,0x10,0xf0}; counts[2]=5;
    CHECK(jpeg_huffman_build(&h,counts,symbols,5,1) && h.eob_runs,"progressive AC table");
    int16_t block[64]; uint32_t eobrun=0;
    // Refinement: (1,1)+sign 1, correction 1 for the nonzero zz[1], then EOB0.
    { const unsigned char b[]={0x58,0xff}; struct jpeg_reader r={b,2,0,0,0};
      memset(block,0,sizeof(block)); block[jpeg_zigzag[1]]=2; st.ss=1; st.se=5; st.al=0;
      CHECK(jpeg_ac_refine(&r,&st,&h,&eobrun,block) && eobrun==0,"refinement block decoded");
      CHECK(block[jpeg_zigzag[1]]==3 && block[jpeg_zigzag[3]]==1 && block[jpeg_zigzag[2]]==0 &&
            block[jpeg_zigzag[4]]==0,"correction bit and new coefficient placed past one zero"); }
    // Negative coefficients grow in magnitude; a 0 correction bit leaves them.
    { const unsigned char b[]={0x10,0x00}; struct jpeg_reader r={b,2,0,0,0}; // 000 (EOB0), bits 1 0
      memset(block,0,sizeof(block)); block[jpeg_zigzag[1]]=-4; block[jpeg_zigzag[2]]=6; st.ss=1; st.se=2; st.al=1;
      CHECK(jpeg_ac_refine(&r,&st,&h,&eobrun,block) && eobrun==0,"refinement inside an EOB run");
      CHECK(block[jpeg_zigzag[1]]==-6 && block[jpeg_zigzag[2]]==6,"negative refinement and zero correction"); }
    // EOB1 with extra bit 1: this block and two more end their band.
    { const unsigned char b[]={0x7f}; struct jpeg_reader r={b,1,0,0,0}; // 011 1 ...
      memset(block,0,sizeof(block)); st.ss=1; st.se=63; st.al=0; eobrun=0;
      CHECK(jpeg_ac_first(&r,&st,&h,&eobrun,block) && eobrun==2,"EOB1 run length 3");
      CHECK(jpeg_ac_first(&r,&st,&h,&eobrun,block) && jpeg_ac_first(&r,&st,&h,&eobrun,block) && eobrun==0 && r.pos==1 && r.remaining==4,
            "blocks inside the run read no bits"); }
    // First scan: (0,1) value +1 at Al=3 stores 8; ZRL past Se is invalid.
    { const unsigned char b[]={0x3f}; struct jpeg_reader r={b,1,0,0,0}; // 001 1 1111 -> then 111 (invalid code)
      memset(block,0,sizeof(block)); st.ss=1; st.se=1; st.al=3; eobrun=0;
      CHECK(jpeg_ac_first(&r,&st,&h,&eobrun,block) && block[jpeg_zigzag[1]]==8,"point transform restores scale"); }
    { const unsigned char b[]={0x80,0}; struct jpeg_reader r={b,2,0,0,0};
      memset(block,0,sizeof(block)); st.ss=1; st.se=10; st.al=0; eobrun=0;
      CHECK(!jpeg_ac_first(&r,&st,&h,&eobrun,block),"ZRL beyond the band refused"); }
    { const unsigned char b[]={0x3f}; struct jpeg_reader r={b,1,0,0,0};
      memset(block,0,sizeof(block)); st.ss=1; st.se=1; st.al=10; eobrun=0;
      CHECK(!jpeg_ac_first(&r,&st,&h,&eobrun,block),"AC value above 1023 after the point transform refused"); }
    { const unsigned char b[]={0x5f}; struct jpeg_reader r={b,1,0,0,0}; // (1,1) + sign at the band's last slot
      memset(block,0,sizeof(block)); st.ss=1; st.se=1; st.al=0; eobrun=0;
      CHECK(!jpeg_ac_refine(&r,&st,&h,&eobrun,block),"refinement coefficient past Se refused"); }
    { const unsigned char b[]={0x3f}; struct jpeg_reader r={b,1,0,0,0};
      memset(block,0,sizeof(block)); st.ss=1; st.se=5; st.al=10; eobrun=0;
      CHECK(!jpeg_ac_refine(&r,&st,&h,&eobrun,block),"refinement value above the AC range refused"); }
    // DC: first scan stores diff*2^Al, refinement ORs bit Al.
    { struct jpeg_huffman dc; unsigned char c2[16]={0}, s2[1]={2}; c2[0]=1;
      CHECK(jpeg_huffman_build(&dc,c2,s2,1,0),"DC table");
      const unsigned char b[]={0x5f}; struct jpeg_reader r={b,1,0,0,0}; // 0 10 (+2) 1 (refine) 1111
      int32_t predictor=3; int16_t coef=0;
      CHECK(jpeg_dc_first(&r,&dc,2,&predictor,&coef) && predictor==5 && coef==20,"DC prediction on transformed values");
      CHECK(jpeg_dc_refine(&r,1,&coef) && coef==22,"DC refinement bit");
      r=(struct jpeg_reader){b,1,0,0,0}; predictor=255;
      CHECK(!jpeg_dc_first(&r,&dc,2,&predictor,&coef),"DC beyond the coefficient range refused"); }
}

/* Hand-made progressive greyscale: 8 bits per blocks_w*8 x 8, a DC scan
 * (diff 0) and optionally one AC scan with a single EOB1 run of `run`. */
static size_t progressive_minimal(unsigned blocks_w, unsigned restart, unsigned run, unsigned ac_scan) {
    used=pending=bit_count=0; mark(0xd8);
    unsigned char q[65]={0}; memset(q+1,1,64); segment(0xdb,q,sizeof(q));
    unsigned char h[37]={0}; h[1]=1; h[17]=0; h[18]=0x10; h[20]=2; h[35]=0x00; h[36]=0x10; segment(0xc4,h,sizeof(h));
    unsigned width=blocks_w*8;
    unsigned char f[9]={8,0,8,(unsigned char)(width>>8),(unsigned char)width,1,7,0x11,0};
    segment(0xc2,f,sizeof(f));
    if(restart) { unsigned char r[2]={0,(unsigned char)restart}; segment(0xdd,r,2); }
    const unsigned char dc[6]={1,7,0x00,0,0,0}; segment(0xda,dc,6);
    for(unsigned i=0;i<blocks_w;++i) { if(i && restart && i%restart==0) { pad(); mark(0xd0+((i/restart-1)&7)); } bits(0,1); }
    pad();
    if(ac_scan) {
        const unsigned char ac[6]={1,7,0x00,1,63,0}; segment(0xda,ac,6);
        if(run==1) { for(unsigned i=0;i<blocks_w;++i) { if(i && restart && i%restart==0) { pad(); mark(0xd0+((i/restart-1)&7)); } bits(0,2); } }
        else { bits(1,2); bits(run-2,1); }  // EOB1: runs of 2 or 3
        pad();
    }
    mark(0xd9); return used;
}

static void progressive_synthetic(void)
{
    static uint32_t pixels[642]; struct sx_image_metadata info={0};
    progressive_minimal(2,0,2,1);
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,128,&info)==0 && info.width==16 && info.height==8,"EOB run covering the scan");
    for(unsigned i=0;i<128;++i) CHECK(pixels[i+1]==0x808080,"progressive synthetic pixels");
    progressive_minimal(2,0,0,0);
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,128,NULL)==0,"DC-only progressive frame decoded");
    progressive_minimal(2,0,3,1); error_expected(SX_IMAGE_ERROR_DECODE,0); // EOB run past the scan
    progressive_minimal(2,1,2,1); error_expected(SX_IMAGE_ERROR_DECODE,0); // EOB run across RST0
    progressive_minimal(2,1,1,1);
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,128,NULL)==0,"per-block EOB with restarts");
    progressive_minimal(10,1,1,1);
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,640,NULL)==0,"restart numbering across progressive scans");
    unsigned before=allocations;
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,639,NULL)==SX_IMAGE_ERROR_BUFFER && allocations==before,
          "progressive capacity checked before allocation");
    // The coefficient store bounds the source: 8192x6144 greyscale fits, one more block row does not.
    progressive_minimal(2,0,2,1); size_t sof=find_marker(0xc2);
    fixture[sof+5]=0x18; fixture[sof+6]=0x00; fixture[sof+7]=0x20; fixture[sof+8]=0x00;
    before=allocations;
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==0 && info.scale_denom==2 && info.width==4096 && info.height==3072,
          "largest progressive store planned at scale 2");
    fixture[sof+6]=0x01;
    CHECK(sx_jpeg_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_LIMIT && sx_image_info(fixture,used,NULL,&info)==SX_IMAGE_ERROR_LIMIT,
          "progressive coefficient store limit");
    CHECK(allocations==before,"progressive info allocates nothing");
    // Header mutations of the hand-made stream.
    for(unsigned i=0;i<9;++i) {
        progressive_minimal(2,0,2,1); size_t dc=find_marker(0xda), ac=later_marker(0xda,1);
        int first=1;
        switch(i) {
            case 0: fixture[dc+8]=1; break;                     // DC scan with Se=1
            case 1: fixture[dc+9]=0x10; break;                  // refinement before a first scan
            case 2: fixture[dc+7]=1; fixture[dc+8]=63; break;   // AC band before the component's DC
            case 3: fixture[ac+9]=0x10; first=0; break;          // AC refinement of an uncoded band
            case 4: fixture[ac+8]=64; first=0; break;            // Se beyond 63
            case 5: fixture[ac+7]=9; fixture[ac+8]=8; first=0; break; // Ss > Se
            case 6: fixture[ac+7]=0; fixture[ac+8]=0; first=0; break; // DC first scan twice
            case 7: fixture[dc+9]=0x0e; break;                  // Al beyond 13
            case 8: fixture[ac+6]=0x01; first=0; break;          // missing AC table 1
        }
        error_expected(first?SX_IMAGE_ERROR_HEADER:SX_IMAGE_ERROR_DECODE,first);
    }
}

static void progressive_compare(const char *name,const unsigned char *prog,size_t prog_size,
                                const unsigned char *seq,size_t seq_size)
{
    static uint32_t a[4096+2], b[4096+2];
    struct sx_image_metadata pi={0}, si={0}, zero={0};
    for(unsigned scale=1;scale<=8;scale*=2) {
        const struct sx_image_options options={0,0,0,scale};
        CHECK(sx_jpeg_info(seq,seq_size,&options,&si)==0,"sequential source plan");
        unsigned count=si.width*si.height, before=allocations;
        CHECK(sx_jpeg_info(prog,prog_size,&options,&pi)==0 && metadata_equal(&pi,&si) && allocations==before,
              "progressive plan matches its source without allocating");
        for(unsigned i=0;i<count+2;++i) a[i]=b[i]=0xdeadbeef;
        unsigned freed=releases;
        int pr=sx_jpeg_decode_into(prog,prog_size,&options,a+1,count,&pi);
        CHECK(pr==0 && allocations==before+1 && releases==freed+1,"one progressive workspace released");
        CHECK(sx_jpeg_decode_into(seq,seq_size,&options,b+1,count,&si)==0 && metadata_equal(&pi,&si),"sequential source decoded");
        if(pr!=0 || memcmp(a,b,(count+2)*4)) printf("progressive %s scale %u differs (%d)\n",name,scale,pr);
        CHECK(!memcmp(a,b,(count+2)*4),"progressive pixels identical to the sequential source");
        CHECK(sx_image_decode_into(prog,prog_size,&options,b+1,count,&si)==0 && !memcmp(a,b,(count+2)*4),
              "common API decodes progressive");
        if(scale==1) {
            for(size_t length=0;length<prog_size;++length) {
                struct sx_image_metadata partial={9,9,9,9,9,9};
                before=allocations; freed=releases; a[0]=a[count+1]=0xdeadbeef;
                CHECK(sx_jpeg_decode_into(prog,length,&options,a+1,count,&partial)<0 && metadata_equal(&partial,&zero),
                      "every truncated progressive prefix refused");
                CHECK(a[0]==0xdeadbeef && a[count+1]==0xdeadbeef && allocations-before==releases-freed,
                      "truncated progressive guards and release");
            }
            fail_allocation=1; for(unsigned i=0;i<count+2;++i) a[i]=0xdeadbeef;
            CHECK(sx_jpeg_decode_into(prog,prog_size,&options,a+1,count,&pi)==SX_IMAGE_ERROR_DECODE &&
                  metadata_equal(&pi,&zero),"progressive allocation failure");
            for(unsigned i=0;i<count+2;++i) CHECK(a[i]==0xdeadbeef,"progressive allocation failure precedes writes");
            fail_allocation=0;
            CHECK(sx_jpeg_decode_into(prog,prog_size,&options,a+1,count-1,&pi)==SX_IMAGE_ERROR_BUFFER,"progressive short buffer");
        }
    }
}

static void progressive_fixtures(void)
{
#define X(prog,seq) progressive_compare(#prog,prog,sizeof(prog),seq,sizeof(seq));
    PROGRESSIVE_FIXTURES(X)
#undef X
    progressive_compare("large",jpeg_large_progressive,sizeof(jpeg_large_progressive),
                        jpeg_large_sequential,sizeof(jpeg_large_sequential));
    // Independent reference: libjpeg-turbo's own decode of the progressive file.
    static uint32_t pixels[64*48]; struct sx_image_metadata info={0};
    CHECK(sx_jpeg_decode_into(jpeg_large_progressive,sizeof(jpeg_large_progressive),NULL,pixels,64*48,&info)==0 &&
          info.width==64 && info.height==48,"large progressive decoded");
    unsigned max_error=0;
    for(unsigned i=0;i<64*48;++i) for(unsigned c=0;c<3;++c) {
        unsigned delta=(unsigned)abs((int)((pixels[i]>>(16-8*c))&255)-expected_large_progressive[3*i+c]);
        if(delta>max_error) max_error=delta;
    }
    printf("progressive large: maximum channel difference %u\n",max_error);
    CHECK(max_error<=3,"progressive reference tolerance is three RGB levels");
    // Workspace: 24 MCU-padded blocks of int16_t coefficients, then 503 plane bytes.
    CHECK(sx_jpeg_decode_into(jpeg_progressive_color_y420,sizeof(jpeg_progressive_color_y420),NULL,pixels,17*19,NULL)==0 &&
          last_allocation_size==24u*64u*2u+503u,"progressive workspace size");
    // Mutations of a real progressive stream stay bounded and clean.
    const unsigned char *original=jpeg_progressive_deep_color_y420; size_t size=sizeof(jpeg_progressive_deep_color_y420);
    uint32_t seed=0x7a11;
    for(unsigned i=0;i<3000;++i) {
        static uint32_t out[17*19+2];
        memcpy(fixture,original,size); seed=seed*1664525u+1013904223u;
        size_t at=seed%size; seed=seed*1664525u+1013904223u; fixture[at]^=(unsigned char)(1u<<(seed%8));
        out[0]=out[17*19+1]=0xdeadbeef;
        unsigned before=allocations, freed=releases;
        int result=sx_image_decode_into(fixture,size,NULL,out+1,17*19,&info);
        CHECK(out[0]==0xdeadbeef && out[17*19+1]==0xdeadbeef && allocations-before==releases-freed,"progressive mutation guards");
        if(!result) CHECK(info.width*info.height<=17*19,"progressive mutation dimensions bounded");
    }
}

int main(void) {
    stages(); reduced_stages(); fixtures(); color_fixtures(); color_scans(); color_malformed(); malformed();
    reduced_fixtures(); scale_plans(); reduced_errors();
    progressive_stages(); progressive_synthetic(); progressive_fixtures();
    if(failures) { printf("SXJPEG TEST FAIL %d\n",failures); return 1; }
    puts("SXJPEG TEST PASS"); return 0;
}
