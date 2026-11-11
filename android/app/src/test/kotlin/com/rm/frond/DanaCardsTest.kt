package com.rm.frond

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.File

class DanaCardsTest {
    @get:Rule val tmp = TemporaryFolder()
    private val dana get() = Device.byId("alphasmart-dana")!!
    private val key = "12345678000000000000000000000000"
    private val second = "23456789000000000000000000000000"
    private val linked get() = "frond-linked-sd 3\n0 sd \"/cards/first.frondsd\" $key 305419896 33554432 1 0 1 2 3 sd 9\n2 sd \"/cards/second.frondsd\" $second 591751049 33554432 0 1 1 4 5 mmc 7\n"
    private val portable get() = "frond-portable-sd 3\n0 sd sd0.frondsd $key 305419896 33554432 1 0 123 sd\n2 sd sd2.frondsd $second 591751049 33554432 0 1 456 mmc\n"
    private fun fixture(manifest: String, portable: Boolean = false, name: String = "alphasmart-dana", version: Long = 8): File {
        val inner = ByteArray(80)
        fun put(bytes: ByteArray, at: Int, value: Long) { for (n in 0..3) bytes[at+n] = (value shr (8*n)).toByte() }
        put(inner,0,0x19800819); put(inner,4,version)
        name.toByteArray().copyInto(inner,8)
        fun hash(bytes: ByteArray): Long { var hash=2166136261L; for (b in bytes) hash=((hash xor (b.toLong() and 255))*16777619) and 0xffffffffL; return hash }
        val text=manifest.toByteArray()
        val descriptor=ByteArray(12); put(descriptor,0,text.size.toLong()); put(descriptor,4,hash(inner)); put(descriptor,8,hash(text))
        return tmp.newFile().also { file -> file.outputStream().use { out ->
            out.write((if (portable) "FRONDPORTSD1\n" else "FRONDLINKSD1\n").toByteArray()); out.write(descriptor); out.write(text); out.write(inner)
        } }
    }
    @Test fun slotsAndFamiliesUseExactSharedConstraints() {
        assertEquals(listOf(0,2),dana.storageSlots.map { it.id })
        assertEquals(listOf(1,2),dana.storageSlots.map { it.label })
        for (slot in dana.storageSlots) {
            assertEquals(listOf(32,64,128,256,512),slot.families[0].sizes)
            assertEquals(listOf(32,64,128,256),slot.families[1].sizes)
            assertTrue(slot.families[0].accepts((1L shl 30)-(256L shl 10)))
            for (bytes in listOf(1024L shl 20,1886L shl 20,2048L shl 20,33554433L)) assertFalse(slot.families[0].accepts(bytes))
        }
        assertEquals(listOf(32,64,128,256,512),dana.sdSizes)
        assertEquals(listOf(32,64,128,256,512,1024,1886),Device.TX.sdSizes)
        assertTrue(dana.keyboard); assertFalse(dana.infraredKeyboard); assertTrue(dana.infrared)
        assertTrue(Device.TX.infraredKeyboard)
    }
    @Test fun independentCommandPrefixesAndQuotedPathsKeepMmc() {
        assertEquals(listOf("sd-","sd2-"),dana.storageSlots.map { it.commandPrefix })
        val argument=DanaCardProtocol.attach("/cards/a \" b.frondsd",key,true,dana.storageSlots[1].families[1])
        assertEquals("\"/cards/a \\\" b.frondsd\" $key protected mmc",argument)
        for (text in listOf("relative", "/bad\npath", "/bad\u0000path")) {
            try { DanaCardProtocol.quote(text); fail(text) } catch (_: IllegalArgumentException) {}
        }
    }
    @Test fun cardStatePreservesProtectionProtocolAndIoError() {
        val state=DanaCardProtocol.state("ejected $key protected io-error mmc")!!
        assertFalse(state.inserted); assertTrue(state.writeProtected); assertTrue(state.failed); assertEquals("mmc",state.protocol)
        assertEquals("sd",DanaCardProtocol.state("inserted $key writable")!!.protocol)
        for (text in listOf("inserted", "ejected $key maybe", "inserted $key writable mmc mmc", "ejected bad protected")) assertNull(DanaCardProtocol.state(text))
    }
    @Test fun currentSessionVersionIsInspected() {
        // The core saves wrapped Dana sessions at the current session version.
        assertEquals(9L,DanaSession.inspect(fixture(linked,version=9))!!.version)
        assertNull(DanaSession.inspect(fixture(linked,version=5)))
    }
    @Test fun wirelessSessionsKeepTheirDevice() {
        assertEquals("alphasmart-dana",DanaSession.inspect(fixture(linked))!!.device)
        assertEquals("alphasmart-dana-wireless",DanaSession.inspect(fixture(linked,false,"alphasmart-dana-wireless"))!!.device)
        assertNull(DanaSession.inspect(fixture(linked,false,"alphasmart-dana-other")))
        val wireless=Device.byId("alphasmart-dana-wireless")!!
        assertEquals(dana.storageSlots.map { it.id },wireless.storageSlots.map { it.id })
        assertTrue(wireless.isDana() && wireless.keyboard && wireless.infrared)
    }
    @Test fun wrapperInspectionKeepsBothSlotDescriptors() {
        for (portable in listOf(false,true)) {
            val file=fixture(if (portable) this.portable else linked,portable)
            assertTrue(DanaSession.wrapped(file)); val inspected=DanaSession.inspect(file)!!
            assertEquals(portable,inspected.portable); assertEquals(8L,inspected.version)
        }
    }
    @Test fun damagedAndUnsafeWrappersAreRefused() {
        val good=fixture(linked)
        val bytes=good.readBytes(); bytes[bytes.lastIndex]=1; good.writeBytes(bytes); assertNull(DanaSession.inspect(good))
        for (text in listOf(linked.replace("sd 3", "sd 2"), linked.replace("2 sd", "0 sd"), linked.replace(second,key),
                linked.replace(" mmc ", " imaginary "), linked.replace(" 33554432 ", " 1073741824 "))) assertNull(DanaSession.inspect(fixture(text)))
        assertNull(DanaSession.inspect(fixture(portable.replace("sd2.frondsd", "../sd2.frondsd"),true)))
    }
    @Test fun genericCopyAndZipRefuseCardWrappersBeforeWriting() {
        val from=tmp.newFolder(); fixture(linked).copyTo(File(from,"session.frond"))
        val dest=File(tmp.root,"copy")
        assertNotNull(duplicateMachine(tmp.root,from,dest,{ false },CardChoice.COPY)); assertFalse(dest.exists())
        try { zipPackable(from,ByteArrayOutputStream()); fail() } catch (_: java.io.IOException) {}
    }
    @Test fun androidCopyChecksBothSlotsBeforePublishing() {
        assertNull(DanaCardProtocol.sessionCopyError(listOf("none","none")))
        for (states in listOf(listOf("none","ejected $key protected mmc"),listOf("inserted $key writable","none"),
                listOf("none",null),listOf("none","unknown"),listOf("none")))
            assertNotNull(DanaCardProtocol.sessionCopyError(states))
    }
    @Test fun frondsdImportIsCheckedAgainstTheFamilySize() {
        fun head(payload: Long) = "FRONDSD\u0000".toByteArray(Charsets.US_ASCII) + ByteArray(8) +
            ByteArray(8) { (payload shr (8 * it)).toByte() }
        val (sd, mmc) = dana.storageSlots[0].families
        assertTrue(DanaCardProtocol.frondsdFits(head(512L shl 20), sd))
        assertFalse(DanaCardProtocol.frondsdFits(head(512L shl 20), mmc))
        assertTrue(DanaCardProtocol.frondsdFits(head(256L shl 20), mmc))
        assertFalse(DanaCardProtocol.frondsdFits(head(0), sd))
        assertFalse(DanaCardProtocol.frondsdFits(head(32L shl 20).copyOf(20), sd))
        assertFalse(DanaCardProtocol.frondsdFits("FRONDMS\u0000".toByteArray(Charsets.US_ASCII) + head(32L shl 20).copyOfRange(8, 24), sd))
    }
}
