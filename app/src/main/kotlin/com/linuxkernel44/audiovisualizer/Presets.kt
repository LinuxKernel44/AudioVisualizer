package com.linuxkernel44.audiovisualizer

import android.content.Context
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.File

/** A named look: the scene (-1 = automatic) plus the live controls. */
data class Preset(val name: String, val scene: Int, val controls: Controls, val source: Source = Source.User) {
    enum class Source { Builtin, Downloaded, User }
}

/** Reading and writing presets as JSON. Input is untrusted (imported files, downloads), so every value is validated and clamped. */
object PresetJson {
    const val VERSION = 1
    private const val MAX_PRESETS = 200
    private const val MAX_NAME = 40

    fun toJson(presets: List<Preset>): String {
        val arr = JSONArray()
        presets.forEach { p ->
            arr.put(JSONObject().put("name", p.name).put("scene", p.scene).put("controls", JSONObject()
                .put("sensitivity", p.controls.sensitivity.toDouble()).put("gainBass", p.controls.gainBass.toDouble())
                .put("gainMid", p.controls.gainMid.toDouble()).put("gainTreble", p.controls.gainTreble.toDouble())
                .put("smoothing", p.controls.smoothing.toDouble()).put("palette", p.controls.palette)
                .put("beatFlash", p.controls.beatFlash.toDouble()).put("bloom", p.controls.bloom.toDouble())))
        }
        return JSONObject().put("version", VERSION).put("presets", arr).toString(2)
    }

    /** Returns the valid presets in [json]; malformed entries are skipped. Throws only if the document itself is unusable. */
    fun parse(json: String, source: Preset.Source): List<Preset> {
        val root = try {
            JSONObject(json)
        } catch (e: JSONException) {
            throw IllegalArgumentException("Not a valid presets file", e)
        }
        if (root.optInt("version", 0) != VERSION) throw IllegalArgumentException("Unsupported presets version")
        val arr = root.optJSONArray("presets") ?: throw IllegalArgumentException("Missing \"presets\" list")
        val out = ArrayList<Preset>()
        for (i in 0 until minOf(arr.length(), MAX_PRESETS)) {
            val o = arr.optJSONObject(i) ?: continue
            val name = o.optString("name", "").trim().take(MAX_NAME)
            val c = o.optJSONObject("controls") ?: continue
            if (name.isEmpty()) continue
            fun f(key: String, def: Double, lo: Double, hi: Double): Float {
                val v = c.optDouble(key, def)
                return (if (v.isNaN()) def else v).coerceIn(lo, hi).toFloat()
            }
            out += Preset(
                name = name,
                scene = o.optInt("scene", -1).coerceIn(-1, SCENE_NAMES.size - 1),
                controls = Controls(
                    sensitivity = f("sensitivity", 1.0, 0.25, 4.0), gainBass = f("gainBass", 0.0, -20.0, 20.0),
                    gainMid = f("gainMid", 0.0, -20.0, 20.0), gainTreble = f("gainTreble", 0.0, -20.0, 20.0),
                    smoothing = f("smoothing", 0.3, 0.0, 1.0), palette = c.optInt("palette", -1).coerceIn(-1, PALETTE_NAMES.size - 1),
                    beatFlash = f("beatFlash", 1.0, 0.0, 2.0), bloom = f("bloom", 1.0, 0.0, 2.0),
                ),
                source = source,
            )
        }
        return out
    }
}

/** Built-in presets (asset), downloaded presets (file) and the user's own (preferences). */
class PresetStore(private val ctx: Context) {
    private val prefs = ctx.getSharedPreferences("presets", Context.MODE_PRIVATE)
    private val downloadedFile get() = File(File(ctx.filesDir, "presets").apply { mkdirs() }, "downloaded.json")

    fun builtin(): List<Preset> = try {
        PresetJson.parse(ctx.assets.open("presets/builtin.json").bufferedReader().use { it.readText() }, Preset.Source.Builtin)
    } catch (e: Exception) {
        emptyList()
    }

    fun downloaded(): List<Preset> = try {
        if (downloadedFile.exists()) PresetJson.parse(downloadedFile.readText(), Preset.Source.Downloaded) else emptyList()
    } catch (e: Exception) {
        emptyList()
    }

    fun user(): List<Preset> = try {
        PresetJson.parse(prefs.getString("user", null) ?: "{\"version\":1,\"presets\":[]}", Preset.Source.User)
    } catch (e: Exception) {
        emptyList()
    }

    /** Built-in first, then downloaded, then the user's; a later preset replaces an earlier one with the same name. */
    fun all(): List<Preset> = (builtin() + downloaded() + user()).associateBy { it.name }.values.toList()

    fun save(p: Preset) {
        val others = user().filter { it.name != p.name }
        prefs.edit().putString("user", PresetJson.toJson(others + p.copy(source = Preset.Source.User))).apply()
    }

    fun delete(name: String) {
        prefs.edit().putString("user", PresetJson.toJson(user().filter { it.name != name })).apply()
    }

    /** Imports presets from a JSON document; returns how many were added or replaced. */
    fun import(json: String): Int {
        val list = PresetJson.parse(json, Preset.Source.User)
        list.forEach { save(it) }
        return list.size
    }

    fun exportUserJson(): String = PresetJson.toJson(user())

    fun saveDownloaded(json: String): Int {
        val n = PresetJson.parse(json, Preset.Source.Downloaded).size  // validates before replacing the old file
        if (n == 0) throw IllegalArgumentException("The downloaded file contains no valid presets")
        val tmp = File(downloadedFile.parentFile, "downloaded.tmp")
        tmp.writeText(json)
        tmp.renameTo(downloadedFile)
        return n
    }
}
