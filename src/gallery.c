/*
 * PS5 Media Gallery v0.1.3 — read-only screenshot/video viewer.
 * Inspired by BFpilot's browser-based payload architecture (MIT).
 * This is an independent, much smaller implementation.
 * Copyright 2026 YAHBALLON contributors. MIT license.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "web_asset.h"

#ifndef MEDIA_PORT
#define MEDIA_PORT 3737
#endif
#ifndef AV_BASE
#define AV_BASE "/user/av_contents"
#endif
#ifndef APP_ROOT
#define APP_ROOT "/user/app"
#endif
#ifndef APPMETA_ROOT
#define APPMETA_ROOT "/user/appmeta"
#endif
#ifndef SYS_APP_ROOT
#define SYS_APP_ROOT "/system_ex/app"
#endif
#ifndef SYSMETA_ROOT
#define SYSMETA_ROOT "/system_data/priv/appmeta"
#endif
#define MAX_MEDIA 12000
#define MAX_DEPTH 9
#define MAX_CLIENTS 24
#define MAX_JSON (12u*1024u*1024u)
#define PATHBUF 2048

static volatile int clients = 0;

typedef struct {
  char type[8];
  char app[40];
  char rel[PATHBUF];
  char name[256];
  time_t stamp;
  uint64_t bytes;
} media_t;

typedef struct { media_t *v; size_t len, cap; } media_list_t;
typedef struct { char *p; size_t len, cap; int failed; } textbuf_t;

static int send_all(int fd, const void *data, size_t size) {
  const char *p = (const char *)data;
  while(size) {
    ssize_t n = send(fd, p, size,
#ifdef MSG_NOSIGNAL
                     MSG_NOSIGNAL
#else
                     0
#endif
    );
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    size -= (size_t)n;
    p += n;
  }
  return 0;
}

static int header(int fd, int status, const char *kind, unsigned long long length,
                  const char *extra) {
  const char *desc = status==200?"OK":status==206?"Partial Content":
                     status==400?"Bad Request":status==403?"Forbidden":
                     status==404?"Not Found":status==405?"Method Not Allowed":
                     status==416?"Range Not Satisfiable":status==503?"Service Unavailable":"Error";
  char b[1200];
  int n = snprintf(b,sizeof(b),
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
    "Connection: close\r\nX-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'self'; img-src 'self' data:; media-src 'self'; "
    "style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
    "base-uri 'none'; object-src 'none'\r\n%s\r\n",
    status,desc,kind,length,extra?extra:"");
  return n>0 && (size_t)n<sizeof(b) ? send_all(fd,b,(size_t)n) : -1;
}
static void respond(int fd, int status, const char *kind, const char *body, int head) {
  size_t n = strlen(body);
  if(header(fd,status,kind,(unsigned long long)n,"Cache-Control: no-store\r\n")==0 && !head)
    send_all(fd,body,n);
}

static int tb_grow(textbuf_t *b, size_t n) {
  if(b->failed || n > MAX_JSON || b->len > MAX_JSON-n) {b->failed=1; return -1;}
  if(b->len+n+1 <= b->cap) return 0;
  size_t c=b->cap?b->cap:16384;
  while(c < b->len+n+1) c*=2;
  if(c>MAX_JSON+1) c=MAX_JSON+1;
  char *p=realloc(b->p,c);
  if(!p){b->failed=1;return -1;}
  b->p=p;b->cap=c;return 0;
}
static void tb_addn(textbuf_t *b,const char *s,size_t n) {
  if(tb_grow(b,n)) return;
  memcpy(b->p+b->len,s,n); b->len+=n; b->p[b->len]=0;
}
static void tb_add(textbuf_t *b,const char *s){tb_addn(b,s,strlen(s));}
static void tb_json(textbuf_t *b,const char *s){
  tb_add(b,"\"");
  for(const unsigned char *p=(const unsigned char *)s;*p;p++){
    if(*p=='"'||*p=='\\'){char t[2]={'\\',(char)*p};tb_addn(b,t,2);}
    else if(*p<32){char esc[7];snprintf(esc,sizeof(esc),"\\u%04x",*p);tb_add(b,esc);}
    else tb_addn(b,(const char *)p,1);
  }
  tb_add(b,"\"");
}
static void tb_num(textbuf_t *b,unsigned long long x){char n[40];snprintf(n,sizeof(n),"%llu",x);tb_add(b,n);}

static int is_titleid(const char *s){
  const char *pfx[]={"PPSA","CUSA","NPXS","NPUB","NPEB","NPJA","UP00","EP00"};
  if(!s) return 0;
  size_t n=strlen(s);
  if(n<9 || n>16) return 0;
  for(size_t i=0;i<sizeof(pfx)/sizeof(pfx[0]);i++){
    size_t x=strlen(pfx[i]);
    if(!strncasecmp(s,pfx[i],x)) {
      size_t digits=0;
      for(size_t j=x;j<n;j++) {if(isdigit((unsigned char)s[j]))digits++;else return 0;}
      return digits>=5;
    }
  }
  return 0;
}
static int safe_id(const char *s){
  size_t n=strlen(s);if(!n||n>32)return 0;
  for(size_t i=0;i<n;i++) if(!isalnum((unsigned char)s[i])&&s[i]!='_'&&s[i]!='-')return 0;
  return 1;
}
static void find_app_id(const char *rel,char out[40]) {
  snprintf(out,40,"Other");
  char first[40]="";
  const char *p=rel;
  while(*p){
    const char *end=strchr(p,'/');
    size_t n=end?(size_t)(end-p):strlen(p);
    if(n && n<=32){
      char segment[40];memcpy(segment,p,n);segment[n]=0;
      if(!first[0] && safe_id(segment) && end)snprintf(first,sizeof(first),"%s",segment);
      if(is_titleid(segment))snprintf(out,40,"%s",segment);
    }
    if(!end)break;
    p=end+1;
  }
  if(first[0]&&!strcmp(out,"Other"))snprintf(out,40,"%s",first);
}
static const char *file_type(const char *name,const char *type){
  const char *ext=strrchr(name,'.');if(!ext)return NULL;
  if(!strcmp(type,"photo")){
    if(!strcasecmp(ext,".jpg")||!strcasecmp(ext,".jpeg"))return "image/jpeg";
    if(!strcasecmp(ext,".png"))return "image/png";
    if(!strcasecmp(ext,".webp"))return "image/webp";
  } else {
    if(!strcasecmp(ext,".mp4")||!strcasecmp(ext,".m4v"))return "video/mp4";
    if(!strcasecmp(ext,".webm"))return "video/webm";
    if(!strcasecmp(ext,".mov"))return "video/quicktime";
  }
  return NULL;
}
static int push_media(media_list_t *a,const char *type,const char *rel,const char *name,const struct stat *st){
  if(a->len>=MAX_MEDIA)return -1;
  if(a->len==a->cap){size_t cap=a->cap?a->cap*2:256;media_t *v=realloc(a->v,cap*sizeof(*v));if(!v)return -1;a->v=v;a->cap=cap;}
  media_t *m=&a->v[a->len++];memset(m,0,sizeof(*m));
  snprintf(m->type,sizeof(m->type),"%s",type);
  snprintf(m->rel,sizeof(m->rel),"%s",rel);
  snprintf(m->name,sizeof(m->name),"%s",name);
  find_app_id(rel,m->app);m->stamp=st->st_mtime;m->bytes=(uint64_t)st->st_size;
  return 0;
}
static void scan(media_list_t *a,const char *root,const char *rel,const char *type,int depth){
  if(depth>MAX_DEPTH||a->len>=MAX_MEDIA)return;
  char path[PATHBUF];
  int n=snprintf(path,sizeof(path),"%s%s%s",root,*rel?"/":"",rel);
  if(n<0||(size_t)n>=sizeof(path))return;
  DIR *d=opendir(path);if(!d)return;
  struct dirent *e;
  while((e=readdir(d)) && a->len<MAX_MEDIA){
    if(e->d_name[0]=='.')continue;
    char next[PATHBUF],absolute[PATHBUF];
    n=snprintf(next,sizeof(next),"%s%s%s",rel,*rel?"/":"",e->d_name);
    if(n<0||(size_t)n>=sizeof(next))continue;
    n=snprintf(absolute,sizeof(absolute),"%s/%s",root,next);
    if(n<0||(size_t)n>=sizeof(absolute))continue;
    struct stat st;
    if(lstat(absolute,&st)!=0)continue; // skip symlink entirely
    if(S_ISDIR(st.st_mode)) scan(a,root,next,type,depth+1);
    else if(S_ISREG(st.st_mode) && file_type(e->d_name,type)) push_media(a,type,next,e->d_name,&st);
  }
  closedir(d);
}
static int media_sort(const void *aa,const void *bb){
  const media_t *a=aa,*b=bb;
  if(a->stamp>b->stamp)return -1;
  if(a->stamp<b->stamp)return 1;
  return strcmp(a->rel,b->rel);
}

static int valid_media_path(const char *rel){
  size_t n=strlen(rel);if(!n||n>=PATHBUF-128||rel[0]=='/')return 0;
  const char *s=rel;
  while(*s){
    const char *end=strchr(s,'/');size_t k=end?(size_t)(end-s):strlen(s);
    if(k==0||(k==1&&s[0]=='.')||(k==2&&s[0]=='.'&&s[1]=='.'))return 0;
    for(size_t i=0;i<k;i++)if(s[i]=='\\'||(unsigned char)s[i]<32)return 0;
    if(!end)break;
    s=end+1;
  }
  return 1;
}
static int decode_url(const char *url,char *out,size_t cap){
  size_t n=0;
  for(size_t i=0;url[i];i++){
    unsigned char c=(unsigned char)url[i];
    if(c=='%'){
      if(!isxdigit((unsigned char)url[i+1])||!isxdigit((unsigned char)url[i+2]))return -1;
      char hex[3]={url[i+1],url[i+2],0};c=(unsigned char)strtoul(hex,NULL,16);i+=2;
    }
    if(c==0||n+1>=cap)return -1;
    out[n++]=(char)c;
  }
  out[n]=0;return 0;
}

static int resolve_media(const char *type,const char *rel,char *out,size_t cap){
  if(!valid_media_path(rel)||!file_type(rel,type))return -1;
  char root[PATHBUF];char path[PATHBUF];
  if(snprintf(path,sizeof(path),"%s/%s/%s",AV_BASE,type,rel)>=(int)sizeof(path))return -1;
  if(snprintf(root,sizeof(root),"%s/%s",AV_BASE,type)>=(int)sizeof(root))return -1;
  char resolved_root[PATH_MAX];
  char resolved_file[PATH_MAX];
  if(!realpath(root,resolved_root)||!realpath(path,resolved_file))return -1;
  size_t n=strlen(resolved_root);
  if(strncmp(resolved_file,resolved_root,n)!=0||resolved_file[n]!='/')return -1;
  if(strlen(resolved_file)>=cap)return -1;
  snprintf(out,cap,"%s",resolved_file);
  return 0;
}

static int icon_path(const char *id,char *out,size_t cap){
  if(!safe_id(id))return -1;
  const char *bases[]={APP_ROOT,APPMETA_ROOT,SYS_APP_ROOT,SYSMETA_ROOT};
  const char *ends[]={"/sce_sys/icon0.png","/icon0.png"};
  for(size_t i=0;i<4;i++)for(size_t k=0;k<2;k++){
    if(snprintf(out,cap,"%s/%s%s",bases[i],id,ends[k])>=(int)cap)continue;
    struct stat st;
    if(lstat(out,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>8&&st.st_size<4*1024*1024)return 0;
  }
  return -1;
}
static void app_name(const char *id,char *name,size_t cap){
  snprintf(name,cap,"%s",id);
  if(!safe_id(id))return;
  const char *bases[]={APP_ROOT,SYS_APP_ROOT};
  char path[PATHBUF];
  for(size_t i=0;i<2;i++){
    snprintf(path,sizeof(path),"%s/%s/sce_sys/param.json",bases[i],id);
    FILE *f=fopen(path,"rb");if(!f)continue;
    char buf[65537];size_t n=fread(buf,1,sizeof(buf)-1,f);fclose(f);buf[n]=0;
    const char *key=strstr(buf,"\"titleName\"");if(!key)continue;
    const char *p=strchr(key,':');if(!p)continue;p++;
    while(*p&&isspace((unsigned char)*p))p++;
    if(*p!='"')continue;
    p++;
    size_t j=0;
    while(*p&&*p!='"'&&j+1<cap){
      if(*p=='\\'&&p[1]){p++; if(*p=='n'||*p=='r')continue;}
      name[j++]=*p++;
    }
    name[j]=0;
    if(j)return;
  }
}

static void serve_gallery(int fd,int head){
  media_list_t a={0};
  char root[PATHBUF];struct stat st;
  int photo=0,video=0;
  snprintf(root,sizeof(root),"%s/photo",AV_BASE);
  photo=(stat(root,&st)==0&&S_ISDIR(st.st_mode));if(photo)scan(&a,root,"","photo",0);
  snprintf(root,sizeof(root),"%s/video",AV_BASE);
  video=(stat(root,&st)==0&&S_ISDIR(st.st_mode));if(video)scan(&a,root,"","video",0);
  if(a.len>1)qsort(a.v,a.len,sizeof(media_t),media_sort);
  textbuf_t b={0};
  tb_add(&b,"{\"version\":\"0.1.3\",\"roots\":{\"photo\":");tb_add(&b,photo?"true":"false");
  tb_add(&b,",\"video\":");tb_add(&b,video?"true":"false");
  tb_add(&b,"},\"truncated\":");tb_add(&b,a.len>=MAX_MEDIA?"true":"false");
  tb_add(&b,",\"apps\":[");
  char **ids=calloc(a.len?a.len:1,sizeof(*ids));size_t ids_n=0;
  if(ids){
    for(size_t i=0;i<a.len;i++){
      int seen=0;for(size_t j=0;j<ids_n;j++)if(!strcmp(ids[j],a.v[i].app)){seen=1;break;}
      if(seen)continue;
      ids[ids_n++]=a.v[i].app;
      if(ids_n>1)tb_add(&b,",");
      char name[256],icon[PATHBUF];app_name(a.v[i].app,name,sizeof(name));
      tb_add(&b,"{\"id\":");tb_json(&b,a.v[i].app);tb_add(&b,",\"title\":");tb_json(&b,name);
      tb_add(&b,",\"icon\":");tb_add(&b,icon_path(a.v[i].app,icon,sizeof(icon))==0?"true":"false");
      tb_add(&b,"}");
    }
  }
  tb_add(&b,"],\"items\":[");
  for(size_t i=0;i<a.len;i++){
    const media_t *m=&a.v[i];if(i)tb_add(&b,",");
    tb_add(&b,"{\"type\":");tb_json(&b,m->type);
    tb_add(&b,",\"app\":");tb_json(&b,m->app);
    tb_add(&b,",\"path\":");tb_json(&b,m->rel);
    tb_add(&b,",\"name\":");tb_json(&b,m->name);
    tb_add(&b,",\"date\":");tb_num(&b,m->stamp<0?0:(unsigned long long)m->stamp);
    tb_add(&b,",\"size\":");tb_num(&b,m->bytes);tb_add(&b,"}");
    if(b.failed)break;
  }
  tb_add(&b,"]}");
  if(b.failed){respond(fd,503,"application/json; charset=utf-8","{\"error\":\"Gallery too large\"}",head);}
  else if(header(fd,200,"application/json; charset=utf-8",b.len,"Cache-Control: no-store\r\n")==0&&!head)send_all(fd,b.p,b.len);
  free(ids);free(a.v);free(b.p);
}

static void serve_file(int fd,const char *path,const char *mime,const char *range,int head){
  int f=open(path,O_RDONLY);if(f<0){respond(fd,404,"text/plain","Not found",head);return;}
  struct stat st;
  if(fstat(f,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<0){close(f);respond(fd,404,"text/plain","Not found",head);return;}
  uint64_t size=(uint64_t)st.st_size,begin=0,end=size?size-1:0;
  int partial=0;
  if(range&&*range){
    if(strncasecmp(range,"bytes=",6)!=0||!size){close(f);respond(fd,416,"text/plain","Invalid range",head);return;}
    const char *p=range+6;char *stop=NULL;
    if(*p=='-'){
      unsigned long long suffix=strtoull(p+1,&stop,10);
      if(stop==p+1||*stop||suffix==0){close(f);respond(fd,416,"text/plain","Invalid range",head);return;}
      begin=suffix>=size?0:size-suffix;
    }else{
      unsigned long long b=strtoull(p,&stop,10);
      if(stop==p||*stop!='-'||b>=size){close(f);respond(fd,416,"text/plain","Invalid range",head);return;}
      begin=b;p=stop+1;
      if(*p){unsigned long long e=strtoull(p,&stop,10);
        if(stop==p||*stop||e<b){close(f);respond(fd,416,"text/plain","Invalid range",head);return;}
        end=e<size?e:size-1;
      }
    }
    partial=1;
  }
  uint64_t count=size?(end-begin+1):0;
  char extra[256];
  if(partial)snprintf(extra,sizeof(extra),"Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\nCache-Control: private, max-age=3600\r\n",
    (unsigned long long)begin,(unsigned long long)end,(unsigned long long)size);
  else snprintf(extra,sizeof(extra),"Accept-Ranges: bytes\r\nCache-Control: private, max-age=3600\r\n");
  if(header(fd,partial?206:200,mime,count,extra)==0&&!head&&lseek(f,(off_t)begin,SEEK_SET)>=0){
    /* Keep streaming buffers off the small payload thread stack. */
    const size_t chunk_size=64*1024;
    char *chunk=malloc(chunk_size);
    if(!chunk){close(f);return;}
    while(count){
      size_t want=count<chunk_size?(size_t)count:chunk_size;
      ssize_t n=read(f,chunk,want);
      if(n<0&&errno==EINTR)continue;
      if(n<=0||send_all(fd,chunk,(size_t)n)!=0)break;
      count-=(size_t)n;
    }
    free(chunk);
  }
  close(f);
}
static void serve_media(int fd,const char *url,const char *range,int head){
  const char *slash=strchr(url,'/');
  if(!slash){respond(fd,404,"text/plain","Not found",head);return;}
  char type[16];size_t n=(size_t)(slash-url);
  if(n>=sizeof(type)){respond(fd,404,"text/plain","Not found",head);return;}
  memcpy(type,url,n);type[n]=0;
  if(strcmp(type,"photo")&&strcmp(type,"video")){respond(fd,404,"text/plain","Not found",head);return;}
  char rel[PATHBUF],path[PATHBUF];
  if(decode_url(slash+1,rel,sizeof(rel))!=0||resolve_media(type,rel,path,sizeof(path))!=0){
    respond(fd,404,"text/plain","Not found",head);return;
  }
  const char *mime=file_type(rel,type);
  if(!mime){respond(fd,404,"text/plain","Not found",head);return;}
  serve_file(fd,path,mime,range,head);
}


static pthread_mutex_t delete_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct {unsigned removed,failed;} delete_result_t;
static void delete_tree(const char *type,const char *rel,const char *app,int depth,delete_result_t *result){
  if(depth>MAX_DEPTH)return;
  char dir[PATHBUF];
  int n=snprintf(dir,sizeof(dir),"%s/%s%s%s",AV_BASE,type,*rel?"/":"",rel);
  if(n<0||(size_t)n>=sizeof(dir))return;
  DIR *d=opendir(dir);if(!d)return;
  struct dirent *e;
  while((e=readdir(d))){
    if(e->d_name[0]=='.')continue;
    char next[PATHBUF],path[PATHBUF];
    n=snprintf(next,sizeof(next),"%s%s%s",rel,*rel?"/":"",e->d_name);
    if(n<0||(size_t)n>=sizeof(next))continue;
    n=snprintf(path,sizeof(path),"%s/%s/%s",AV_BASE,type,next);
    if(n<0||(size_t)n>=sizeof(path))continue;
    struct stat st;if(lstat(path,&st))continue;
    if(S_ISDIR(st.st_mode))delete_tree(type,next,app,depth+1,result);
    else if(S_ISREG(st.st_mode)&&file_type(e->d_name,type)){
      char id[40];find_app_id(next,id);
      if(app&&strcmp(id,app))continue;
      if(resolve_media(type,next,path,sizeof(path))||unlink(path))result->failed++;
      else result->removed++;
    }
  }
  closedir(d);
}
static void delete_media(int fd,const char *url){
  delete_result_t result={0};
  pthread_mutex_lock(&delete_lock);
  if(!strcmp(url,"/api/delete/all")){
    delete_tree("photo","",NULL,0,&result);delete_tree("video","",NULL,0,&result);
  }else if(!strncmp(url,"/api/delete/app/",16)){
    char id[40];
    if(decode_url(url+16,id,sizeof(id))||!safe_id(id)){
      pthread_mutex_unlock(&delete_lock);respond(fd,400,"application/json","{\"error\":\"Invalid app\"}",0);return;
    }
    delete_tree("photo","",id,0,&result);delete_tree("video","",id,0,&result);
  }else if(!strncmp(url,"/api/delete/media/",18)){
    const char *part=url+18,*slash=strchr(part,'/');
    char type[16],rel[PATHBUF],path[PATHBUF];
    size_t n=slash?(size_t)(slash-part):sizeof(type);
    if(n>=sizeof(type))goto invalid;
    memcpy(type,part,n);type[n]=0;
    if((strcmp(type,"photo")&&strcmp(type,"video"))||decode_url(slash+1,rel,sizeof(rel))||resolve_media(type,rel,path,sizeof(path)))goto invalid;
    if(unlink(path))result.failed++;else result.removed++;
  }else{
invalid:
    pthread_mutex_unlock(&delete_lock);respond(fd,400,"application/json","{\"error\":\"Invalid media path\"}",0);return;
  }
  pthread_mutex_unlock(&delete_lock);
  char body[160];snprintf(body,sizeof(body),"{\"deleted\":%u,\"failed\":%u}",result.removed,result.failed);
  respond(fd,200,"application/json",body,0);
}

static void *client_thread(void *arg){
  int fd=*(int *)arg;free(arg);
  struct timeval timeout={.tv_sec=15,.tv_usec=0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  char req[8192];size_t len=0;int complete=0;
  while(len+1<sizeof(req)){
    ssize_t n=recv(fd,req+len,sizeof(req)-len-1,0);
    if(n<0&&errno==EINTR)continue;
    if(n<=0)break;
    len+=(size_t)n;req[len]=0;
    if(strstr(req,"\r\n\r\n")){complete=1;break;}
  }
  if(complete){
    char method[8],url[4096],version[20];
    if(sscanf(req,"%7s %4095s %19s",method,url,version)!=3){respond(fd,400,"text/plain","Invalid request",0);goto done;}
    int head=!strcmp(method,"HEAD");
    if(strcmp(method,"GET")&&strcmp(method,"DELETE")&& !head){respond(fd,405,"text/plain","Read-only gallery",0);goto done;}
    if(strncmp(version,"HTTP/1.",7)){respond(fd,400,"text/plain","Invalid HTTP version",head);goto done;}
    char *q=strchr(url,'?');if(q)*q=0;
    char range[160]="";int delete_confirmed=0;
    char *saveptr=NULL;
    for(char *line=strtok_r(req,"\r\n",&saveptr);line;line=strtok_r(NULL,"\r\n",&saveptr)){
      if(!strcasecmp(line,"X-Gallery-Confirm: delete"))delete_confirmed=1;
      if(!strncasecmp(line,"Range:",6)){
        const char *p=line+6;while(*p==' '||*p=='\t')p++;
        if(strlen(p)<sizeof(range))strcpy(range,p);
      }
    }
    if(!strcmp(method,"DELETE")){
      if(!delete_confirmed)respond(fd,403,"application/json","{\"error\":\"Confirmation required\"}",0);
      else delete_media(fd,url);
      goto done;
    }
    if(!strcmp(url,"/")||!strcmp(url,"/index.html")){
      if(header(fd,200,"text/html; charset=utf-8",gallery_html_size,"Cache-Control: no-store\r\n")==0&&!head)
        send_all(fd,gallery_html,gallery_html_size);
    }else if(!strcmp(url,"/api/gallery"))serve_gallery(fd,head);
    else if(!strncmp(url,"/media/",7))serve_media(fd,url+7,range,head);
    else if(!strncmp(url,"/icon/",6)){
      char id[48],path[PATHBUF];
      if(decode_url(url+6,id,sizeof(id))==0&&icon_path(id,path,sizeof(path))==0)
        serve_file(fd,path,"image/png",range,head);
      else respond(fd,404,"text/plain","No app icon",head);
    }else respond(fd,404,"text/plain","Not found",head);
  }
done:
  shutdown(fd,SHUT_RDWR);close(fd);
  __sync_sub_and_fetch(&clients,1);
  return NULL;
}

static void startup_info(void){
  char ip[INET_ADDRSTRLEN]="unavailable";
  struct ifaddrs *all=NULL;
  if(getifaddrs(&all)==0){
    for(struct ifaddrs *i=all;i;i=i->ifa_next){
      if(!i->ifa_addr||i->ifa_addr->sa_family!=AF_INET)continue;
      struct sockaddr_in *a=(struct sockaddr_in *)i->ifa_addr;
      unsigned long address=ntohl(a->sin_addr.s_addr);
      if(!address||(address>>24)==127)continue;
      inet_ntop(AF_INET,&a->sin_addr,ip,sizeof(ip));
      break;
    }
    freeifaddrs(all);
  }
  char message[512];
  snprintf(message,sizeof(message),"PS5 Media Gallery v0.1.3\nhttp://%s:%d/\nPhotos and videos | Auto-refresh: 10s",ip,MEDIA_PORT);
  puts(message);fflush(stdout);
#ifdef MEDIA_PS5
  /* PS5 notification packet: message begins at byte 45. */
  extern int sceKernelSendNotificationRequest(int,void *,size_t,int);
  unsigned char packet[3120]={0};
  snprintf((char *)packet+45,sizeof(packet)-45,"%s",message);
  sceKernelSendNotificationRequest(0,packet,sizeof(packet),0);
#endif
}

int main(void){
  signal(SIGPIPE,SIG_IGN);
  int s=socket(AF_INET,SOCK_STREAM,0);
  if(s<0){perror("socket");return 1;}
  int yes=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  struct sockaddr_in addr;memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;addr.sin_port=htons(MEDIA_PORT);addr.sin_addr.s_addr=htonl(INADDR_ANY);
  if(bind(s,(struct sockaddr *)&addr,sizeof(addr))!=0){perror("bind");close(s);return 2;}
  if(listen(s,32)!=0){perror("listen");close(s);return 3;}
  startup_info();
  printf("Gallery server: http://<PS5-IP>:%d/\n",MEDIA_PORT);
  printf("Gallery paths: %s/photo and %s/video\n",AV_BASE,AV_BASE);
  fflush(stdout);
  pthread_attr_t attr;
  int attr_rc=pthread_attr_init(&attr);
  if(attr_rc){fprintf(stderr,"Thread attributes: %d\n",attr_rc);close(s);return 4;}
  attr_rc=pthread_attr_setstacksize(&attr,1024*1024);
  if(attr_rc){fprintf(stderr,"Thread stack: %d\n",attr_rc);pthread_attr_destroy(&attr);close(s);return 4;}
  while(1){
    int fd=accept(s,NULL,NULL);
    if(fd<0){if(errno==EINTR)continue;perror("accept");sleep(1);continue;}
    if(__sync_add_and_fetch(&clients,1)>MAX_CLIENTS){
      __sync_sub_and_fetch(&clients,1);respond(fd,503,"text/plain","Busy",0);close(fd);continue;
    }
#ifdef SO_NOSIGPIPE
    setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
#endif
    int *arg=malloc(sizeof(*arg));
    if(!arg){__sync_sub_and_fetch(&clients,1);close(fd);continue;}
    *arg=fd;
    pthread_t thr;
    if(pthread_create(&thr,&attr,client_thread,arg)!=0){free(arg);__sync_sub_and_fetch(&clients,1);close(fd);continue;}
    pthread_detach(thr);
  }
  return 0;
}
