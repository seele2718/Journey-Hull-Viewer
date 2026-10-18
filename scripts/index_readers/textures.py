"""Decode supported DDS top mip levels to PNG using the standard library.

Supports BC1/2/3 and byte-aligned 32-bit RGB channel masks.
"""
import struct
import zlib


class UnsupportedTexture(ValueError):
    pass


def dds_info(raw):
    if len(raw)<128 or raw[:4]!=b'DDS ' or struct.unpack_from('<I',raw,4)[0]!=124:
        raise UnsupportedTexture('Not a supported DDS header')
    height,width,pitch,depth=struct.unpack_from('<4I',raw,12)
    if not 0<width<=4096 or not 0<height<=4096 or width*height>8388608 or depth>1:
        raise UnsupportedTexture('Texture dimensions exceed preview limits')
    if struct.unpack_from('<I',raw,112)[0]&0x200200:
        raise UnsupportedTexture('Cube/volume textures are not supported')
    size,flags=struct.unpack_from('<II',raw,76)
    fourcc=raw[84:88]
    if size!=32:
        raise UnsupportedTexture('Invalid DDS pixel format')
    if flags&4 and fourcc in (b'DXT1',b'DXT3',b'DXT5'):
        fmt=fourcc.decode();length=((width+3)//4)*((height+3)//4)*(8 if fmt=='DXT1' else 16)
        channels=None;row_pitch=0
    elif not flags&4 and flags&0x40 and struct.unpack_from('<I',raw,88)[0]==32:
        masks=struct.unpack_from('<4I',raw,92)
        if any(m not in (0xff,0xff00,0xff0000,0xff000000) for m in masks[:3]) or len(set(masks[:3]))!=3:
            raise UnsupportedTexture('Unsupported RGB channel masks')
        if masks[3] not in (0,0xff,0xff00,0xff0000,0xff000000) or (masks[3] and masks[3] in masks[:3]):
            raise UnsupportedTexture('Unsupported alpha channel mask')
        channels=[None if not m else (m.bit_length()-8)//8 for m in masks]
        row_pitch=pitch if struct.unpack_from('<I',raw,8)[0]&8 else width*4
        if row_pitch<width*4 or row_pitch>width*4+4096:
            raise UnsupportedTexture('Invalid DDS row pitch')
        length=row_pitch*height;fmt='RGB32'
    else:
        raise UnsupportedTexture('DDS format not supported by texture preview')
    if 128+length>len(raw):
        raise UnsupportedTexture('Truncated DDS top mip')
    return {'width':width,'height':height,'format':fmt,'channels':channels,'pitch':row_pitch,'bytes':length}


def decode_dds(raw):
    info=dds_info(raw);w,h,fmt=info['width'],info['height'],info['format']
    out=bytearray(w*h*4)
    if fmt=='RGB32':
        channels=info['channels']
        for y in range(h):
            row=raw[128+y*info['pitch']:128+y*info['pitch']+w*4]
            for k,index in enumerate(channels):
                out[y*w*4+k:(y+1)*w*4:4]=row[index::4] if index is not None else b'\xff'*w
    else:
        offset=128
        for by in range((h+3)//4):
            for bx in range((w+3)//4):
                alphas=[255]*16
                if fmt=='DXT3':
                    bits=int.from_bytes(raw[offset:offset+8],'little');offset+=8
                    alphas=[((bits>>(4*i))&15)*17 for i in range(16)]
                elif fmt=='DXT5':
                    a,b=raw[offset:offset+2];codes=[a,b]
                    if a>b:codes.extend(((7-i)*a+i*b)//7 for i in range(1,7))
                    else:codes.extend(((5-i)*a+i*b)//5 for i in range(1,5));codes.extend([0,255])
                    bits=int.from_bytes(raw[offset+2:offset+8],'little');offset+=8
                    alphas=[codes[(bits>>(3*i))&7] for i in range(16)]
                a,b,bits=struct.unpack_from('<HHI',raw,offset);offset+=8
                def rgb565(value):
                    r=(value>>11)&31;g=(value>>5)&63;v=value&31
                    return ((r<<3)|(r>>2),(g<<2)|(g>>4),(v<<3)|(v>>2))
                ca,cb=rgb565(a),rgb565(b);colours=[ca,cb]
                transparent=fmt=='DXT1' and a<=b
                if transparent:colours.extend([tuple((x+y)//2 for x,y in zip(ca,cb)),(0,0,0)])
                else:colours.extend([tuple((2*x+y)//3 for x,y in zip(ca,cb)),tuple((x+2*y)//3 for x,y in zip(ca,cb))])
                for i in range(16):
                    x,y=bx*4+i%4,by*4+i//4
                    if x>=w or y>=h:continue
                    code=(bits>>(2*i))&3;alpha=0 if transparent and code==3 else alphas[i]
                    at=(y*w+x)*4;out[at:at+4]=bytes((*colours[code],alpha))
    return info,bytes(out)


def png_rgba(width,height,rgba):
    def chunk(kind,payload):
        return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)
    rows=b''.join(b'\0'+rgba[y*width*4:(y+1)*width*4] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))
            +chunk(b'IDAT',zlib.compress(rows,3))+chunk(b'IEND',b''))


def dds_png(raw,terrain_shadow=False):
    info,rgba=decode_dds(raw)
    if terrain_shadow:
        # Approximate baked shadows with a nonzero minimum brightness.
        # Environment lighting and irradiance are not evaluated here.
        pixels=bytearray(rgba)
        for at in range(0,len(pixels),4):
            shadow=max(0,min(1,10*(pixels[at+3]/255-.25)))
            gain=.45+.55*shadow
            for k in range(3):pixels[at+k]=round(pixels[at+k]*gain)
            pixels[at+3]=255
        rgba=pixels
    return png_rgba(info['width'],info['height'],rgba)
