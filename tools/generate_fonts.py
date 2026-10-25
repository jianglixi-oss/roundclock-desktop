"""Generate local 4-bit display atlases from fonts supplied by the developer."""
import argparse
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont

parser=argparse.ArgumentParser()
parser.add_argument('--cjk-font',required=True)
parser.add_argument('--digit-font',required=True)
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'firmware/main')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)

def atlas(path,size,characters):
    font=ImageFont.truetype(path,size);data=bytearray();glyphs=[]
    for ch in characters:
        width=int(font.getlength(ch)+1);height=size+8
        image=Image.new('L',(width,height));ImageDraw.Draw(image).text((0,0),ch,font=font,fill=255)
        raw=list(image.getdata());offset=len(data)
        data.extend(((raw[i]>>4)<<4)|(raw[i+1]>>4 if i+1<len(raw) else 0) for i in range(0,len(raw),2))
        glyphs.append((ord(ch),width,width,height,offset))
    return data,glyphs

def descriptors(name,glyphs):
    return 'static const Glyph '+name+'_glyphs[]={'+','.join('{'+','.join(map(str,g))+'}' for g in glyphs)+'};\n#define '+name.upper()+'_COUNT '+str(len(glyphs))+'\n'

characters=''.join(sorted(set('应用时钟轻触打开返回菜单即将接入待配置网络未校时星期一二三四五六日年月已连接离线请配网时间设置长按空白处'+''.join(chr(c) for c in range(32,127)))))
header=['#pragma once','#include <stdint.h>','typedef struct {uint32_t code; uint16_t offset_w,w,h; uint32_t offset;} Glyph;']
for name,path,size,chars in [('small',args.cjk_font,20,characters),('label',args.cjk_font,26,characters),('big',args.digit_font,108,'0123456789:-'),('dial',args.digit_font,32,'0123456789'),('setup',args.cjk_font,20,'免密码连接')]:
    data,glyphs=atlas(path,size,chars)
    header.append('static const uint8_t '+name+'_data[]={'+','.join(map(str,data))+'};\n'+descriptors(name,glyphs))
(args.output/'fonts.h').write_text('\n'.join(header),encoding='utf-8')
chars=set(chr(c) for c in range(32,127))
for first in range(0xa1,0xf8):
    for second in range(0xa1,0xff):
        try:chars.add(bytes([first,second]).decode('gb2312'))
        except UnicodeDecodeError:pass
chars.update('“”‘’…—·')
data,glyphs=atlas(args.cjk_font,20,sorted(chars))
(args.output/'chinese_font.bin').write_bytes(data)
(args.output/'chinese_font.h').write_text('#pragma once\nextern const uint8_t chinese_data[] asm("_binary_chinese_font_bin_start");\n'+descriptors('chinese',glyphs),encoding='utf-8')
print('Generated local fonts:',len(glyphs),'CJK/ASCII glyphs,',len(data),'atlas bytes')
