package com.linuxkernel44.audiovisualizer

import org.json.JSONObject

data class SectionInfo(
    val start: Float,
    val end: Float,
    val label: String,
    val energy: Float,
    val cluster: Int,
)

/** UI-facing view of the native analysis result (parsed from the engine's JSON summary). */
data class AnalysisSummary(
    val durationSec: Float,
    val bpm: Float,
    val beatsPerBar: Int,
    val beatSource: String,
    val key: String,
    val minor: Boolean,
    val lufs: Float,
    val lra: Float,
    val sections: List<SectionInfo>,
    val downbeats: FloatArray,
    val energy: FloatArray,
) {
    val keyText: String get() = if (key == "?") "Key ?" else "$key ${if (minor) "minor" else "major"}"

    companion object {
        fun parse(json: String): AnalysisSummary? = try {
            val o = JSONObject(json)
            val secs = o.getJSONArray("sections")
            val sections = List(secs.length()) { i ->
                val s = secs.getJSONObject(i)
                SectionInfo(
                    s.getDouble("start").toFloat(), s.getDouble("end").toFloat(), s.getString("label"),
                    s.getDouble("energy").toFloat(), s.getInt("cluster"),
                )
            }
            fun floats(name: String): FloatArray {
                val a = o.getJSONArray(name)
                return FloatArray(a.length()) { a.getDouble(it).toFloat() }
            }
            AnalysisSummary(
                durationSec = o.getDouble("durationSec").toFloat(),
                bpm = o.getDouble("bpm").toFloat(),
                beatsPerBar = o.getInt("beatsPerBar"),
                beatSource = o.optString("beatSource", "dsp"),
                key = o.getString("key"),
                minor = o.getBoolean("minor"),
                lufs = o.getDouble("lufs").toFloat(),
                lra = o.getDouble("lra").toFloat(),
                sections = sections,
                downbeats = floats("downbeats"),
                energy = floats("energy"),
            )
        } catch (e: Exception) {
            null
        }
    }
}
