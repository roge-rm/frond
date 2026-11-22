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
static void hook(const void* data,uint16_t len,void* context){(void)data;(void)len;(void)context;}
static struct cid_hook* add_hook(struct btconn* connection,uint16_t cid){
    struct cid_hook* h=calloc(1,sizeof(*h));assert(h);h->cid=cid;h->func=hook;h->next=connection->cid_hooks;connection->cid_hooks=h;return h;
}
/* A closed channel's CID hook goes with it, so a later channel on the same CID (Palm OS reuses
   0x0040 for RFCOMM after SDP) reaches RFCOMM instead of the old SDP hook. */
static void closed_channel_hooks(bool response){
    struct bthost host={0};struct btconn connection={0};struct l2conn* channel=calloc(1,sizeof(*channel));
    struct bt_l2cap_pdu_disconn_req request={0};struct bt_l2cap_pdu_disconn_rsp reply={0};
    assert(channel);host.acl_mtu=192;host.send_handler=output;host.conns=&connection;connection.handle=1;
    channel->scid=0x40;channel->dcid=0x40;channel->psm=1;connection.l2conns=channel;
    add_hook(&connection,0x40);add_hook(&connection,0x41);
    request.dcid=cpu_to_le16(0x40);request.scid=cpu_to_le16(0x40);reply.dcid=cpu_to_le16(0x40);reply.scid=cpu_to_le16(0x40);
    if(response)assert(l2cap_disconn_rsp(&host,&connection,3,&reply,sizeof(reply)));
    else assert(l2cap_disconn_req(&host,&connection,3,&request,sizeof(request)));
    assert(!connection.l2conns);assert(!find_cid_hook(&connection,0x40));assert(find_cid_hook(&connection,0x41));
    free(connection.cid_hooks);
}
struct stream_fixture {
    struct bthost host;
    struct btconn connection;
    struct l2conn link;
    uint8_t frame[FROND_RFCOMM_MAX_FRAME + 5];
    size_t frame_size;
    unsigned frames, received;
};

static void stream_output(const struct iovec* iov, int count, void* context) {
    struct stream_fixture* f = context;
    uint8_t packet[sizeof(f->frame) + 9];
    size_t size = 0;
    for (int i = 0; i < count; ++i) {
        assert(size + iov[i].iov_len <= sizeof(packet));
        memcpy(packet + size, iov[i].iov_base, iov[i].iov_len);
        size += iov[i].iov_len;
    }
    assert(size >= 13 && packet[0] == 2 && packet[7] == 0x41 && packet[8] == 0);
    f->frame_size = size - 9;
    memcpy(f->frame, packet + 9, f->frame_size);
    ++f->frames;
}

static void stream_wire(struct stream_fixture* f, uint8_t address, uint8_t control,
                        uint8_t length, const void* body, size_t size) {
    uint8_t packet[256] = {2, 1, 0x20, 8 + size, 0, 4 + size, 0, 0x40, 0,
                          address, control, length};
    assert(size <= sizeof(packet) - 13);
    if (size) memcpy(packet + 12, body, size);
    packet[12 + size] = RFCOMM_GET_TYPE(control) == RFCOMM_UIH ?
                        rfcomm_fcs(packet + 9) : rfcomm_fcs2(packet + 9);
    bthost_receive_h4(&f->host, packet, 13 + size);
}

static void stream_payload(const void* data, uint16_t size, void* context) {
    struct stream_fixture* f = context;
    const uint8_t expected[] = {0xbe, 0xef, 0xed};
    assert(size == sizeof(expected) && !memcmp(data, expected, size));
    ++f->received;
}

static void stream_pn(struct stream_fixture* f, uint16_t mtu, uint8_t credits) {
    const uint8_t pn[] = {RFCOMM_MCC_TYPE(1, RFCOMM_PN), RFCOMM_LEN8(8),
                          18, 0xf0, 7, 0, mtu & 255, mtu >> 8, 0, credits};
    stream_wire(f, 3, 0xef, RFCOMM_LEN8(sizeof(pn)), pn, sizeof(pn));
    assert(f->frame[6] == 0xe0 && f->frame[9] == (mtu & 255) &&
           f->frame[10] == mtu >> 8 && f->frame[12] == 7);
    stream_wire(f, RFCOMM_ADDR(1, 18), 0x3f, 1, NULL, 0);
}

static unsigned stream_disconnects;

static void stream_disconnect(uint16_t handle, uint16_t cid) {
    assert(handle == 1 && cid == 0x40);
    ++stream_disconnects;
}

static void stream_wire_test(void) {
    struct stream_fixture f = {0};
    f.host.conns = &f.connection;
    f.host.acl_mtu = 1024;
    f.host.send_handler = stream_output;
    f.host.send_data = &f;
    f.connection.handle = 1;
    f.connection.l2conns = &f.link;
    f.link.scid = 0x40;
    f.link.dcid = 0x41;
    f.link.psm = 3;
    bthost_add_rfcomm_server(&f.host, 9, NULL, NULL);
    bthost_add_rfcomm_chan_hook(&f.host, 1, 9, stream_payload, &f);
    frond_rfcomm_stream_enable(&f.host, 9, stream_disconnect);
    stream_wire(&f, 3, 0x3f, 1, NULL, 0);  // Open the multiplexor.
    stream_pn(&f, 3, 0);
    assert(!frond_rfcomm_stream_space(&f.host, 1, 0x40));

    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7};
    const uint8_t with_credit[] = {2, 0xbe, 0xef, 0xed};
    stream_wire(&f, RFCOMM_ADDR(1, 18), 0xff, RFCOMM_LEN8(3), with_credit, sizeof(with_credit));
    assert(f.received == 1);
    assert(f.frame_size == 5 && f.frame[1] == 0xff && f.frame[2] == 1 && f.frame[3] == 1);

    stream_wire(&f, RFCOMM_ADDR(1, 18), 0xff, RFCOMM_LEN8(3), with_credit + 1, 3);
    assert(f.received == 1);  // Missing credit octet, despite P/F being set.

    size_t offset = 0;
    size_t space;
    while ((space = frond_rfcomm_stream_space(&f.host, 1, 0x40))) {
        assert(space == 3 && offset + space <= sizeof(data));
        const unsigned frames = f.frames;
        assert(!frond_rfcomm_stream_send(&f.host, 1, 0x40, data + offset, space + 1));
        assert(f.frames == frames);
        assert(frond_rfcomm_stream_send(&f.host, 1, 0x40, data + offset, space));
        assert(f.frame_size == 7 && f.frame[1] == 0xef && f.frame[2] == RFCOMM_LEN8(3));
        assert(!memcmp(f.frame + 3, data + offset, space));
        offset += space;
    }
    assert(offset == 6 && !frond_rfcomm_stream_send(&f.host, 1, 0x40, data + offset, 1));
    const uint8_t credit = 1;
    stream_wire(&f, RFCOMM_ADDR(1, 18), 0xff, 1, &credit, 1);
    assert(f.received == 1 && frond_rfcomm_stream_send(&f.host, 1, 0x40, data + offset, 1));
    assert(f.frame_size == 5 && f.frame[3] == 7);

    stream_wire(&f, RFCOMM_ADDR(1, 18), 0xff, 1, &credit, 1);
    stream_wire(&f, RFCOMM_ADDR(1, 18), 0x53, 1, NULL, 0);
    assert(stream_disconnects == 1 && !frond_rfcomm_stream_space(&f.host, 1, 0x40));
    stream_wire(&f, RFCOMM_ADDR(1, 18), 0x3f, 1, NULL, 0);
    assert(frond_rfcomm_stream_space(&f.host, 1, 0x40) == 127);
    stream_wire(&f, RFCOMM_ADDR(1, 18), 0x53, 1, NULL, 0);
    assert(stream_disconnects == 2);
    stream_pn(&f, 130, 0);
    assert(!frond_rfcomm_stream_space(&f.host, 1, 0x40));
    stream_wire(&f, RFCOMM_ADDR(1, 18), 0xff, 1, &credit, 1);
    uint8_t long_frame[130];
    memset(long_frame, 0x5a, sizeof(long_frame));
    assert(frond_rfcomm_stream_space(&f.host, 1, 0x40) == sizeof(long_frame));
    assert(frond_rfcomm_stream_send(&f.host, 1, 0x40, long_frame, sizeof(long_frame)));
    assert(f.frame_size == 135 && f.frame[2] == 4 && f.frame[3] == 1);
    assert(!memcmp(f.frame + 4, long_frame, sizeof(long_frame)));

    free(f.host.new_rfcomm_conn_data);
    free(f.connection.rfcomm_chan_hooks);
    while (f.connection.rcconns) {
        struct rcconn* next = f.connection.rcconns->next;
        free(f.connection.rcconns);
        f.connection.rcconns = next;
    }
    puts("PASS RFCOMM wire credits, frame sizes, exhaustion, refill and reconnect");
}
int main(void) {
    unsigned failure;for(failure=0;failure<5;++failure){run(false,failure);run(true,failure);}
    stream_wire_test();
    closed_channel_hooks(false);closed_channel_hooks(true);puts("PASS closed channel drops its CID hook");
    puts("PASS bilateral RFCOMM config orders, duplicate, failure, continuation and removed-channel response");return 0;
}
