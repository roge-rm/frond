/* Exercise the pinned test host's real configuration handlers and emitted H4. */
#include <assert.h>
#include "frond-bthost.c"
static unsigned starts;
static void output(const struct iovec *iov,int count,void* context) {
    uint8_t packet[256];size_t n=0;int i;
    for(i=0;i<count;++i){assert(n+iov[i].iov_len<=sizeof(packet));memcpy(packet+n,iov[i].iov_base,iov[i].iov_len);n+=iov[i].iov_len;}
    if(n==13 && packet[0]==2 && packet[7]==0x41 && packet[8]==0 && packet[9]==3 && packet[10]==0x3f){
        static const uint8_t sabm[]={3,0x3f,1,0x1c};assert(!memcmp(packet+9,sabm,4));++starts;
    }
}
static void run(bool reverse,unsigned failure) {
    struct bthost host={0};struct btconn connection={0};struct l2conn channel={0};
    struct rfcomm_connection_data pending={0};
    struct bt_l2cap_pdu_config_req request={0};struct bt_l2cap_pdu_config_rsp response={0};
    starts=0;host.acl_mtu=192;host.send_handler=output;host.conns=&connection;
    host.rfcomm_conn_data=&pending;pending.conn=&connection;pending.scid=0x40;connection.handle=1;connection.l2conns=&channel;
    channel.scid=0x40;channel.dcid=0x41;channel.psm=3;request.dcid=cpu_to_le16(0x40);response.scid=cpu_to_le16(0x40);
    if(failure==1)response.result=cpu_to_le16(2);
    if(failure==2)response.flags=cpu_to_le16(1);
    if(failure==3)request.flags=cpu_to_le16(1);
    if(failure==4)pending.scid=0x42;
    if(reverse){assert(l2cap_config_req(&host,&connection,1,&request,sizeof(request)));assert(!starts);}
    assert(l2cap_config_rsp(&host,&connection,2,&response,sizeof(response)));
    if(!reverse){assert(!starts);assert(l2cap_config_req(&host,&connection,1,&request,sizeof(request)));}
    assert(starts==(failure?0:1));
    /* Duplicates cannot start a second multiplexor; a failed exchange stays failed. */
    assert(l2cap_config_req(&host,&connection,1,&request,sizeof(request)));
    assert(l2cap_config_rsp(&host,&connection,2,&response,sizeof(response)));
    assert(starts==(failure?0:1));
    response.result=0;response.flags=0;request.flags=0;
    assert(l2cap_config_req(&host,&connection,1,&request,sizeof(request)));
    assert(l2cap_config_rsp(&host,&connection,2,&response,sizeof(response)));
    assert(starts==(failure==1||failure==4?0:1));
    /* A late response after channel removal cannot start anything. */
    connection.l2conns=NULL;
    assert(!l2cap_config_rsp(&host,&connection,2,&response,sizeof(response)));
    assert(starts==(failure==1||failure==4?0:1));
}
int main(void) {
    unsigned failure;for(failure=0;failure<5;++failure){run(false,failure);run(true,failure);}
    puts("PASS bilateral RFCOMM config orders, duplicate, failure, continuation and removed-channel response");return 0;
}
