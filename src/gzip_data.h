#ifndef HV_GZIP_DATA_H
#define HV_GZIP_DATA_H
#include "../third_party/puff/puff.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint32_t hd_u32(const unsigned char *p) {
  return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint32_t hd_crc(const unsigned char *p,size_t n) {
  uint32_t crc=~0u;
  for(size_t i=0;i<n;i++){crc^=p[i];for(unsigned k=0;k<8;k++)crc=(crc>>1)^(0xedb88320u&-(crc&1));}
  return ~crc;
}
/* One bounded gzip member; reject truncation, CRC/size mismatch and trailers.
 * No file names in the header are used as paths. Never executes decompressed data. */
static unsigned char *hd_gunzip(const unsigned char *p,size_t size,size_t *length) {
  if(size<18||p[0]!=31||p[1]!=139||p[2]!=8||(p[3]&0xe0))return NULL;
  unsigned flags=p[3];size_t at=10;
  if(flags&4){if(at+2>size-8)return NULL;size_t n=p[at]|(unsigned)p[at+1]<<8;at+=2;if(n>size-8-at)return NULL;at+=n;}
  for(unsigned bit=8;bit<=16;bit*=2)if(flags&bit){while(at<size-8&&p[at])at++;if(at>=size-8)return NULL;at++;}
  if(flags&2){if(at+2>size-8||(hd_crc(p,at)&65535)!=(p[at]|(unsigned)p[at+1]<<8))return NULL;at+=2;}
  if(at>=size-8)return NULL;
  unsigned long output=hd_u32(p+size-4),input=(unsigned long)(size-8-at);
  if(!output||output>64u*1024u*1024u)return NULL;
  unsigned char *raw=malloc((size_t)output+1);if(!raw)return NULL;
  unsigned long expected=output;
  if(puff(raw,&output,p+at,&input)||input!=size-8-at||output!=expected||hd_crc(raw,output)!=hd_u32(p+size-8)){free(raw);return NULL;}
  raw[output]=0;*length=output;return raw;
}
#endif
