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

    @JvmStatic external fun linkDial(secret: ByteArray, id: ByteArray, name: String, peerId: ByteArray, peerKey: ByteArray, now: Long): Long
    // paired: 48 bytes per paired peer, id then pinned key.
    @JvmStatic external fun linkAnswer(secret: ByteArray, id: ByteArray, name: String, paired: ByteArray, now: Long): Long
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
    @JvmStatic external fun linkPeerName(h: Long): String
    @JvmStatic external fun linkRtt(h: Long): Long

    // The Bluetooth radio choice once Up: ours, and the peer's as [senderHolds 0 or 1, at ms] or null.
    @JvmStatic external fun linkSendRadio(h: Long, senderHolds: Boolean, at: Long)
    @JvmStatic external fun linkNextRadio(h: Long): LongArray?

    @JvmStatic external fun muxOpen(h: Long, kind: Int, machine: String, device: String): Int
    @JvmStatic external fun muxAccept(h: Long, ch: Int, machine: String, device: String): Boolean
    @JvmStatic external fun muxWaiting(h: Long, ch: Int): Boolean
    @JvmStatic external fun muxBusy(h: Long, ch: Int, machine: String, device: String): Boolean
    @JvmStatic external fun muxClose(h: Long, ch: Int, reason: Int)
    @JvmStatic external fun muxSendState(h: Long, ch: Int, state: Int)
    @JvmStatic external fun muxSend(h: Long, ch: Int, data: ByteArray, offset: Int, n: Int): Int
    @JvmStatic external fun muxConsumed(h: Long, ch: Int, n: Int)
    // LinkWire.ChannelEvent.decode reads it.
    @JvmStatic external fun muxNextEvent(h: Long): ByteArray?

    @JvmStatic external fun codeNew(now: Long): Long
    @JvmStatic external fun codeFree(h: Long)
    @JvmStatic external fun codeText(h: Long): String
    @JvmStatic external fun codeTriesLeft(h: Long): Int
    @JvmStatic external fun codeSucceeded(h: Long)

    @JvmStatic external fun pairDial(secret: ByteArray, id: ByteArray, name: String, code: String, now: Long): Long
    @JvmStatic external fun pairAnswer(secret: ByteArray, id: ByteArray, name: String, code: Long, now: Long): Long
    @JvmStatic external fun pairFree(h: Long)
    @JvmStatic external fun pairReceive(h: Long, data: ByteArray, n: Int, now: Long)
    @JvmStatic external fun pairTick(h: Long, now: Long)
    @JvmStatic external fun pairTakeOutput(h: Long): ByteArray?
    // LinkWire.PairResult by ordinal.
    @JvmStatic external fun pairResult(h: Long): Int
    // id[16], key[32], name.
    @JvmStatic external fun pairPeer(h: Long): ByteArray
}
