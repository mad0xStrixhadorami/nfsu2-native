import sys, numpy as np
from PIL import Image
def load(p):
    lines=open(p).read().split('\n'); v=[];f=[]
    for l in lines:
        if l.startswith('v '): v.append([float(x) for x in l.split()[1:7]])
        elif l.startswith('f '): f.append([int(x)-1 for x in l.split()[1:4]])
    return np.array(v),np.array(f)
def render(V,F,eye,target,W=1500,H=1000,fov=48):
    P=V[:,:3]; C=V[:,3:6]
    up=np.array([0,1,0.]); f=target-eye; f/=np.linalg.norm(f); r=np.cross(f,up); r/=np.linalg.norm(r); u=np.cross(r,f)
    rel=P-eye; x=rel@r; y=rel@u; z=rel@f
    t=np.tan(np.radians(fov/2)); s=H/2/t
    sx=W/2+x/z*s; sy=H/2-y/z*s
    img=np.zeros((H,W,3)); zb=np.full((H,W),1e9)
    # sky gradient
    for j in range(H): img[j,:]=np.array([0.55,0.72,0.92])*(1-j/H*0.55)+np.array([0.9,0.92,0.95])*(j/H*0.55)
    order=np.argsort(-z[F].mean(1))
    for fi in order:
        a,b,c=F[fi]
        if z[a]<0.5 or z[b]<0.5 or z[c]<0.5: continue
        n=np.cross(P[b]-P[a],P[c]-P[a]); nl=np.linalg.norm(n)
        if nl<1e-9: continue
        n/=nl
        if np.dot(n, eye-P[a])<0 and False: pass
        x0=int(max(0,min(sx[[a,b,c]])));x1=int(min(W-1,max(sx[[a,b,c]])+1));y0=int(max(0,min(sy[[a,b,c]])));y1=int(min(H-1,max(sy[[a,b,c]])+1))
        if x1<=x0 or y1<=y0: continue
        xs,ys=np.meshgrid(np.arange(x0,x1+1),np.arange(y0,y1+1))
        d=(sy[b]-sy[c])*(sx[a]-sx[c])+(sx[c]-sx[b])*(sy[a]-sy[c])
        if abs(d)<1e-9: continue
        l1=((sy[b]-sy[c])*(xs-sx[c])+(sx[c]-sx[b])*(ys-sy[c]))/d
        l2=((sy[c]-sy[a])*(xs-sx[c])+(sx[a]-sx[c])*(ys-sy[c]))/d
        l3=1-l1-l2
        m=(l1>=-0.001)&(l2>=-0.001)&(l3>=-0.001)
        if not m.any(): continue
        zz=1/(l1/z[a]+l2/z[b]+l3/z[c])
        col=(l1[...,None]*C[a]+l2[...,None]*C[b]+l3[...,None]*C[c])
        sub=zb[y0:y1+1,x0:x1+1]
        w=m&(zz<sub)
        sub[w]=zz[w]
        img[y0:y1+1,x0:x1+1][w]=col[w]
    return Image.fromarray((np.clip(img,0,1)*255).astype('uint8'))
if __name__=='__main__':
    V,F=load(sys.argv[1]); out=sys.argv[2]; ex,ey,ez,tx,ty,tz=[float(a) for a in sys.argv[3:9]]
    render(V,F,np.array([ex,ey,ez]),np.array([tx,ty,tz])).save(out)
