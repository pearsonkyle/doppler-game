/* ============================================================================
 * DOPPLER — time moves when you move.
 *
 * A single-file SUPERHOT x Matrix homage built on the .murkk/.kkrieger rules:
 *   - one C file, no assets on disk, no engine
 *   - textures, levels, meshes, audio, font: all synthesized at startup/runtime
 *   - deterministic seed (it's a demo); --seed N to override, --level N to pick
 *   - stripped dynamic binary should stay tiny; -Os, immediate-mode GL2
 *
 * The gimmick: the simulation timescale follows YOUR motion (walk, look, act).
 * Stand still and the world freezes to a crawl — bullets hang mid-air on
 * oscilloscope trails. Everything that moves relative to you is shaded by
 * Doppler shift: blue approaching, red receding. The synth pitch-bends with
 * the timescale, so freezing time drops the whole soundtrack an octave.
 *
 * Agents are faceted low-poly humanoids with emerald eyes. One hit shatters
 * them into glowing polygon shards. RMB swings a katana that deflects bullets
 * back at the nearest agent; F throws your pistol. Clear all agents to win.
 *
 * Movement: SPACE jumps (twice — once more in the air), kicking off a wall
 * mid-air rebounds you upward, SHIFT/CTRL dodge-rolls under fire. Sectors are
 * built vertically: platforms, stairs, train roofs, mezzanines.
 *
 * build: gcc -Os doppler.c -o doppler -lSDL2 -lGL -lm
 * smoke: ./doppler --smoke      (headless-friendly; writes PPM screenshots)
 * license: CC0 / public domain. greets to .theprodukkt & SUPERHOT team.
 * ==========================================================================*/

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- GL loader
 * Pull GL2 entry points through SDL_GL_GetProcAddress. Bulletproof across
 * Mesa/NVIDIA without GL_GLEXT_PROTOTYPES link games. */
#define GLFUNCS \
  GF(PFNGLCREATESHADERPROC,      glCreateShader)      \
  GF(PFNGLSHADERSOURCEPROC,      glShaderSource)      \
  GF(PFNGLCOMPILESHADERPROC,     glCompileShader)     \
  GF(PFNGLGETSHADERIVPROC,       glGetShaderiv)       \
  GF(PFNGLGETSHADERINFOLOGPROC,  glGetShaderInfoLog)  \
  GF(PFNGLCREATEPROGRAMPROC,     glCreateProgram)     \
  GF(PFNGLATTACHSHADERPROC,      glAttachShader)      \
  GF(PFNGLLINKPROGRAMPROC,       glLinkProgram)       \
  GF(PFNGLGETPROGRAMIVPROC,      glGetProgramiv)      \
  GF(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
  GF(PFNGLUSEPROGRAMPROC,        glUseProgram)        \
  GF(PFNGLGETUNIFORMLOCATIONPROC,glGetUniformLocation)\
  GF(PFNGLUNIFORM1FPROC,         glUniform1f)         \
  GF(PFNGLUNIFORM1IPROC,         glUniform1i)         \
  GF(PFNGLUNIFORM3FPROC,         glUniform3f)         \
  GF(PFNGLUNIFORM3FVPROC,        glUniform3fv)        \
  GF(PFNGLUNIFORM4FVPROC,        glUniform4fv)        \
  GF(PFNGLUNIFORMMATRIX3FVPROC,  glUniformMatrix3fv)  \
  GF(PFNGLACTIVETEXTUREPROC,     glActiveTexture_)

#define GF(t,n) static t n;
GLFUNCS
#undef GF
static void load_gl(void){
#define GF(t,n) n = (t)SDL_GL_GetProcAddress(#n[strlen(#n)-1]=='_'?"glActiveTexture":#n);
  GLFUNCS
#undef GF
}

/* ---------------------------------------------------------------- constants */
#define WINW 1280
#define WINH 720
#define G 44              /* grid cells per side  */
#define CELL 2.0f         /* world units per cell */
#define EYE 1.62f
#define PI 3.14159265358979f
#define MAXENEMY 32
#define MAXITEM 32
#define MAXLIGHT 64
#define MAXTEMPL 16
#define MAXPART 512
#define MAXVOICE 16
#define MAXBUL 256
#define TRAILN 12         /* trail points per bullet */
#define MAXSHARD 512
#define SHLIGHTS 8        /* lights fed to the shader per frame */
#define MINTS 0.045f      /* simulation never fully stops (SUPERHOT creep) */
#define NLEVEL 3
#define STEP 0.55f        /* max auto step-up: anything taller is a wall */
static float wallh=3.4f;  /* hall height, set per sector */

/* ---------------------------------------------------------------- rng/noise */
static unsigned rngs;
static unsigned xs(void){ rngs^=rngs<<13; rngs^=rngs>>17; rngs^=rngs<<5; return rngs; }
static float frand(void){ return (xs()&0xffffff)/(float)0x1000000; }

static unsigned ihash(unsigned x){
  x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x;
}
static float hash2(int x,int y,unsigned s){
  return (ihash((unsigned)x*374761393u + (unsigned)y*668265263u + s*1442695041u)&0xffffff)/(float)0x1000000;
}
/* tileable value noise on a power-of-two lattice */
static float vnoise(float x,float y,int per,unsigned s){
  int ix=(int)floorf(x), iy=(int)floorf(y);
  float fx=x-ix, fy=y-iy;
  fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy);
  int m=per-1;
  float a=hash2(ix&m,iy&m,s),     b=hash2((ix+1)&m,iy&m,s);
  float c=hash2(ix&m,(iy+1)&m,s), d=hash2((ix+1)&m,(iy+1)&m,s);
  return a+(b-a)*fx+(c-a)*fy+(a-b-c+d)*fx*fy;
}
static float fbm(float u,float v,int oct,int per,unsigned s){
  float sum=0,amp=0.5f; int p=per;
  for(int i=0;i<oct;i++){ sum+=amp*vnoise(u*p,v*p,p,s+i*131u); p<<=1; amp*=0.5f; }
  return sum;
}
static float clampf(float v,float lo,float hi){ return v<lo?lo:v>hi?hi:v; }
/* easing: the difference between animation and jank */
static float sstep(float x){ x=clampf(x,0,1); return x*x*(3-2*x); }
static float easeOutBack(float x){ x=clampf(x,0,1); float c=1.70158f,k=x-1;
  return 1.0f+(c+1.0f)*k*k*k+c*k*k; }
static float toward(float v,float t,float k){ return v+(t-v)*clampf(k,0,1); }
/* like toward, but on an angle: takes the short way around */
static float angto(float a,float b,float k){
  float d=fmodf(b-a+PI,2*PI); if(d<0)d+=2*PI; d-=PI;
  return a+d*clampf(k,0,1);
}

/* ---------------------------------------------------------------- mat3 (col-major) */
static void m3id(float*m){ memset(m,0,36); m[0]=m[4]=m[8]=1; }
static void m3rotY(float*m,float a){ float c=cosf(a),s=sinf(a);
  m[0]=c;m[1]=0;m[2]=-s; m[3]=0;m[4]=1;m[5]=0; m[6]=s;m[7]=0;m[8]=c; }
static void m3rotX(float*m,float a){ float c=cosf(a),s=sinf(a);
  m[0]=1;m[1]=0;m[2]=0; m[3]=0;m[4]=c;m[5]=s; m[6]=0;m[7]=-s;m[8]=c; }
static void m3rotZ(float*m,float a){ float c=cosf(a),s=sinf(a);
  m[0]=c;m[1]=s;m[2]=0; m[3]=-s;m[4]=c;m[5]=0; m[6]=0;m[7]=0;m[8]=1; }
static void m3mul(float*o,const float*a,const float*b){ float t[9];
  for(int j=0;j<3;j++)for(int i=0;i<3;i++)
    t[j*3+i]=a[i]*b[j*3]+a[3+i]*b[j*3+1]+a[6+i]*b[j*3+2];
  memcpy(o,t,36); }
static void m3scl(float*m,float x,float y,float z){
  m[0]*=x;m[1]*=x;m[2]*=x; m[3]*=y;m[4]*=y;m[5]*=y; m[6]*=z;m[7]*=z;m[8]*=z; }
static void m3v(const float*m,float x,float y,float z,float*o){
  o[0]=m[0]*x+m[3]*y+m[6]*z; o[1]=m[1]*x+m[4]*y+m[7]*z; o[2]=m[2]*x+m[5]*y+m[8]*z; }

/* ---------------------------------------------------------------- textures
 * The construct is black-on-black: dark panel walls (the digital rain is
 * painted by the fragment shader, not the texture), glossy obsidian floor
 * tiles with hairline emerald seams, slotted ceiling. */
enum { TX_WALL, TX_FLOOR, TX_CEIL, TX_GLOW, TX_COUNT };
static GLuint texAlb[TX_COUNT], texNrm[TX_COUNT];
#define TS 256

static GLuint mktex(unsigned char*px){
  GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
  glTexParameteri(GL_TEXTURE_2D,GL_GENERATE_MIPMAP,GL_TRUE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,TS,TS,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
  return t;
}
/* heightfield -> tangent-space normal map */
static void h2n(float*h,unsigned char*out,float str){
  for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){
    float l=h[y*TS+((x-1)&(TS-1))], r=h[y*TS+((x+1)&(TS-1))];
    float u=h[((y-1)&(TS-1))*TS+x], d=h[((y+1)&(TS-1))*TS+x];
    float nx=(l-r)*str, ny=(u-d)*str, nz=1.0f;
    float il=1.0f/sqrtf(nx*nx+ny*ny+nz*nz);
    unsigned char*p=&out[(y*TS+x)*4];
    p[0]=(unsigned char)((nx*il*0.5f+0.5f)*255);
    p[1]=(unsigned char)((ny*il*0.5f+0.5f)*255);
    p[2]=(unsigned char)((nz*il*0.5f+0.5f)*255);
    p[3]=255;
  }
}
static void putrgb(unsigned char*p,float r,float g,float b){
  r=r<0?0:r>1?1:r; g=g<0?0:g>1?1:g; b=b<0?0:b>1?1:b;
  p[0]=(unsigned char)(r*255); p[1]=(unsigned char)(g*255); p[2]=(unsigned char)(b*255); p[3]=255;
}

static void gen_textures(void){
  float *hh=malloc(TS*TS*sizeof(float));
  unsigned char *alb=malloc(TS*TS*4), *nrm=malloc(TS*TS*4);

  /* --- walls: brutalist black panels, faceted bevels, faint circuitry --- */
  for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){
    float u=x/(float)TS, v=y/(float)TS;
    int px_=x&127, py=y&63;                       /* tall 1x0.5 panels */
    float dx=px_<64?px_:127-px_, dy=py<32?py:63-py;
    float d=dx<dy?dx:dy;
    float bevel=d/3.0f; if(bevel>1)bevel=1;
    bevel=bevel*bevel*(3-2*bevel);
    float grain=fbm(u,v,4,8,77u);
    hh[y*TS+x]=bevel*0.9f+grain*0.1f;
    /* faint traced circuitry inside the panel */
    float tr = fbm(u*2.0f,v*0.3f,3,8,909u);
    float circuit = (tr>0.49f&&tr<0.515f)?0.5f:0.0f;
    float base = 0.030f + grain*0.014f;
    if(bevel<0.4f) base*=0.45f;                   /* seams nearly black  */
    putrgb(&alb[(y*TS+x)*4],
      base*0.85f + circuit*0.010f,
      base       + circuit*0.060f,
      base*0.95f + circuit*0.030f);
  }
  h2n(hh,nrm,3.0f);
  texAlb[TX_WALL]=mktex(alb); texNrm[TX_WALL]=mktex(nrm);

  /* --- floor: obsidian tiles, hairline emerald seams, high gloss --- */
  for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){
    float u=x/(float)TS, v=y/(float)TS;
    int tx=x&127, ty=y&127;
    float dx=tx<64?tx:127-tx, dy=ty<64?ty:127-ty;
    float d=dx<dy?dx:dy;
    float bevel=d/4.0f; if(bevel>1)bevel=1;
    float grain=fbm(u,v,5,16,901u);
    hh[y*TS+x]=bevel*0.8f+grain*0.2f;
    float base=0.020f+grain*0.012f;
    float seam=(d<2.0f)?1.0f:0.0f;                /* glowing grout line  */
    putrgb(&alb[(y*TS+x)*4],
      base + seam*0.015f,
      base + seam*0.110f,
      base + seam*0.050f);
  }
  h2n(hh,nrm,2.0f);
  texAlb[TX_FLOOR]=mktex(alb); texNrm[TX_FLOOR]=mktex(nrm);

  /* --- ceiling: black slabs with recessed light slots --- */
  for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){
    float u=x/(float)TS, v=y/(float)TS;
    int sx_=x&63;
    float slot = (sx_>28&&sx_<35)?1.0f:0.0f;      /* strip every half cell */
    float grain=fbm(u,v,4,8,71u);
    hh[y*TS+x]=(1.0f-slot)*0.8f+grain*0.2f;
    float base=0.018f+grain*0.010f;
    putrgb(&alb[(y*TS+x)*4],
      base+slot*0.022f, base+slot*0.13f, base+slot*0.06f);
  }
  h2n(hh,nrm,2.4f);
  texAlb[TX_CEIL]=mktex(alb); texNrm[TX_CEIL]=mktex(nrm);

  /* --- radial glow sprite --- */
  for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){
    float dx=(x-128)/128.0f, dy=(y-128)/128.0f;
    float r=sqrtf(dx*dx+dy*dy);
    float a=1.0f-r; if(a<0)a=0; a=a*a*a;
    unsigned char*p=&alb[(y*TS+x)*4];
    p[0]=p[1]=p[2]=(unsigned char)(a*255); p[3]=(unsigned char)(a*255);
  }
  texAlb[TX_GLOW]=mktex(alb);

  free(hh); free(alb); free(nrm);
}

/* ---------------------------------------------------------------- 5x7 bitfont
 * 0-9 A-Z and '-' ; 7 row bytes per glyph, bit4 = leftmost column. */
static const unsigned char font[37][7]={
 {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
 {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
 {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
 {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
 {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
 {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
 {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
 {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
 {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
 {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
 {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
 {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
 {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
 {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
 {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
 {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
 {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
 {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}};

static float textw(const char*s,float sc){ return (float)strlen(s)*6*sc; }
static void draw_text(float x,float y,float sc,const char*s){
  glBegin(GL_QUADS);
  for(;*s;s++,x+=6*sc){
    int gi=-1; char c=*s;
    if(c>='0'&&c<='9')gi=c-'0'; else if(c>='A'&&c<='Z')gi=10+c-'A'; else if(c=='-')gi=36;
    if(gi<0)continue;
    for(int r=0;r<7;r++){ unsigned char row=font[gi][r];
      for(int col=0;col<5;col++) if(row&(0x10>>col)){
        float px=x+col*sc, py=y+r*sc, e=sc*0.92f;
        glVertex2f(px,py); glVertex2f(px+e,py); glVertex2f(px+e,py+e); glVertex2f(px,py+e);
      }}}
  glEnd();
}

/* ---------------------------------------------------------------- level */
static unsigned char grid[G][G];           /* 1 = solid floor-to-ceiling   */
static float hgt[G][G];                    /* open cells: raised floor height
                                              (platforms, stairs, train roofs) */
static float startx,startz,startyaw;

typedef struct { float x,y,z,r; float cr,cg,cb; } Light;
static Light lights[MAXLIGHT]; static int nlights;
typedef struct { float x,y,z,r,life; float cr,cg,cb; } TempL;
static TempL templ_[MAXTEMPL];

typedef struct { float x,z; int type,taken,amt; } Item; /* 0 health 1 pistol/ammo */
static Item items[MAXITEM]; static int nitems;

typedef struct {
  float x,z,yaw,flash,anim,phase,state_t,armp;
  float lx,lz,vx,vz;            /* last pos -> velocity, for Doppler tint  */
  float hue;                    /* per-agent facet jitter                   */
  float y;                      /* floor height under the agent             */
  float moveb,fwdb,latb;        /* idle<->walk blend + local move direction */
  float flare,recoil;           /* smoothed eye flare, fire recoil 1->0     */
  int type;                     /* 0 shooter 1 striker                      */
  int state;                    /* 0 advance 1 aim 2 cooldown 3 lunge 4 dead*/
} Enemy;
static Enemy en[MAXENEMY]; static int nen, nalive;

/* bullets: the slow, visible, deflectable kind. owner 0=enemy 1=player.
 * kind 0=round 1=thrown pistol. trail is a ring buffer of past positions. */
typedef struct {
  float x,y,z,vx,vy,vz,life,spin;
  int on,owner,kind,amt;
  float tr[TRAILN][3]; int tn,th; float trd;
} Bullet;
static Bullet bul[MAXBUL];

/* shards: agents shatter into these glowing polygons */
typedef struct {
  float x,y,z,vx,vy,vz,yaw,pit,wy,wp,sx,sy,sz,life,max;
  float r,g,b;
} Shard;
static Shard shards[MAXSHARD]; static int shHead=0;

/* level definitions: three hand-tuned sectors, each with its own climate —
 * same Matrix family, different temperature. */
typedef struct {
  const char*name; int style; unsigned seed;
  int nshoot,nstrike;
  float ceil;                   /* hall height */
  float fog[3];                 /* fog / horizon color  */
  float wallt[3];               /* wall tint            */
  float floort[3];              /* floor tint           */
} LevelDef;
static const LevelDef LEVELS[NLEVEL]={
  {"LOBBY",   0,0x1A0BB7u,10,3, 5.4f,
    {0.005f,0.013f,0.009f},{1.10f,1.22f,1.12f},{1.05f,1.15f,1.08f}},
  {"SUBWAY",  1,0x5ABBA7u,14,5, 4.8f,
    {0.011f,0.008f,0.004f},{1.25f,1.02f,0.78f},{1.10f,0.98f,0.82f}},
  {"TERMINAL",2,0x7E2211u,18,7, 5.8f,
    {0.004f,0.009f,0.013f},{0.82f,1.02f,1.22f},{0.85f,1.00f,1.18f}},
};
static int curlevel=0;

/* level mesh batches: 0 walls 1 floor 2 ceil; interleaved p3 n3 uv2 */
static float *batch[3]; static int bn[3], bcap[3];
static void emit_v(int b,float px,float py,float pz,float nx,float ny,float nz){
  if(bn[b]+8>bcap[b]){ bcap[b]=bcap[b]?bcap[b]*2:4096; batch[b]=realloc(batch[b],bcap[b]*sizeof(float)); }
  /* tangent/bitangent from axis-aligned normal — must match shader */
  float tx,ty,tz,bx,by,bz;
  if(fabsf(ny)>0.5f){ tx=1;ty=0;tz=0; } else { tx=nz;ty=0;tz=-nx; }
  bx=ny*tz-nz*ty; by=nz*tx-nx*tz; bz=nx*ty-ny*tx;
  float u=(px*tx+py*ty+pz*tz)*0.5f, v=(px*bx+py*by+pz*bz)*0.5f;
  float*o=&batch[b][bn[b]];
  o[0]=px;o[1]=py;o[2]=pz; o[3]=nx;o[4]=ny;o[5]=nz; o[6]=u;o[7]=v;
  bn[b]+=8;
}
static int solid(int cx,int cz){ if(cx<0||cz<0||cx>=G||cz>=G)return 1; return grid[cz][cx]; }
/* floor-of-cell: solid cells are infinitely tall walls */
static float cellh(int cx,int cz){
  if(cx<0||cz<0||cx>=G||cz>=G)return 1e9f;
  return grid[cz][cx]? 1e9f : hgt[cz][cx];
}
/* floor height at a point, with solid cells flattened to ground level —
 * for effects and pickups that just need somewhere to sit */
static float floor_at(float x,float z){
  float h=cellh((int)floorf(x/CELL),(int)floorf(z/CELL));
  return h>100.0f?0:h;
}
/* does a circle at (x,z) overlap cell (cx,cz)? */
static int circ_cell(float x,float z,float r,int cx,int cz){
  float bx0=cx*CELL,bx1=bx0+CELL,bz0=cz*CELL,bz1=bz0+CELL;
  float nx=x<bx0?bx0:(x>bx1?bx1:x), nz=z<bz0?bz0:(z>bz1?bz1:z);
  float dx=x-nx,dz=z-nz;
  return dx*dx+dz*dz < r*r;
}
static void carve(int x,int y,int w,int h){
  for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)
    if(i>=0&&j>=0&&i<G&&j<G){ grid[j][i]=0; hgt[j][i]=0; }
}
static void fill(int x,int y,int w,int h){
  for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++) if(i>=0&&j>=0&&i<G&&j<G) grid[j][i]=1;
}
/* open cells with a raised floor: platforms, desks, stairs, train roofs */
static void raise(int x,int y,int w,int h,float ht){
  for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)
    if(i>=0&&j>=0&&i<G&&j<G){ grid[j][i]=0; hgt[j][i]=ht; }
}
static void add_light(float x,float y,float z,float r,float cr,float cg,float cb){
  if(nlights<MAXLIGHT){ Light*l=&lights[nlights++]; l->x=x;l->y=y;l->z=z;l->r=r;l->cr=cr;l->cg=cg;l->cb=cb; }
}
static void add_item(float x,float z,int type,int amt){
  for(int i=0;i<nitems;i++) if(items[i].taken){ items[i]=(Item){x,z,type,0,amt}; return; }
  if(nitems<MAXITEM) items[nitems++]=(Item){x,z,type,0,amt};
}

/* drop an agent on a random free cell, away from spawn and other agents */
static void place_agent(int type,int x0,int z0,int x1,int z1){
  if(nen>=MAXENEMY)return;
  for(int tries=0;tries<200;tries++){
    int cx=x0+(int)(frand()*(x1-x0)), cz=z0+(int)(frand()*(z1-z0));
    if(solid(cx,cz))continue;
    float wx=(cx+0.5f)*CELL, wz=(cz+0.5f)*CELL;
    float dx=wx-startx,dz=wz-startz;
    if(dx*dx+dz*dz<8*8)continue;
    int ok=1;
    for(int i=0;i<nen;i++){ float ax=en[i].x-wx,az=en[i].z-wz;
      if(ax*ax+az*az<3*3){ok=0;break;} }
    if(!ok)continue;
    Enemy*e=&en[nen++];
    memset(e,0,sizeof*e);
    e->x=e->lx=wx; e->z=e->lz=wz; e->type=type;
    e->y=hgt[cz][cx];
    e->yaw=frand()*2*PI; e->phase=frand()*6.28f;
    e->state=0; e->state_t=-frand()*2.0f;   /* stagger the first volley */
    e->hue=(frand()-0.5f)*0.08f;
    return;
  }
}

static void gen_level(int li,unsigned seedmix){
  const LevelDef*L=&LEVELS[li];
  rngs=L->seed^seedmix; if(!rngs)rngs=0xC0FFEEu;
  memset(grid,1,sizeof grid);
  memset(hgt,0,sizeof hgt);
  wallh=L->ceil;
  nlights=0; nitems=0; nen=0;
  for(int b=0;b<3;b++)bn[b]=0;
  int ax0=0,az0=0,ax1=G,az1=G;                 /* agent placement region */

  switch(L->style){
    case 0:{ /* LOBBY: a corporate atrium — open central corridor between twin
                colonnades, a mezzanine gallery with two staircases, low
                reception desks you vault or duck behind. ivory-emerald light. */
      carve(4,4,36,36);
      raise(4,4,36,4,1.6f);                       /* mezzanine, north edge   */
      for(int s=0;s<3;s++){                       /* twin stairs, 0.4 risers */
        raise(8,8+s,3,1,1.2f-0.4f*s);
        raise(33,8+s,3,1,1.2f-0.4f*s);
      }
      for(int j=12;j<38;j+=4){ fill(14,j,1,1); fill(29,j,1,1); } /* colonnades */
      raise(19,18,6,1,1.0f); raise(19,26,6,1,1.0f);              /* desks      */
      for(int j=13;j<38;j+=7){
        add_light(14.5f*CELL,3.6f,(j+0.5f)*CELL,8.5f, 0.85f,0.95f,0.80f);
        add_light(29.5f*CELL,3.6f,(j+0.5f)*CELL,8.5f, 0.85f,0.95f,0.80f);
      }
      add_light(22*CELL,4.6f,20*CELL,16.0f, 0.30f,1.30f,0.65f);  /* atrium core */
      add_light(22*CELL,3.2f, 6*CELL,11.0f, 0.60f,1.00f,0.78f);  /* mezzanine   */
      startx=22*CELL; startz=38.0f*CELL; startyaw=0;
      add_item(22*CELL, 6*CELL,1,6);              /* ammo cache up top */
      add_item( 7*CELL,30*CELL,0,35);
      add_item(37*CELL,20*CELL,1,4);
      ax0=5; az0=10; ax1=39; az1=34;
    } break;
    case 1:{ /* SUBWAY: raised platforms over a track trench, a parked train
                whose roofs are a fighting surface, steps at both ends.
                sodium amber over the platforms, cold cyan over the rails. */
      carve(4,12,36,20);
      raise(4,12,36,4,1.1f);                      /* north platform */
      raise(4,28,36,4,1.1f);                      /* south platform */
      for(int c=0;c<4;c++) raise(6+c*9,20,7,4,2.35f); /* train cars + gaps */
      raise(4,16,2,2,0.55f); raise(38,26,2,2,0.55f);  /* track steps       */
      for(int i=6;i<39;i+=5){ fill(i,15,1,1); fill(i,28,1,1); } /* pillars  */
      for(int i=6;i<39;i+=6){
        add_light((i+0.5f)*CELL,3.7f,13.5f*CELL,7.5f, 1.05f,0.72f,0.35f);
        add_light((i+0.5f)*CELL,3.7f,29.5f*CELL,7.5f, 1.05f,0.72f,0.35f);
      }
      for(int i=9;i<39;i+=9)
        add_light((i+0.5f)*CELL,3.3f,22.0f*CELL,9.5f, 0.25f,0.95f,1.05f);
      startx=5.5f*CELL; startz=13.5f*CELL; startyaw=90;
      add_item(18*CELL,22*CELL,1,6);              /* ammo on a train roof */
      add_item(37*CELL,30*CELL,0,35);
      add_item(22*CELL,18*CELL,0,35);
      add_item(28*CELL,22*CELL,1,4);
      ax0=8; az0=12; ax1=39; az1=31;
    } break;
    default:{ /* TERMINAL: vast ice-teal departure hall; monoliths at mixed
                 heights chain into parkour up to a high corner terrace. */
      carve(5,5,34,34);
      raise(32,5,7,6,2.8f);                       /* the terrace            */
      raise(28,12,3,2,2.0f); raise(24,16,3,2,1.2f); /* the way up           */
      for(int k=0;k<16;k++){
        int w=(xs()&1)?2:1, h=3-w;
        int x=7+(int)(frand()*28), z=9+(int)(frand()*26);
        if(x>18&&x<26&&z>30)continue;             /* keep spawn clear       */
        if(x>26&&z<15)continue;                   /* keep the climb honest  */
        if(k%5==0) fill(x,z,w,h);
        else       raise(x,z,w,h,(k%3==0)?2.0f:1.2f);
      }
      for(int k=0;k<8;k++){
        float lx=(8+frand()*27)*CELL, lz=(8+frand()*27)*CELL;
        add_light(lx,3.6f,lz,8.5f, 0.40f,0.85f,1.00f);
      }
      add_light(22*CELL,4.8f,22*CELL,15.0f, 0.25f,1.10f,1.20f);
      add_light(35*CELL,4.4f, 8*CELL, 9.0f, 0.45f,1.05f,0.95f); /* terrace */
      startx=22*CELL; startz=37.0f*CELL; startyaw=0;
      add_item(35*CELL, 7*CELL,1,8);              /* the terrace reward */
      add_item(10*CELL,10*CELL,0,35);
      add_item( 8*CELL,30*CELL,1,4);
      add_item(30*CELL,34*CELL,0,35);
      ax0=7; az0=7; ax1=38; az1=33;
    } break;
  }

  for(int k=0;k<L->nshoot;k++)  place_agent(0,ax0,az0,ax1,az1);
  for(int k=0;k<L->nstrike;k++) place_agent(1,ax0,az0,ax1,az1);
  nalive=nen;

  /* mesh: floor at each cell's height, ceiling, and vertical faces — full
   * walls against solid cells, skirts where a neighbour's floor is higher */
  for(int z=0;z<G;z++)for(int x=0;x<G;x++){
    if(grid[z][x])continue;
    float x0=x*CELL,x1=x0+CELL,z0=z*CELL,z1=z0+CELL;
    float fh=hgt[z][x];
    emit_v(1,x0,fh,z0, 0,1,0); emit_v(1,x1,fh,z0, 0,1,0);
    emit_v(1,x1,fh,z1, 0,1,0); emit_v(1,x0,fh,z1, 0,1,0);
    emit_v(2,x0,wallh,z0, 0,-1,0); emit_v(2,x0,wallh,z1, 0,-1,0);
    emit_v(2,x1,wallh,z1, 0,-1,0); emit_v(2,x1,wallh,z0, 0,-1,0);
    float nh;
    nh=solid(x-1,z)?wallh:hgt[z][x-1];
    if(nh>fh+0.001f){ emit_v(0,x0,fh,z0, 1,0,0); emit_v(0,x0,fh,z1, 1,0,0);
                      emit_v(0,x0,nh,z1, 1,0,0); emit_v(0,x0,nh,z0, 1,0,0); }
    nh=solid(x+1,z)?wallh:hgt[z][x+1];
    if(nh>fh+0.001f){ emit_v(0,x1,fh,z1, -1,0,0); emit_v(0,x1,fh,z0, -1,0,0);
                      emit_v(0,x1,nh,z0, -1,0,0); emit_v(0,x1,nh,z1, -1,0,0); }
    nh=solid(x,z-1)?wallh:hgt[z-1][x];
    if(nh>fh+0.001f){ emit_v(0,x1,fh,z0, 0,0,1); emit_v(0,x0,fh,z0, 0,0,1);
                      emit_v(0,x0,nh,z0, 0,0,1); emit_v(0,x1,nh,z0, 0,0,1); }
    nh=solid(x,z+1)?wallh:hgt[z+1][x];
    if(nh>fh+0.001f){ emit_v(0,x0,fh,z1, 0,0,-1); emit_v(0,x1,fh,z1, 0,0,-1);
                      emit_v(0,x1,nh,z1, 0,0,-1); emit_v(0,x0,nh,z1, 0,0,-1); }
  }
}

/* ---------------------------------------------------------------- audio synth
 * Voices carry a pitch factor (Doppler) and a world flag: world-bound voices
 * advance at the simulation timescale, so freezing time pitch-bends every
 * sound down with it. g_ats is a single float written by the game thread and
 * read by the audio callback — a word-sized store, same lock-free style the
 * original used for voices[]. */
enum { V_SHOT, V_ESHOT, V_DEFLECT, V_SWING, V_SHATTER, V_HURT,
       V_PICK, V_STEP, V_CLICK, V_WIN, V_WHOOSH, V_THROW };
typedef struct { int type,on,world; float t,p; } Voice;
static Voice voices[MAXVOICE];
static int audioOK=0; static SDL_AudioDeviceID adev;
static volatile float g_ats=1.0f;
static unsigned arng=0xBADC0DEu;
static float arand(void){ arng^=arng<<13;arng^=arng>>17;arng^=arng<<5; return (arng&0xffffff)/(float)0x800000-1.0f; }

static void sfxp(int type,float pitch){
  if(!audioOK)return;
  int world = !(type==V_CLICK||type==V_WIN||type==V_PICK);
  for(int i=0;i<MAXVOICE;i++) if(!voices[i].on){
    voices[i].type=type; voices[i].t=0; voices[i].p=pitch; voices[i].world=world;
    voices[i].on=1; return; }
}
static void sfx(int type){ sfxp(type,1.0f); }

static void audio_cb(void*ud,Uint8*stream,int len){
  (void)ud;
  float*out=(float*)stream; int n=len/4;
  static double gt=0; static float lpn=0, lps=0;
  float ats=g_ats;
  for(int i=0;i<n;i++){
    float s=0;
    /* the construct's drone: deep detuned pair + filtered hiss + 4Hz data
     * ticker. Everything rides ats, so a frozen world hums an octave low. */
    float drt=(float)gt*ats;
    lpn+=0.012f*(arand()-lpn);
    float sw=1.0f+0.3f*sinf(drt*0.5f);
    s+=(0.040f*sinf(drt*2*PI*41.2f)+0.034f*sinf(drt*2*PI*61.8f)+lpn*0.5f)*sw*0.7f;
    float tick=fmodf(drt*4.0f,1.0f);
    s+=sinf(drt*2*PI*1244.0f)*expf(-tick*60.0f)*0.020f;
    for(int v=0;v<MAXVOICE;v++){
      if(!voices[v].on)continue;
      float t=voices[v].t, p=voices[v].p;
      switch(voices[v].type){
        case V_SHOT:{ float nz=arand()*expf(-t*30)*0.5f;
          float f=(170.0f-t*460.0f)*p; if(f<35)f=35;
          s+=nz+sinf(2*PI*f*t)*expf(-t*15)*0.75f;
          if(t>0.45f)voices[v].on=0; }break;
        case V_ESHOT:{ float nz=arand()*expf(-t*24)*0.4f;
          float f=(95.0f-t*180.0f)*p; if(f<28)f=28;
          s+=nz+sinf(2*PI*f*t)*expf(-t*10)*0.6f;
          if(t>0.55f)voices[v].on=0; }break;
        case V_DEFLECT:{ /* metallic ping: inharmonic partials */
          s+=(sinf(2*PI*1318*p*t)+0.6f*sinf(2*PI*2093*p*t)+0.4f*sinf(2*PI*3322*p*t))
             *expf(-t*9)*0.22f + arand()*expf(-t*70)*0.2f;
          if(t>0.6f)voices[v].on=0; }break;
        case V_SWING:{ lps+=(0.04f+0.30f*t)*(arand()-lps);
          s+=lps*sinf(1+t*40)*expf(-t*8)*1.2f;
          if(t>0.35f)voices[v].on=0; }break;
        case V_SHATTER:{ float nz=arand();
          s+=nz*expf(-t*7)*0.4f
            +(sinf(2*PI*2637*t)+sinf(2*PI*3520*t*1.013f))*expf(-t*12)*0.14f;
          if(t>0.8f)voices[v].on=0; }break;
        case V_HURT:{ float sq=sinf(2*PI*62*t)>0?1:-1; s+=sq*expf(-t*10)*0.4f
            +arand()*expf(-t*30)*0.25f;
          if(t>0.4f)voices[v].on=0; }break;
        case V_PICK:{ float f=t<0.09f?660:990; s+=sinf(2*PI*f*t)*expf(-t*9)*0.28f;
          if(t>0.3f)voices[v].on=0; }break;
        case V_STEP: lps+=0.22f*(arand()-lps); s+=lps*expf(-t*70)*0.8f;
          if(t>0.08f)voices[v].on=0; break;
        case V_CLICK: s+=sinf(2*PI*1500*t)*expf(-t*170)*0.25f;
          if(t>0.05f)voices[v].on=0; break;
        case V_WIN:{ float f= t<0.16f?262: t<0.32f?392: t<0.48f?523: 784;
          s+=sinf(2*PI*f*t)*expf(-(t>0.48f?(t-0.48f)*3:0))*0.26f;
          if(t>1.4f)voices[v].on=0; }break;
        case V_WHOOSH:{ lps+=(0.5f-0.4f*t)*(arand()-lps);   /* passing bullet */
          s+=lps*0.9f*expf(-t*6)*p;
          if(t>0.5f)voices[v].on=0; }break;
        case V_THROW:{ lps+=0.15f*(arand()-lps);
          s+=lps*expf(-t*9)*0.9f + sinf(2*PI*(300-t*500)*t)*expf(-t*8)*0.2f;
          if(t>0.4f)voices[v].on=0; }break;
      }
      voices[v].t += (voices[v].world?ats:1.0f)/44100.0f;
    }
    s=tanhf(s*1.3f)*0.8f;
    out[i]=s; gt+=1.0/44100.0;
  }
}

/* ---------------------------------------------------------------- shaders
 * One program for everything lit. uRain paints procedural digital rain as
 * emissive emerald streaks (walls only). uGloss drives the specular that
 * sells "black reflective". uAlpha lets the floor blend over the mirrored
 * reflection pass. Fog folds everything into green-black murk. */
static GLuint prog;
static GLint uCam,uNL,uLpos,uLcol,uM3,uT,uTint,uBump,uEmis,uAlb,uNrm,
             uTime,uRain,uGloss,uAlpha,uFog;
static const char*VS=
"#version 120\n"
"uniform mat3 uM3; uniform vec3 uT;\n"
"varying vec3 vP; varying vec3 vN; varying vec2 vUV;\n"
"void main(){\n"
"  vec3 wp = uM3*gl_Vertex.xyz + uT;\n"
"  vP=wp; vN=uM3*gl_Normal; vUV=gl_MultiTexCoord0.xy;\n"
"  gl_Position = gl_ModelViewProjectionMatrix * vec4(wp,1.0);\n"
"}\n";
static const char*FS=
"#version 120\n"
"uniform sampler2D uAlb; uniform sampler2D uNrm;\n"
"uniform vec3 uCam; uniform int uNL;\n"
"uniform vec4 uLpos[8]; uniform vec3 uLcol[8];\n"
"uniform vec3 uTint; uniform float uBump; uniform float uEmis;\n"
"uniform float uTime; uniform float uRain; uniform float uGloss; uniform float uAlpha;\n"
"uniform vec3 uFog;\n"
"varying vec3 vP; varying vec3 vN; varying vec2 vUV;\n"
"float h1(float x){ return fract(sin(x*127.1)*43758.5453); }\n"
"void main(){\n"
"  vec3 base = texture2D(uAlb,vUV).rgb * uTint;\n"
"  vec3 N = normalize(vN);\n"
"  if(uBump>0.5){\n"
"    vec3 T = (abs(N.y)>0.5)? vec3(1.0,0.0,0.0) : vec3(N.z,0.0,-N.x);\n"
"    vec3 B = cross(N,T);\n"
"    vec3 tn = texture2D(uNrm,vUV).xyz*2.0-1.0;\n"
"    N = normalize(T*tn.x + B*tn.y + N*tn.z);\n"
"  }\n"
"  vec3 col = base*0.05;\n"
"  vec3 V = normalize(uCam - vP);\n"
"  for(int i=0;i<8;i++){ if(i>=uNL)break;\n"
"    vec3 Ld = uLpos[i].xyz - vP;\n"
"    float d = length(Ld); Ld/=d;\n"
"    float a = max(0.0, 1.0 - d/uLpos[i].w); a*=a;\n"
"    float dif = max(dot(N,Ld),0.0);\n"
"    vec3 H = normalize(Ld+V);\n"
"    float spec = pow(max(dot(N,H),0.0), mix(22.0,64.0,uGloss))*(0.25+1.6*uGloss);\n"
"    col += uLcol[i]*a*(base*dif + vec3(spec)*a*(0.3+0.7*uGloss));\n"
"  }\n"
"  if(uRain>0.5){\n"
"    /* digital rain: world-aligned columns of flickering glyph cells */\n"
"    float cx = floor(vUV.x*16.0);\n"
"    float on = step(0.60, h1(cx*3.7+11.0));\n"
"    float spd = 0.08 + 0.20*h1(cx*1.3);\n"
"    float head = fract(uTime*spd + h1(cx*7.7));\n"
"    float d2 = fract(head - vUV.y*0.45);\n"
"    float tail = pow(1.0-d2, 10.0);\n"
"    float glyph = step(0.50, h1(cx*91.0 + floor(vUV.y*34.0)*17.0 + floor(uTime*9.0)*3.0));\n"
"    col += vec3(0.10,1.00,0.42)*tail*glyph*on*(0.18+0.22*h1(cx*5.1));\n"
"  }\n"
"  col = mix(col, base, uEmis);\n"
"  float fd = clamp((distance(uCam,vP)-6.0)/30.0, 0.0, 1.0);\n"
"  col = mix(col, uFog, fd);\n"
"  gl_FragColor = vec4(sqrt(col),uAlpha);\n"
"}\n";

static GLuint shader(GLenum ty,const char*src){
  GLuint s=glCreateShader(ty);
  glShaderSource(s,1,&src,0); glCompileShader(s);
  GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok){ char log[2048]; glGetShaderInfoLog(s,2048,0,log);
    fprintf(stderr,"[doppler] shader fail:\n%s\n",log); exit(1); }
  return s;
}
static void init_shaders(void){
  prog=glCreateProgram();
  glAttachShader(prog,shader(GL_VERTEX_SHADER,VS));
  glAttachShader(prog,shader(GL_FRAGMENT_SHADER,FS));
  glLinkProgram(prog);
  GLint ok; glGetProgramiv(prog,GL_LINK_STATUS,&ok);
  if(!ok){ char log[2048]; glGetProgramInfoLog(prog,2048,0,log);
    fprintf(stderr,"[doppler] link fail:\n%s\n",log); exit(1); }
  uCam=glGetUniformLocation(prog,"uCam");   uNL =glGetUniformLocation(prog,"uNL");
  uLpos=glGetUniformLocation(prog,"uLpos[0]"); uLcol=glGetUniformLocation(prog,"uLcol[0]");
  uM3 =glGetUniformLocation(prog,"uM3");    uT  =glGetUniformLocation(prog,"uT");
  uTint=glGetUniformLocation(prog,"uTint"); uBump=glGetUniformLocation(prog,"uBump");
  uEmis=glGetUniformLocation(prog,"uEmis");
  uAlb=glGetUniformLocation(prog,"uAlb");   uNrm=glGetUniformLocation(prog,"uNrm");
  uTime=glGetUniformLocation(prog,"uTime"); uRain=glGetUniformLocation(prog,"uRain");
  uGloss=glGetUniformLocation(prog,"uGloss"); uAlpha=glGetUniformLocation(prog,"uAlpha");
  uFog=glGetUniformLocation(prog,"uFog");
}

/* ---------------------------------------------------------------- particles */
typedef struct { float x,y,z,vx,vy,vz,life,max; float cr,cg,cb; } Part;
static Part parts[MAXPART]; static int pHead=0;
static void spawn_parts(int n,float x,float y,float z,float spd,float cr,float cg,float cb){
  for(int i=0;i<n;i++){
    Part*p=&parts[pHead]; pHead=(pHead+1)%MAXPART;
    float a=frand()*2*PI, b=(frand()-0.5f)*PI;
    p->x=x;p->y=y;p->z=z;
    p->vx=cosf(a)*cosf(b)*spd*(0.4f+frand());
    p->vy=sinf(b)*spd*(0.4f+frand())+1.5f;
    p->vz=sinf(a)*cosf(b)*spd*(0.4f+frand());
    p->life=p->max=0.25f+frand()*0.3f;
    p->cr=cr;p->cg=cg;p->cb=cb;
  }
}
static void spawn_shards(float x,float y,float z,float vr){
  /* an agent comes apart: 16 glowing facets, tinted by its final Doppler */
  for(int i=0;i<16;i++){
    Shard*s=&shards[shHead]; shHead=(shHead+1)%MAXSHARD;
    float a=frand()*2*PI, b=(frand()-0.4f)*PI*0.5f;
    float sp=1.5f+frand()*3.5f;
    s->x=x+(frand()-0.5f)*0.4f; s->y=y+0.3f+frand()*1.5f; s->z=z+(frand()-0.5f)*0.4f;
    s->vx=cosf(a)*cosf(b)*sp; s->vy=sinf(b)*sp+2.0f; s->vz=sinf(a)*cosf(b)*sp;
    s->yaw=frand()*2*PI; s->pit=frand()*2*PI;
    s->wy=(frand()-0.5f)*14; s->wp=(frand()-0.5f)*14;
    s->sx=0.06f+frand()*0.16f; s->sy=0.06f+frand()*0.22f; s->sz=0.02f+frand()*0.05f;
    s->life=s->max=1.1f+frand()*0.7f;
    /* doppler-tinted emissive: vr<0 was closing on you when it died */
    float k=clampf(vr/8.0f,-1,1);
    if(k<0){ s->r=0.15f-0.05f*k; s->g=0.95f-0.30f*k; s->b=0.40f-1.10f*k; }
    else   { s->r=0.15f+1.30f*k; s->g=0.95f-0.62f*k; s->b=0.40f-0.27f*k; }
  }
}

/* ---------------------------------------------------------------- game state */
enum { ST_TITLE, ST_PLAY, ST_DEAD, ST_WIN };
static int gstate=ST_TITLE;
static float px,pz,py,pyaw,ppitch,php;
static float pvx,pvz,pvy;                   /* player velocity, for Doppler   */
static int   pammo,haspistol,jumps;         /* jumps = air jumps remaining    */
static float tscale=1, actT, mouseAcc;
static float rollT,rollCD,rollDX,rollDZ;    /* dodge roll: timer + direction  */
static float kvx,kvz;                       /* wall-kick horizontal impulse   */
static float pmoveb;                        /* idle<->run blend for the avatar*/
static float avYaw;                         /* smoothed avatar facing (rad)   */
static float camDist=4.05f,camYs=-1.0f;     /* smoothed camera boom + height  */

static float player_height(void){ return rollT>0 ? 0.85f : 1.72f; }
static float player_eye(void){ return py + (rollT>0 ? 0.55f : 1.52f); }
static float player_camh(void){ return py + (rollT>0 ? 1.45f : 1.92f); }
static float fireCD,swingT,swingCD,dmgFlash,stepT,shake,bobT,winT,gtime,wtime,msgT;
static unsigned gseed=0;                    /* xor'd into the level seed      */
static int smoke=0;

static void reset_game(void){
  gen_level(curlevel,gseed);
  px=startx; pz=startz; pyaw=startyaw; ppitch=0; pvx=pvz=pvy=0;
  avYaw=startyaw*PI/180.0f;
  py=hgt[(int)(startz/CELL)][(int)(startx/CELL)];
  php=100; pammo=6; haspistol=1; jumps=1;
  tscale=1; actT=0; mouseAcc=0;
  rollT=rollCD=rollDX=rollDZ=kvx=kvz=pmoveb=0;
  camDist=4.05f; camYs=-1.0f;
  fireCD=swingT=swingCD=dmgFlash=stepT=shake=bobT=winT=wtime=0;
  msgT=3.0f;
  for(int i=0;i<MAXTEMPL;i++)templ_[i].life=0;
  for(int i=0;i<MAXPART;i++)parts[i].life=0;
  for(int i=0;i<MAXSHARD;i++)shards[i].life=0;
  for(int i=0;i<MAXBUL;i++)bul[i].on=0;
}

/* circle-vs-grid, axis separated. y = mover's foot height: cells whose floor
 * is within STEP of the feet are walkable, anything taller blocks. */
static int circ_free(float x,float z,float r,float y){
  for(int dz=-1;dz<=1;dz++)for(int dx=-1;dx<=1;dx++){
    int cx=(int)floorf(x/CELL)+dx, cz=(int)floorf(z/CELL)+dz;
    if(cellh(cx,cz)<=y+STEP)continue;
    if(circ_cell(x,z,r,cx,cz)) return 0;
  }
  return 1;
}
static void move_circ(float*x,float*z,float dx,float dz,float r,float y){
  if(circ_free(*x+dx,*z,r,y)) *x+=dx;
  if(circ_free(*x,*z+dz,r,y)) *z+=dz;
}
/* the floor under a standing circle: highest reachable cell top it overlaps */
static float ground_h(float x,float z,float y){
  float g=0;
  for(int dz=-1;dz<=1;dz++)for(int dx=-1;dx<=1;dx++){
    int cx=(int)floorf(x/CELL)+dx, cz=(int)floorf(z/CELL)+dz;
    if(cx<0||cz<0||cx>=G||cz>=G||grid[cz][cx])continue;
    float h=hgt[cz][cx];
    if(h>y+STEP||h<=g)continue;
    if(circ_cell(x,z,0.30f,cx,cz)) g=h;
  }
  return g;
}
/* is there a kickable wall right beside us? returns its outward normal */
static int wall_kick(float x,float z,float y,float*nx,float*nz){
  static const float D[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
  for(int k=0;k<4;k++){
    int cx=(int)floorf((x+D[k][0]*0.55f)/CELL);
    int cz=(int)floorf((z+D[k][1]*0.55f)/CELL);
    if(cellh(cx,cz)>y+1.0f){ *nx=-D[k][0]; *nz=-D[k][1]; return 1; }
  }
  return 0;
}
/* DDA ray vs grid; returns hit distance (<= maxd). Height-aware: raised
 * floors block the ray where it passes below their top. */
static float ray_wall(float ox,float oy,float oz,float dx,float dy,float dz,float maxd){
  float t=0; int cx=(int)floorf(ox/CELL), cz=(int)floorf(oz/CELL);
  int sx=dx>0?1:-1, sz=dz>0?1:-1;
  float tdx=fabsf(dx)>1e-6f?CELL/fabsf(dx):1e9f, tdz=fabsf(dz)>1e-6f?CELL/fabsf(dz):1e9f;
  float nx=(sx>0?(cx+1)*CELL-ox:ox-cx*CELL), nz=(sz>0?(cz+1)*CELL-oz:oz-cz*CELL);
  float tx=fabsf(dx)>1e-6f?nx/fabsf(dx):1e9f, tz=fabsf(dz)>1e-6f?nz/fabsf(dz):1e9f;
  for(int it=0;it<160;it++){
    if(tx<tz){ t=tx; tx+=tdx; cx+=sx; } else { t=tz; tz+=tdz; cz+=sz; }
    if(t>maxd)return maxd;
    float h=cellh(cx,cz);
    if(h>1e8f)return t;                        /* full wall */
    if(h>0.001f){
      float t2=tx<tz?tx:tz; if(t2>maxd)t2=maxd;
      float y0=oy+dy*t, y1=oy+dy*t2;           /* entry / exit heights */
      if(y0<h||y1<h)return t;                  /* clipped the platform */
    }
  }
  return maxd;
}
static int los(float ax,float ay,float az,float bx,float by,float bz){
  float dx=bx-ax,dy=by-ay,dz=bz-az; float d=sqrtf(dx*dx+dy*dy+dz*dz);
  if(d<0.01f)return 1;
  return ray_wall(ax,ay,az,dx/d,dy/d,dz/d,d) >= d-0.05f;
}
static void add_templ(float x,float y,float z,float r,float life,float cr,float cg,float cb){
  for(int i=0;i<MAXTEMPL;i++) if(templ_[i].life<=0){
    templ_[i]=(TempL){x,y,z,r,life,cr,cg,cb}; return; }
}

/* ---------------------------------------------------------------- doppler
 * The signature shade: radial velocity of a thing relative to the player.
 * Negative vr = closing in = blueshift; positive = receding = redshift. */
static float radial_v(float x,float z,float vx,float vz){
  float dx=x-px, dz=z-pz, d=sqrtf(dx*dx+dz*dz);
  if(d<0.01f)return 0;
  return ((vx-pvx)*dx + (vz-pvz)*dz)/d;
}
static void dopp_rgb(float vr,float*r,float*g,float*b){
  float k=clampf(vr/8.0f,-1,1);
  if(k<0){ k=-k;
    *r=0.15f+(0.22f-0.15f)*k; *g=0.95f+(0.60f-0.95f)*k; *b=0.40f+(1.65f-0.40f)*k;
  } else {
    *r=0.15f+(1.55f-0.15f)*k; *g=0.95f+(0.30f-0.95f)*k; *b=0.40f+(0.12f-0.40f)*k;
  }
}

/* ---------------------------------------------------------------- combat */
static void hurt_player(float dmg){
  if(gstate!=ST_PLAY)return;
  php-=dmg; dmgFlash=0.6f; shake=0.3f; sfx(V_HURT);
  if(php<=0){ php=0; gstate=ST_DEAD; SDL_SetRelativeMouseMode(SDL_FALSE); }
}
static Bullet* new_bullet(void){
  for(int i=0;i<MAXBUL;i++) if(!bul[i].on){
    Bullet*b=&bul[i]; memset(b,0,sizeof*b); b->on=1; return b; }
  return 0;
}
static void spawn_bullet(float x,float y,float z,float dx,float dy,float dz,
                         float spd,int owner,int kind,int amt){
  Bullet*b=new_bullet(); if(!b)return;
  float il=1.0f/sqrtf(dx*dx+dy*dy+dz*dz+1e-9f);
  b->x=x;b->y=y;b->z=z;
  b->vx=dx*il*spd; b->vy=dy*il*spd; b->vz=dz*il*spd;
  b->life=6.0f; b->owner=owner; b->kind=kind; b->amt=amt;
  b->tn=0; b->th=0; b->trd=0;
}
static void shatter_enemy(Enemy*e){
  if(e->state==4)return;
  e->state=4; nalive--;
  float vr=radial_v(e->x,e->z,e->vx,e->vz);
  spawn_shards(e->x,e->y,e->z,vr);
  spawn_parts(14,e->x,e->y+1.1f,e->z,2.5f, 0.2f,1.0f,0.5f);
  add_templ(e->x,e->y+1.2f,e->z,6.0f,0.35f, 0.3f,2.2f,1.0f);
  sfx(V_SHATTER);
  if(e->type==0)add_item(e->x,e->z,1,3+(int)(frand()*3)); /* shooters drop pistols */
  if(nalive<=0 && gstate==ST_PLAY){
    gstate=ST_WIN; winT=0; sfx(V_WIN); SDL_SetRelativeMouseMode(SDL_FALSE);
  }
}
static void player_aim(float*dx,float*dy,float*dz){
  float yr=pyaw*PI/180, pr=ppitch*PI/180;
  *dx=sinf(yr)*cosf(pr); *dy=-sinf(pr); *dz=-cosf(yr)*cosf(pr);
}
static void fire(void){
  if(fireCD>0||rollT>0)return;
  if(!haspistol||pammo<1){ sfx(V_CLICK); fireCD=0.3f; return; }
  pammo--; fireCD=0.34f; actT=0.22f;
  sfx(V_SHOT);
  float dx,dy,dz; player_aim(&dx,&dy,&dz);
  float ey=player_eye();
  spawn_bullet(px+dx*0.4f,ey-0.06f+dy*0.4f,pz+dz*0.4f,dx,dy,dz,26.0f,1,0,0);
  add_templ(px+dx*0.7f,ey+dy*0.7f-0.1f,pz+dz*0.7f,5.0f,0.07f, 1.2f,3.2f,1.8f);
}
static void throw_pistol(void){
  if(!haspistol||swingT>0)return;
  haspistol=0; actT=0.25f;
  sfx(V_THROW);
  float dx,dy,dz; player_aim(&dx,&dy,&dz);
  float ey=player_eye();
  spawn_bullet(px+dx*0.4f,ey+dy*0.4f,pz+dz*0.4f,dx,dy+0.06f,dz,13.0f,1,1,pammo);
  pammo=0;
}
static void katana(void){
  if(swingCD>0||swingT>0)return;
  swingT=0.0001f; swingCD=0.5f; actT=0.26f;
  sfx(V_SWING);
}
/* the active swing window: kill close agents, bat bullets back */
static void katana_strike(void){
  float yr=pyaw*PI/180, fx=sinf(yr), fz=-cosf(yr);
  for(int i=0;i<nen;i++){
    Enemy*e=&en[i]; if(e->state==4)continue;
    float dx=e->x-px,dz=e->z-pz,d=sqrtf(dx*dx+dz*dz);
    if(d>1.9f||fabsf(e->y-py)>1.4f)continue;
    if((dx*fx+dz*fz)/(d+1e-6f) < 0.45f)continue;
    shatter_enemy(e);
  }
  for(int i=0;i<MAXBUL;i++){
    Bullet*b=&bul[i];
    if(!b->on||b->owner!=0)continue;
    float dx=b->x-px,dz=b->z-pz,d=sqrtf(dx*dx+dz*dz);
    if(d>2.5f||b->y<py||b->y>py+2.2f)continue;
    if((dx*fx+dz*fz)/(d+1e-6f) < 0.30f)continue;
    if(b->vx*dx+b->vz*dz > 0)continue;            /* must be inbound */
    /* deflect: retarget the nearest living agent in line of sight */
    int t=-1; float bd=1e9f;
    for(int j=0;j<nen;j++){ Enemy*e=&en[j]; if(e->state==4)continue;
      float ex=e->x-b->x,ez=e->z-b->z,ed=ex*ex+ez*ez;
      if(ed<bd && los(b->x,b->y,b->z,e->x,e->y+1.3f,e->z)){bd=ed;t=j;} }
    float ndx,ndy,ndz;
    if(t>=0){ ndx=en[t].x-b->x; ndy=(en[t].y+1.35f)-b->y; ndz=en[t].z-b->z; }
    else    { ndx=fx; ndy=0.02f; ndz=fz; }
    float il=1.0f/sqrtf(ndx*ndx+ndy*ndy+ndz*ndz+1e-9f);
    b->vx=ndx*il*19.0f; b->vy=ndy*il*19.0f; b->vz=ndz*il*19.0f;
    b->owner=1; b->life=6.0f;
    spawn_parts(6,b->x,b->y,b->z,2.0f, 0.4f,1.2f,1.8f);
    add_templ(b->x,b->y,b->z,4.0f,0.12f, 0.8f,2.4f,2.6f);
    sfxp(V_DEFLECT,0.9f+frand()*0.25f);
  }
}

/* ---------------------------------------------------------------- bullets */
static void update_bullets(float wdt){
  for(int i=0;i<MAXBUL;i++){
    Bullet*b=&bul[i]; if(!b->on)continue;
    b->life-=wdt; if(b->life<=0){b->on=0;continue;}
    if(b->kind==1){ b->vy-=4.0f*wdt; b->spin+=wdt*14; }
    float spd=sqrtf(b->vx*b->vx+b->vy*b->vy+b->vz*b->vz);
    float step=spd*wdt;
    int nsub=(int)(step/0.22f)+1;
    float sdt=wdt/nsub;
    for(int s=0;s<nsub && b->on;s++){
      b->x+=b->vx*sdt; b->y+=b->vy*sdt; b->z+=b->vz*sdt;
      /* trail breadcrumb every 0.30 units of travel */
      b->trd+=spd*sdt;
      if(b->trd>0.30f){ b->trd=0;
        b->tr[b->th][0]=b->x; b->tr[b->th][1]=b->y; b->tr[b->th][2]=b->z;
        b->th=(b->th+1)%TRAILN; if(b->tn<TRAILN)b->tn++;
      }
      /* walls / floors (raised or not) / ceiling */
      if(b->y>wallh-0.03f ||
         b->y<cellh((int)floorf(b->x/CELL),(int)floorf(b->z/CELL))+0.03f){
        if(b->kind==1){ /* thrown pistol clatters down, becomes a pickup */
          float ix=b->x-b->vx*sdt, iz=b->z-b->vz*sdt;
          add_item(ix,iz,1,b->amt>0?b->amt:2);
        }
        spawn_parts(7,b->x,b->y,b->z,2.4f, 0.3f,1.1f,0.6f);
        add_templ(b->x,b->y,b->z,3.0f,0.10f, 0.5f,2.0f,1.0f);
        b->on=0; break;
      }
      if(b->owner==0){ /* enemy round vs player capsule — roll under it or
                          jump it: ankle grace makes the leap readable */
        float dx=b->x-px,dz=b->z-pz;
        float ph=player_height();
        if(dx*dx+dz*dz<0.34f*0.34f && b->y>py+0.28f && b->y<py+ph){
          hurt_player(22);
          spawn_parts(8,b->x,b->y,b->z,2.0f, 1.2f,0.3f,0.2f);
          b->on=0; break;
        }
        /* near miss: doppler whoosh, pitch from closing speed */
        float d2=dx*dx+dz*dz;
        if(d2<1.3f*1.3f && d2>1.0f && (b->vx*dx+b->vz*dz)>0 && frand()<0.5f)
          sfxp(V_WHOOSH, 0.7f+clampf(spd/14.0f,0,1)*0.8f);
      } else { /* player round / thrown pistol vs agents */
        for(int j=0;j<nen;j++){
          Enemy*e=&en[j]; if(e->state==4)continue;
          float dx=b->x-e->x,dz=b->z-e->z;
          if(dx*dx+dz*dz<0.45f*0.45f && b->y>e->y && b->y<e->y+2.0f){
            shatter_enemy(e);
            if(b->kind==1)add_item(e->x,e->z,1,b->amt>0?b->amt:2);
            b->on=0; break;
          }
        }
      }
    }
  }
}

/* ---------------------------------------------------------------- agent AI
 * Shooters keep 4-14u of range, telegraph with an eye-flare aim phase, then
 * loose a slow round at where you ARE — stand still and it still creeps at
 * you, because the world only freezes, never stops. Strikers just run you
 * down and lunge. All of it advances on world-time. */
static void update_enemies(float wdt){
  for(int i=0;i<nen;i++){
    Enemy*e=&en[i];
    if(e->flash>0)e->flash-=wdt;
    /* velocity estimate for the Doppler tint */
    if(wdt>1e-4f){ e->vx=(e->x-e->lx)/wdt; e->vz=(e->z-e->lz)/wdt; }
    e->lx=e->x; e->lz=e->z;
    if(e->state==4)continue;
    /* settle onto whatever floor is underfoot (stairs, platforms) */
    e->y=toward(e->y, ground_h(e->x,e->z,e->y), wdt*10.0f);
    /* animation blends, fed from real velocity so they're honest in every
     * state: walk amount + local move direction (forward vs lateral) */
    float sp2=sqrtf(e->vx*e->vx+e->vz*e->vz);
    e->moveb=toward(e->moveb, clampf(sp2/2.2f,0,1), wdt*8.0f);
    if(sp2>0.1f){
      float ivx=e->vx/sp2, ivz=e->vz/sp2;
      e->fwdb=toward(e->fwdb, ivx*sinf(e->yaw)-ivz*cosf(e->yaw), wdt*6.0f);
      e->latb=toward(e->latb, ivx*cosf(e->yaw)+ivz*sinf(e->yaw), wdt*6.0f);
    }
    e->flare=toward(e->flare, e->state==1? sstep(e->armp):0.0f,
                    wdt*(e->state==1?7.0f:3.5f));
    if(e->recoil>0){ e->recoil-=wdt*3.5f; if(e->recoil<0)e->recoil=0; }

    float dx=px-e->x, dz=pz-e->z, d=sqrtf(dx*dx+dz*dz);
    int see=los(e->x,e->y+1.55f,e->z, px,py+1.2f,pz);
    float spd = e->type==1?4.4f:2.6f;
    switch(e->state){
      case 0:{ /* advance / reposition */
        e->yaw=atan2f(dx,-dz);
        float mx=0,mz=0;
        if(e->type==1||!see||d>13.0f){ mx=dx/d; mz=dz/d; }
        else if(d<4.0f){ mx=-dx/d; mz=-dz/d; }
        else { float sgn=sinf(wtime*0.7f+e->phase)>0?1:-1;   /* strafe */
               mx=-dz/d*sgn; mz=dx/d*sgn; spd*=0.7f; }
        float ox=e->x,oz=e->z;
        move_circ(&e->x,&e->z,mx*spd*wdt,mz*spd*wdt,0.32f,e->y);
        if(fabsf(e->x-ox)+fabsf(e->z-oz) < spd*wdt*0.25f){
          float a=e->yaw+(sinf(wtime*3+e->phase)>0?1.4f:-1.4f);
          move_circ(&e->x,&e->z,sinf(a)*spd*wdt,-cosf(a)*spd*wdt,0.32f,e->y);
        }
        /* never walk off a high edge (train roofs, the mezzanine) */
        if(e->y-ground_h(e->x,e->z,e->y) > 0.65f){ e->x=ox; e->z=oz; }
        e->anim+=wdt*spd*3.2f;
        e->state_t+=wdt;
        if(e->type==0){ if(see&&d<15.0f&&d>3.0f&&e->state_t>0){ e->state=1; e->state_t=0; } }
        else          { if(d<1.7f&&fabsf(py-e->y)<1.2f&&e->state_t>0){ e->state=3; e->state_t=0; } }
      } break;
      case 1: /* aim: eyes flare, arm rises */
        e->yaw=atan2f(dx,-dz);
        e->state_t+=wdt;
        e->armp=clampf(e->state_t/0.25f,0,1);
        if(!see){ e->state=0; e->state_t=0; break; }
        if(e->state_t>0.5f){
          /* a three-round fan, aimed at your stance RIGHT NOW —
           * roll under it, jump over it, or stand still and study it */
          float my=e->y+1.36f, aimY=py+1.28f;
          float hx=e->x+sinf(e->yaw)*0.45f, hz=e->z-cosf(e->yaw)*0.45f;
          for(int k=-1;k<=1;k++){
            float sp=k*0.14f+(frand()-0.5f)*0.03f;
            float bdx=dx+(-dz)*sp, bdz=dz+dx*sp;
            spawn_bullet(hx,my,hz,bdx,aimY-my,bdz,7.0f+frand()*2.5f,0,0,0);
          }
          e->recoil=1.0f;
          sfxp(V_ESHOT,0.9f+frand()*0.2f);
          add_templ(hx,my,hz,4.0f,0.10f, 2.0f,1.2f,0.5f);
          e->state=2; e->state_t=0;
        } break;
      case 2: /* cooldown: drift sideways */
        e->state_t+=wdt;
        e->armp-=wdt*2; if(e->armp<0)e->armp=0;
        { float sgn=sinf(e->phase*9)>0?1:-1;
          move_circ(&e->x,&e->z,-dz/d*sgn*1.8f*wdt,dx/d*sgn*1.8f*wdt,0.32f,e->y);
          e->anim+=wdt*5; }
        if(e->state_t>0.7f+frand()*0.5f){ e->state=0; e->state_t=0; }
        break;
      case 3: /* striker lunge */
        e->yaw=atan2f(dx,-dz);
        e->state_t+=wdt;
        if(e->state_t<0.32f){ /* windup: rear back */ }
        else {
          if(d<2.0f && fabsf(py-e->y)<1.3f)hurt_player(26);
          e->state=2; e->state_t=0.4f;   /* borrow cooldown */
        } break;
    }
    if(e->state!=4 && d<0.7f && d>0.001f && fabsf(py-e->y)<1.5f){
      e->x-=dx/d*(0.7f-d); e->z-=dz/d*(0.7f-d);
    }
  }
}

/* ---------------------------------------------------------------- drawing */
static int refl=0;   /* mirror pass: flip Y of every model transform */
static void set_uM(const float*m,float tx,float ty,float tz){
  if(refl){ float f[9]; memcpy(f,m,36); f[1]=-f[1]; f[4]=-f[4]; f[7]=-f[7];
    glUniformMatrix3fv(uM3,1,GL_FALSE,f); glUniform3f(uT,tx,-ty,tz); }
  else { glUniformMatrix3fv(uM3,1,GL_FALSE,m); glUniform3f(uT,tx,ty,tz); }
}
/* display-list cache: the humanoids redraw the same handful of fixed shapes
 * hundreds of times a frame; bake each unique signature once and replay it.
 * Armed only inside the figure draws — shards/items have randomized sizes
 * that would churn the cache. Returns 0 emit, 1 replayed, 2 emit-and-record. */
static int primArm=0;
#define MAXPRIM 96
static struct { float p[3]; int seg,ring,type; GLuint l; } primc[MAXPRIM];
static int nprim;
static int prim_open(int type,float a,float b,float c,int seg,int ring){
  if(!primArm)return 0;
  for(int i=0;i<nprim;i++)
    if(primc[i].type==type&&primc[i].seg==seg&&primc[i].ring==ring
       &&primc[i].p[0]==a&&primc[i].p[1]==b&&primc[i].p[2]==c){
      glCallList(primc[i].l); return 1; }
  if(nprim>=MAXPRIM)return 0;
  GLuint l=glGenLists(1); if(!l)return 0;
  primc[nprim].type=type; primc[nprim].seg=seg; primc[nprim].ring=ring;
  primc[nprim].p[0]=a; primc[nprim].p[1]=b; primc[nprim].p[2]=c;
  primc[nprim].l=l; nprim++;
  glNewList(l,GL_COMPILE_AND_EXECUTE);
  return 2;
}

static void box_sh(float sx,float sy,float sz){ /* shader-lit box, centred */
  int pc=prim_open(0,sx,sy,sz,0,0); if(pc==1)return;
  float x=sx*0.5f,y=sy*0.5f,z=sz*0.5f;
  glBegin(GL_QUADS);
  glNormal3f(0,0,1);  glTexCoord2f(0.5f,0.5f);
  glVertex3f(-x,-y,z); glVertex3f(x,-y,z); glVertex3f(x,y,z); glVertex3f(-x,y,z);
  glNormal3f(0,0,-1);
  glVertex3f(x,-y,-z); glVertex3f(-x,-y,-z); glVertex3f(-x,y,-z); glVertex3f(x,y,-z);
  glNormal3f(1,0,0);
  glVertex3f(x,-y,z); glVertex3f(x,-y,-z); glVertex3f(x,y,-z); glVertex3f(x,y,z);
  glNormal3f(-1,0,0);
  glVertex3f(-x,-y,-z); glVertex3f(-x,-y,z); glVertex3f(-x,y,z); glVertex3f(-x,y,-z);
  glNormal3f(0,1,0);
  glVertex3f(-x,y,z); glVertex3f(x,y,z); glVertex3f(x,y,-z); glVertex3f(-x,y,-z);
  glNormal3f(0,-1,0);
  glVertex3f(-x,-y,-z); glVertex3f(x,-y,-z); glVertex3f(x,-y,z); glVertex3f(-x,-y,z);
  glEnd();
  if(pc==2)glEndList();
}
/* shader-lit tapered cylinder along local Y, centred. r0=bottom r1=top radius,
 * seg sides. ONE normal per facet (SUPERHOT cut): limbs read as crystal prisms,
 * each plane catching its own band of light. */
static void cyl_sh(float r0,float r1,float h,int seg){
  int pc=prim_open(1,r0,r1,h,seg,0); if(pc==1)return;
  float y0=-h*0.5f,y1=h*0.5f,dr=r1-r0;
  float nl=sqrtf(h*h+dr*dr); if(nl<1e-6f)nl=1;
  glBegin(GL_TRIANGLES); glTexCoord2f(0.5f,0.5f);
  for(int i=0;i<seg;i++){
    float a0=i*2*PI/seg,a1=(i+1)*2*PI/seg, am=(a0+a1)*0.5f;
    float c0=cosf(a0),s0=sinf(a0),c1=cosf(a1),s1=sinf(a1);
    float bx0=r0*c0,bz0=r0*s0,tx0=r1*c0,tz0=r1*s0;
    float bx1=r0*c1,bz1=r0*s1,tx1=r1*c1,tz1=r1*s1;
    glNormal3f(cosf(am)*h/nl,-dr/nl,sinf(am)*h/nl);
    glVertex3f(bx0,y0,bz0); glVertex3f(bx1,y0,bz1); glVertex3f(tx1,y1,tz1);
    glVertex3f(bx0,y0,bz0); glVertex3f(tx1,y1,tz1); glVertex3f(tx0,y1,tz0);
    if(r0>1e-4f){ glNormal3f(0,-1,0);
      glVertex3f(0,y0,0); glVertex3f(bx1,y0,bz1); glVertex3f(bx0,y0,bz0); }
    if(r1>1e-4f){ glNormal3f(0,1,0);
      glVertex3f(0,y1,0); glVertex3f(tx0,y1,tz0); glVertex3f(tx1,y1,tz1); }
  }
  glEnd();
  if(pc==2)glEndList();
}
/* one ellipsoid position (normal is emitted per patch, not per vertex) */
static void spos(float rx,float ry,float rz,float ph,float th){
  glVertex3f(rx*cosf(ph)*cosf(th), ry*sinf(ph), rz*cosf(ph)*sinf(th));
}
/* shader-lit faceted ellipsoid, centred. One analytic normal per patch,
 * evaluated at the patch centre — at low seg/ring it reads as a cut gem. */
static void sphere_sh(float rx,float ry,float rz,int seg,int ring){
  int pc=prim_open(2,rx,ry,rz,seg,ring); if(pc==1)return;
  glBegin(GL_TRIANGLES); glTexCoord2f(0.5f,0.5f);
  for(int j=0;j<ring;j++){
    float p0=PI*j/ring-PI*0.5f, p1=PI*(j+1)/ring-PI*0.5f, pm=(p0+p1)*0.5f;
    for(int i=0;i<seg;i++){
      float a0=2*PI*i/seg, a1=2*PI*(i+1)/seg, am=(a0+a1)*0.5f;
      float x=rx*cosf(pm)*cosf(am), y=ry*sinf(pm), z=rz*cosf(pm)*sinf(am);
      float nx=x/(rx*rx),ny=y/(ry*ry),nz=z/(rz*rz);
      float il=1.0f/sqrtf(nx*nx+ny*ny+nz*nz+1e-9f);
      glNormal3f(nx*il,ny*il,nz*il);
      spos(rx,ry,rz,p0,a0); spos(rx,ry,rz,p1,a0); spos(rx,ry,rz,p1,a1);
      spos(rx,ry,rz,p0,a0); spos(rx,ry,rz,p1,a1); spos(rx,ry,rz,p0,a1);
    }
  }
  glEnd();
  if(pc==2)glEndList();
}

/* the agents: SUPERHOT-cut crystal humanoids in dark suits, emerald eyes.
 * Hard V-taper silhouette, flat facet planes, eased motion. Suit tint =
 * charcoal mixed with the Doppler shade of their motion. */
static void draw_agent(Enemy*e,float dim){
  if(e->state==4)return;
  primArm=1;
  float M[9],R[9],X[9];
  float by=e->y;
  float walk = sinf(e->anim), walk2=sinf(e->anim+PI);
  float lean = e->state==3 ? (e->state_t<0.32f? -0.35f : 0.5f)
             : (e->type==1&&e->state==0 ? 0.18f : 0.0f);
  lean -= e->recoil*e->recoil*0.10f;            /* fire recoil rocks the torso */
  m3rotY(R,e->yaw); m3rotX(X,lean); m3mul(M,R,X);

  float vr=radial_v(e->x,e->z,e->vx,e->vz);
  float dr,dg,db; dopp_rgb(vr,&dr,&dg,&db);
  float mvel=clampf(sqrtf(e->vx*e->vx+e->vz*e->vz)/5.0f,0,1);
  float mixk=0.30f+0.55f*mvel;                  /* faster = stronger shift */
  float sr=(0.080f+e->hue*0.4f)*(1-mixk)+dr*0.22f*mixk;
  float sg=(0.090f+e->hue*0.3f)*(1-mixk)+dg*0.22f*mixk;
  float sb= 0.085f            *(1-mixk)+db*0.22f*mixk;
  float fl=e->flash>0?0.6f:0.0f;
  glUniform1f(uBump,0); glUniform1f(uGloss,0.6f); glUniform1f(uEmis,0.12f+fl);
  glUniform3f(uTint,sr+fl,sg+fl*0.9f,sb+fl*0.8f);

  /* legs: swing splits across forward/lateral axes, so strafing reads as
   * side-steps instead of a moonwalk; amplitude blends with moveb */
  for(int li=0;li<2;li++){
    float side=li?0.14f:-0.14f;
    float sw=(li?walk:walk2)*0.55f*e->moveb;
    float kb=0.14f+0.34f*clampf(0.5f-0.5f*(li?walk:walk2),0,1)*e->moveb;
    float hip[3]; m3v(M,side,0,0,hip);
    float hx=e->x+hip[0], hy=by+0.92f+hip[1], hz=e->z+hip[2];
    float UL[9],RX[9],RZ[9],S[9];
    m3rotX(RX,sw*e->fwdb); m3rotZ(RZ,-sw*e->latb*0.7f);
    m3mul(S,RX,RZ); m3mul(UL,M,S);
    float ulc[3]; m3v(UL,0,-0.23f,0,ulc);
    set_uM(UL,hx+ulc[0],hy+ulc[1],hz+ulc[2]); cyl_sh(0.105f,0.082f,0.48f,6);
    float knee[3]; m3v(UL,0,-0.46f,0,knee);
    float kx=hx+knee[0],ky=hy+knee[1],kz=hz+knee[2];
    float LL[9],RK[9]; m3rotX(RK,sw*e->fwdb+kb); m3mul(S,RK,RZ); m3mul(LL,M,S);
    float llc[3]; m3v(LL,0,-0.22f,0,llc);
    set_uM(LL,kx+llc[0],ky+llc[1],kz+llc[2]); cyl_sh(0.078f,0.052f,0.44f,6);
    float ank[3]; m3v(LL,0,-0.42f,0,ank);
    float fo[3]; m3v(M,0,-0.02f,-0.07f,fo);
    set_uM(M,kx+ank[0]+fo[0],ky+ank[1]+fo[1],kz+ank[2]+fo[2]);
    box_sh(0.13f,0.07f,0.27f);
  }
  /* pelvis + chest: hips wider than the waist, chest flares to the shoulders
   * — the V-taper. Depth-squashed to a slab; idle gets a breathing swell. */
  float br=1.0f+0.018f*sinf(wtime*1.8f+e->phase)*(1.0f-e->moveb);
  float Mt[9]; memcpy(Mt,M,36); m3scl(Mt,1.0f,1.0f,0.60f);
  set_uM(Mt,e->x,by+1.02f,e->z); cyl_sh(0.185f,0.165f,0.26f,7);
  float Mb[9]; memcpy(Mb,Mt,36); m3scl(Mb,br,1.0f,br);
  set_uM(Mb,e->x,by+1.40f,e->z); cyl_sh(0.175f,0.27f,0.54f,7);
  /* shoulders: small caps on a wide frame */
  for(int si=-1;si<=1;si+=2){ float so[3]; m3v(M,si*0.29f,0.24f,0,so);
    set_uM(M,e->x+so[0],by+1.40f+so[1],e->z+so[2]); sphere_sh(0.085f,0.085f,0.085f,6,4); }
  /* tie: a darker sliver down the chest */
  glUniform3f(uTint,0.02f,0.03f,0.025f);
  { float to[3]; m3v(Mt,0,0,-0.22f,to);
    set_uM(M,e->x+to[0],by+1.42f+to[1],e->z+to[2]); box_sh(0.08f,0.42f,0.02f); }
  glUniform3f(uTint,sr+fl,sg+fl*0.9f,sb+fl*0.8f);
  /* arms: anticipation dip -> eased raise with overshoot -> recoil kick */
  for(int ai=0;ai<2;ai++){
    float t=e->armp, raise;
    if(ai){
      raise = t<0.22f ? -0.14f*sstep(t/0.22f)
                      : 1.45f*easeOutBack((t-0.22f)/0.78f);
      if(e->state==2) raise=1.45f*sstep(t);           /* eased lowering */
      if(e->state==3) raise=1.2f;
      raise += e->recoil*e->recoil*0.35f;
    } else raise = e->state==3? 1.2f : 0.0f;
    float swA=(ai?walk2:walk)*0.4f*e->moveb*clampf(1.0f-raise*2.0f,0,1);
    float eb=0.30f+0.55f*sstep(clampf(raise/1.45f,0,1));
    float sh[3]; m3v(M,ai?0.29f:-0.29f,0.24f,0,sh);
    float shx=e->x+sh[0],shy=by+1.40f+sh[1],shz=e->z+sh[2];
    float A[9],RX[9],RZ[9],S[9];
    m3rotX(RX,-raise+swA*e->fwdb); m3rotZ(RZ,-swA*e->latb*0.5f);
    m3mul(S,RX,RZ); m3mul(A,M,S);
    float uac[3]; m3v(A,0,-0.18f,0,uac);
    set_uM(A,shx+uac[0],shy+uac[1],shz+uac[2]); cyl_sh(0.072f,0.060f,0.38f,6);
    float elb[3]; m3v(A,0,-0.37f,0,elb);
    float ex=shx+elb[0],ey=shy+elb[1],ez=shz+elb[2];
    float F[9],RE[9]; m3rotX(RE,-raise+swA*e->fwdb-eb); m3mul(S,RE,RZ); m3mul(F,M,S);
    float fac[3]; m3v(F,0,-0.17f,0,fac);
    set_uM(F,ex+fac[0],ey+fac[1],ez+fac[2]); cyl_sh(0.058f,0.050f,0.36f,6);
    float hnd[3]; m3v(F,0,-0.36f,0,hnd);
    float hx2=ex+hnd[0],hy2=ey+hnd[1],hz2=ez+hnd[2];
    set_uM(F,hx2,hy2,hz2); box_sh(0.075f,0.13f,0.06f);    /* wedge mitt */
    if(ai&&e->type==0&&raise>0.05f){ /* pistol in the raised hand */
      glUniform3f(uTint,0.03f,0.035f,0.04f);
      float go[3]; m3v(F,0,-0.10f,-0.14f,go);
      set_uM(F,hx2+go[0],hy2+go[1],hz2+go[2]); box_sh(0.06f,0.09f,0.24f);
      glUniform3f(uTint,sr+fl,sg+fl*0.9f,sb+fl*0.8f);
    }
  }
  /* defined neck + cut-gem head with hard brow and jaw lines */
  set_uM(M,e->x,by+1.665f,e->z); cyl_sh(0.055f,0.070f,0.15f,6);
  set_uM(M,e->x,by+1.85f,e->z); sphere_sh(0.125f,0.16f,0.135f,6,4);
  { float jo[3]; m3v(M,0,-0.12f,-0.045f,jo);
    set_uM(M,e->x+jo[0],by+1.85f+jo[1],e->z+jo[2]); box_sh(0.16f,0.09f,0.18f); }
  { float bo[3]; m3v(M,0,0.075f,-0.075f,bo);
    set_uM(M,e->x+bo[0],by+1.85f+bo[1],e->z+bo[2]); box_sh(0.20f,0.035f,0.10f); }
  /* the eyes: emerald, flaring smoothly through the aim phase */
  float flare = 1.0f+e->flare*2.5f;
  glUniform1f(uEmis,1);
  glUniform3f(uTint,(0.25f+0.9f*(flare-1))*dim,1.8f*flare*dim,(0.8f*flare)*dim);
  for(int s=-1;s<=1;s+=2){
    float eo[3]; m3v(M,s*0.060f,0.015f,-0.125f,eo);
    set_uM(M,e->x+eo[0],by+1.85f+eo[1],e->z+eo[2]);
    box_sh(0.055f,0.038f,0.025f);
  }

  glUniform1f(uEmis,0);
  primArm=0;
}

/* third-person player avatar: compact, sleek, low-poly and readable from
 * behind. The whole figure hangs off a mid-body pivot so the dodge roll can
 * somersault it; the camera never rolls with it. */
static void draw_player(void){
  primArm=1;
  float M[9],R[9],X[9];
  int rolling = rollT>0;
  float rp = rolling? sstep(1.0f-rollT/0.42f) : 0;     /* roll progress 0..1 */
  float tuck = rolling? sinf(rp*PI) : 0;               /* curl: 0 -> 1 -> 0  */
  float tk = 1.0f-0.50f*tuck;                          /* anchors pull in    */
  float walk = sinf(bobT*7.5f), walk2 = sinf(bobT*7.5f+PI);
  float lean = rolling? rp*2*PI : 0.10f*pmoveb;        /* full forward flip  */
  m3rotY(R,avYaw); m3rotX(X,lean); m3mul(M,R,X);
  float pcy=py+0.55f+0.25f*tuck;     /* pivot lifts so the ball clears floor */

  float s = swingT>0? clampf(swingT/0.26f,0,1) : 0;    /* katana swing phase */
  int blade = s>0 || swingCD>0;                        /* + follow-through   */

  glUniform1f(uBump,0);
  glUniform1f(uGloss,0.55f);
  glUniform1f(uEmis,0.08f);
  glUniform3f(uTint,0.04f,0.05f,0.045f);

  /* legs — tucked hard during the roll */
  for(int li=0;li<2;li++){
    float side=li?0.14f:-0.14f;
    float sw = rolling? 1.0f : (li?walk:walk2)*0.42f*pmoveb;
    float kb = rolling? 1.5f : 0.12f+0.30f*clampf(0.5f-0.5f*(li?walk:walk2),0,1)*pmoveb;
    float hip[3]; m3v(M,side,0.37f*tk,0,hip);
    float hx=px+hip[0], hy=pcy+hip[1], hz=pz+hip[2];
    float UL[9],RX[9]; m3rotX(RX,sw); m3mul(UL,M,RX);
    float ulc[3]; m3v(UL,0,-0.22f,0,ulc);
    set_uM(UL,hx+ulc[0],hy+ulc[1],hz+ulc[2]); cyl_sh(0.10f,0.076f,0.46f,6);
    float knee[3]; m3v(UL,0,-0.44f,0,knee);
    float kx=hx+knee[0],ky=hy+knee[1],kz=hz+knee[2];
    float LL[9],RK[9]; m3rotX(RK,sw+kb); m3mul(LL,M,RK);
    float llc[3]; m3v(LL,0,-0.20f,0,llc);
    set_uM(LL,kx+llc[0],ky+llc[1],kz+llc[2]); cyl_sh(0.074f,0.050f,0.42f,6);
    float ank[3]; m3v(LL,0,-0.40f,0,ank);
    set_uM(LL,kx+ank[0],ky+ank[1],kz+ank[2]); box_sh(0.13f,0.06f,0.24f);
  }

  /* pelvis / jacket: same V-taper language as the agents */
  float Mt[9]; memcpy(Mt,M,36); m3scl(Mt,1.0f,1.0f,0.62f);
  glUniform3f(uTint,0.03f,0.04f,0.035f);
  { float o[3];
    m3v(M,0,0.49f*tk,0,o); set_uM(Mt,px+o[0],pcy+o[1],pz+o[2]); cyl_sh(0.175f,0.155f,0.24f,7);
    m3v(M,0,0.87f*tk,0,o); set_uM(Mt,px+o[0],pcy+o[1],pz+o[2]); cyl_sh(0.165f,0.25f,0.50f,7); }
  glUniform3f(uTint,0.05f,0.06f,0.055f);
  for(int si=-1;si<=1;si+=2){ float so[3]; m3v(M,si*0.27f,1.05f*tk,0,so);
    set_uM(M,px+so[0],pcy+so[1],pz+so[2]); sphere_sh(0.08f,0.08f,0.08f,6,4); }

  /* shoulders and arms. right arm: pistol raise on fire, katana sweep on
   * swing — wind-up across the left shoulder, cut down and across. */
  glUniform3f(uTint,0.04f,0.05f,0.045f);
  for(int ai=0;ai<2;ai++){
    float raise, swYaw=0;
    if(ai){
      float fr=clampf(fireCD/0.34f,0,1);
      raise = haspistol&&fr>0 ? (0.85f+0.50f*sstep(fr)) : 0.12f;
      if(s>0){ raise = 0.5f+0.9f*sinf(s*PI); swYaw = 0.9f-2.0f*sstep(s); }
      else if(blade) raise=0.35f;
    } else raise = 0.0f;
    if(rolling){ raise=1.2f; swYaw=0; }    /* both arms hug the tucked knees */
    float sw = rolling? 1.0f : (ai?walk2:walk)*0.32f*pmoveb*clampf(1.0f-raise*2.0f,0,1);
    float sh[3]; m3v(M,ai?0.27f:-0.27f,1.05f*tk,0,sh);
    float shx=px+sh[0], shy=pcy+sh[1], shz=pz+sh[2];
    float A[9],RX[9],RY[9],S[9];
    m3rotX(RX,-raise+sw); m3rotY(RY,swYaw); m3mul(S,RY,RX); m3mul(A,M,S);
    float uac[3]; m3v(A,0,-0.18f,0,uac);
    set_uM(A,shx+uac[0],shy+uac[1],shz+uac[2]); cyl_sh(0.068f,0.056f,0.36f,6);
    float elb[3]; m3v(A,0,-0.35f,0,elb);
    float ex=shx+elb[0],ey=shy+elb[1],ez=shz+elb[2];
    float F[9],RE[9]; m3rotX(RE,-raise+sw-0.16f-(ai&&s>0?0.35f:0.0f));
    m3mul(S,RY,RE); m3mul(F,M,S);
    float fac[3]; m3v(F,0,-0.16f,0,fac);
    set_uM(F,ex+fac[0],ey+fac[1],ez+fac[2]); cyl_sh(0.054f,0.046f,0.34f,6);
    float hnd[3]; m3v(F,0,-0.33f,0,hnd);
    float hx2=ex+hnd[0],hy2=ey+hnd[1],hz2=ez+hnd[2];
    set_uM(F,hx2,hy2,hz2); box_sh(0.07f,0.12f,0.055f);
    if(ai&&blade){
      /* the katana: dark blade, emissive emerald edge, square tsuba */
      float B[9],RB[9]; m3rotX(RB,-1.35f); m3mul(B,F,RB);
      float bo[3];
      glUniform3f(uTint,0.10f,0.16f,0.12f);
      m3v(B,0,-0.10f,0,bo);
      set_uM(B,hx2+bo[0],hy2+bo[1],hz2+bo[2]); box_sh(0.10f,0.02f,0.10f);
      glUniform3f(uTint,0.45f,0.55f,0.50f); glUniform1f(uEmis,0.30f);
      m3v(B,0,-0.62f,0,bo);
      set_uM(B,hx2+bo[0],hy2+bo[1],hz2+bo[2]); box_sh(0.022f,1.05f,0.045f);
      glUniform3f(uTint,0.30f,2.0f,0.9f); glUniform1f(uEmis,1.0f);
      m3v(B,-0.014f,-0.62f,0,bo);
      set_uM(B,hx2+bo[0],hy2+bo[1],hz2+bo[2]); box_sh(0.006f,1.05f,0.03f);
      glUniform1f(uEmis,0.08f);
      glUniform3f(uTint,0.04f,0.05f,0.045f);
    } else if(ai&&haspistol){
      glUniform3f(uTint,0.03f,0.035f,0.04f);
      float go[3]; m3v(F,0,-0.08f,-0.12f,go);
      set_uM(F,hx2+go[0],hy2+go[1],hz2+go[2]); box_sh(0.05f,0.08f,0.22f);
      glUniform3f(uTint,0.04f,0.05f,0.045f);
    }
  }

  /* neck / head */
  glUniform3f(uTint,0.06f,0.07f,0.065f);
  { float o[3];
    m3v(M,0,1.10f*tk,0,o); set_uM(M,px+o[0],pcy+o[1],pz+o[2]); cyl_sh(0.052f,0.062f,0.13f,6);
    m3v(M,0,1.29f*tk,0,o); set_uM(M,px+o[0],pcy+o[1],pz+o[2]); sphere_sh(0.125f,0.155f,0.135f,6,4);
    /* slim collar and sunglasses band for the Matrix feel */
    glUniform3f(uTint,0.02f,0.02f,0.02f);
    m3v(M,0,1.18f*tk,-0.04f,o); set_uM(M,px+o[0],pcy+o[1],pz+o[2]); box_sh(0.18f,0.045f,0.16f);
    m3v(M,0,1.30f*tk,-0.02f,o); set_uM(M,px+o[0],pcy+o[1],pz+o[2]); box_sh(0.20f,0.045f,0.12f); }
  glUniform1f(uEmis,0.0f);
  primArm=0;
}

static void draw_items(void){
  float M[9];
  for(int i=0;i<nitems;i++){
    if(items[i].taken)continue;
    float bob=floor_at(items[i].x,items[i].z)+0.45f+0.1f*sinf(wtime*2.5f+i);
    m3rotY(M,wtime*1.5f+i);
    glUniform1f(uEmis,1); glUniform1f(uBump,0); glUniform1f(uGloss,0.4f);
    if(items[i].type==0){ /* health: white cross-ish twin cubes */
      glUniform3f(uTint,1.5f,1.5f,1.6f);
      set_uM(M,items[i].x,bob,items[i].z); box_sh(0.30f,0.10f,0.10f);
      set_uM(M,items[i].x,bob,items[i].z); box_sh(0.10f,0.30f,0.10f);
    } else {              /* pistol pickup */
      glUniform3f(uTint,0.25f,1.5f,0.7f);
      set_uM(M,items[i].x,bob,items[i].z); box_sh(0.09f,0.13f,0.30f);
      float M2[9],R[9]; m3rotX(R,PI/3); m3mul(M2,M,R);
      set_uM(M2,items[i].x,bob-0.07f,items[i].z); box_sh(0.07f,0.16f,0.07f);
    }
  }
  glUniform1f(uEmis,0);
}

static void draw_shards(void){
  float M[9],RY[9],RX[9];
  glUniform1f(uEmis,1); glUniform1f(uBump,0); glUniform1f(uGloss,0.6f);
  for(int i=0;i<MAXSHARD;i++){
    Shard*s=&shards[i]; if(s->life<=0)continue;
    float a=s->life/s->max;
    m3rotY(RY,s->yaw); m3rotX(RX,s->pit); m3mul(M,RY,RX);
    glUniform3f(uTint,s->r*a*1.6f,s->g*a*1.6f,s->b*a*1.6f);
    set_uM(M,s->x,s->y,s->z);
    box_sh(s->sx,s->sy,s->sz);
  }
  glUniform1f(uEmis,0);
}

/* thrown pistols render as spinning dark hardware in the shader pass */
static void draw_thrown(void){
  float M[9],R[9];
  glUniform1f(uEmis,0.25f); glUniform1f(uBump,0); glUniform1f(uGloss,0.7f);
  for(int i=0;i<MAXBUL;i++){
    Bullet*b=&bul[i]; if(!b->on||b->kind!=1)continue;
    m3rotY(M,b->spin); m3rotX(R,b->spin*0.7f); m3mul(M,M,R);
    glUniform3f(uTint,0.10f,0.30f,0.18f);
    set_uM(M,b->x,b->y,b->z); box_sh(0.09f,0.13f,0.32f);
  }
  glUniform1f(uEmis,0);
}

static void billboard(float x,float y,float z,float s,float r,float g,float b,float a,float ryaw){
  float yr=ryaw*PI/180;
  float rx=cosf(yr), rz=sinf(yr);
  glColor4f(r,g,b,a);
  glTexCoord2f(0,0); glVertex3f(x-rx*s,y-s,z-rz*s);
  glTexCoord2f(1,0); glVertex3f(x+rx*s,y-s,z+rz*s);
  glTexCoord2f(1,1); glVertex3f(x+rx*s,y+s,z+rz*s);
  glTexCoord2f(0,1); glVertex3f(x-rx*s,y+s,z-rz*s);
}

/* oscilloscope trails + doppler-shaded bullet heads. my<0 mirrors into the
 * floor reflection. fixed-function additive pass. */
static void draw_bullets(float my){
  glLineWidth(2.0f);
  for(int i=0;i<MAXBUL;i++){
    Bullet*b=&bul[i]; if(!b->on)continue;
    float vr=radial_v(b->x,b->z,b->vx,b->vz);
    float r,g,bl; dopp_rgb(vr,&r,&g,&bl);
    glBegin(GL_LINE_STRIP);
    int n=b->tn;
    for(int k=0;k<n;k++){
      int idx=(b->th-n+k+TRAILN*2)%TRAILN;
      float a=(float)(k+1)/(n+1);
      /* a faint sine wobble across the trail: the oscilloscope read */
      float wob=sinf(k*1.7f+wtime*9.0f)*0.015f;
      glColor4f(r*a,g*a,bl*a,a*0.8f);
      glVertex3f(b->tr[idx][0],my*(b->tr[idx][1]+wob),b->tr[idx][2]);
    }
    glColor4f(r,g,bl,1);
    glVertex3f(b->x,my*b->y,b->z);
    glEnd();
  }
  /* heads */
  glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,texAlb[TX_GLOW]);
  glBegin(GL_QUADS);
  for(int i=0;i<MAXBUL;i++){
    Bullet*b=&bul[i]; if(!b->on||b->kind==1)continue;
    float vr=radial_v(b->x,b->z,b->vx,b->vz);
    float r,g,bl; dopp_rgb(vr,&r,&g,&bl);
    billboard(b->x,my*b->y,b->z,0.09f,r,g,bl,0.95f,pyaw+90);
    billboard(b->x,my*b->y,b->z,0.22f,r*0.4f,g*0.4f,bl*0.4f,0.5f,pyaw+90);
  }
  glEnd();
  glDisable(GL_TEXTURE_2D);
}

/* the aim-phase laser: a thin pulsing thread from agent gun to player */
static void draw_lasers(void){
  glLineWidth(1.0f);
  glBegin(GL_LINES);
  for(int i=0;i<nen;i++){
    Enemy*e=&en[i];
    if(e->state!=1||e->armp<0.6f)continue;
    float hx=e->x+sinf(e->yaw)*0.45f, hz=e->z-cosf(e->yaw)*0.45f;
    float a=0.10f+0.10f*sinf(wtime*30+e->phase)+0.25f*e->armp*e->state_t;
    glColor4f(1.6f*a,0.6f*a,0.2f*a,a);
    glVertex3f(hx,e->y+1.36f,hz);
    glVertex3f(px,py+1.28f,pz);
  }
  glEnd();
}

/* the katana's wake: an emerald crescent swept across the cut plane,
 * strongest at the blade and fading along the arc behind it */
static void draw_slash(void){
  if(swingT<=0)return;
  float s=clampf(swingT/0.26f,0,1);
  float yr=pyaw*PI/180.0f;
  float aS=yr+1.30f, aE=aS-2.40f*sstep(s);
  float cy=py+1.25f;
  glBegin(GL_TRIANGLE_STRIP);
  for(int k=0;k<=16;k++){
    float f=k/16.0f, a=aS+(aE-aS)*f;
    float yk=cy+0.40f-0.65f*f;                /* high left -> low right */
    float al=0.55f*f*f*(0.4f+0.6f*s);
    float sa=sinf(a),ca=-cosf(a);
    glColor4f(0.20f*al,1.00f*al,0.45f*al,al);
    glVertex3f(px+sa*0.50f,yk,pz+ca*0.50f);
    glVertex3f(px+sa*1.60f,yk-0.10f,pz+ca*1.60f);
  }
  glEnd();
}

static void draw_world(float camx,float camy,float camz){
  const LevelDef*L=&LEVELS[curlevel];
  glUseProgram(prog);
  glUniform3f(uCam,camx,camy,camz);
  glUniform1i(uAlb,0); glUniform1i(uNrm,1);
  glUniform1f(uTime,wtime); glUniform1f(uRain,0); glUniform1f(uAlpha,1);
  glUniform3f(uFog,L->fog[0],L->fog[1],L->fog[2]);

  /* pick 8 nearest lights (static + temp) */
  float lp[SHLIGHTS*4], lc[SHLIGHTS*3]; int ln=0;
  typedef struct{float d2;int i;int tmp;}LS; LS sl[MAXLIGHT+MAXTEMPL]; int sn=0;
  for(int i=0;i<nlights;i++){ float dx=lights[i].x-camx,dz=lights[i].z-camz;
    sl[sn++] = (LS){dx*dx+dz*dz,i,0}; }
  for(int i=0;i<MAXTEMPL;i++) if(templ_[i].life>0){ float dx=templ_[i].x-camx,dz=templ_[i].z-camz;
    sl[sn++] = (LS){dx*dx+dz*dz,i,1}; }
  for(int a=0;a<sn;a++)for(int b=a+1;b<sn;b++) if(sl[b].d2<sl[a].d2){LS t=sl[a];sl[a]=sl[b];sl[b]=t;}
  for(int k=0;k<sn && ln<SHLIGHTS;k++){
    if(sl[k].tmp){ TempL*t=&templ_[sl[k].i];
      lp[ln*4]=t->x;lp[ln*4+1]=t->y;lp[ln*4+2]=t->z;lp[ln*4+3]=t->r;
      lc[ln*3]=t->cr;lc[ln*3+1]=t->cg;lc[ln*3+2]=t->cb;
    } else { Light*l=&lights[sl[k].i];
      lp[ln*4]=l->x;lp[ln*4+1]=l->y;lp[ln*4+2]=l->z;lp[ln*4+3]=l->r;
      lc[ln*3]=l->cr;lc[ln*3+1]=l->cg;lc[ln*3+2]=l->cb; }
    ln++;
  }
  glUniform1i(uNL,ln);
  glUniform4fv(uLpos,SHLIGHTS,lp);
  glUniform3fv(uLcol,SHLIGHTS,lc);

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_NORMAL_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  /* ---- the cheap planar mirror: draw the movers y-flipped under the
   * world, then lay the glossy floor OVER them at partial alpha. demo
   * trick, not physics — but on black obsidian it reads as reflection. */
  refl=1;
  draw_agentsetc:;
  /* props read the glow sprite's white centre texel: base = uTint verbatim */
  glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texAlb[TX_GLOW]);
  if(refl){
    /* mirror only grounded agents near the camera — elevated figures have
     * no floor to reflect in, and far ones aren't worth the double draw */
    for(int i=0;i<nen;i++){
      float ddx=en[i].x-camx, ddz=en[i].z-camz;
      if(en[i].y>0.2f || ddx*ddx+ddz*ddz>22.0f*22.0f)continue;
      draw_agent(&en[i],0.6f);
    }
    draw_shards(); draw_thrown();
    refl=0;
    /* mirrored additive trails, faint */
    glUseProgram(0);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    draw_bullets(-1.0f);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glUseProgram(prog);
    /* floor, blended over the mirror image */
    glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texAlb[TX_FLOOR]);
    glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,texNrm[TX_FLOOR]);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(uAlpha,0.78f); glUniform1f(uGloss,1.0f);
    glUniform3f(uTint,L->floort[0],L->floort[1],L->floort[2]);
    glUniform1f(uBump,1); glUniform1f(uEmis,0);
    float I[9]; m3id(I); set_uM(I,0,0,0);
    glVertexPointer(3,GL_FLOAT,32,batch[1]);
    glNormalPointer(GL_FLOAT,32,batch[1]+3);
    glTexCoordPointer(2,GL_FLOAT,32,batch[1]+6);
    glDrawArrays(GL_QUADS,0,bn[1]/8);
    glDisable(GL_BLEND);
    glUniform1f(uAlpha,1);
    /* walls (with rain) and ceiling, opaque, in the sector's climate */
    int texof[2]={TX_WALL,TX_CEIL}; int bid[2]={0,2}; float gls[2]={0.55f,0.3f};
    for(int b=0;b<2;b++){
      glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texAlb[texof[b]]);
      glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,texNrm[texof[b]]);
      glUniform1f(uGloss,gls[b]); glUniform1f(uRain,b==0?1.0f:0.0f);
      float tk=b==0?1.0f:0.8f;
      glUniform3f(uTint,L->wallt[0]*tk,L->wallt[1]*tk,L->wallt[2]*tk);
      glVertexPointer(3,GL_FLOAT,32,batch[bid[b]]);
      glNormalPointer(GL_FLOAT,32,batch[bid[b]]+3);
      glTexCoordPointer(2,GL_FLOAT,32,batch[bid[b]]+6);
      glDrawArrays(GL_QUADS,0,bn[bid[b]]/8);
    }
    glUniform1f(uRain,0);
    goto draw_agentsetc;   /* fall through to the upright pass */
  }
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);

  glActiveTexture_(GL_TEXTURE0);
  for(int i=0;i<nen;i++){       /* beyond ~36u the fog has swallowed them */
    float ddx=en[i].x-camx, ddz=en[i].z-camz;
    if(ddx*ddx+ddz*ddz>38.0f*38.0f)continue;
    draw_agent(&en[i],1.0f);
  }
  if(gstate==ST_PLAY) draw_player();
  draw_items();
  draw_shards();
  draw_thrown();
  glUseProgram(0);

  /* additive pass: light orbs, particles, trails, lasers */
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE);
  glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,texAlb[TX_GLOW]);
  glBegin(GL_QUADS);
  for(int i=0;i<nlights;i++)
    billboard(lights[i].x,lights[i].y,lights[i].z,0.30f,
      lights[i].cr*0.22f,lights[i].cg*0.22f,lights[i].cb*0.22f,1,pyaw+90);
  glEnd();
  glDisable(GL_TEXTURE_2D);
  glPointSize(4);
  glBegin(GL_POINTS);
  for(int i=0;i<MAXPART;i++){
    Part*p=&parts[i]; if(p->life<=0)continue;
    float a=p->life/p->max;
    glColor4f(p->cr,p->cg,p->cb,a);
    glVertex3f(p->x,p->y,p->z);
  }
  glEnd();
  draw_bullets(1.0f);
  draw_lasers();
  if(gstate==ST_PLAY)draw_slash();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
}

/* ---------------------------------------------------------------- HUD
 * scanlines + signal glitch on damage. all text is the synthesized bitfont. */
static int rainInit=0;
static float rainX[40],rainSpd[40],rainPh[40]; static int rainLen[40];
static void draw_title_rain(void){
  if(!rainInit){ rainInit=1; unsigned sv=rngs; rngs=0xD161741u;
    for(int i=0;i<40;i++){ rainX[i]=frand()*WINW; rainSpd[i]=60+frand()*180;
      rainPh[i]=frand()*2000; rainLen[i]=6+(int)(frand()*9); }
    rngs=sv; }
  for(int i=0;i<40;i++){
    float span=WINH+rainLen[i]*20.0f;
    float hy=fmodf(rainPh[i]+gtime*rainSpd[i],span)-rainLen[i]*20.0f;
    for(int k=0;k<rainLen[i];k++){
      float gy=hy-k*20.0f; if(gy<-20||gy>WINH)continue;
      float a=(k==0)?0.9f:0.55f*(1.0f-(float)k/rainLen[i]);
      char cs[2]={0,0};
      unsigned h=ihash((unsigned)i*131u+(unsigned)k*17u+(unsigned)(gtime*(k==0?9:2)));
      cs[0]= (h&1)? 'A'+(h>>1)%26 : '0'+(h>>1)%10;
      if(k==0)glColor4f(0.75f,1.0f,0.8f,a); else glColor4f(0.05f,0.85f,0.35f,a);
      draw_text(rainX[i],gy,2.0f,cs);
    }
  }
}
static void draw_hud(void){
  glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
  glOrtho(0,WINW,WINH,0,-1,1);
  glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

  /* CRT scanlines, always */
  glColor4f(0,0,0,0.18f);
  glBegin(GL_QUADS);
  for(int y=0;y<WINH;y+=4){ glVertex2f(0,y);glVertex2f(WINW,y);glVertex2f(WINW,y+1.2f);glVertex2f(0,y+1.2f); }
  glEnd();

  if(gstate==ST_PLAY||gstate==ST_WIN){
    /* crosshair: dot + ticks */
    glColor4f(0.5f,1,0.7f,0.85f);
    glBegin(GL_QUADS);
    glVertex2f(WINW/2-1,WINH/2-1);glVertex2f(WINW/2+1,WINH/2-1);
    glVertex2f(WINW/2+1,WINH/2+1);glVertex2f(WINW/2-1,WINH/2+1);
    glEnd();
    glColor4f(0.5f,1,0.7f,0.4f);
    glBegin(GL_QUADS);
    glVertex2f(WINW/2-10,WINH/2-0.7f);glVertex2f(WINW/2-5,WINH/2-0.7f);
    glVertex2f(WINW/2-5,WINH/2+0.7f);glVertex2f(WINW/2-10,WINH/2+0.7f);
    glVertex2f(WINW/2+5,WINH/2-0.7f);glVertex2f(WINW/2+10,WINH/2-0.7f);
    glVertex2f(WINW/2+10,WINH/2+0.7f);glVertex2f(WINW/2+5,WINH/2+0.7f);
    glEnd();

    /* agents remaining, top right */
    { char b2[24]; snprintf(b2,24,"AGENTS %02d",nalive);
      glColor4f(0.6f,1.0f,0.75f,0.9f);
      draw_text(WINW-30-textw(b2,2.6f),26,2.6f,b2); }
    /* hp bar bottom left */
    glColor4f(0,0,0,0.5f);
    glBegin(GL_QUADS);glVertex2f(28,WINH-50);glVertex2f(232,WINH-50);glVertex2f(232,WINH-32);glVertex2f(28,WINH-32);glEnd();
    glColor4f(0.2f,0.95f,0.5f,0.85f);
    float hw=200*php/100.0f;
    glBegin(GL_QUADS);glVertex2f(30,WINH-48);glVertex2f(30+hw,WINH-48);glVertex2f(30+hw,WINH-34);glVertex2f(30,WINH-34);glEnd();
    glColor4f(0.8f,1,0.9f,0.8f); draw_text(30,WINH-72,2.0f,"SIGNAL");
    /* ammo / blade, bottom right */
    if(haspistol){ char am[8]; snprintf(am,8,"%02d",pammo);
      glColor4f(0.5f,1.0f,0.7f,0.95f);
      draw_text(WINW-30-textw(am,4.4f),WINH-64,4.4f,am);
      glColor4f(0.7f,1,0.85f,0.6f); draw_text(WINW-30-textw("ROUNDS",1.8f),WINH-84,1.8f,"ROUNDS");
    } else {
      glColor4f(0.7f,1.0f,0.85f,0.9f);
      draw_text(WINW-30-textw("BLADE",3),WINH-58,3,"BLADE");
    }
    /* level intro card */
    if(msgT>0){
      float a=clampf(msgT,0,1);
      char b2[32]; snprintf(b2,32,"SECTOR %d - %s",curlevel+1,LEVELS[curlevel].name);
      glColor4f(0.7f,1.0f,0.8f,a);
      draw_text((WINW-textw(b2,3.4f))/2,90,3.4f,b2);
      glColor4f(0.4f,0.9f,0.6f,a*0.8f);
      draw_text((WINW-textw("TIME MOVES WHEN YOU DO",2.0f))/2,134,2.0f,"TIME MOVES WHEN YOU DO");
    }
    /* damage: signal distortion — torn horizontal slices + red wash */
    if(dmgFlash>0){
      glColor4f(0.5f,0.04f,0.03f,dmgFlash*0.45f);
      glBegin(GL_QUADS);glVertex2f(0,0);glVertex2f(WINW,0);glVertex2f(WINW,WINH);glVertex2f(0,WINH);glEnd();
      glBlendFunc(GL_SRC_ALPHA,GL_ONE);
      for(int k=0;k<5;k++){
        float gy=frand()*WINH, gh=3+frand()*14, gx=(frand()-0.5f)*70*dmgFlash;
        glColor4f(0.05f,0.8f,0.3f,dmgFlash*0.35f);
        glBegin(GL_QUADS);
        glVertex2f(gx,gy);glVertex2f(WINW+gx,gy);glVertex2f(WINW+gx,gy+gh);glVertex2f(gx,gy+gh);
        glEnd();
      }
      glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    }
  }
  if(gstate==ST_TITLE){
    glColor4f(0,0,0,0.55f);
    glBegin(GL_QUADS);glVertex2f(0,0);glVertex2f(WINW,0);glVertex2f(WINW,WINH);glVertex2f(0,WINH);glEnd();
    glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    draw_title_rain();
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    /* shadowed wordmark */
    glColor4f(0.0f,0.25f,0.10f,0.9f);
    draw_text((WINW-textw("DOPPLER",13))/2+5,125,13,"DOPPLER");
    glColor4f(0.65f,1.0f,0.75f,1);
    draw_text((WINW-textw("DOPPLER",13))/2,120,13,"DOPPLER");
    glColor4f(0.35f,0.85f,0.55f,1);
    draw_text((WINW-textw("TIME MOVES WHEN YOU MOVE",2.6f))/2,250,2.6f,"TIME MOVES WHEN YOU MOVE");
    /* level select */
    for(int i=0;i<NLEVEL;i++){
      char b2[24]; snprintf(b2,24,"%d %s",i+1,LEVELS[i].name);
      float bw=textw(b2,2.6f)+40, bx=(WINW-bw)/2, by=330+i*62;
      if(i==curlevel){
        glColor4f(0.06f,0.55f,0.25f,0.35f+0.12f*sinf(gtime*5));
        glBegin(GL_QUADS);glVertex2f(bx,by-12);glVertex2f(bx+bw,by-12);
        glVertex2f(bx+bw,by+32);glVertex2f(bx,by+32);glEnd();
        glColor4f(0.8f,1,0.85f,1);
      } else glColor4f(0.3f,0.6f,0.42f,0.9f);
      draw_text(bx+20,by,2.6f,b2);
    }
    glColor4f(0.85f,1.0f,0.6f,0.7f+0.3f*sinf(gtime*4));
    draw_text((WINW-textw("CLICK TO JACK IN",3.2f))/2,548,3.2f,"CLICK TO JACK IN");
    glColor4f(0.35f,0.55f,0.42f,1);
    draw_text((WINW-textw("WASD MOVE - SPACE JUMP AND JUMP AGAIN - WALLS KICK BACK - SHIFT ROLL",1.55f))/2,608,1.55f,
      "WASD MOVE - SPACE JUMP AND JUMP AGAIN - WALLS KICK BACK - SHIFT ROLL");
    draw_text((WINW-textw("LMB FIRE - RMB KATANA - F THROW - 1-3 SECTOR",1.55f))/2,632,1.55f,
      "LMB FIRE - RMB KATANA - F THROW - 1-3 SECTOR");
  } else if(gstate==ST_DEAD){
    glColor4f(0,0,0,0.6f);
    glBegin(GL_QUADS);glVertex2f(0,0);glVertex2f(WINW,0);glVertex2f(WINW,WINH);glVertex2f(0,WINH);glEnd();
    glColor4f(1.0f,0.25f,0.18f,1);
    draw_text((WINW-textw("SIGNAL LOST",9))/2,250,9,"SIGNAL LOST");
    glColor4f(0.8f,0.85f,0.8f,0.85f);
    draw_text((WINW-textw("CLICK TO RE-ENTER",3))/2,430,3,"CLICK TO RE-ENTER");
    draw_text((WINW-textw("ESC FOR SECTOR SELECT",2))/2,480,2,"ESC FOR SECTOR SELECT");
  } else if(gstate==ST_WIN){
    glColor4f(0,0,0,0.45f);
    glBegin(GL_QUADS);glVertex2f(0,0);glVertex2f(WINW,0);glVertex2f(WINW,WINH);glVertex2f(0,WINH);glEnd();
    glColor4f(0.3f,1.0f,0.6f,1);
    draw_text((WINW-textw("SECTOR CLEAR",8))/2,250,8,"SECTOR CLEAR");
    glColor4f(0.8f,0.9f,0.85f,0.9f);
    char b2[64]; snprintf(b2,64,"%d SECONDS REAL - %d SIMULATED",(int)winT,(int)wtime);
    draw_text((WINW-textw(b2,2.6f))/2,400,2.6f,b2);
    draw_text((WINW-textw("CLICK TO REPLAY - ESC FOR SECTOR SELECT",2.2f))/2,460,2.2f,
      "CLICK TO REPLAY - ESC FOR SECTOR SELECT");
  }
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
  glMatrixMode(GL_PROJECTION); glPopMatrix();
  glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* ---------------------------------------------------------------- screenshot */
static void shot_ppm(const char*path){
  unsigned char*buf=malloc(WINW*WINH*3);
  glPixelStorei(GL_PACK_ALIGNMENT,1);
  glReadPixels(0,0,WINW,WINH,GL_RGB,GL_UNSIGNED_BYTE,buf);
  FILE*f=fopen(path,"wb");
  if(f){
    fprintf(f,"P6\n%d %d\n255\n",WINW,WINH);
    for(int y=WINH-1;y>=0;y--) fwrite(buf+y*WINW*3,1,WINW*3,f);
    fclose(f);
    printf("[doppler] wrote %s\n",path);
  }
  free(buf);
}

/* WASD -> world-space move direction, relative to the view yaw */
static void wasd_dir(int w,int s,int a,int d,float*mx,float*mz){
  float yr=pyaw*PI/180;
  float fx=sinf(yr),fz=-cosf(yr),rx=cosf(yr),rz=sinf(yr);
  *mx=0;*mz=0;
  if(w){*mx+=fx;*mz+=fz;} if(s){*mx-=fx;*mz-=fz;}
  if(d){*mx+=rx;*mz+=rz;} if(a){*mx-=rx;*mz-=rz;}
}

/* ---------------------------------------------------------------- main */
int main(int argc,char**argv){
  unsigned t0;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"--smoke"))smoke=1;
    else if(!strcmp(argv[i],"--seed")&&i+1<argc)gseed=(unsigned)strtoul(argv[++i],0,0);
    else if(!strcmp(argv[i],"--level")&&i+1<argc){
      long l=strtol(argv[++i],0,0);
      curlevel=(int)(l<0?0:l>=NLEVEL?NLEVEL-1:l);
    }
  }
  if(smoke) SDL_setenv("SDL_AUDIODRIVER","dummy",1);

  if(SDL_Init(SDL_INIT_VIDEO)<0){ fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1; }
  SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
  SDL_Window*win=SDL_CreateWindow("DOPPLER",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
    WINW,WINH,SDL_WINDOW_OPENGL);
  SDL_GLContext ctx=SDL_GL_CreateContext(win);
  if(!ctx){ fprintf(stderr,"GL: %s\n",SDL_GetError()); return 1; }
  SDL_GL_SetSwapInterval(smoke?0:1);
  load_gl();
  printf("[doppler] GL: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));

  t0=SDL_GetTicks(); gen_textures();
  printf("[doppler] textures synthesized in %ums\n",SDL_GetTicks()-t0);
  if(smoke){ /* sanity: every sector must carve and populate */
    for(int l=0;l<NLEVEL;l++){
      gen_level(l,gseed);
      printf("[doppler] sector %d %-8s: %d agents, %d quads\n",
        l+1,LEVELS[l].name,nen,(bn[0]+bn[1]+bn[2])/32);
      float sy=cellh((int)(startx/CELL),(int)(startz/CELL));
      if(nen<1||sy>100.0f||!circ_free(startx,startz,0.34f,sy)){
        fprintf(stderr,"[doppler] SMOKE FAIL: bad sector %d\n",l); return 1; }
    }
  }
  t0=SDL_GetTicks(); reset_game();
  printf("[doppler] world carved in %ums (%d quads)\n",SDL_GetTicks()-t0,(bn[0]+bn[1]+bn[2])/32);
  init_shaders();
  printf("[doppler] shaders up\n");

  SDL_AudioSpec want={0},have;
  want.freq=44100; want.format=AUDIO_F32SYS; want.channels=1; want.samples=512; want.callback=audio_cb;
  adev=SDL_OpenAudioDevice(0,0,&want,&have,0);
  if(adev){ audioOK=1; SDL_PauseAudioDevice(adev,0); printf("[doppler] audio up\n"); }
  else printf("[doppler] no audio device, running silent\n");

  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glClearColor(0.004f,0.012f,0.008f,1);

  int running=1, frame=0, wdown=0,adown=0,sdown=0,ddown=0,mdown=0;
  unsigned last=SDL_GetTicks();
  float titleYaw=0;
  gstate=ST_TITLE;

  while(running){
    SDL_Event ev;
    while(SDL_PollEvent(&ev)){
      if(ev.type==SDL_QUIT)running=0;
      else if(ev.type==SDL_KEYDOWN||ev.type==SDL_KEYUP){
        int d=ev.type==SDL_KEYDOWN;
        switch(ev.key.keysym.sym){
          case SDLK_w:wdown=d;break; case SDLK_a:adown=d;break;
          case SDLK_s:sdown=d;break; case SDLK_d:ddown=d;break;
          case SDLK_f: if(d&&gstate==ST_PLAY)throw_pistol(); break;
          case SDLK_SPACE:
            if(d&&gstate==ST_PLAY&&rollT<=0){
              float gh=ground_h(px,pz,py), wnx,wnz;
              if(py<=gh+0.001f){ pvy=7.5f; jumps=1; actT=0.20f; }
              else if(wall_kick(px,pz,py,&wnx,&wnz)){
                /* kick off the wall: up and away, air jump restored */
                pvy=7.4f; kvx=wnx*5.5f; kvz=wnz*5.5f; jumps=1; actT=0.20f;
                spawn_parts(8,px-wnx*0.4f,py+0.8f,pz-wnz*0.4f,2.5f, 0.30f,1.2f,0.6f);
                sfxp(V_STEP,1.6f);
              }
              else if(jumps>0){ jumps--; pvy=6.6f; actT=0.20f;
                spawn_parts(10,px,py+0.1f,pz,2.2f, 0.25f,1.1f,0.55f);
                sfxp(V_SWING,1.8f);
              }
            } break;
          case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_c:
          case SDLK_LSHIFT: case SDLK_RSHIFT:
            if(d&&gstate==ST_PLAY&&rollCD<=0&&rollT<=0
               &&py<=ground_h(px,pz,py)+0.05f){
              /* dodge roll along current input, or straight ahead */
              float mx,mz;
              wasd_dir(wdown,sdown,adown,ddown,&mx,&mz);
              float ml2=sqrtf(mx*mx+mz*mz);
              if(ml2<0.01f){ float yr2=pyaw*PI/180;
                mx=sinf(yr2); mz=-cosf(yr2); ml2=1; }
              rollDX=mx/ml2; rollDZ=mz/ml2;
              rollT=0.42f; rollCD=0.75f; actT=0.42f;
              sfxp(V_SWING,0.7f);
            } break;
          case SDLK_1: case SDLK_2: case SDLK_3:
            if(d&&gstate==ST_TITLE){ curlevel=ev.key.keysym.sym-SDLK_1; sfx(V_CLICK); }
            break;
          case SDLK_LEFT: if(d&&gstate==ST_TITLE){ curlevel=(curlevel+NLEVEL-1)%NLEVEL; sfx(V_CLICK); } break;
          case SDLK_RIGHT:if(d&&gstate==ST_TITLE){ curlevel=(curlevel+1)%NLEVEL; sfx(V_CLICK); } break;
          case SDLK_ESCAPE:
            if(d){
              if(gstate==ST_TITLE)running=0;
              else { gstate=ST_TITLE; SDL_SetRelativeMouseMode(SDL_FALSE); }
            } break;
        }
      }
      else if(ev.type==SDL_MOUSEMOTION && gstate==ST_PLAY && !smoke){
        pyaw  += ev.motion.xrel*0.13f;
        ppitch+= ev.motion.yrel*0.13f;
        ppitch=ppitch>89?89:ppitch<-89?-89:ppitch;
        mouseAcc+=fabsf((float)ev.motion.xrel)+fabsf((float)ev.motion.yrel);
      }
      else if(ev.type==SDL_MOUSEBUTTONDOWN && ev.button.button==SDL_BUTTON_LEFT){
        if(gstate==ST_TITLE){ reset_game(); gstate=ST_PLAY; SDL_SetRelativeMouseMode(SDL_TRUE); sfx(V_CLICK); }
        else if(gstate==ST_DEAD||gstate==ST_WIN){ reset_game(); gstate=ST_PLAY; SDL_SetRelativeMouseMode(SDL_TRUE); }
        else mdown=1;
      }
      else if(ev.type==SDL_MOUSEBUTTONUP && ev.button.button==SDL_BUTTON_LEFT)mdown=0;
      else if(ev.type==SDL_MOUSEBUTTONDOWN && ev.button.button==SDL_BUTTON_RIGHT && gstate==ST_PLAY)
        katana();
    }

    unsigned now=SDL_GetTicks();
    float dt=smoke?1.0f/60:(now-last)/1000.0f;
    last=now;
    if(dt>0.05f)dt=0.05f;
    gtime+=dt;

    /* smoke choreography: gen-check done above; now title shot, jack in,
     * stage an agent, trade fire, shatter it, screenshot the lot. */
    if(smoke){
      frame++;
      if(frame==25)shot_ppm("shot_title.ppm");
      if(frame==30){
        reset_game(); gstate=ST_PLAY;
        float best=0,besta=0;
        for(int k=0;k<64;k++){
          float a=k*2*PI/64;
          float d=ray_wall(px,EYE,pz,sinf(a),0,-cosf(a),40);
          if(d>best){best=d;besta=a;}
        }
        pyaw=besta*180/PI; ppitch=2;
      }
      if(frame==40&&nen>0){ /* stage a shooter dead ahead, mid-aim */
        float yr=pyaw*PI/180;
        for(float d=5.5f;d>2.0f;d-=0.3f){
          float ex=px+sinf(yr)*d, ez=pz-cosf(yr)*d;
          if(circ_free(ex,ez,0.4f,py)&&los(px,py+EYE,pz,ex,1.4f,ez)){
            en[0].x=en[0].lx=ex; en[0].z=en[0].lz=ez;
            en[0].y=ground_h(ex,ez,py);
            en[0].type=0; en[0].state=1; en[0].state_t=0.3f; break; }
        }
      }
      if(frame>=70&&frame<80&&nen>0){ /* track it for the camera */
        float dx=en[0].x-px, dz=en[0].z-pz;
        float d=sqrtf(dx*dx+dz*dz);
        pyaw=atan2f(dx,-dz)*180/PI;
        ppitch=atan2f(EYE-1.3f,d)*180/PI;
      }
      if(frame==76)fire();
      if(frame==84)shot_ppm("shot_game.ppm");
      if(frame==100&&nen>0)shatter_enemy(&en[0]);
      if(frame==106)shot_ppm("shot_shatter.ppm");
      if(frame>=130){ printf("[doppler] SMOKE OK\n"); running=0; }
    }

    if(gstate==ST_TITLE){ titleYaw+=dt*7; pyaw=titleYaw; ppitch=4;
      px=startx; pz=startz; tscale=1; wtime=gtime; }

    /* THE mechanic: world time follows your motion. walking, looking and
     * acting each push the target timescale toward 1; stillness lets it
     * sink to the MINTS creep. the player always moves in real time. */
    float wdt=dt;
    if(gstate==ST_PLAY){
      float mx,mz;
      wasd_dir(wdown,sdown,adown,ddown,&mx,&mz);
      float ml=sqrtf(mx*mx+mz*mz);
      float ox=px,oz=pz;
      if(rollT>0){                 /* the roll owns the legs while it runs */
        rollT-=dt;
        move_circ(&px,&pz,rollDX*8.5f*dt,rollDZ*8.5f*dt,0.34f,py);
      } else if(ml>0.01f){
        mx/=ml;mz/=ml;
        move_circ(&px,&pz,mx*5.0f*dt,mz*5.0f*dt,0.34f,py);
        stepT-=dt;
        if(stepT<=0&&py<=ground_h(px,pz,py)+0.01f){ sfx(V_STEP); stepT=0.40f; }
      }
      if(kvx*kvx+kvz*kvz>0.01f){   /* wall-kick momentum, easing off */
        move_circ(&px,&pz,kvx*dt,kvz*dt,0.34f,py);
        kvx-=kvx*clampf(dt*4.0f,0,1); kvz-=kvz*clampf(dt*4.0f,0,1);
      }
      if(rollCD>0)rollCD-=dt;
      pvx=(px-ox)/dt; pvz=(pz-oz)/dt;
      pmoveb=toward(pmoveb,(ml>0.01f||rollT>0)?1.0f:0.0f,dt*9.0f);
      bobT+=dt*(0.15f+0.85f*pmoveb);   /* stride phase never freezes mid-step */
      /* the avatar's facing eases toward the roll direction and back —
       * no yaw snap entering or leaving a sideways roll */
      avYaw=angto(avYaw, rollT>0? atan2f(rollDX,-rollDZ) : pyaw*PI/180.0f, dt*14.0f);

      pvy-=18.0f*dt;
      py += pvy*dt;
      float gh=ground_h(px,pz,py);
      if(py<gh){ if(pvy<-9.0f)sfxp(V_STEP,0.6f); py=gh; pvy=0; jumps=1; }
      int air = py>gh+0.01f;

      float look=clampf(mouseAcc*0.05f,0,1); mouseAcc=0;
      if(actT>0)actT-=dt;
      float target=(ml>0.01f||rollT>0||air)?1.0f:0.0f;
      if(look>target)target=look;
      if(actT>0)target=1.0f;
      target=MINTS+(1.0f-MINTS)*target;
      tscale+=(target-tscale)*clampf(dt*14.0f,0,1);
      if(smoke)tscale=1;          /* determinism for the harness */
      wdt=dt*tscale;
      wtime+=wdt;

      if(mdown)fire();
      if(fireCD>0)fireCD-=dt;
      if(swingCD>0)swingCD-=dt;
      if(swingT>0){
        float pt=swingT; swingT+=dt;
        if(pt<0.10f&&swingT>=0.10f)katana_strike();  /* one strike per swing */
        if(swingT>0.26f)swingT=0;
      }
      if(dmgFlash>0)dmgFlash-=dt;
      if(shake>0)shake-=dt*1.2f;
      if(msgT>0)msgT-=dt;
      winT+=dt;

      update_enemies(wdt);
      update_bullets(wdt);

      for(int i=0;i<nitems;i++){
        if(items[i].taken)continue;
        float dx=items[i].x-px,dz=items[i].z-pz;
        if(dx*dx+dz*dz<0.8f*0.8f && fabsf(floor_at(items[i].x,items[i].z)-py)<1.4f){
          items[i].taken=1; sfx(V_PICK);
          if(items[i].type==0){ php+=35; if(php>100)php=100; }
          else { haspistol=1; pammo+=items[i].amt; if(pammo>18)pammo=18; }
        }
      }
    }
    if(gstate==ST_DEAD){ wdt=dt*MINTS; wtime+=wdt; update_bullets(wdt); }
    if(gstate==ST_WIN){ wdt=dt*0.25f; wtime+=wdt; update_bullets(wdt); }  /* victory slow-mo */
    g_ats = gstate==ST_PLAY ? tscale : (gstate==ST_TITLE?1.0f:0.3f);

    for(int i=0;i<MAXPART;i++){
      Part*p=&parts[i]; if(p->life<=0)continue;
      p->life-=wdt; p->vy-=6.0f*wdt;
      p->x+=p->vx*wdt; p->y+=p->vy*wdt; p->z+=p->vz*wdt;
      float pg=floor_at(p->x,p->z);
      if(p->y<pg+0.02f){p->y=pg+0.02f;p->vy*=-0.3f;p->vx*=0.7f;p->vz*=0.7f;}
    }
    for(int i=0;i<MAXSHARD;i++){
      Shard*s=&shards[i]; if(s->life<=0)continue;
      s->life-=wdt; s->vy-=7.0f*wdt;
      s->x+=s->vx*wdt; s->y+=s->vy*wdt; s->z+=s->vz*wdt;
      s->yaw+=s->wy*wdt; s->pit+=s->wp*wdt;
      float sg=floor_at(s->x,s->z);
      if(s->y<sg+s->sy*0.5f){ s->y=sg+s->sy*0.5f; s->vy*=-0.35f; s->vx*=0.6f; s->vz*=0.6f;
        s->wy*=0.5f; s->wp*=0.5f; }
    }
    for(int i=0;i<MAXTEMPL;i++) if(templ_[i].life>0)templ_[i].life-=wdt;

    /* render */
    { const LevelDef*LV=&LEVELS[curlevel];
      glClearColor(LV->fog[0],LV->fog[1],LV->fog[2],1); }
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    { float zn=0.08f, t=zn*tanf(35*PI/180), a=(float)WINW/WINH;
      glFrustum(-t*a,t*a,-t,t,zn,80.0f); }
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float shx=shake>0?(frand()-0.5f)*shake*6:0;
    float shy=shake>0?(frand()-0.5f)*shake*6:0;
    float yr=pyaw*PI/180.0f;
    float fx=sinf(yr), fz=-cosf(yr), rx=cosf(yr), rz=sinf(yr);
    /* third-person boom with occlusion: cast at the desired offset, snap IN
     * the moment something blocks (never clip), ease back OUT smoothly.
     * Vertical follows a smoothed pivot so rolls and landings don't pop. */
    float pivy=(gstate==ST_PLAY?player_camh():EYE)+0.18f;
    if(camYs<0)camYs=pivy;
    camYs=toward(camYs,pivy,dt*10.0f);
    float ox2=-fx*4.05f+rx*0.70f, oz2=-fz*4.05f+rz*0.70f;
    float boom=sqrtf(ox2*ox2+oz2*oz2);
    float ux=ox2/boom, uz=oz2/boom;
    float hit=ray_wall(px,camYs,pz,ux,0,uz,boom+0.30f);
    float maxd=hit-0.28f; if(maxd<0.40f)maxd=0.40f;
    float tgt=boom<maxd?boom:maxd;
    if(tgt<camDist)camDist=tgt; else camDist=toward(camDist,tgt,dt*5.0f);
    float camx=px+ux*camDist;
    float camz=pz+uz*camDist;
    float camy=clampf(camYs+(gstate==ST_PLAY?sinf(bobT*7.0f)*0.02f:0.0f),
                      0.25f,wallh-0.15f);
    glRotatef(ppitch+shy,1,0,0);
    glRotatef(pyaw+shx,0,1,0);
    glTranslatef(-camx,-camy,-camz);

    draw_world(camx,camy,camz);
    draw_hud();

    SDL_GL_SwapWindow(win);
  }

  if(adev)SDL_CloseAudioDevice(adev);
  SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
