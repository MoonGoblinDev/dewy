import sys,glob; from PIL import Image; import numpy as np
T=sys.argv[1]; on_dir=sys.argv[2]; off_dir=sys.argv[3]; tag=sys.argv[4] if len(sys.argv)>4 else 'x'
fr=[np.asarray(Image.open(p).convert('RGB')).astype(float) for p in sorted(glob.glob(f'{T}/{on_dir}/f*.png'))]
st=np.stack(fr); sd=st.std(0).mean(2)
off=np.asarray(Image.open(sorted(glob.glob(f'{T}/{off_dir}/f*.png'))[0]).convert('RGB')).astype(float)
print('frames',len(fr),' temporal std: mean %.3f  p99.9 %.2f  max %.2f  px with std>2: %d'%(sd.mean(),np.percentile(sd,99.9),sd.max(),(sd>2).sum()))
d=np.abs(st[0]-off).mean(2); ys,xs=np.nonzero(d>3)
x0,x1,y0,y1=max(xs.min()-12,0),xs.max()+12,max(ys.min()-12,0),ys.max()+12
crop=lambda a: Image.fromarray(a[y0:y1,x0:x1].astype('uint8')).resize(((x1-x0)*3,(y1-y0)*3),Image.LANCZOS)
w=Image.new('RGB',((x1-x0)*6,(y1-y0)*3)); w.paste(crop(off),(0,0)); w.paste(crop(st[0]),((x1-x0)*3,0)); w.save(f'{T}/cmp_{tag}.png')
Image.fromarray(np.clip(sd[y0:y1,x0:x1]*40,0,255).astype('uint8')).resize(((x1-x0)*3,(y1-y0)*3)).save(f'{T}/jitter_{tag}.png')
