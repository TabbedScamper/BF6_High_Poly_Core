#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <bf6_core.h>

// Engine-neutral transcription of BF6HighPolyWaterFFT.cpp's production CPU
// replay (2026-09-12). Positive, UNNORMALISED inverse FFT; -X/+Z wave vectors;
// corrected reciprocal foam gain. Engine textures and scheduling stay outside.
namespace bf6_ocean {
struct Complex { float r=0, i=0; };
struct Pixel { float x=0,y=0,z=0,w=0; };
inline Complex mul(Complex a,float c,float s) { return {a.r*c-a.i*s,a.r*s+a.i*c}; }
inline void fft(Complex* v,int n) {
    for(int i=1,j=0;i<n;++i) {
        int bit=n>>1;
        for(;j&bit;bit>>=1)j^=bit;
        j^=bit; if(i<j)std::swap(v[i],v[j]);
    }
    for(int len=2;len<=n;len<<=1) {
        const float a=2.f*3.1415927410125732421875f/len;
        const float wc=std::cos(a),ws=std::sin(a);
        for(int i=0;i<n;i+=len) {
            float c=1,s=0;
            for(int j=0;j<len/2;++j) {
                Complex a=v[i+j],b=mul(v[i+j+len/2],c,s);
                v[i+j]={a.r+b.r,a.i+b.i}; v[i+j+len/2]={a.r-b.r,a.i-b.i};
                const float nc=c*wc-s*ws; s=c*ws+s*wc; c=nc;
            }
        }
    }
}
inline void fft2(std::vector<Complex>& v,int n) {
    for(int y=0;y<n;++y)fft(v.data()+y*n,n);
    std::vector<Complex> col(n);
    for(int x=0;x<n;++x) {
        for(int y=0;y<n;++y)col[y]=v[y*n+x]; fft(col.data(),n);
        for(int y=0;y<n;++y)v[y*n+x]=col[y];
    }
}
struct Cascade {
    bf6_water_sim_v2 input={};
    std::vector<Complex> h0,height,dx,dz;
    std::vector<Pixel> displacement,normal;
    std::vector<float> history,work;
    bool initialize(const bf6_water_sim_v2& s) {
        int n=s.resolution;
        if(n<2 || n>2048 || (n&(n-1)) || !(s.tile_dimension>0))return false;
        input=s; h0.resize(n*n); height.resize(n*n); dx.resize(n*n); dz.resize(n*n);
        displacement.resize(n*n); normal.resize(n*n); history.assign(n*n,0); work.resize(n*n);
        return bf6_water_spectrum_h0(&s,reinterpret_cast<float*>(h0.data()),n*n*2)==n*n*2;
    }
    void evolve(float time,float dt) {
        int n=input.resolution;
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            int i=y*n+x,m=((n-y)&(n-1))*n+((n-x)&(n-1));
            float kx=float(x+x-n)*-3.1415927410125732421875f/input.tile_dimension;
            float kz=float(y+y-n)* 3.1415927410125732421875f/input.tile_dimension;
            float k=std::sqrt(kx*kx+kz*kz),phase=std::sqrt(9.8f*k)*time;
            float co=std::cos(phase),si=std::sin(phase);
            Complex a=mul(h0[i],co,si),b=mul({h0[m].r,-h0[m].i},co,-si);
            Complex h={a.r+b.r,a.i+b.i}; height[i]=h;
            float inv=1.f/(k+1e-5f),fx=kx*inv,fz=kz*inv;
            dx[i]={h.i*fx,-h.r*fx}; dz[i]={h.i*fz,-h.r*fz};
        }
        fft2(height,n); fft2(dx,n); fft2(dz,n);
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            int i=y*n+x; float s=((x+y)&1)?-1.f:1.f;
            displacement[i]={s*dx[i].r,s*height[i].r,s*dz[i].r,0};
        }
        float ds=float(n)/(2.f*input.tile_dimension);
        float lag=(input.foam_enable && input.foam_half_life>0)?std::pow(.5f,dt/input.foam_half_life):0;
        float norm=input.foam_half_life>0?1.f-std::pow(.5f,1.f/input.foam_half_life):1.f;
        float gain=(input.foam_enable && input.foam_max>0 && norm>1e-6f)?1.f/(norm*input.foam_max):0;
        auto at=[n](int x,int y) { return (y&(n-1))*n+(x&(n-1)); };
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            float fold=ds*(displacement[at(x+1,y)].x-displacement[at(x-1,y)].x
                         +displacement[at(x,y+1)].z-displacement[at(x,y-1)].z);
            float instant=std::max(0.f,fold-input.foam_threshold)*gain;
            work[at(x,y)]=instant+(history[at(x,y)]-instant)*lag;
        }
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            float nx=-ds*(displacement[at(x+1,y)].y-displacement[at(x-1,y)].y);
            float nz=-ds*(displacement[at(x,y+1)].y-displacement[at(x,y-1)].y);
            float inv=1.f/std::sqrt(nx*nx+nz*nz+1.f),foam=0;
            for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox)
                foam+=(ox==0?2.f:1.f)*(oy==0?2.f:1.f)*work[at(x+ox,y+oy)];
            normal[at(x,y)]={foam/16.f,nx*inv*.5f+.5f,nz*inv*.5f+.5f,1};
        }
        history=work;
    }
};
}
