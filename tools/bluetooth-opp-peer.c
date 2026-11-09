/* Owned virtual BR/EDR protocol peer. No host Bluetooth adapter or guest API hooks. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include "src/shared/mainloop.h"
#include "src/shared/timeout.h"
#include "emulator/bthost.h"
#include "bluetooth-dun.h"
#include <time.h>
#define CHANNEL 9
#define RECORD_HANDLE 0x10001u
#define OPP_MTU 1024
static struct bthost *host;
static int sk=-1;
static uint16_t rfhandle,rfcid;
static unsigned rfchannel=CHANNEL;
static bool push_mode,hold_put,client_held,spp_mode,spp_waiting_disconnect;
/* DUN: a phone's Dial-up Networking service, the channel's bytes going to a Hayes modem and PPP (bluetooth-dun.h). */
static bool dun_mode;
static uint16_t service_uuid(void){return dun_mode?0x1103:spp_mode?0x1101:0x1105;}
static const char* service_name(void){return dun_mode?"Dial-up Networking":spp_mode?"Frond SPP endpoint":"Frond OPP endpoint";}
static const char* gate_path;
static uint8_t remote_address[6];
static const char* output_path;
static const char* pin_code="1234";
static bool completed,disconnected,selftest,client_failed,push_link_closed;
static uint8_t payload[4096],obex[8192],last_response[16];
static size_t payload_size,obex_size,last_response_size;
static bool type_ok,obex_connected;
static uint32_t declared_size;
struct bytes{uint8_t b[2048];size_t n;};
static void add(struct bytes* b,const void* p,size_t n){assert(n<=sizeof(b->b)-b->n);memcpy(b->b+b->n,p,n);b->n+=n;}
static void u8(struct bytes*b,uint8_t x){add(b,&x,1);}
static void u16(struct bytes*b,uint16_t x){u8(b,x>>8);u8(b,x);}
static void u32(struct bytes*b,uint32_t x){u16(b,x>>16);u16(b,x);}
static uint16_t be16(const uint8_t*p){return (p[0]<<8)|p[1];}
static uint32_t be32(const uint8_t*p){return ((uint32_t)be16(p)<<16)|be16(p+2);}
static size_t seq(struct bytes*b){u8(b,0x35);u8(b,0);return b->n-1;}
static void endseq(struct bytes*b,size_t at){assert(b->n-at-1<=255);b->b[at]=b->n-at-1;}
static void uuid(struct bytes*b,uint16_t x){u8(b,0x19);u16(b,x);}
static void attr(struct bytes*b,uint16_t x){u8(b,0x09);u16(b,x);}
struct element{uint8_t type;const uint8_t* value;size_t size,span;};
static bool element(const uint8_t*p,size_t n,struct element*e){
 if(!n)return false;unsigned k=p[0]&7;size_t h=1,z;e->type=p[0]>>3;
 if(!e->type)z=0;
 else if(k<5)z=(size_t)1<<k;
 else{unsigned w=1u<<(k-5);if(n<1+w)return false;h+=w;z=0;for(unsigned i=1;i<h;++i){if(z>65535)return false;z=(z<<8)|p[i];}}
 if(h>n||z>n-h)return false;e->value=p+h;e->size=z;e->span=h+z;return true;
}
static bool has_uuid(const uint8_t*p,size_t n,uint16_t wanted,unsigned depth){
 if(depth>8)return false;struct element e;if(!element(p,n,&e))return false;
 if(e.type==3){
  static const uint8_t tail[]={0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb};
  if(e.size==2)return be16(e.value)==wanted;
  if(e.size==4)return be32(e.value)==wanted;
  return e.size==16&&be32(e.value)==wanted&&!memcmp(e.value+4,tail,12);
 }
 if(e.type==6||e.type==7){size_t at=0;while(at<e.size){struct element child;if(!element(e.value+at,e.size-at,&child))return false;if(has_uuid(e.value+at,child.span,wanted,depth+1))return true;at+=child.span;}}
 return false;
}
struct selection{uint16_t lo[32],hi[32];size_t n;};
static bool selection(const uint8_t*p,size_t n,struct selection*s,size_t*used){
 struct element list;if(!element(p,n,&list)||list.type!=6)return false;size_t at=0;s->n=0;
 while(at<list.size){struct element e;if(s->n==32||!element(list.value+at,list.size-at,&e)||e.type!=1||(e.size!=2&&e.size!=4))return false;
  s->lo[s->n]=be16(e.value);s->hi[s->n]=e.size==2?s->lo[s->n]:be16(e.value+2);if(s->lo[s->n]>s->hi[s->n])return false;++s->n;at+=e.span;}
 *used=list.span;return s->n!=0;
}
static bool selected(const struct selection*s,uint16_t id){for(size_t i=0;i<s->n;++i)if(id>=s->lo[i]&&id<=s->hi[i])return true;return false;}
static struct bytes record(const struct selection*s){
 struct bytes b={0};size_t all=seq(&b),a,c;
 if(selected(s,0)){attr(&b,0);u8(&b,0x0a);u32(&b,RECORD_HANDLE);}
 if(selected(s,1)){attr(&b,1);a=seq(&b);uuid(&b,service_uuid());if(dun_mode)uuid(&b,0x1201);endseq(&b,a);}
 if(selected(s,4)){attr(&b,4);a=seq(&b);c=seq(&b);uuid(&b,0x0100);endseq(&b,c);c=seq(&b);uuid(&b,3);u8(&b,8);u8(&b,CHANNEL);endseq(&b,c);if(!spp_mode&&!dun_mode){c=seq(&b);uuid(&b,8);endseq(&b,c);}endseq(&b,a);}
 if(selected(s,5)){attr(&b,5);a=seq(&b);uuid(&b,0x1002);endseq(&b,a);}
 if(selected(s,9)){attr(&b,9);a=seq(&b);c=seq(&b);uuid(&b,service_uuid());u8(&b,9);u16(&b,0x0100);endseq(&b,c);endseq(&b,a);}
 if(selected(s,0x100)){const char* name=service_name();attr(&b,0x100);u8(&b,0x25);u8(&b,strlen(name));add(&b,name,strlen(name));}
 if(!spp_mode&&!dun_mode&&selected(s,0x303)){attr(&b,0x303);a=seq(&b);u8(&b,8);u8(&b,0xff);endseq(&b,a);}
 endseq(&b,all);return b;
}
struct sdplink{uint16_t handle,cid;};static struct sdplink links[32];static unsigned link_count;
static void sdp_reply(struct sdplink*l,uint8_t opcode,uint16_t transaction,const struct bytes*parameters){struct bytes b={0};u8(&b,opcode);u16(&b,transaction);u16(&b,parameters->n);add(&b,parameters->b,parameters->n);bthost_send_cid(host,l->handle,l->cid,b.b,b.n);}
static void sdp_error(struct sdplink*l,uint16_t t,uint16_t code){struct bytes b={0};u16(&b,code);sdp_reply(l,1,t,&b);}
static void sdp_data(const void*data,uint16_t length,void*context){
 struct sdplink*l=context;const uint8_t*p=data;if(length<5)return;uint16_t t=be16(p+1);if(be16(p+3)!=length-5){sdp_error(l,t,3);return;}
 uint8_t op=p[0];p+=5;size_t n=length-5,at=0;bool match=true;uint16_t limit;struct element pattern;struct selection attrs={0};
 if(op==2||op==6){if(!element(p,n,&pattern)||pattern.type!=6){sdp_error(l,t,3);return;}match=has_uuid(p,pattern.span,service_uuid(),0)||has_uuid(p,pattern.span,0x1002,0);at=pattern.span;}
 else if(op==4){if(n<4||be32(p)!=RECORD_HANDLE){sdp_error(l,t,2);return;}at=4;}
 else{sdp_error(l,t,3);return;}
 if(n-at<2){sdp_error(l,t,3);return;}limit=be16(p+at);at+=2;
 if(op!=2){size_t used;if(!selection(p+at,n-at,&attrs,&used)){sdp_error(l,t,3);return;}at+=used;}
 if(at>=n){sdp_error(l,t,3);return;}uint8_t continuation=p[at++];size_t offset=0;
 if(continuation==2&&n-at==2)offset=be16(p+at);else if(continuation||n!=at){sdp_error(l,t,5);return;}
 struct bytes response={0};
 if(op==2){if(!limit||offset){sdp_error(l,t,3);return;}u16(&response,match?1:0);u16(&response,match?1:0);if(match)u32(&response,RECORD_HANDLE);u8(&response,0);sdp_reply(l,3,t,&response);}
 else{struct bytes value={0};if(op==6){size_t outer=seq(&value);if(match){struct bytes r=record(&attrs);add(&value,r.b,r.n);}endseq(&value,outer);}else value=record(&attrs);
  if(limit<7||offset>value.n){sdp_error(l,t,3);return;}size_t count=value.n-offset;if(count>limit)count=limit;
  u16(&response,count);add(&response,value.b+offset,count);if(offset+count<value.n){u8(&response,2);u16(&response,offset+count);}else u8(&response,0);sdp_reply(l,op==6?7:5,t,&response);
 }
 printf("SDP opcode=%u match=%u channel=%u\n",op,match,CHANNEL);fflush(stdout);
}
static void sdp_connected(uint16_t h,uint16_t c,void*p){assert(link_count<32);struct sdplink*l=&links[link_count++];l->handle=h;l->cid=c;bthost_add_cid_hook(host,h,c,sdp_data,l);}
static uint8_t uih_fcs(const uint8_t*p){
 uint8_t crc=0xff;for(unsigned i=0;i<2;++i){crc^=p[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^((crc&1)?0xe0:0);}return 0xff-crc;
}
static struct bytes uih_frame(const void*p,size_t n){
 assert(n<=1024);struct bytes frame={0};u8(&frame,(rfchannel*2<<2)|(push_mode?3:1));u8(&frame,0xef);if(n<=127)u8(&frame,(n<<1)|1);else{u8(&frame,(n<<1)&255);u8(&frame,n>>7);}add(&frame,p,n);u8(&frame,uih_fcs(frame.b));return frame;
}
static void send_obex(const void*p,size_t n){assert(n<=sizeof(last_response));memcpy(last_response,p,n);last_response_size=n;if(!selftest){struct bytes frame=uih_frame(p,n);bthost_send_cid(host,rfhandle,rfcid,frame.b,frame.n);}}
static void obex_status(uint8_t status){uint8_t p[]={status,0,3};send_obex(p,sizeof(p));}
static void save_payload(void){if(selftest)return;int fd=open(output_path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);size_t at=0;while(at<payload_size){ssize_t n=write(fd,payload+at,payload_size-at);assert(n>0);at+=n;}assert(!close(fd));}
static bool put_headers(const uint8_t*p,size_t n){
 for(size_t at=0;at<n;){uint8_t id=p[at];size_t bytes;
  if((id>>6)<2){if(n-at<3)return false;bytes=be16(p+at+1);if(bytes<3||bytes>n-at)return false;
   if(id==0x42){const char type[]="application/x-frond-bt";size_t z=bytes-3;if(z&&p[at+bytes-1]==0)--z;type_ok=z==sizeof(type)-1&&!memcmp(p+at+3,type,z);}
   if(id==0x48||id==0x49){if(bytes-3>sizeof(payload)-payload_size)return false;memcpy(payload+payload_size,p+at+3,bytes-3);payload_size+=bytes-3;}
  }else{bytes=(id>>6)==2?2:5;if(bytes>n-at)return false;if(id==0xc3)declared_size=be32(p+at+1);}
  at+=bytes;
 }return true;
}
static void obex_packet(const uint8_t*p,size_t n){
 uint8_t op=p[0];printf("OBEX opcode=%02x bytes=%zu body=%zu\n",op,n,payload_size);fflush(stdout);
 if(op==0x80){if(n<7||(p[3]>>4)!=1||be16(p+5)<255){obex_status(0xc0);return;}obex_connected=true;payload_size=0;type_ok=false;declared_size=0;const uint8_t response[]={0xa0,0,7,0x10,0,OPP_MTU>>8,OPP_MTU&255};send_obex(response,sizeof(response));return;}
 if(!obex_connected){obex_status(0xc0);return;}
 if(op==2||op==0x82){if(!put_headers(p+3,n-3)){obex_status(0xc0);return;}if(hold_put && payload_size){printf("OPP_PUT_HELD bytes=%zu\n",payload_size);fflush(stdout);return;}if(!(op&0x80)){obex_status(0x90);return;}
  bool valid=payload_size>0 && (!declared_size||declared_size==payload_size);
  /* This endpoint accepts a bounded standard OPP object. */
  if(!valid){obex_status(0xc0);return;}save_payload();completed=true;obex_status(0xa0);printf("OPP_PUT_COMPLETE bytes=%zu\n",payload_size);fflush(stdout);return;
 }
 if(op==0x81){disconnected=true;obex_status(0xa0);puts("OPP_DISCONNECT_COMPLETE");fflush(stdout);return;}
 if(op==0xff){payload_size=0;type_ok=false;declared_size=0;obex_status(0xa0);puts("OPP_ABORT");fflush(stdout);return;}
 obex_status(0xc0);
}
static void rfcomm_data(const void*data,uint16_t length,void*p){
 if(spp_mode){assert(length<=4096-payload_size);memcpy(payload+payload_size,data,length);payload_size+=length;struct bytes frame=uih_frame(data,length);if(!selftest)bthost_send_cid(host,rfhandle,rfcid,frame.b,frame.n);printf("SPP_RECEIVE bytes=%u total=%zu\n",length,payload_size);if(payload_size==4096){if(!selftest)save_payload();completed=true;puts("SPP_RECEIVE_COMPLETE bytes=4096");}fflush(stdout);return;}
 if(length>sizeof(obex)-obex_size){obex_status(0xc0);return;}memcpy(obex+obex_size,data,length);obex_size+=length;
 while(obex_size>=3){size_t want=be16(obex+1);if(want<3||want>OPP_MTU){obex_status(0xc0);obex_size=0;return;}if(obex_size<want)return;obex_packet(obex,want);memmove(obex,obex+want,obex_size-want);obex_size-=want;}
}
/* DUN server: the channel's bytes go to the modem, and what it sends back goes out in UIH frames
   small enough for any RFCOMM frame size the guest picked. */
static struct frond_dun* dun;
static bool dun_open,dun_hooked;static uint16_t dun_hooked_handle;
static char dun_state[32]="off";
static struct bytes dun_sent;
static uint64_t monotonic_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static void dun_report(void){const char* now=frond_dun_state(dun);if(strcmp(now,dun_state)){snprintf(dun_state,sizeof(dun_state),"%s",now);printf("DUN_STATE %s\n",dun_state);fflush(stdout);}}
static void dun_flush(void){
 uint8_t out[120];size_t n;
 while((n=frond_dun_output(dun,out,sizeof(out),monotonic_ns()))>0){
  if(selftest){add(&dun_sent,out,n);continue;}
  struct bytes frame=uih_frame(out,n);bthost_send_cid(host,rfhandle,rfcid,frame.b,frame.n);
 }
 dun_report();
}
static void dun_data(const void*data,uint16_t length,void*p){if(dun_open){frond_dun_input(dun,data,length,monotonic_ns());dun_flush();}}
static bool dun_tick(void*p){dun_flush();return true;}
static void dun_connected(uint16_t h,uint16_t c){
 rfhandle=h;rfcid=c;if(dun_open)frond_dun_close(dun,monotonic_ns());dun_open=true;frond_dun_open(dun,monotonic_ns());
 /* bthost takes the first hook for a channel, so one per ACL link is enough. */
 if(!dun_hooked||dun_hooked_handle!=h){bthost_add_rfcomm_chan_hook(host,h,CHANNEL,dun_data,NULL);dun_hooked=true;dun_hooked_handle=h;}
 printf("DUN_CONNECTED channel=%u handle=%04x\n",CHANNEL,h);fflush(stdout);dun_report();
}
static void dun_closed(uint16_t h){if(!dun_open||h!=rfhandle)return;dun_open=false;frond_dun_close(dun,monotonic_ns());puts("DUN_CLOSED");fflush(stdout);dun_report();}
static void rfcomm_connected(uint16_t h,uint16_t c,void*p,bool ok){assert(ok);if(dun_mode){dun_connected(h,c);return;}rfhandle=h;rfcid=c;bthost_add_rfcomm_chan_hook(host,h,CHANNEL,rfcomm_data,NULL);printf("RFCOMM_CONNECTED channel=%u handle=%04x\n",CHANNEL,h);fflush(stdout);}
static void rfcomm_closed(uint16_t h,uint16_t c,void*p){if(dun_mode){dun_closed(h);return;}printf("RFCOMM_L2CAP_CLOSED complete=%u\n",completed);fflush(stdout);}
static void send_h4(const struct iovec*iov,int n,void*p){
 uint8_t packet[65540];size_t count=0;for(int i=0;i<n;++i){assert(iov[i].iov_len<=sizeof(packet)-count);memcpy(packet+count,iov[i].iov_base,iov[i].iov_len);count+=iov[i].iov_len;}
 size_t at=0;while(at<count){ssize_t z=send(sk,packet+at,count-at,MSG_NOSIGNAL);if(z<0&&errno==EINTR)continue;assert(z>0);at+=z;}
}
/* Host-initiated OPP uses standard SDP ServiceSearch + ServiceAttribute. */
static struct {uint16_t handle,local,remote;} cidmap[16];static unsigned cid_count;
static uint16_t sdp_handle,sdp_remote;static uint32_t remote_record;static uint16_t transaction=1;
static uint8_t sdp_attributes[2048],continuation[16];static size_t sdp_size,continuation_size;
static unsigned client_state;static size_t sent_body;static uint16_t remote_obex_mtu=255;
static bool have_connection_id;static uint32_t connection_id;
static void map_cids(const uint8_t*p,size_t n){
 if(n<21||p[0]!=2||(p[7]|p[8]<<8)!=1||p[9]!=3||(p[11]!=8||p[12]!=0))return;
 /* L2CAP signaling fields are little endian, unlike SDP/OBEX. */
 uint16_t h=(p[1]|p[2]<<8)&0x0fff,remote=p[13]|p[14]<<8,local=p[15]|p[16]<<8,result=p[17]|p[18]<<8;
 if(result)return;unsigned i;for(i=0;i<cid_count;++i)if(cidmap[i].handle==h&&cidmap[i].local==local)break;
 assert(i<16);if(i==cid_count)++cid_count;cidmap[i].handle=h;cidmap[i].local=local;cidmap[i].remote=remote;
}
static uint16_t remote_cid(uint16_t h,uint16_t local){for(unsigned i=0;i<cid_count;++i)if(cidmap[i].handle==h&&cidmap[i].local==local)return cidmap[i].remote;fprintf(stderr,"No remote CID for %04x/%04x\n",h,local);exit(2);}
static void client_send(struct bytes b){assert(b.n>=3&&b.n<=127&&b.n<=remote_obex_mtu);b.b[1]=b.n>>8;b.b[2]=b.n;struct bytes frame=uih_frame(b.b,b.n);bthost_send_cid(host,rfhandle,rfcid,frame.b,frame.n);}
static void add_connection(struct bytes*b){if(have_connection_id){u8(b,0xcb);u32(b,connection_id);}}
static void send_metadata(void){
 struct bytes b={0};u8(&b,2);u16(&b,0);add_connection(&b);const char name[]="FrondBTPayload.frbt";
 u8(&b,1);u16(&b,3+2*sizeof(name));for(size_t i=0;i<sizeof(name);++i)u16(&b,(uint8_t)name[i]);
 const char type[]="application/x-frond-bt";u8(&b,0x42);u16(&b,3+sizeof(type));add(&b,type,sizeof(type));u8(&b,0xc3);u32(&b,4096);/* Exact original Palm Exchange headers observed in sender wire capture. */u8(&b,0xcf);u32(&b,0x4672424f);u8(&b,0xc0);u32(&b,1);client_state=2;client_send(b);
}
static void send_body(void){
 struct bytes b={0};size_t count=4096-sent_body;if(count>64)count=64;bool final=sent_body+count==4096;
 u8(&b,final?0x82:2);u16(&b,0);add_connection(&b);u8(&b,final?0x49:0x48);u16(&b,3+count);
 for(size_t i=0;i<count;++i){size_t at=sent_body+i;uint8_t v=(uint8_t)((at*73+17)^((at>>8)*31));payload[at]=v;u8(&b,v);}sent_body+=count;client_state=3;client_send(b);
}
static bool check_gate(void*p){if(spp_waiting_disconnect&&access(gate_path,F_OK)==0){spp_waiting_disconnect=false;client_state=5;bthost_hci_disconnect(host,rfhandle,0x13);puts("SPP_DISCONNECT_RELEASED");fflush(stdout);}if(client_held && access(gate_path,F_OK)==0){client_held=false;send_body();puts("OPP_PUSH_RELEASED");fflush(stdout);}return true;}
static void client_packet(const uint8_t*p,size_t n){
 printf("OBEX_RESPONSE state=%u code=%02x body_sent=%zu bytes=%zu\n",client_state,p[0],sent_body,n);fflush(stdout);
 uint8_t expected=client_state==1||client_state==4||(client_state==3&&sent_body==4096)?0xa0:0x90;
 if(p[0]!=expected){client_failed=true;printf("OPP_PUSH_REFUSED state=%u code=%02x submitted=%zu\n",client_state,p[0],sent_body);fflush(stdout);client_state=5;bthost_hci_disconnect(host,rfhandle,0x13);return;}

 if(client_state==1){assert(p[0]==0xa0&&n>=7);remote_obex_mtu=be16(p+5);assert(remote_obex_mtu>=255);
  for(size_t at=7;at<n;){uint8_t id=p[at];size_t z;if((id>>6)<2){assert(n-at>=3);z=be16(p+at+1);assert(z>=3&&z<=n-at);}else z=(id>>6)==2?2:5;assert(z<=n-at);if(id==0xcb){have_connection_id=true;connection_id=be32(p+at+1);}at+=z;}
  send_metadata();return;
 }
 if(client_state==2){assert(p[0]==0x90);send_body();return;}
 if(client_state==3){if(sent_body<4096){assert(p[0]==0x90);if(gate_path && sent_body==64){client_held=true;puts("OPP_PUSH_HELD bytes=64");fflush(stdout);return;}send_body();return;}assert(p[0]==0xa0);completed=true;payload_size=4096;save_payload();puts("OPP_PUSH_COMPLETE bytes=4096");fflush(stdout);struct bytes b={0};u8(&b,0x81);u16(&b,0);add_connection(&b);client_state=4;client_send(b);return;}
 if(client_state==4){assert(p[0]==0xa0);disconnected=true;puts("OPP_PUSH_DISCONNECT_COMPLETE");fflush(stdout);client_state=5;bthost_hci_disconnect(host,rfhandle,0x13);return;}
 assert(false);
}
static void client_data(const void*data,uint16_t length,void*p){
 assert(length<=sizeof(obex)-obex_size);memcpy(obex+obex_size,data,length);obex_size+=length;
 while(obex_size>=3){size_t n=be16(obex+1);assert(n>=3&&n<=OPP_MTU);if(n>obex_size)return;client_packet(obex,n);memmove(obex,obex+n,obex_size-n);obex_size-=n;}
}
static size_t spp_echoed;
static void client_spp_send(void){size_t at=sent_body,count=4096-at;if(count>120)count=120;assert(count);for(size_t i=at;i<at+count;++i)payload[i]=(uint8_t)((i*41+23)^((i>>7)*13));sent_body+=count;struct bytes frame=uih_frame(payload+at,count);bthost_send_cid(host,rfhandle,rfcid,frame.b,frame.n);}
static void client_spp_data(const void*data,uint16_t length,void*p){assert(length<=sent_body-spp_echoed&&!memcmp(data,payload+spp_echoed,length));spp_echoed+=length;printf("SPP_ECHO_VERIFIED bytes=%zu\n",spp_echoed);fflush(stdout);if(spp_echoed!=sent_body)return;if(spp_echoed<4096){client_spp_send();return;}completed=true;payload_size=4096;save_payload();puts("SPP_ECHO_COMPLETE bytes=4096");fflush(stdout);if(gate_path){spp_waiting_disconnect=true;puts("SPP_DISCONNECT_HELD");fflush(stdout);return;}client_state=5;bthost_hci_disconnect(host,rfhandle,0x13);}
/* DUN client, for the peer test: dials, then brings LCP and IPCP up the way a Palm would. */
static char dun_text[4096];static size_t dun_text_size;
static uint8_t dun_frame[1600];static size_t dun_frame_size;static bool dun_escaped,dun_dialled,dun_lcp_up,dun_ipcp_up,dun_ipcp_ours;
static uint8_t dun_ipcp_id=1;static uint32_t dun_address,dun_dns;
static uint16_t ppp_fcs(const uint8_t*p,size_t n){uint16_t f=0xffff;for(size_t i=0;i<n;++i){f^=p[i];for(int b=0;b<8;++b)f=(f&1)?(f>>1)^0x8408:f>>1;}return f;}
static void dun_client_send(const void*p,size_t n){const uint8_t*b=p;for(size_t at=0;at<n;at+=120){size_t z=n-at<120?n-at:120;struct bytes frame=uih_frame(b+at,z);bthost_send_cid(host,rfhandle,rfcid,frame.b,frame.n);}}
/* One PPP packet with the address and control fields and every control character escaped. */
static void dun_ppp_send(uint16_t protocol,uint8_t code,uint8_t id,const uint8_t*options,size_t size){
 struct bytes raw={0},out={0};u8(&raw,0xff);u8(&raw,3);u16(&raw,protocol);u8(&raw,code);u8(&raw,id);u16(&raw,4+size);add(&raw,options,size);
 uint16_t fcs=ppp_fcs(raw.b,raw.n)^0xffff;u8(&raw,fcs&255);u8(&raw,fcs>>8);
 u8(&out,0x7e);for(size_t i=0;i<raw.n;++i){uint8_t c=raw.b[i];if(c==0x7e||c==0x7d||c<32){u8(&out,0x7d);u8(&out,c^0x20);}else u8(&out,c);}u8(&out,0x7e);dun_client_send(out.b,out.n);
}
static void dun_ipcp_request(void){uint8_t o[18]={3,6,0,0,0,0,129,6,0,0,0,0,131,6,0,0,0,0};
 for(int i=0;i<4;++i){o[2+i]=dun_address>>(24-8*i);o[8+i]=o[14+i]=dun_dns>>(24-8*i);}dun_ppp_send(0x8021,1,dun_ipcp_id++,o,sizeof(o));}
static void dun_finish(void){
 completed=true;disconnected=true;printf("DUN_IPCP_UP address=%u.%u.%u.%u dns=%u.%u.%u.%u\n",dun_address>>24,(dun_address>>16)&255,(dun_address>>8)&255,dun_address&255,dun_dns>>24,(dun_dns>>16)&255,(dun_dns>>8)&255,dun_dns&255);fflush(stdout);
 client_state=5;bthost_hci_disconnect(host,rfhandle,0x13);
}
static void dun_packet(const uint8_t*f,size_t n){
 if(n<8||f[0]!=0xff||f[1]!=3)return;uint16_t protocol=be16(f+2);uint8_t code=f[4],id=f[5];size_t length=be16(f+6);if(length<4||length>n-4)return;
 const uint8_t*options=f+8;size_t size=length-4;printf("DUN_PPP protocol=%04x code=%u\n",protocol,code);fflush(stdout);
 if(protocol==0xc021&&code==1){dun_ppp_send(0xc021,2,id,options,size);if(dun_lcp_up)return;dun_lcp_up=true;puts("DUN_LCP_UP");fflush(stdout);dun_ipcp_request();return;}
 if(protocol==0x8021&&code==1){dun_ppp_send(0x8021,2,id,options,size);dun_ipcp_ours=true;}
 if(protocol==0x8021&&code==3){for(size_t at=0;at+6<=size&&options[at+1]==6;at+=6){uint32_t v=be32(options+at+2);if(options[at]==3)dun_address=v;if(options[at]==129)dun_dns=v;}dun_ipcp_request();return;}
 if(protocol==0x8021&&code==2&&dun_address)dun_ipcp_up=true;
 if(dun_ipcp_up&&dun_ipcp_ours)dun_finish();
}
static void dun_client_data(const void*data,uint16_t length,void*p){
 const uint8_t*b=data;
 if(!dun_dialled){assert(length<sizeof(dun_text)-dun_text_size);memcpy(dun_text+dun_text_size,b,length);dun_text_size+=length;dun_text[dun_text_size]=0;
  char*at=strstr(dun_text,"CONNECT");if(!at||!strstr(at,"\r\n"))return;*strstr(at,"\r\n")=0;printf("DUN_DIALLED %s\n",at);fflush(stdout);dun_dialled=true;
  /* The Palm starts LCP itself rather than waiting for the server's request. */
  const uint8_t magic[]={5,6,0x12,0x34,0x56,0x78};dun_ppp_send(0xc021,1,1,magic,sizeof(magic));return;}
 for(uint16_t i=0;i<length;++i){uint8_t c=b[i];
  if(c==0x7e){if(dun_frame_size>=4&&ppp_fcs(dun_frame,dun_frame_size)==0xf0b8)dun_packet(dun_frame,dun_frame_size-2);dun_frame_size=0;dun_escaped=false;}
  else if(c==0x7d)dun_escaped=true;
  else{if(dun_escaped)c^=0x20;dun_escaped=false;if(dun_frame_size<sizeof(dun_frame))dun_frame[dun_frame_size++]=c;}}
}
static void client_rfcomm(uint16_t h,uint16_t c,void*p,bool ok){assert(ok);rfhandle=h;rfcid=remote_cid(h,c);if(dun_mode){bthost_add_rfcomm_chan_hook(host,h,rfchannel,dun_client_data,NULL);const char dial[]="ATZ\rATDT*99#\r";dun_client_send(dial,sizeof(dial)-1);printf("CLIENT_RFCOMM channel=%u\n",rfchannel);fflush(stdout);return;}if(spp_mode){bthost_add_rfcomm_chan_hook(host,h,rfchannel,client_spp_data,NULL);client_spp_send();return;}bthost_add_rfcomm_chan_hook(host,h,rfchannel,client_data,NULL);struct bytes b={0};u8(&b,0x80);u16(&b,0);u8(&b,0x10);u8(&b,0);u16(&b,OPP_MTU);client_state=1;client_send(b);printf("CLIENT_RFCOMM channel=%u local=%04x remote=%04x\n",rfchannel,c,rfcid);fflush(stdout);}
static unsigned find_channel(const uint8_t*p,size_t n,unsigned depth){
 struct element e;if(depth>8||!element(p,n,&e)||e.type!=6)return 0;bool rfcomm=false;size_t at=0;
 while(at<e.size){struct element child;if(!element(e.value+at,e.size-at,&child))return 0;
  if(rfcomm&&child.type==1&&child.size==1&&child.value[0]&&child.value[0]<=30)return child.value[0];
  rfcomm=child.type==3&&child.size==2&&be16(child.value)==3;
  unsigned channel=find_channel(e.value+at,child.span,depth+1);if(channel)return channel;at+=child.span;
 }return 0;
}
static void request_attributes(void){
 struct bytes b={0},params={0};u32(&params,remote_record);u16(&params,512);const uint8_t ids[]={0x35,3,9,0,4};add(&params,ids,sizeof(ids));u8(&params,continuation_size);add(&params,continuation,continuation_size);u8(&b,4);u16(&b,++transaction);u16(&b,params.n);add(&b,params.b,params.n);bthost_send_cid(host,sdp_handle,sdp_remote,b.b,b.n);
}
static void client_sdp(const void*data,uint16_t length,void*p){
 const uint8_t*b=data;assert(length>=5&&be16(b+3)==length-5);printf("CLIENT_SDP opcode=%u bytes=%u\n",b[0],length);fflush(stdout);
 if(b[0]==3){if(length==10 && !be16(b+5) && !be16(b+7) && !b[9]){client_failed=true;puts(dun_mode?"DUN_SERVICE_UNAVAILABLE":spp_mode?"SPP_SERVICE_UNAVAILABLE":"OPP_SERVICE_UNAVAILABLE");fflush(stdout);mainloop_quit();return;}assert(length>=14&&be16(b+5)>=1&&be16(b+7)==1);remote_record=be32(b+9);assert(b[13]==0);request_attributes();return;}
 assert(b[0]==5&&length>=8);size_t n=be16(b+5);assert(n<=length-8&&n<=sizeof(sdp_attributes)-sdp_size);memcpy(sdp_attributes+sdp_size,b+7,n);sdp_size+=n;continuation_size=b[7+n];assert(continuation_size<=sizeof(continuation)&&8+n+continuation_size==length);
 if(continuation_size){memcpy(continuation,b+8+n,continuation_size);request_attributes();return;}
 rfchannel=find_channel(sdp_attributes,sdp_size,0);assert(rfchannel&&rfchannel<=30);printf("CLIENT_%s_CHANNEL %u\n",dun_mode?"DUN":spp_mode?"SPP":"OPP",rfchannel);fflush(stdout);assert(bthost_connect_rfcomm(host,sdp_handle,rfchannel,client_rfcomm,NULL));
}
static bool search_service(void*p){const uint8_t req[]={2,0,1,0,8,0x35,3,0x19,0x11,service_uuid()&255,0,1,0};bthost_send_cid(host,sdp_handle,sdp_remote,req,sizeof(req));return false;}
static void client_sdp_open(uint8_t code,const void*data,uint16_t length,void*p){const uint8_t*b=data;assert(code==3&&length==8&&!b[4]&&!b[5]);sdp_remote=b[0]|b[1]<<8;bthost_add_cid_hook(host,sdp_handle,0x004f,client_sdp,NULL);timeout_add(500,search_service,NULL,NULL);}
static void client_connected(uint16_t h,void*p){sdp_handle=h;const uint8_t request[]={1,0,0x4f,0};assert(bthost_l2cap_req(host,h,2,request,sizeof(request),client_sdp_open,NULL));}
static bool deadline(void*p){fprintf(stderr,"Owned OPP peer deadline\n");mainloop_quit();return false;}

static void command(uint16_t op,uint8_t status,const void*data,uint8_t len,void*p){
 printf("CMD opcode=%04x status=%u len=%u\n",op,status,len);
 if(op==0x1009&&len>=7){const uint8_t*b=(const uint8_t*)data+1;printf("BDADDR_LE ");for(unsigned i=0;i<6;++i)printf("%02x",b[i]);puts("");}
 if(op==0x0c13&&dun_mode){assert(!status);/* Class of device: a phone (smartphone) with networking, object transfer and telephony. */uint8_t cmd[]={1,0x24,0x0c,3,0x0c,0x02,0x52};assert(send(sk,cmd,sizeof(cmd),MSG_NOSIGNAL)==sizeof(cmd));}
 else if(op==0x0c13||op==0x0c24){assert(!status);bthost_write_scan_enable(host,3);}
 if(op==0x0c1a){assert(!status);printf("READY %s channel=9\n",service_name());}fflush(stdout);
}
static void ready(void){if(push_mode){bthost_hci_connect(host,remote_address,0);return;}uint8_t cmd[252]={1,0x13,0x0c,248};const char*name=dun_mode?"Frond Phone":"Frond OPP endpoint";memcpy(cmd+4,name,strlen(name)+1);assert(send(sk,cmd,sizeof(cmd),MSG_NOSIGNAL)==sizeof(cmd));}
static void read_h4(int fd,uint32_t events,void*p){
 static uint8_t data[65540];static size_t count;if(events&(EPOLLERR|EPOLLHUP)){mainloop_quit();return;}
 ssize_t n=recv(fd,data+count,sizeof(data)-count,MSG_DONTWAIT);if(n<0&&(errno==EAGAIN||errno==EINTR))return;if(n<=0){mainloop_quit();return;}count+=n;
 for(;;){if(!count)break;size_t want;if(data[0]==4){if(count<3)break;want=3+data[2];}else if(data[0]==2){if(count<5)break;want=5+data[3]+((size_t)data[4]<<8);}else if(data[0]==3){if(count<4)break;want=4+data[3];}else{fprintf(stderr,"Unexpected H4 type %u\n",data[0]);exit(2);}assert(want<=sizeof(data));if(count<want)break;bool disconnected_link=push_mode && client_state==5 && want==7 && data[0]==4 && data[1]==5 && data[2]==4 && !data[3] && (data[4]|data[5]<<8)==rfhandle;if(spp_mode&&!push_mode&&completed&&want==7&&data[0]==4&&data[1]==5&&data[2]==4&&!data[3]&&(data[4]|data[5]<<8)==rfhandle){disconnected=true;puts("SPP_HCI_DISCONNECT_COMPLETE");mainloop_quit();}if(dun&&!push_mode&&want==7&&data[0]==4&&data[1]==5&&!data[3])dun_closed(data[4]|data[5]<<8);map_cids(data,want);bthost_receive_h4(host,data,want);if(disconnected_link){if(spp_mode)disconnected=true;push_link_closed=true;puts("HCI_DISCONNECT_COMPLETE");fflush(stdout);mainloop_quit();}memmove(data,data+want,count-want);count-=want;}
}
static void stopped(int sig,void*p){mainloop_quit();}
static void debug(const char*line,void*p){printf("H4 %s\n",line);fflush(stdout);}
static void tests(void){
 const uint8_t known[]={3,0xef};assert(uih_fcs(known)==0x70);const uint8_t reply[]={0xa0,0,7,0x10,0,4,0};struct bytes frame=uih_frame(reply,sizeof(reply));assert(frame.n==11&&frame.b[0]==0x49&&frame.b[1]==0xef&&frame.b[2]==0x0f&&!memcmp(frame.b+3,reply,sizeof(reply)));
 selftest=true;struct selection s={.lo={0},.hi={0xffff},.n=1};struct bytes r=record(&s);struct element e;assert(element(r.b,r.n,&e)&&e.type==6&&e.span==r.n);assert(has_uuid(r.b,r.n,0x1105,0));
 const uint8_t connect[]={0x80,0,7,0x10,0,4,0};rfcomm_data(connect,sizeof(connect),NULL);assert(last_response[0]==0xa0&&last_response_size==7);
 const char type[]="application/x-frond-bt";struct bytes packet={0};u8(&packet,2);u16(&packet,3+3+sizeof(type)+5);u8(&packet,0x42);u16(&packet,3+sizeof(type));add(&packet,type,sizeof(type));u8(&packet,0xc3);u32(&packet,4096);rfcomm_data(packet.b,packet.n,NULL);assert(last_response[0]==0x90);
 for(unsigned offset=0;offset<4096;offset+=512){struct bytes put={0};u8(&put,offset+512==4096?0x82:2);u16(&put,518);u8(&put,offset+512==4096?0x49:0x48);u16(&put,515);for(unsigned i=offset;i<offset+512;++i)u8(&put,(uint8_t)((i*73+17)^((i>>8)*31)));rfcomm_data(put.b,2,NULL);rfcomm_data(put.b+2,put.n-2,NULL);assert(last_response[0]==(offset+512==4096?0xa0:0x90));}
 assert(completed&&payload_size==4096);puts("PASS owned SDP record and fragmented OBEX Connect/Put exact4096");
}
static void spp_tests(void){spp_mode=true;selftest=true;struct selection s={.lo={0},.hi={0xffff},.n=1};struct bytes r=record(&s);assert(has_uuid(r.b,r.n,0x1101,0)&&!has_uuid(r.b,r.n,0x1105,0)&&!has_uuid(r.b,r.n,8,0));uint8_t bytes[128];for(size_t at=0;at<4096;at+=sizeof(bytes)){for(size_t i=0;i<sizeof(bytes);++i)bytes[i]=(uint8_t)(((at+i)*73+19)^(((at+i)>>8)*11));struct bytes f=uih_frame(bytes,sizeof(bytes));assert(f.n==133&&f.b[2]==0&&f.b[3]==1&&!memcmp(f.b+4,bytes,sizeof(bytes)));rfcomm_data(bytes,sizeof(bytes),NULL);}assert(completed&&payload_size==4096);for(size_t i=0;i<4096;++i)assert(payload[i]==(uint8_t)((i*73+19)^((i>>8)*11)));puts("PASS SPP UUID1101, extended UIH framing and exact4096 stream");}
static void dun_tests(void){
 dun_mode=true;selftest=true;struct selection s={.lo={0},.hi={0xffff},.n=1};struct bytes r=record(&s);
 assert(has_uuid(r.b,r.n,0x1103,0)&&has_uuid(r.b,r.n,0x1201,0)&&!has_uuid(r.b,r.n,0x1105,0)&&!has_uuid(r.b,r.n,8,0)&&find_channel(r.b,r.n,0)==CHANNEL);
 dun=frond_dun_create();assert(dun);assert(!strcmp(frond_dun_state(dun),"off"));
 dun_open=true;frond_dun_open(dun,monotonic_ns());dun_flush();assert(!strcmp(dun_state,"idle"));
 const char at[]="AT\r";dun_data(at,3,NULL);add(&dun_sent,"",1);assert(strstr((char*)dun_sent.b,"OK"));dun_sent.n=0;
 const char dial[]="ATDT*99#\r";dun_data(dial,sizeof(dial)-1,NULL);assert(!strcmp(dun_state,"dialling"));
 dun_closed(rfhandle);assert(!strcmp(dun_state,"off"));frond_dun_destroy(dun);dun=NULL;
 puts("PASS DUN UUID1103 record on channel 9 and modem commands");
}
int main(int argc,char**argv){
 if(argc==2&&!strcmp(argv[1],"--selftest-spp")){spp_tests();return 0;}
 if(argc==2&&!strcmp(argv[1],"--selftest-dun")){dun_tests();return 0;}
 if(argc==2&&!strcmp(argv[1],"--selftest")){tests();return 0;}assert(argc>=3);
 /* SOCKET OUTPUT [options] for OPP and SPP; SOCKET --dun [--pin PIN] for the phone and SOCKET --dun-dial ADDRESS for its test client. */
 int first=3;if(!strncmp(argv[2],"--",2))first=2;else output_path=argv[2];
 for(int at=first;at<argc;++at){if(!strcmp(argv[at],"--spp-echo")){spp_mode=true;continue;}if(!strcmp(argv[at],"--dun")){dun_mode=true;continue;}if(!strcmp(argv[at],"--hold-put")){hold_put=true;continue;}if(!strcmp(argv[at],"--pin")){assert(++at<argc && strlen(argv[at])>=1 && strlen(argv[at])<=16);pin_code=argv[at];continue;}if(!strcmp(argv[at],"--gate")){assert(++at<argc);gate_path=argv[at];assert(access(gate_path,F_OK)!=0);continue;}bool spp_arg=!strcmp(argv[at],"--spp-push"),dun_arg=!strcmp(argv[at],"--dun-dial");assert((!strcmp(argv[at],"--push")||spp_arg||dun_arg) && ++at<argc && strlen(argv[at])==12);spp_mode|=spp_arg;dun_mode|=dun_arg;push_mode=true;for(unsigned i=0;i<6;++i){unsigned value;assert(sscanf(argv[at]+2*i,"%2x",&value)==1);remote_address[i]=value;}}assert(!hold_put||!push_mode);assert(!gate_path||push_mode);assert(dun_mode!=!!output_path&&!(dun_mode&&spp_mode));if(output_path)assert(access(output_path,F_OK)!=0);
 if(dun_mode&&!push_mode){dun=frond_dun_create();if(!dun){fprintf(stderr,"slirp did not start\n");return 2;}}
 sk=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(sk>=0);struct sockaddr_un a={.sun_family=AF_UNIX};assert(strlen(argv[1])<sizeof(a.sun_path));strcpy(a.sun_path,argv[1]);
 /* The phone may start beside a btvirt that hasn't made its socket yet. */
 for(int tries=dun_mode&&!push_mode?50:1;connect(sk,(void*)&a,sizeof(a));){assert(--tries>0);usleep(100000);}
 mainloop_init();host=bthost_create();assert(host);bthost_set_send_handler(host,send_h4,NULL);bthost_set_debug(host,debug,NULL,NULL);bthost_set_cmd_complete_cb(host,command,NULL);bthost_notify_ready(host,ready);bthost_set_pin_code(host,(const uint8_t*)pin_code,strlen(pin_code));if(push_mode)bthost_set_connect_cb(host,client_connected,NULL);else{bthost_add_l2cap_server(host,1,sdp_connected,NULL,NULL);bthost_add_l2cap_server(host,3,NULL,rfcomm_closed,NULL);bthost_add_rfcomm_server(host,CHANNEL,rfcomm_connected,NULL);}assert(!mainloop_add_fd(sk,EPOLLIN,read_h4,NULL,NULL));bthost_start(host);if(push_mode)timeout_add(60000,deadline,NULL,NULL);if(gate_path)timeout_add(20,check_gate,NULL,NULL);if(dun&&!push_mode)timeout_add(5,dun_tick,NULL,NULL);mainloop_run_with_signal(stopped,NULL);printf("RESULT put_complete=%u disconnect_seen=%u bytes=%zu\n",completed,disconnected,payload_size);bthost_destroy(host);close(sk);if(dun_mode&&!push_mode){frond_dun_destroy(dun);return 0;}return completed && disconnected && (!push_mode || push_link_closed) && !client_failed?0:2;
}
