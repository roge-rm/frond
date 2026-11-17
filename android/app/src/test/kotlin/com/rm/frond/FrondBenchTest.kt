package com.rm.frond

import java.io.File
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FrondBenchTest {
    // The asset is the committed PRC, copied by Gradle; unit tests run in android/app.
    @Test fun assetIsTheCommittedPrc() {
        val committed = File("../../tools/frondbench/FrondBench.prc")
        val built = File("build/generated/licences-assets/frondbench/FrondBench.prc")
        assertTrue(committed.length() > 1000)
        assertTrue("copied asset missing: ${built.path}", built.isFile)
        assertArrayEquals(committed.readBytes(), built.readBytes())
        assertTrue(FrondBench.ASSET.endsWith(FrondBench.FILE_NAME))
    }
}
