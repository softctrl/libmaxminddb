#include "GeoIP2.h"
#include <sys/stat.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <netdb.h>


static geoipv6_t IPV6_NULL;

static int __GEOIP_V6_IS_NULL(geoipv6_t v6) {
        int i;
        for (i=0;i<16;i++) {
                if (v6.s6_addr[i])
                        return 0;
        }
        return 1;
}

geoipv6_t
GeoIP2_lookupaddress_v6(const char *host)
{
    geoipv6_t       ipnum;
    int             gaierr;
    struct addrinfo hints, *aifirst;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;

    if ((gaierr = getaddrinfo(host, NULL, &hints, &aifirst)) != 0) {
        return IPV6_NULL;
    }
    memcpy(ipnum.s6_addr, ((struct sockaddr_in6 *) aifirst->ai_addr)->sin6_addr.s6_addr, sizeof(geoipv6_t));
    freeaddrinfo(aifirst);

    return ipnum;
}

U32
GeoIP2_lookupaddress(const char *host)
{
    U32       ipnum;
    int             gaierr;
    struct addrinfo hints, *aifirst;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((gaierr = getaddrinfo(host, NULL, &hints, &aifirst)) != 0) {
        return 0;
    }
    ipnum = ((struct sockaddr_in *) aifirst->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(aifirst);

    return htonl(ipnum);
}

static U32 _get_uint32( const U8 * p){
  return (p[0] * 16777216UL +  p[1] * 65536 + p[2] * 256 + p[3]);
}

static U32 _get_uint24(const U8 * p){
  return (p[0] * 65536UL + p[1] * 256 + p[2]);
}

static U32 _get_uint16(const U8 * p){
  return (p[0] * 256UL + p[1]);
}

static U32 _get_uintX(const U8 * p, int length){
  U32 r = 0;
  while ( length-- > 0 ){
    r <<= 8;
    r += *p++;
  }
  return r;
}

static double _get_double( const U8 * ptr, int length){
    char fmt[256];
    double d;
    sprintf(fmt, "%%%dlf", length);
    sscanf(ptr,fmt, &d);
    return (d);
}

static int
_read(int fd, U8 * buffer, ssize_t to_read, off_t offset)
{
  while (to_read > 0) {
    ssize_t         have_read = pread(fd, buffer, to_read, offset);
    if (have_read <= 0)
      return GEOIP2_IOERROR;
    to_read -= have_read;
    if (to_read == 0)
      break;
    offset += have_read;
    buffer += have_read;
  }
  return GEOIP2_SUCCESS;
}

static int
_fddecode_key(struct GeoIP2 * gi, int offset, struct GeoIP2_Decode_Key * ret_key)
{
  const int       segments = gi->segments * gi->recbits * 2 / 8;;
  U8              ctrl;
  int             type;
  U8              b[4];
  int             fd = gi->fd;
  if (_read(fd, &ctrl, 1, segments + offset++) != GEOIP2_SUCCESS)
    return GEOIP2_IOERROR;
  type = (ctrl >> 5) & 7;
  if (type == GEOIP2_DTYPE_EXT) {
    if (_read(fd, &b[0], 1, segments + offset++) != GEOIP2_SUCCESS)
      return GEOIP2_IOERROR;
    type = 8 + b[0];
  }

  if (type == GEOIP2_DTYPE_PTR) {
    int             psize = (ctrl >> 3) & 3;
    int             new_offset;
    if (_read(fd, &b[0], psize + 1, segments + offset) != GEOIP2_SUCCESS)
      return GEOIP2_IOERROR;
    switch (psize) {
    case 0:
      new_offset = (ctrl & 7) * 256 + b[0];
      break;
    case 1:
      new_offset = 2048 + (ctrl & 7) * 65536 + b[0] * 256 + b[1];
      break;
    case 2:
      new_offset = 2048 + 524288 + (ctrl & 7) * 16777216 + _get_uint24(b);
      break;
    case 3:
    default:
      new_offset = _get_uint32(b);
      break;
    }
    if (_fddecode_key(gi, new_offset, ret_key) != GEOIP2_SUCCESS)
      return GEOIP2_IOERROR;
    ret_key->new_offset = offset + psize + 1;
    return GEOIP2_SUCCESS;
  }

  int             size = ctrl & 31;
  switch (size) {
  case 29:
    if (_read(fd, &b[0], 1, segments + offset++) != GEOIP2_SUCCESS)
      return GEOIP2_IOERROR;
    size = 29 + b[0];
    break;
  case 30:
    if (_read(fd, &b[0], 2, segments + offset) != GEOIP2_SUCCESS)
      return GEOIP2_IOERROR;
    size = 285 + b[0] * 256 + b[1];
    offset += 2;
    break;
  case 31:
    if (_read(fd, &b[0], 3, segments + offset) != GEOIP2_SUCCESS)
      return GEOIP2_IOERROR;
    size = 65821 + _get_uint24(b);
    offset += 3;
  default:
    break;
  }

  if (size == 0) {
    ret_key->ptr = NULL;
    ret_key->size = 0;
    ret_key->new_offset = offset;
    return GEOIP2_SUCCESS;
  }

  ret_key->ptr = (void *) 0 + segments + offset;
  ret_key->size = size;
  ret_key->new_offset = offset + size;
  return GEOIP2_SUCCESS;
}

#define GEOIP_CHKBIT_V6(bit,ptr) ((ptr)[((127UL - (bit)) >> 3)] & (1UL << (~(127UL - (bit)) & 7)))

static int
_GeoIP2_inet_pton(int af, const char *src, void *dst)
{
  return inet_pton(af, src, dst);
}
static const char *
_GeoIP2_inet_ntop(int af, const void *src, char *dst, socklen_t cnt)
{
  return inet_ntop(af, src, dst, cnt);
}

static          geoipv6_t
_addr_to_num_v6(const U8 *addr)
{
  geoipv6_t       ipnum;
  if (1 == _GeoIP2_inet_pton(AF_INET6, (char*)addr, &ipnum.s6_addr[0]))
    return ipnum;
  return IPV6_NULL;
}

void
GeoIP2_free_all(struct GeoIP2 * gi)
{
  if (gi) {
    if (gi->fd >= 0)
      close(gi->fd);
    if (gi->file_in_mem_ptr)
      free((void *) gi->file_in_mem_ptr);
    free((void *) gi);
  }
}

static int
_fdlookup_by_ipnum(struct GeoIP2 * gi, U32 ipnum, struct GeoIP2_Lookup * result)
{
  int             segments = gi->segments;
  off_t             offset = 0;
  int             byte_offset;
  int             rl = gi->recbits * 2 / 8;
  int             fd = gi->fd;
  U32             mask = 0x80000000UL;
  int             depth;
  U8              b[4];


  if (rl == 6) {
    for (depth = 32 - 1; depth >= 0; depth--, mask >>= 1) {
      if (_read(fd, &b[0], 3, offset * rl + ((ipnum & mask) ? 3 : 0)) != GEOIP2_SUCCESS)
	return GEOIP2_IOERROR;
      offset = _get_uint24(b);
      if (offset >= segments) {
	result->netmask = 32 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  else if (rl == 7) {
    for (depth = 32 - 1; depth >= 0; depth--, mask >>= 1) {
      byte_offset = offset * rl;
      if (ipnum & mask) {
	if (_read(fd, &b[0], 4, byte_offset + 3) != GEOIP2_SUCCESS)
	  return GEOIP2_IOERROR;
	offset = _get_uint32(b);
	offset &= 0xfffffff;
      }
      else {
	if (_read(fd, &b[0], 4, byte_offset) != GEOIP2_SUCCESS)
	  return GEOIP2_IOERROR;
	offset = b[0] * 65536 + b[1] * 256 + b[2] + ((b[3] & 0xf0) << 20);
      }
      if (offset >= segments) {
	result->netmask = 32 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  else if (rl == 8) {
    for (depth = 32 - 1; depth >= 0; depth--, mask >>= 1) {
      if (_read(fd, &b[0], 4, offset * rl + ((ipnum & mask) ? 4 : 0)) != GEOIP2_SUCCESS)
	return GEOIP2_IOERROR;
      offset = _get_uint32(b);
      if (offset >= segments) {
	result->netmask = 32 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  //uhhh should never happen !
    return GEOIP2_CORRUPTDATABASE;
}


static int
_fdlookup_by_ipnum_v6(struct GeoIP2 * gi, geoipv6_t ipnum, struct GeoIP2_Lookup * result)
{
  int             segments = gi->segments;
  int             offset = 0;
  int             byte_offset;
  int             rl = gi->recbits * 2 / 8;
  int             fd = gi->fd;
  int             depth;
  U8              b[4];
  if (rl == 6) {

    for (depth = gi->depth - 1; depth >= 0; depth--) {
      byte_offset = offset * rl;
      if (GEOIP_CHKBIT_V6(depth, (U8 *) & ipnum))
	byte_offset += 3;
      if ( _read(fd, &b[0], 3, byte_offset) != GEOIP2_SUCCESS )
	return GEOIP2_IOERROR;
      offset = _get_uint24(b);
      if (offset >= segments) {
	result->netmask = 128 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  else if (rl == 7) {
    for (depth = gi->depth - 1; depth >= 0; depth--) {
      byte_offset = offset * rl;
      if (GEOIP_CHKBIT_V6(depth, (U8 *) & ipnum)) {
	byte_offset += 3;
        if ( _read(fd, &b[0], 4, byte_offset) != GEOIP2_SUCCESS )
	  return GEOIP2_IOERROR;
	offset = _get_uint32(b);
	offset &= 0xfffffff;
      }
      else {

        if ( _read(fd, &b[0], 4, byte_offset) != GEOIP2_SUCCESS )
	  return GEOIP2_IOERROR;
	offset = b[0] * 65536 + b[1] * 256 + b[2] + ((b[3] & 0xf0) << 20);
      }
      if (offset >= segments) {
	result->netmask = 128 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  else if (rl == 8) {
    for (depth = gi->depth - 1; depth >= 0; depth--) {
      byte_offset = offset * rl;
      if (GEOIP_CHKBIT_V6(depth, (U8 *) & ipnum))
	byte_offset += 4;
      if ( _read(fd, &b[0], 4, byte_offset) != GEOIP2_SUCCESS )
        return GEOIP2_IOERROR;
      offset = _get_uint32(b);
      if (offset >= segments) {
	result->netmask = 128 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  //uhhh should never happen !
    return GEOIP2_CORRUPTDATABASE;
}

static int
_lookup_by_ipnum_v6(struct GeoIP2 * gi, geoipv6_t ipnum,  struct GeoIP2_Lookup * result)
{
  int             segments = gi->segments;
  int             offset = 0;
  int             rl = gi->recbits * 2 / 8;
  const U8       *mem = gi->file_in_mem_ptr;
  const U8       *p;
  int             depth;
  if (rl == 6) {

    for (depth = gi->depth - 1; depth >= 0; depth--) {
      p = &mem[offset * rl];
      if (GEOIP_CHKBIT_V6(depth, (U8 *) & ipnum))
	p += 3;
      offset = _get_uint24(p);
      if (offset >= segments) {
        result->netmask = 128 - depth;
        result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  else if (rl == 7) {
    for (depth = gi->depth - 1; depth >= 0; depth--) {
      p = &mem[offset * rl];
      if (GEOIP_CHKBIT_V6(depth, (U8 *) & ipnum)) {
	p += 3;
	offset = _get_uint32(p);
	offset &= 0xfffffff;
      }
      else {

	offset = p[0] * 65536 + p[1] * 256 + p[2] + ((p[3] & 0xf0) << 20);
      }
      if (offset >= segments) {
	result->netmask = 128 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  else if (rl == 8) {
    for (depth = gi->depth - 1; depth >= 0; depth--) {
      p = &mem[offset * rl];
      if (GEOIP_CHKBIT_V6(depth, (U8 *) & ipnum))
	p += 4;
        offset = _get_uint32(p);
      if (offset >= segments) {
        result->netmask = 128 - depth;
	result->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
    }
  }
  //uhhh should never happen !
    return GEOIP2_CORRUPTDATABASE;
}

static int
_lookup_by_ipnum(struct GeoIP2 * gi, U32 ipnum, struct GeoIP2_Lookup * res)
{
  int             segments = gi->segments;
  int             offset = 0;
  int             rl = gi->recbits * 2 / 8;
  const U8       *mem = gi->file_in_mem_ptr;
  const U8       *p;
  U32             mask = 0x80000000UL;
  int             depth;
  if (rl == 6) {
    for (depth = 32 - 1; depth >= 0; depth--) {
      p = &mem[offset * rl];
      if (ipnum & mask)
	p += 3;
      offset = _get_uint24(p);
      if (offset >= segments) {
	res->netmask = 32 - depth;
	res->ptr = offset - segments;
	return GEOIP2_SUCCESS;
      }
      mask >>= 1;
    }
  }
  else if (rl == 7) {
    for (depth = 32 - 1; depth >= 0; depth--) {
      p = &mem[offset * rl];
      if (ipnum & mask) {
	p += 3;
	offset = _get_uint32(p);
	offset &= 0xfffffff;
      }
      else {
	offset = p[0] * 65536 + p[1] * 256 + p[2] + ((p[3] & 0xf0) << 20);
      }
      if (offset >= segments) {
	res->netmask = 32 - depth;
	res->ptr =  offset - segments;
        return GEOIP2_SUCCESS;
      }
      mask >>= 1;
    }
  }
  else if (rl == 8) {
    for (depth = 32 - 1; depth >= 0; depth--) {
      p = &mem[offset * rl];
      if (ipnum & mask)
	p += 4;
      offset = _get_uint32(p);
      if (offset >= segments) {
	res->netmask = 32 - depth;
	res->ptr =  offset - segments;
	return GEOIP2_SUCCESS;
      }
      mask >>= 1;
    }
  }
  //uhhh should never happen !
    return GEOIP2_CORRUPTDATABASE;
}

static          U32
_addr_to_num(const U8 * addr)
{
  U32             c, octet, t;
  U32             ipnum;
  int             i = 3;
  octet = ipnum = 0;
  while ((c = *addr++)) {
    if (c == '.') {
      if (octet > 255)
	return 0;
      ipnum <<= 8;
      ipnum += octet;
      i--;
      octet = 0;
    }
    else {
      t = octet;
      octet <<= 3;
      octet += t;
      octet += t;
      c -= '0';
      if (c > 9)
	return 0;
      octet += c;
    }
  }
  if ((octet > 255) || (i != 0))
    return 0;
  ipnum <<= 8;
  return ipnum + octet;
}

static int
_lookup_by_addr(struct GeoIP2 * gi, const U8 * addr, struct GeoIP2_Lookup * result)
{
  return _lookup_by_ipnum(gi, _addr_to_num(addr), result);
}

static int
_fdlookup_by_addr(struct GeoIP2 * gi, const U8 * addr, struct GeoIP2_Lookup * result)
{
  return _fdlookup_by_ipnum(gi, _addr_to_num(addr), result);
}
static int
_lookup_by_addr_v6(struct GeoIP2 * gi, const U8 * addr, struct GeoIP2_Lookup * result)
{
  return _lookup_by_ipnum_v6(gi, _addr_to_num_v6(addr), result);
}

static int
_fdlookup_by_addr_v6(struct GeoIP2 * gi, const U8 * addr, struct GeoIP2_Lookup * result)
{
  return _fdlookup_by_ipnum_v6(gi, _addr_to_num_v6(addr), result);
}

static void
_decode_key(struct GeoIP2 * gi, int offset, struct GeoIP2_Decode_Key * ret_key)
{
  //int           segments = gi->segments;
  const int       segments = 0;
  const U8       *mem = gi->dataptr;
  U8              ctrl, type;
  ctrl = mem[segments + offset++];
  type = (ctrl >> 5) & 7;
  if (type == GEOIP2_DTYPE_EXT) {
    type = 8 + mem[segments + offset++];
  }

  if (type == GEOIP2_DTYPE_PTR) {
    int             psize = (ctrl >> 3) & 3;
    int             new_offset;
    switch (psize) {
    case 0:
      new_offset = (ctrl & 7) * 256 + mem[segments + offset];
      break;
    case 1:
      new_offset = 2048 + (ctrl & 7) * 65536 + _get_uint16(&mem[segments + offset]);
      break;
    case 2:
      new_offset = 2048 + 524288 + (ctrl & 7) * 16777216 + _get_uint24(&mem[segments + offset]);
      break;
    case 3:
    default:
      new_offset = _get_uint32(&mem[segments + offset]);
      break;
    }
    _decode_key(gi, new_offset, ret_key);
    ret_key->new_offset = offset + psize + 1;
    return;
  }

  int             size = ctrl & 31;
  switch (size) {
  case 29:
    size = 29 + mem[segments + offset++];
    break;
  case 30:
    size = 285 + _get_uint16(&mem[segments + offset]);
    offset += 2;
    break;
  case 31:
    size = 65821 +_get_uint24(&mem[segments + offset]);
    offset += 3;
  default:
    break;
  }


  if (size == 0) {
    ret_key->ptr = "";
    ret_key->size = 0;
    ret_key->new_offset = offset;
    return;
  }

  ret_key->ptr = &mem[segments + offset];
  ret_key->size = size;
  ret_key->new_offset = offset + size;
  return;
}


static int
_init(GEOIP2_T * gi, char *fname, U32 flags)
{
  struct stat     s;
  int             fd;
  U8             *ptr;
  ssize_t         iread;
  ssize_t         size;
  off_t         offset;
  gi->fd = fd = open(fname, O_RDONLY);
  if (fd < 0)
    return GEOIP2_OPENFILEERROR;
  fstat(fd, &s);
  gi->flags = flags;
  if ((flags & GEOIP2_MODE_MASK) == GEOIP2_MODE_MEMORY_CACHE) {
    gi->fd = -1;
    size = s.st_size;
    offset = 0;
  }
  else {
    gi->fd = fd;
    size = s.st_size < 2000 ? s.st_size : 2000;
    offset = s.st_size - size;
  }
  ptr = malloc(size);
  if (ptr == NULL)
    return GEOIP2_INVALIDDATABASE;

  iread = pread(fd, ptr, size, offset);

  const U8       *p = memmem(ptr, size, "\xab\xcd\xefMaxMind.com", 14);
  if (p == NULL) {
    free(ptr);
    return GEOIP2_INVALIDDATABASE;
  }
  p += 14;
  gi->file_format = p[0] * 256 + p[1];
  gi->recbits = p[2];
  gi->depth = p[3];
  gi->database_type = p[4] * 256 + p[5];
  gi->minor_database_type = p[6] * 256 + p[7];
  gi->segments = p[8] * 16777216 + p[9] * 65536 + p[10] * 256 + p[11];


  if ((flags & GEOIP2_MODE_MASK) == GEOIP2_MODE_MEMORY_CACHE) {
    gi->file_in_mem_ptr = ptr;
    gi->dataptr = gi->file_in_mem_ptr + gi->segments * gi->recbits * 2 / 8;

    close(fd);
  }
  else {
    gi->dataptr = (const U8 *) 0 + (gi->segments * gi->recbits * 2 / 8);
    free(ptr);
  }
  return GEOIP2_SUCCESS;
}



GEOIP2_T       *
GeoIP2_open(char *fname, U32 flags)
{
  GEOIP2_T       *gi = calloc(1, sizeof(GEOIP2_T));
  if (GEOIP2_SUCCESS != _init(gi, fname, flags)) {
    GeoIP2_free_all(gi);
    return NULL;
  }
  return gi;
}

