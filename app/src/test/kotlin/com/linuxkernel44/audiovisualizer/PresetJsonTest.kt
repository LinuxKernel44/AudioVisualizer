package com.linuxkernel44.audiovisualizer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class PresetJsonTest {
    private val doc = """{"version":1,"presets":[
        {"name":"Club","scene":2,"controls":{"sensitivity":1.5,"gainBass":4,"gainMid":0,"gainTreble":2,"smoothing":0.2,"palette":3,"beatFlash":1.5,"bloom":1.2}},
        {"name":"  ","scene":0,"controls":{}},
        {"scene":1,"controls":{}},
        {"name":"NoControls","scene":0},
        "garbage",
        {"name":"Wild","scene":99,"controls":{"sensitivity":1000,"gainBass":-999,"smoothing":"x","palette":42,"beatFlash":-5,"bloom":1e9}}
    ]}"""

    @Test fun parsesValidEntriesAndSkipsTheRest() {
        val list = PresetJson.parse(doc, Preset.Source.User)
        assertEquals(listOf("Club", "Wild"), list.map { it.name })
        val club = list[0]
        assertEquals(2, club.scene)
        assertEquals(1.5f, club.controls.sensitivity, 1e-6f)
        assertEquals(3, club.controls.palette)
        assertEquals(Preset.Source.User, club.source)
    }

    @Test fun clampsOutOfRangeValues() {
        val wild = PresetJson.parse(doc, Preset.Source.Downloaded).first { it.name == "Wild" }
        assertEquals(SCENE_NAMES.size - 1, wild.scene)
        assertEquals(4f, wild.controls.sensitivity, 0f)
        assertEquals(-20f, wild.controls.gainBass, 0f)
        assertEquals(0.3f, wild.controls.smoothing, 1e-6f)   // unparsable -> default
        assertEquals(PALETTE_NAMES.size - 1, wild.controls.palette)
        assertEquals(0f, wild.controls.beatFlash, 0f)
        assertEquals(2f, wild.controls.bloom, 0f)
    }

    @Test fun roundTripsThroughJson() {
        val original = PresetJson.parse(doc, Preset.Source.User)
        val again = PresetJson.parse(PresetJson.toJson(original), Preset.Source.User)
        assertEquals(original, again)
    }

    @Test fun rejectsUnusableDocuments() {
        assertThrows(IllegalArgumentException::class.java) { PresetJson.parse("not json", Preset.Source.User) }
        assertThrows(IllegalArgumentException::class.java) { PresetJson.parse("""{"version":2,"presets":[]}""", Preset.Source.User) }
        assertThrows(IllegalArgumentException::class.java) { PresetJson.parse("""{"version":1}""", Preset.Source.User) }
    }

    @Test fun namesAreTruncated() {
        val long = "x".repeat(200)
        val list = PresetJson.parse("""{"version":1,"presets":[{"name":"$long","controls":{}}]}""", Preset.Source.User)
        assertEquals(40, list[0].name.length)
    }

    @Test fun limitsTheNumberOfPresets() {
        val many = (0 until 500).joinToString(",") { """{"name":"p$it","controls":{}}""" }
        assertTrue(PresetJson.parse("""{"version":1,"presets":[$many]}""", Preset.Source.User).size <= 200)
        assertFalse(PresetJson.parse("""{"version":1,"presets":[]}""", Preset.Source.User).isNotEmpty())
    }
}
