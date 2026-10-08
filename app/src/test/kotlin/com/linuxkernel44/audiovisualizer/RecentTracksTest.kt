package com.linuxkernel44.audiovisualizer

import android.content.SharedPreferences
import org.junit.Assert.assertEquals
import org.junit.Test
import java.lang.reflect.Proxy

/** A minimal in-memory SharedPreferences (the Android stubs do nothing in JVM tests). */
private fun memoryPrefs(): SharedPreferences {
    val map = HashMap<String, Any?>()
    val editor = Proxy.newProxyInstance(SharedPreferences.Editor::class.java.classLoader, arrayOf(SharedPreferences.Editor::class.java)) { proxy, m, args ->
        when (m.name) {
            "putString" -> { map[args[0] as String] = args[1]; proxy }
            "apply", "commit" -> null
            else -> proxy
        }
    } as SharedPreferences.Editor
    return Proxy.newProxyInstance(SharedPreferences::class.java.classLoader, arrayOf(SharedPreferences::class.java)) { _, m, args ->
        when (m.name) {
            "getString" -> map[args[0] as String] ?: args[1]
            "edit" -> editor
            else -> null
        }
    } as SharedPreferences
}

class RecentTracksTest {
    @Test fun newestFirstWithoutDuplicates() {
        val r = RecentTracks(memoryPrefs())
        r.add(RecentTrack("content://a", "A"))
        r.add(RecentTrack("content://b", "B"))
        val list = r.add(RecentTrack("content://a", "A again"))
        assertEquals(listOf("content://a", "content://b"), list.map { it.uri })
        assertEquals("A again", list[0].name)
    }

    @Test fun limitsAndRemoves() {
        val r = RecentTracks(memoryPrefs(), limit = 3)
        (1..5).forEach { r.add(RecentTrack("content://$it", "T$it")) }
        assertEquals(listOf("content://5", "content://4", "content://3"), r.list().map { it.uri })
        assertEquals(listOf("content://5", "content://3"), r.remove("content://4").map { it.uri })
    }

    @Test fun corruptStorageReadsAsEmpty() {
        val p = memoryPrefs()
        p.edit().putString("recent", "{not json").apply()
        assertEquals(emptyList<RecentTrack>(), RecentTracks(p).list())
    }
}
