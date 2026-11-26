package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PartnershipQuestionTest {
    @Test fun opensOnlyWhileTheCoreWaitsAtTheQuestion() {
        assertTrue(PartnershipQuestion.open("syncing usb partnership-question"))
        assertTrue(PartnershipQuestion.open("syncing serial partnership-question; profile Work"))
        assertFalse(PartnershipQuestion.open("syncing usb partnership"))
        assertFalse(PartnershipQuestion.open("syncing usb files"))
        assertFalse(PartnershipQuestion.open("done usb: Windows Mobile connected. Connected as a guest."))
        assertFalse(PartnershipQuestion.open("failed usb: ActiveSync ended before the partnership question was answered."))
        assertFalse(PartnershipQuestion.open("off"))
    }

    @Test fun answersMatchTheCoreCommand() {
        assertEquals("activesync-answer", PartnershipQuestion.ANSWER_COMMAND)
        assertEquals("set-up", PartnershipQuestion.answer(true))
        assertEquals("guest", PartnershipQuestion.answer(false))
        assertEquals("ask", PartnershipQuestion.START_ARGUMENT)
    }
}
