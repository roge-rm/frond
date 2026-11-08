package com.rm.frond

// The link/ library through libfrondlink.so (cpp/LinkJni.cpp), the same C++ the desktop hub uses. Handles
// are native pointers; each is used from the link engine's thread only and freed once. Times are ms on a
// monotonic clock.
object LinkNative {
    init {
        System.loadLibrary("frondlink")
    }

    @JvmStatic external fun newSecret(): ByteArray
    @JvmStatic external fun publicKey(secret: ByteArray): ByteArray?
    @JvmStatic external fun randomBytes(n: Int): ByteArray

    // LinkWire.FirstFrame by ordinal.
    @JvmStatic external fun peekFirstFrame(data: ByteArray): Int
    @JvmStatic external fun refuseFrame(reason: Int): ByteArray

    fun linkDial(secret: ByteArray, id: ByteArray, name: String, peerId: ByteArray, peerKey: ByteArray, now: Long): Long =
        linkDialUtf8(secret, id, LinkText.encode(name), peerId, peerKey, now)
    @JvmStatic private external fun linkDialUtf8(secret: ByteArray, id: ByteArray, name: ByteArray, peerId: ByteArray, peerKey: ByteArray, now: Long): Long
    // paired: 48 bytes per paired peer, id then pinned key.
    fun linkAnswer(secret: ByteArray, id: ByteArray, name: String, paired: ByteArray, now: Long): Long =
        linkAnswerUtf8(secret, id, LinkText.encode(name), paired, now)
    @JvmStatic private external fun linkAnswerUtf8(secret: ByteArray, id: ByteArray, name: ByteArray, paired: ByteArray, now: Long): Long
    @JvmStatic external fun linkFree(h: Long)
    @JvmStatic external fun linkReceive(h: Long, data: ByteArray, n: Int, now: Long)
    @JvmStatic external fun linkTick(h: Long, now: Long)
    @JvmStatic external fun linkTakeOutput(h: Long, max: Int): ByteArray?
    @JvmStatic external fun linkHasOutput(h: Long): Boolean
    @JvmStatic external fun linkClose(h: Long, reason: Int)
    // LinkWire.LinkState and LinkError by ordinal.
    @JvmStatic external fun linkState(h: Long): Int
    @JvmStatic external fun linkError(h: Long): Int
    @JvmStatic external fun linkPeerId(h: Long): ByteArray
    fun linkPeerName(h: Long): String = LinkText.decode(linkPeerNameUtf8(h))
    @JvmStatic private external fun linkPeerNameUtf8(h: Long): ByteArray?
    @JvmStatic external fun linkRtt(h: Long): Long

    // The Bluetooth radio choice once Up: ours, and the peer's as [senderHolds 0 or 1, at ms] or null.
    @JvmStatic external fun linkSendRadio(h: Long, senderHolds: Boolean, at: Long)
    @JvmStatic external fun linkNextRadio(h: Long): LongArray?

    fun muxOpen(h: Long, kind: Int, machine: String, device: String): Int =
        muxOpenUtf8(h, kind, LinkText.encode(machine), LinkText.encode(device))
    @JvmStatic private external fun muxOpenUtf8(h: Long, kind: Int, machine: ByteArray, device: ByteArray): Int
    fun muxAccept(h: Long, ch: Int, machine: String, device: String): Boolean =
        muxAcceptUtf8(h, ch, LinkText.encode(machine), LinkText.encode(device))
    @JvmStatic private external fun muxAcceptUtf8(h: Long, ch: Int, machine: ByteArray, device: ByteArray): Boolean
    @JvmStatic external fun muxWaiting(h: Long, ch: Int): Boolean
    fun muxBusy(h: Long, ch: Int, machine: String, device: String): Boolean =
        muxBusyUtf8(h, ch, LinkText.encode(machine), LinkText.encode(device))
    @JvmStatic private external fun muxBusyUtf8(h: Long, ch: Int, machine: ByteArray, device: ByteArray): Boolean
    @JvmStatic external fun muxClose(h: Long, ch: Int, reason: Int)
    @JvmStatic external fun muxSendState(h: Long, ch: Int, state: Int)
    @JvmStatic external fun muxSend(h: Long, ch: Int, data: ByteArray, offset: Int, n: Int): Int
    @JvmStatic external fun muxConsumed(h: Long, ch: Int, n: Int)
    // LinkWire.ChannelEvent.decode reads it.
    @JvmStatic external fun muxNextEvent(h: Long): ByteArray?

    @JvmStatic external fun codeNew(now: Long): Long
    @JvmStatic external fun codeFree(h: Long)
    fun codeText(h: Long): String = LinkText.decode(codeTextUtf8(h))
    @JvmStatic private external fun codeTextUtf8(h: Long): ByteArray?
    @JvmStatic external fun codeTriesLeft(h: Long): Int
    @JvmStatic external fun codeSucceeded(h: Long)

    fun pairDial(secret: ByteArray, id: ByteArray, name: String, code: String, now: Long): Long =
        pairDialUtf8(secret, id, LinkText.encode(name), LinkText.encode(code), now)
    @JvmStatic private external fun pairDialUtf8(secret: ByteArray, id: ByteArray, name: ByteArray, code: ByteArray, now: Long): Long
    fun pairAnswer(secret: ByteArray, id: ByteArray, name: String, code: Long, now: Long): Long =
        pairAnswerUtf8(secret, id, LinkText.encode(name), code, now)
    @JvmStatic private external fun pairAnswerUtf8(secret: ByteArray, id: ByteArray, name: ByteArray, code: Long, now: Long): Long
    @JvmStatic external fun pairFree(h: Long)
    @JvmStatic external fun pairReceive(h: Long, data: ByteArray, n: Int, now: Long)
    @JvmStatic external fun pairTick(h: Long, now: Long)
    @JvmStatic external fun pairTakeOutput(h: Long): ByteArray?
    // LinkWire.PairResult by ordinal.
    @JvmStatic external fun pairResult(h: Long): Int
    // id[16], key[32], name; LinkText.peer splits it.
    @JvmStatic external fun pairPeer(h: Long): ByteArray
}

// Text to and from LinkNative: plain UTF-8 bytes both ways, as JNI's own string calls use modified UTF-8,
// which garbles emoji and other characters past U+FFFF.
object LinkText {
    fun encode(text: String): ByteArray = text.toByteArray(Charsets.UTF_8)

    fun decode(bytes: ByteArray?): String = bytes?.toString(Charsets.UTF_8) ?: ""

    data class Peer(val id: ByteArray, val key: ByteArray, val name: String)

    // pairPeer's id[16], key[32] then name. Null if it's short.
    fun peer(info: ByteArray): Peer? {
        if (info.size < 48) return null
        return Peer(info.copyOfRange(0, 16), info.copyOfRange(16, 48), decode(info.copyOfRange(48, info.size)))
    }
}
