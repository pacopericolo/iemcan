/*
 * socketreceive : receive socketCAN messages
 *
 * © 2018 IOhannes m zmölnig, forum::für::umläute
 *        institute of electronic music and acoustics (iem)
 *
 */

#include <m_pd.h>
#include <s_stuff.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------- canparse ------------------------------- */

static t_class *canparse_class=NULL;

typedef struct _canformat {
  struct _canformat *next;
  t_symbol*name;
  unsigned int start; /* start bit */
  unsigned int length; /* bit length */
  unsigned char is_signed;
  t_float offset;
  t_float factor;
  unsigned char is_bigendian;
} t_canformat;

typedef struct _canformatspecs {
  t_symbol *can_id;
  t_canformat *format;
  struct _canformatspecs *next;
} t_canformat_specs;

typedef struct _canparse {
  t_object x_obj;
  t_outlet *x_msgout;
  t_canformat_specs *x_formatspecs;
} t_canparse;

#if 0
static void print_hexbytes(const unsigned char*canbuf, unsigned int length) {
  unsigned int i;

  for(i=length; i>0; i--) {
    startpost("%02X", canbuf[i-1]);
  }
  endpost();
}

static void print_binbyte(unsigned char b) {
  int i=0;
  for(i=0; i<8; i++) {
    startpost("%d", (b>>(7-i))&0x1);
  }
}

static void print_binbytes(unsigned char*bytes, size_t len, const char*space) {
  while(len--) {
    print_binbyte(bytes[len]);
    if(len)startpost("%s", space);
  }
}
#endif

static void shift_left(unsigned int* array, size_t length, unsigned int num)
{
  size_t i;
  const size_t bitsperuint = 8 * sizeof(unsigned int);
  if(!length)return;
  for(; num>=bitsperuint; num-=bitsperuint) {
    for (i=(length-1); i>0; i--)
      array[i] = array[i-1];
    array[0] = 0;
  }
  if(num>0) {
    unsigned int invnum = 8*sizeof(unsigned int)-num;
    unsigned int mask = ((unsigned int)-1) >> invnum;
    for (i=(length-1); i > 0; i--)
    {
      array[i] = array[i] << num;
      if (i > 0)
      {
        unsigned int top_bit = (array[i-1] >> invnum) & mask;
        array[i] = array[i] | top_bit;
      }
    }
  }
}
static void shift_right(unsigned int* array, size_t length,  unsigned int num)
{
  size_t i;
  const size_t bitsperuint = 8 * sizeof(unsigned int);
  if(!length)return;
  for(; num>=bitsperuint; num-=bitsperuint) {
    for (i=0; i < length; i++)
      array[i] = array[i+1];
    array[i-1] = 0;
  }
  if(num>0) {
    unsigned int invnum = bitsperuint-num;
    unsigned int mask = ((unsigned int)-1) >> invnum;
    for (i=0; i < length; i++)
    {
      array[i] = array[i] >> num;
      if (i < (length-1))
      {
        unsigned int bottom_bit = (array[i+1] & mask) << invnum;
        array[i] = array[i] | bottom_bit;
      }
    }
  }
}
static void zeromask_left(unsigned int*array, size_t length, unsigned int numbits)
{
  const size_t bitsperuint = 8 * sizeof(unsigned int);
  size_t i;
  for(i=0; i<length; i++) {
    if(numbits>=bitsperuint) {
      numbits -= bitsperuint;
    } else if (numbits) {
      array[i] &= ((unsigned int)-1) >> (bitsperuint-numbits);
      numbits = 0;
    } else {
      array[i] = 0;
    }
  }
}
/* size in bits */
static double bits2double(const unsigned char*buf, size_t size, int is_signed) {
  double d = 0;
  size_t numbytes = (size+7)>>3;
  size_t i;
  for(i=numbytes+1; i; i--) {
    double x = buf[i-1];
    d = d*256. + x;
  }
  if(is_signed && ((buf[numbytes-1] >> (size-((numbytes-1)<<3)-1)) & 0x1)) {
    d -= (1<<size);
  }
  return d;
}


/* length is in bytes! */
static t_float can2val (const t_canformat*fmt, const unsigned int*canbuf, unsigned int length) {
  unsigned int ilength = (length + 3)>>2;
  if(fmt->is_bigendian) {
    post("BE decoder not yet implemented");
  } else {
    double f = 0.;
    unsigned int bitlen = fmt->length;
    unsigned int buffer[64];
    unsigned int*buf=buffer;
    if(ilength > 64)
      buf = getbytes(ilength * sizeof(unsigned int));
    memcpy(buf, canbuf, length);

    shift_right(buf, ilength, fmt->start);
    zeromask_left(buf, ilength, fmt->length);
    f = bits2double((unsigned char*)buf, bitlen, fmt->is_signed);

    if(buf != buffer)
      freebytes(buf, ilength * sizeof(unsigned int));
    return fmt->offset + f * fmt->factor;
  }
  return 0.;
}

static unsigned int a2ui(t_atom*a) {
  t_float f = atom_getfloat(a);
  if(f<0)
    return 0;
  else
    return (unsigned int)f;
}

static t_canformat_specs*canparse_getspecs(t_canparse *x, t_symbol*can_id)
{
  t_canformat_specs*specs, *newspecs;
  /* first check if we already have specs for this can_id */
  for(specs = x->x_formatspecs; specs; specs = specs->next) {
    if(specs->can_id == can_id)
      return specs;
  }
  /* no, create one and append it to the list */
  newspecs = getbytes(sizeof(*specs));
  newspecs->can_id = can_id;
  newspecs->format = NULL;
  newspecs->next = NULL;
  for(specs = x->x_formatspecs; specs && specs->next; specs = specs->next) { ; }
  if(!specs) {
    x->x_formatspecs = newspecs;
  } else {
    specs->next = newspecs;
  }
  return newspecs;
}
static void canparse_removeformats(t_canparse *x)
{
  /* remove all the format specs */
  t_canformat_specs*specs = x->x_formatspecs;
  while(specs) {
    t_canformat_specs*nextspecs = specs->next;
    t_canformat*fmt = specs->format;
    while(fmt) {
      t_canformat*next = fmt->next;
      freebytes(fmt, sizeof(*fmt));
      fmt = next;
    }
    freebytes(specs, sizeof(*specs));
    specs = nextspecs;
  }
  x->x_formatspecs = 0;
}

static void  canparse_addformat(
  t_canparse *x,
  t_symbol*name,
  t_symbol*can_id,
  unsigned int startbit,
  unsigned int bitlength,
  unsigned int is_signed,
  t_float factor,
  t_float offset,
  unsigned int is_bigendian)
{
  t_canformat_specs*specs = canparse_getspecs(x, can_id);
  t_canformat*fmt, *newfmt;
  /* first check if we already have such a format */
  if(is_bigendian) {
    pd_error(x, "big-endian parameters not supported...skipping %s@%s!", name->s_name, can_id->s_name);
    return;
  }
  for(fmt = specs->format; fmt; fmt=fmt->next) {
    if(fmt->name == name)
      break;
  }
  if(!fmt) {
    if(specs->format)
      for(fmt = specs->format; fmt->next; fmt=fmt->next) { ; }
    if(fmt) {
      fmt->next = getbytes(sizeof*newfmt);
      fmt = fmt->next;
    } else {
      fmt = getbytes(sizeof*newfmt);
      specs->format = fmt;
    }
    fmt->next = NULL;
  }

  fmt->name = name;
  fmt->start = startbit;
  fmt->length = bitlength;
  fmt->is_signed = is_signed;
  fmt->factor = factor;
  fmt->offset = offset;
  fmt->is_bigendian = is_bigendian;
}

#define MARK() post("%s:%d %s", __FILE__, __LINE__, __FUNCTION__);
static void canparse_formatspec(t_canparse*x,
                                t_symbol *s, int argc, t_atom*argv)
{
  t_symbol*name, *can_id;
  unsigned int startbit, bitlength, is_signed;

  t_float factor = 1., offset=0.;
  unsigned int is_bigendian = 0;
  unsigned int i;
  if(!argc) {
    canparse_removeformats(x);
    return;
  }

  if(A_SYMBOL != argv[0].a_type)
    goto error;
  name = atom_getsymbol(argv+0);

  argv++; argc--;
  if(argc<4 || argc>7) {
    pd_error(x, "invalid number of arguments: %d", argc);
    goto error;
  }
  if(A_SYMBOL != argv[0].a_type) {
    pd_error(x, "<name> argument must be a symbol");
    goto error;
  }
  for(i=1; i<3; i++)
    if(A_FLOAT != argv[i].a_type) {
      pd_error(x, "argument#%d must be a number", i+1);
      goto error;
    }

  switch (argc) {
  case 7:
    if(A_FLOAT != argv[6].a_type) {
      pd_error(x, "<offset> argument must be a number");
      goto error;
    }
    offset = atom_getfloat(argv+6);
    /* fall through */
  case 6:
    if(A_FLOAT != argv[5].a_type) {
      pd_error(x, "<factor> argument must be a number");
      goto error;
    }
    factor = atom_getfloat(argv+5);
    /* fall through */
  default:
    can_id = atom_getsymbol(argv+0);
    startbit = a2ui(argv+1);
    bitlength = a2ui(argv+2);

    switch(argv[3].a_type) {
    case A_FLOAT:
      is_bigendian = !a2ui(argv+3);
      break;
    case A_SYMBOL: {
      t_symbol*endian = atom_getsymbol(argv+3);
      if(endian == gensym("l"))
        is_bigendian = 0;
      else if(endian == gensym("L"))
        is_bigendian = 0;
      else if(endian == gensym("b"))
        is_bigendian = 1;
      else if(endian == gensym("B"))
        is_bigendian = 1;
      else {
        is_bigendian = -1;
      }
    }
      break;
    default:
      pd_error(x, "<little?> argument must 'l' (little-endian), 'b' (big-endian) or a number (0,1)");
      goto error;
    }
    if(is_bigendian > 1) {
      pd_error(x, "<little?> argument must 'l' (little-endian), 'b' (big-endian) or a number (0,1)");
      goto error;
    }

    switch(argv[4].a_type) {
    case A_FLOAT:
      is_signed = a2ui(argv+4);
      break;
    case A_SYMBOL: {
      t_symbol*sign = atom_getsymbol(argv+4);
      if(sign == gensym("+"))
        is_signed = 0;
      else if(sign == gensym("-"))
        is_signed = 1;
      else
        is_signed = -1;
    }
      break;
    default: {
      pd_error(x, "<signed?> argument must '+' (unsigned), '-' (signed) or a number (0,1)");
      goto error;
    }
    }
    if(is_signed > 1) {
      pd_error(x, "<signed?> argument must '+' (unsigned), '-' (signed) or a number (0,1)");
      goto error;
    }
  }
  canparse_addformat(x,
                     name,
                     can_id,
                     startbit,
                     bitlength,
                     is_signed,
                     factor,
                     offset,
                     is_bigendian);
  return;
error:
  pd_error(x, "%s <name> <can_id> <start> <length> <little?> <signed?> [<factor> <offset>]", s->s_name);
}

static void canparse_list(t_canparse*x, t_symbol*can_id, int argc, t_atom*argv)
{
  t_atom ap[1];
  unsigned int *canbuf32 = calloc((argc+sizeof(unsigned int)-1)/(sizeof(unsigned int)), sizeof(unsigned int));
  unsigned char*canbuf = (unsigned char*)canbuf32;
  t_canformat_specs *specs;
  int i;
  for(i=0; i<argc; i++) {
    unsigned char c = atom_getfloat(argv+i);
    canbuf[i] = c;
  }
  for(specs = x->x_formatspecs; specs; specs=specs->next) {
    t_canformat *fmt;
    if(specs->can_id != can_id)
      continue;
    for(fmt = specs->format; fmt; fmt=fmt->next) {
      t_float f = can2val (fmt, canbuf32, argc);
      SETFLOAT(ap, f);
      outlet_anything(x->x_msgout, fmt->name, 1, ap);
    }
  }
  free(canbuf32);
}
static void canparse_free(t_canparse*x)
{
  canparse_removeformats(x);
}
static void *canparse_new()
{
  t_canparse *x = (t_canparse *)pd_new(canparse_class);
  x->x_msgout = outlet_new(&x->x_obj, 0);
  x->x_formatspecs = NULL;
  return x;
}

void CANparse_setup(void)
{
  canparse_class = class_new(gensym("CANparse"),
                             (t_newmethod)canparse_new,
                             (t_method)canparse_free,
                             sizeof(t_canparse), 0,
                             A_NULL,  0);

  class_addmethod(canparse_class, (t_method)canparse_formatspec, gensym("signal"), A_GIMME, 0);
  class_addanything(canparse_class, (t_method)canparse_list);
}
