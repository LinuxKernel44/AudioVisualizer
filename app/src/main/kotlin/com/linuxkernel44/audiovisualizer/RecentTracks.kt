package com.linuxkernel44.audiovisualizer

import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject

data class RecentTrack(val uri: String, val name: String)

/** The last few opened tracks, newest first, persisted as JSON. Unreadable entries are skipped. */
class RecentTracks(private val prefs: SharedPreferences, private val limit: Int = 12) {
    fun list(): List<RecentTrack> = try {
        val arr = JSONArray(prefs.getString("recent", "[]"))
        (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            val uri = o.optString("uri", "")
            if (uri.isEmpty()) null else RecentTrack(uri, o.optString("name", uri.substringAfterLast('/')))
        }
    } catch (e: Exception) {
        emptyList()
    }

    /** Puts `track` first, removing an older entry for the same URI. */
    fun add(track: RecentTrack): List<RecentTrack> {
        val updated = (listOf(track) + list().filter { it.uri != track.uri }).take(limit)
        prefs.edit().putString("recent", JSONArray(updated.map { JSONObject().put("uri", it.uri).put("name", it.name) }).toString()).apply()
        return updated
    }

    fun remove(uri: String): List<RecentTrack> {
        val updated = list().filter { it.uri != uri }
        prefs.edit().putString("recent", JSONArray(updated.map { JSONObject().put("uri", it.uri).put("name", it.name) }).toString()).apply()
        return updated
    }
}
