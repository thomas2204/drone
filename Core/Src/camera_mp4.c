#include "camera_mp4.h"
#include <string.h>

static void BE(uint8_t *p, uint32_t n) { p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n; }
static uint32_t Hash(uint32_t h, const uint8_t *p, uint32_t n) { while(n--){h^=*p++;h*=16777619U;}return h; }
static bool Put(CameraMP4 *m, const void *p, uint32_t n)
{
    if(m->failed || !m->write(m->context,p,n)){m->failed=true;return false;}
    m->position+=n;return true;
}

bool CameraMP4_Begin(CameraMP4 *m, CameraAVIWrite write, CameraAVISeek seek, void *context)
{
    if(!m || !write || !seek)return false;
    memset(m,0,sizeof(*m));m->write=write;m->seek=seek;m->context=context;
    uint8_t ftyp[32]={0};BE(ftyp,32);memcpy(ftyp+4,"ftypisom",8);BE(ftyp+12,0x200);
    memcpy(ftyp+16,"isomiso2avc1mp41",16);
    if(!Put(m,ftyp,sizeof(ftyp)))return false;
    m->mdat_offset=m->position;
    uint8_t mdat[8]={0};BE(mdat,8);memcpy(mdat+4,"mdat",4);
    return Put(m,mdat,8);
}

static uint32_t StartCode(const uint8_t *p, uint32_t bytes, uint32_t from, uint32_t *length)
{
    for(uint32_t i=from;i+3U<=bytes;++i){
        if(p[i]==0 && p[i+1]==0){
            if(p[i+2]==1){*length=3;return i;}
            if(i+4U<=bytes && p[i+2]==0 && p[i+3]==1){*length=4;return i;}
        }
    }
    return bytes;
}

bool CameraMP4_Frame(CameraMP4 *m, const uint8_t *p, uint32_t bytes)
{
    if(m->failed || m->frames>=CAMERA_AVI_MAX_FRAMES || !p)return false;
    uint32_t at=0, prefix=0, sample_start=m->position, hash=2166136261U;
    bool idr=false;
    at=StartCode(p,bytes,0,&prefix);
    while(at<bytes){
        uint32_t begin=at+prefix, next_prefix=0;
        uint32_t end=StartCode(p,bytes,begin,&next_prefix);
        uint32_t n=end-begin;
        if(n){
            unsigned type=p[begin]&31U;
            if(type==7 || type==8){
                if(n>256U)return false;
                if(type==7){memcpy(m->sps,p+begin,n);m->sps_bytes=n;}
                else{memcpy(m->pps,p+begin,n);m->pps_bytes=n;}
            }else{
                uint8_t length[4];BE(length,n);
                if(!Put(m,length,4) || !Put(m,p+begin,n))return false;
                hash=Hash(hash,length,4);hash=Hash(hash,p+begin,n);
                if(type==5)idr=true;
            }
        }
        at=end;prefix=next_prefix;
    }
    if(!idr || !m->sps_bytes || !m->pps_bytes || m->position==sample_start)return false;
    unsigned i=m->frames++;
    m->offsets[i]=sample_start;m->sizes[i]=m->position-sample_start;m->hashes[i]=hash;
    return true;
}

/* Bounded metadata builder: no heap, at most 120 independently decodable frames. */
typedef struct {uint8_t data[4096];uint32_t n;bool failed;} Boxes;
static void Data(Boxes *b,const void *p,uint32_t n){if(n>sizeof(b->data)-b->n){b->failed=true;return;}memcpy(b->data+b->n,p,n);b->n+=n;}
static void U32(Boxes *b,uint32_t n){uint8_t p[4];BE(p,n);Data(b,p,4);}
static void U16(Boxes *b,uint16_t n){uint8_t p[2]={(uint8_t)(n>>8),(uint8_t)n};Data(b,p,2);}
static void Zero(Boxes *b,uint32_t n){while(n--) {const uint8_t z=0;Data(b,&z,1);}}
static uint32_t Box(Boxes *b,const char *name){uint32_t at=b->n;U32(b,0);Data(b,name,4);return at;}
static void End(Boxes *b,uint32_t at){if(!b->failed)BE(b->data+at,b->n-at);}
static void Matrix(Boxes *b){U32(b,0x10000);U32(b,0);U32(b,0);U32(b,0);U32(b,0x10000);U32(b,0);U32(b,0);U32(b,0);U32(b,0x40000000);}

bool CameraMP4_End(CameraMP4 *m,uint32_t duration_ms)
{
    if(m->failed || !m->frames || !duration_ms || m->sps_bytes<4)return false;
    uint32_t delta=(duration_ms+m->frames/2U)/m->frames;
    if(!delta)delta=1;
    uint32_t duration=delta*m->frames, end_media=m->position;
    Boxes b={0};uint32_t moov=Box(&b,"moov");
    uint32_t box=Box(&b,"mvhd");U32(&b,0);U32(&b,0);U32(&b,0);U32(&b,1000);U32(&b,duration);
    U32(&b,0x10000);U16(&b,0x100);Zero(&b,10);Matrix(&b);Zero(&b,24);U32(&b,2);End(&b,box);
    uint32_t trak=Box(&b,"trak");box=Box(&b,"tkhd");U32(&b,7);U32(&b,0);U32(&b,0);U32(&b,1);U32(&b,0);U32(&b,duration);
    Zero(&b,16);Matrix(&b);U32(&b,320U<<16);U32(&b,240U<<16);End(&b,box);
    uint32_t mdia=Box(&b,"mdia");box=Box(&b,"mdhd");U32(&b,0);U32(&b,0);U32(&b,0);U32(&b,1000);U32(&b,duration);U16(&b,0x55c4);U16(&b,0);End(&b,box);
    box=Box(&b,"hdlr");U32(&b,0);U32(&b,0);Data(&b,"vide",4);Zero(&b,12);Data(&b,"H743 Video",11);End(&b,box);
    uint32_t minf=Box(&b,"minf");box=Box(&b,"vmhd");U32(&b,1);Zero(&b,8);End(&b,box);
    uint32_t dinf=Box(&b,"dinf"),dref=Box(&b,"dref");U32(&b,0);U32(&b,1);box=Box(&b,"url ");U32(&b,1);End(&b,box);End(&b,dref);End(&b,dinf);
    uint32_t stbl=Box(&b,"stbl"),stsd=Box(&b,"stsd");U32(&b,0);U32(&b,1);
    uint32_t avc1=Box(&b,"avc1");Zero(&b,6);U16(&b,1);Zero(&b,16);U16(&b,320);U16(&b,240);U32(&b,0x480000);U32(&b,0x480000);U32(&b,0);U16(&b,1);Zero(&b,32);U16(&b,24);U16(&b,0xffff);
    box=Box(&b,"avcC");uint8_t config[6]={1,m->sps[1],m->sps[2],m->sps[3],0xff,0xe1};Data(&b,config,6);U16(&b,(uint16_t)m->sps_bytes);Data(&b,m->sps,m->sps_bytes);
    const uint8_t one=1;Data(&b,&one,1);U16(&b,(uint16_t)m->pps_bytes);Data(&b,m->pps,m->pps_bytes);End(&b,box);End(&b,avc1);End(&b,stsd);
    box=Box(&b,"stts");U32(&b,0);U32(&b,1);U32(&b,m->frames);U32(&b,delta);End(&b,box);
    box=Box(&b,"stsc");U32(&b,0);U32(&b,1);U32(&b,1);U32(&b,1);U32(&b,1);End(&b,box);
    box=Box(&b,"stsz");U32(&b,0);U32(&b,0);U32(&b,m->frames);for(unsigned i=0;i<m->frames;++i)U32(&b,m->sizes[i]);End(&b,box);
    box=Box(&b,"stco");U32(&b,0);U32(&b,m->frames);for(unsigned i=0;i<m->frames;++i)U32(&b,m->offsets[i]);End(&b,box);
    End(&b,stbl);End(&b,minf);End(&b,mdia);End(&b,trak);End(&b,moov);
    if(b.failed || !Put(m,b.data,b.n))return false;
    uint32_t file_bytes=m->position;uint8_t length[4];BE(length,end_media-m->mdat_offset);
    if(!m->seek(m->context,m->mdat_offset) || !m->write(m->context,length,4) || !m->seek(m->context,file_bytes)){m->failed=true;return false;}
    return true;
}
