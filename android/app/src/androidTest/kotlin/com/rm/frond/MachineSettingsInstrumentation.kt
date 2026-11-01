package com.rm.frond

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.os.Bundle
import org.json.JSONObject
import java.io.File

// Runs against Android's real preferences, session files and HomeModel without starting a guest.
class MachineSettingsInstrumentation : Instrumentation() {
    private var inputServer = false
    private lateinit var home: HomeActivity
    private val created = mutableListOf<Session>()
    private var passed = 0
    private val failures = mutableListOf<String>()

    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        inputServer = arguments?.getString("inputServer") == "true"
        start()
    }

    private fun <T> ui(work: () -> T): T {
        var result: Result<T>? = null
        runOnMainSync { result = runCatching(work) }
        return result!!.getOrThrow()
    }

    private fun test(name: String, work: () -> Unit) {
        try {
            work()
            passed++
            sendStatus(0, Bundle().apply { putString("stream", "PASS $name\n") })
        } catch (e: Throwable) {
            failures += "$name: ${e.stackTraceToString()}"
            sendStatus(-1, Bundle().apply { putString("stream", "FAIL ${failures.last()}\n") })
        }
    }

    private fun create(settings: MachineSettings = MachineSettings(), device: Device = Device.TX): Session =
        Sessions.create(targetContext, device, name = "S2 boundary ${created.size}", settings = settings).also { created += it }

    private fun newModel() = ui { HomeModel(home).also { it.reload() } }

    private fun read(session: Session) = checkNotNull(Sessions.read(targetContext, session.id))

    override fun onStart() {
        if (inputServer) { InputAcceptance(this).run(); return }
        val preferences = targetContext.getSharedPreferences("settings", 0)
        val original = preferences.all.toMap()
        try {
            home = startActivitySync(Intent(targetContext, HomeActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as HomeActivity
            val expected = MachineSettings(ScreenSize.WHOLE, smoothing = true, sound = false, fastAudio = true)
            test("legacy SharedPreferences migrate once into durable session metadata") {
                preferences.edit().clear().putString("size", "WHOLE").putBoolean("smoothing", true)
                    .putBoolean("sound", false).putBoolean("fastAudio", true).commit()
                val session = create()
                val sidecar = File(Sessions.folder(targetContext), "${session.id}.json")
                val json = JSONObject(sidecar.readText()).apply { remove("settings") }
                sidecar.writeText(json.toString())
                check(read(session).settings == expected)
                check(preferences.getBoolean("fastAudio.palm-tx", false))
                preferences.edit().putString("size", "FIT").putBoolean("sound", true)
                    .putBoolean("fastAudio.palm-tx", false).commit()
                check(read(session).settings == expected)
                check(JSONObject(sidecar.readText()).has("settings"))
            }
            test("HomeModel edit save and same-model isolation") {
                val first = create()
                val second = create()
                val model = newModel()
                ui { model.edit(first); model.changeMachineSettings(expected); model.saveMachine() }
                check(read(first).settings == expected)
                check(read(second).settings == MachineSettings())
                val reopened = newModel()
                ui { reopened.edit(read(first)) }
                check(reopened.machineDraft?.settings == expected)
            }
            test("HomeModel cancel discards edits without writing") {
                val session = create(expected)
                val sidecar = File(Sessions.folder(targetContext), "${session.id}.json")
                val before = sidecar.readText()
                val model = newModel()
                ui {
                    model.edit(session)
                    model.changeMachineSettings(MachineSettings(ScreenSize.FILL))
                    model.cancelMachine()
                    model.edit(session)
                }
                check(model.machineDraft?.settings == expected)
                check(sidecar.readText() == before)
            }
            test("Sessions rename and markUsed preserve live sound from a stale Session") {
                val stale = create(expected)
                Sessions.updateSettings(targetContext, stale.id) { it.copy(sound = true) }
                Sessions.rename(targetContext, stale, "S2 renamed")
                Sessions.markUsed(targetContext, stale)
                val current = read(stale)
                check(current.name == "S2 renamed")
                check(current.used > 0L)
                check(current.settings == expected.copy(sound = true))
                val reopened = newModel()
                ui { reopened.edit(stale) }
                check(reopened.machineDraft?.settings?.sound == true)
            }
            test("Add firmware model changes and cancellation retain only draft settings") {
                val before = Sessions.list(targetContext).map { it.id }.toSet()
                val model = newModel()
                ui {
                    model.addMachine()
                    model.chooseMachine(Device.TX)
                    model.changeMachineSettings(expected)
                    model.chooseMachine(Device.E2)
                }
                check(model.machineDraft?.device == Device.E2)
                check(model.machineDraft?.settings == expected)
                ui { model.openFirmware(Device.E2); model.closeFirmware() }
                check(model.machineDraft?.settings == expected)
                ui { model.openFirmware(Device.E2); model.firmwareImported(Device.TX) }
                check(model.machineDraft?.device == Device.TX)
                check(model.machineDraft?.settings == expected)
                val saved = Bundle()
                ui { model.saveState(saved) }
                val restored = ui { HomeModel(home).also { it.restore(saved) } }
                check(restored.machineDraft == model.machineDraft)
                ui { restored.cancelMachine(); model.cancelMachine() }
                check(Sessions.list(targetContext).map { it.id }.toSet() == before)
            }
            test("busy and deleted machines reject stale edits") {
                val session = create(expected)
                val model = newModel()
                ui { model.copying = true; model.edit(session) }
                check(model.machineDraft == null)
                ui { model.copying = false; model.edit(session); model.changeMachineSettings(MachineSettings()); model.copying = true; model.saveMachine() }
                check(read(session).settings == expected)
                ui { model.copying = false }
                Sessions.delete(targetContext, session)
                ui { model.saveMachine() }
                check(Sessions.read(targetContext, session.id) == null)
                check(model.machineDraft == null)
            }
        } catch (e: Throwable) {
            failures += e.stackTraceToString()
        } finally {
            created.forEach { Sessions.delete(targetContext, it) }
            val edit = preferences.edit().clear()
            original.forEach { (key, value) ->
                when (value) {
                    is Boolean -> edit.putBoolean(key, value)
                    is String -> edit.putString(key, value)
                    is Int -> edit.putInt(key, value)
                    is Long -> edit.putLong(key, value)
                    is Float -> edit.putFloat(key, value)
                    is Set<*> -> @Suppress("UNCHECKED_CAST") edit.putStringSet(key, value as Set<String>)
                }
            }
            edit.commit()
            if (::home.isInitialized) ui { home.finish() }
        }
        finish(if (failures.isEmpty()) Activity.RESULT_OK else Activity.RESULT_CANCELED, Bundle().apply {
            putString("stream", "$passed boundary tests passed, ${failures.size} failed\n" + failures.joinToString("\n"))
            putInt("passed", passed)
            putInt("failed", failures.size)
        })
    }
}
