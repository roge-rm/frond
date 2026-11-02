package com.rm.frond

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File

class SessionsLayoutTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test fun packableNamesIgnoreTempsAndGenerations() {
        assertTrue(packableName("session.frond"))
        assertTrue(packableName("Palm TX.frond"))
        assertTrue(packableName("Palm TX.frond.settings.json"))
        assertTrue(packableName("sd.frondsd"))
        assertTrue(packableName("ms.frondms"))
        assertFalse(packableName("session.frond.new"))
        assertFalse(packableName(".frond.tmp-abc"))
        assertFalse(packableName("session.frond.tmp-abc"))
        assertFalse(packableName(".gen-sd.frondsd-1"))
        assertFalse(packableName("id.json"))
        assertFalse(packableName("nested/sd.frondsd"))
    }

    @Test fun zipRoundTripCopiesPackableFilesAtArchiveRoot() {
        val dir = tmp.newFolder("machine")
        File(dir, "session.frond").writeText("guest")
        File(dir, "sd.frondsd").writeText("card")
        File(dir, ".frond.tmp-leftover").writeText("tmp")
        File(dir, ".gen-sd.frondsd-1").writeText("gen")
        val zip = ByteArrayOutputStream()
        zipPackable(dir, zip)
        val dest = tmp.newFolder("unpacked")
        assertTrue(unzipPackable(ByteArrayInputStream(zip.toByteArray()), dest))
        assertEquals("guest", File(dest, "session.frond").readText())
        assertEquals("card", File(dest, "sd.frondsd").readText())
        assertFalse(File(dest, ".frond.tmp-leftover").exists())
        assertFalse(File(dest, ".gen-sd.frondsd-1").exists())
        assertEquals(File(dir, "session.frond").length() + File(dir, "sd.frondsd").length(), packableSize(dir))
        assertEquals(File(dir, "session.frond"), sessionFile(dir))
    }

    @Test fun sessionFilePrefersFolderNameThenSessionFrond() {
        val dir = tmp.newFolder("Trip")
        File(dir, "other.frond").writeText("a")
        File(dir, "session.frond").writeText("b")
        assertEquals(File(dir, "session.frond"), sessionFile(dir))
        File(dir, "Trip.frond").writeText("c")
        assertEquals(File(dir, "Trip.frond"), sessionFile(dir))
    }
}
