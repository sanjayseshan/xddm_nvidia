/* NVIDIA OpenRM 535 message_queue_priv.h / rpc-message-header ABI. */
#include "nvx_rpc.h"
_Static_assert(sizeof(NVX_SYSTEM_INFO_535)==664,"535 system info ABI");
_Static_assert(offsetof(NVX_SYSTEM_INFO_535,acpi)==120,"535 ACPI offset");
_Static_assert(offsetof(NVX_SYSTEM_INFO_535,timerOffset)==616,"535 timer offset");
static uint32_t get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static uint32_t checksum(const uint8_t *p,size_t size)
{
    uint32_t sum=0;size_t i;
    for(i=0;i<size;i+=4) sum^=get32(p+i);
    return sum;
}
int NvxEncodeRpc(uint8_t *slot,size_t capacity,uint32_t function,
                 uint32_t sequence,const void *payload,size_t payloadSize)
{
    uint32_t elements=0;
    if(payloadSize>4016) return 0;
    return NvxEncodeRpcRecord(slot,capacity,function,sequence,payload,payloadSize,&elements)&&elements==1;
}
int NvxEncodeRpcRecord(uint8_t *record,size_t capacity,uint32_t function,
                       uint32_t sequence,const void *payload,size_t payloadSize,
                       uint32_t *elements)
{
    size_t i,recordBytes;
    uint32_t count;
    if(!record||!elements||(payloadSize&&!payload)||payloadSize>NVX_RPC_MAX_ELEMENTS*NVX_RPC_SLOT_SIZE-NVX_RPC_HEADER_SIZE)
        return 0;
    count=(uint32_t)((payloadSize+NVX_RPC_HEADER_SIZE+NVX_RPC_SLOT_SIZE-1)/NVX_RPC_SLOT_SIZE);
    if(!count||count>NVX_RPC_MAX_ELEMENTS) return 0;
    recordBytes=(size_t)count*NVX_RPC_SLOT_SIZE;
    if(capacity<recordBytes) return 0;
    for(i=0;i<recordBytes;i++) record[i]=0;
    put32(record+36,sequence);put32(record+40,count);
    put32(record+48,0x03000000);put32(record+52,0x43505256);
    put32(record+56,(uint32_t)(32+payloadSize));put32(record+60,function);
    put32(record+64,0xffffffffU);put32(record+68,0xffffffffU);put32(record+72,sequence);
    for(i=0;i<payloadSize;i++) record[80+i]=((const uint8_t *)payload)[i];
    put32(record+32,checksum(record,recordBytes));
    *elements=count;
    return 1;
}
int NvxDecodeRpc(const uint8_t *slot,size_t capacity,uint32_t *function,
                 uint32_t *result,uint32_t *sequence)
{
    uint32_t elements;
    return NvxDecodeRpcRecord(slot,capacity,function,result,sequence,&elements);
}
int NvxDecodeRpcRecord(const uint8_t *slot,size_t capacity,uint32_t *function,
                 uint32_t *result,uint32_t *sequence,uint32_t *elements)
{
    uint32_t length,count;size_t bytes;
    if(!slot||capacity<4096||!function||!result||!sequence) return 0;
    if(!elements) return 0;
    count=get32(slot+40);
    if(!count||count>NVX_RPC_MAX_ELEMENTS) return 0;
    bytes=(size_t)count*4096;
    if(bytes>capacity) return 0;
    length=get32(slot+56);
    if(get32(slot+48)!=0x03000000||get32(slot+52)!=0x43505256||
       length<32||length>bytes-48||length+48<=bytes-4096||checksum(slot,bytes)) return 0;
    *elements=count;
    *function=get32(slot+60);*result=get32(slot+64);*sequence=get32(slot+36);return 1;
}
int NvxGatherRpc(const uint8_t *ring,size_t ringBytes,uint32_t slots,
                 uint32_t read,uint32_t write,uint8_t *out,size_t capacity,
                 uint32_t *elements)
{
    uint32_t count,available;size_t i,j;
    if(!ring||!out||!elements||slots<2||slots>63||read>=slots||write>=slots||
       ringBytes<(size_t)slots*4096) return -1;
    *elements=0;
    available=(write+slots-read)%slots;
    if(!available) return 0;
    count=get32(ring+(size_t)read*4096+40);
    if(!count||count>16||count>=slots||capacity<(size_t)count*4096) return -1;
    if(count>available) return 0;
    for(i=0;i<count;i++) for(j=0;j<4096;j++)
        out[i*4096+j]=ring[((read+i)%slots)*4096+j];
    *elements=count;return 1;
}
