/* Stage-level checks use the actual private implementation, as the MIDI
 * operator tests do; no testing-only functions enter the public SDK. */
#include <stdio.h>
#include <math.h>
#include "../../subsystems/posix/sdk/v1/runtime/sximg.c"
#include "sxjpeg_fixtures.h"

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
    CHECK(!jpeg_huffman_build(&h,counts,bad+1,1,1),"invalid AC zero-size symbol refused");
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
    uint32_t pixels[258]; for(unsigned i=0;i<258;++i) pixels[i]=0xdeadbeef;
    struct sx_image_metadata info={9,9,9,9,9,9}, zero={0};
    int result=sx_jpeg_info(fixture,used,NULL,&info);
    CHECK(result==(header_error?expected:SX_IMAGE_OK),"specific info result");
    if(header_error) CHECK(metadata_equal(&info,&zero),"info cleared on error");
    CHECK(sx_jpeg_decode_into(fixture,used,NULL,pixels+1,256,&info)==expected && metadata_equal(&info,&zero),"specific failure and cleared decode metadata");
    CHECK(pixels[0]==0xdeadbeef && pixels[257]==0xdeadbeef,"error destination guards");
    CHECK(sx_image_info(fixture,used,NULL,&info)==(header_error?expected:SX_IMAGE_OK),"common info forwards JPEG error");
    CHECK(sx_image_decode_into(fixture,used,NULL,pixels+1,256,&info)==expected && metadata_equal(&info,&zero),"common decode forwards JPEG error");
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
            CHECK(sx_jpeg_info(data,size,&exact,&decoded)==SX_IMAGE_ERROR_SCALE &&
                  sx_image_info(data,size,&exact,&decoded)==SX_IMAGE_ERROR_SCALE,"reduction explicitly remains pending"); }
        exact=(struct sx_image_options){w-1,h,w*h,0};
        CHECK(sx_jpeg_decode_into(data,size,&exact,common,w*h,&decoded)==SX_IMAGE_ERROR_LIMIT,"auto currently refuses oversized output");
    }
}

static void malformed(void) {
    minimal(8,8,0); CHECK(sx_jpeg_info(NULL,0,NULL,NULL)==SX_IMAGE_ERROR_ARGUMENT,"null info arguments");
    struct sx_image_metadata info={0}; uint32_t pixels[4098];
    CHECK(sx_jpeg_info(NULL,0,NULL,&info)==SX_IMAGE_ERROR_ARGUMENT && sx_jpeg_decode_into(fixture,used,NULL,NULL,64,&info)==SX_IMAGE_ERROR_ARGUMENT,"null data and pixels");
    struct sx_image_options options={0,0,0,3};
    CHECK(sx_jpeg_info(fixture,used,&options,&info)==SX_IMAGE_ERROR_ARGUMENT,"invalid denominator");
    options=(struct sx_image_options){0,0,63,0};
    CHECK(sx_jpeg_info(fixture,used,&options,&info)==SX_IMAGE_ERROR_LIMIT,"pixel budget");
    options=(struct sx_image_options){0,7,0,0};
    CHECK(sx_jpeg_info(fixture,used,&options,&info)==SX_IMAGE_ERROR_LIMIT,"height limit");
    const struct {unsigned marker; int error;} unsupported[]={
        {0xc2,SX_IMAGE_ERROR_JPEG_PROGRESSIVE},{0xc6,SX_IMAGE_ERROR_JPEG_PROGRESSIVE},
        {0xc9,SX_IMAGE_ERROR_JPEG_ARITHMETIC},{0xca,SX_IMAGE_ERROR_JPEG_ARITHMETIC},
        {0xc3,SX_IMAGE_ERROR_JPEG_LOSSLESS},{0xc7,SX_IMAGE_ERROR_JPEG_LOSSLESS}
    };
    for(unsigned i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
        minimal(8,8,0); fixture[find_marker(0xc0)+1]=(unsigned char)unsupported[i].marker;
        error_expected(unsupported[i].error,1);
    }
    minimal(8,8,0); fixture[find_marker(0xc0)+4]=12; error_expected(SX_IMAGE_ERROR_JPEG_PRECISION,1);
    // Actual three- and four-component frames, not merely a corrupted length.
    for(unsigned components=3;components<=4;++components) {
        minimal(8,8,0); size_t at=find_marker(0xc0), tail=at+13;
        memmove(fixture+tail+(components-1)*3,fixture+tail,used-tail); used+=(components-1)*3;
        fixture[at+3]=(unsigned char)(8+3*components); fixture[at+9]=(unsigned char)components;
        for(unsigned c=1;c<components;++c) { fixture[at+10+3*c]=(unsigned char)c; fixture[at+11+3*c]=0x11; fixture[at+12+3*c]=0; }
        error_expected(SX_IMAGE_ERROR_JPEG_COLOR,1); // RGB/YCbCr and CMYK/YCCK not yet supported
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
        int result=sx_image_decode_into(fixture,size,NULL,pixels+1,256,&info);
        CHECK(pixels[0]==0xdeadbeef && pixels[257]==0xdeadbeef,"mutated input output guards");
        struct sx_image_metadata zero={0};
        if(result) CHECK(metadata_equal(&info,&zero),"mutated failure metadata cleared");
        else CHECK(info.width*info.height<=256 && info.scale_denom==1,"mutated success dimensions bounded");
    }
    CHECK(!strcmp(sx_image_error_string(SX_IMAGE_ERROR_JPEG_PROGRESSIVE),"Progressive JPEG is unsupported"),"progressive diagnostic");
    CHECK(!strcmp(sx_image_error_string(SX_IMAGE_ERROR_JPEG_ARITHMETIC),"Arithmetic JPEG is unsupported"),"arithmetic diagnostic");
    CHECK(!strcmp(sx_image_error_string(SX_IMAGE_ERROR_JPEG_LOSSLESS),"Lossless JPEG is unsupported"),"lossless diagnostic");
}

int main(void) {
    stages(); fixtures(); malformed();
    if(failures) { printf("SXJPEG TEST FAIL %d\n",failures); return 1; }
    puts("SXJPEG TEST PASS"); return 0;
}
