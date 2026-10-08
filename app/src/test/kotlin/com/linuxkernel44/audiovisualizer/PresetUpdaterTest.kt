package com.linuxkernel44.audiovisualizer

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.net.InetSocketAddress
import java.security.MessageDigest

/** Runs the real HTTP code against a local stand-in for the GitHub API and its pre-signed asset storage. */
class PresetUpdaterTest {
    private lateinit var server: HttpServer
    private val auth = HashMap<String, String?>()       // path -> Authorization header seen
    private var presets = """{"version":1,"presets":[{"name":"Remote","scene":2,"controls":{}}]}"""
    private var digest: String? = null
    private var releaseStatus = 200
    private var redirectToStorage = true
    private val saved = ArrayList<String>()
    private val base get() = "http://127.0.0.1:${server.address.port}"

    private fun sha256(s: String) = MessageDigest.getInstance("SHA-256").digest(s.toByteArray()).joinToString("") { "%02x".format(it) }

    private fun reply(e: HttpExchange, code: Int, body: String, extra: Map<String, String> = emptyMap()) {
        extra.forEach { (k, v) -> e.responseHeaders.add(k, v) }
        val b = body.toByteArray()
        e.sendResponseHeaders(code, if (code == 302 || b.isEmpty()) -1 else b.size.toLong())
        if (b.isNotEmpty() && code != 302) e.responseBody.use { it.write(b) } else e.close()
    }

    @Before fun start() {
        server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        server.createContext("/repos/${PresetUpdater.REPO}/releases/latest") { e ->
            auth["release"] = e.requestHeaders.getFirst("Authorization")
            if (releaseStatus != 200) return@createContext reply(e, releaseStatus, "{}")
            val d = digest?.let { ",\"digest\":\"$it\"" } ?: ""
            reply(e, 200, """{"assets":[{"name":"readme.txt","url":"$base/other"},{"name":"presets.json","url":"$base/asset"$d}]}""")
        }
        server.createContext("/asset") { e ->
            auth["asset"] = e.requestHeaders.getFirst("Authorization")
            if (redirectToStorage) reply(e, 302, "", mapOf("Location" to "$base/storage"))
            else reply(e, 200, presets)
        }
        server.createContext("/storage") { e ->
            auth["storage"] = e.requestHeaders.getFirst("Authorization")
            reply(e, 200, presets)
        }
        server.start()
    }

    @After fun stop() = server.stop(0)

    private fun run(token: String? = "tok123") = PresetUpdater.update({ saved += it; PresetJson.parse(it, Preset.Source.Downloaded).size }, token, base)

    @Test fun downloadsAndSavesThePresets() {
        val r = run()
        assertEquals(PresetUpdater.Result.Updated(1), r)
        assertEquals(1, saved.size)
        assertTrue(saved[0].contains("Remote"))
    }

    @Test fun tokenGoesToTheApiButNeverToTheRedirectedStorage() {
        run()
        assertEquals("Bearer tok123", auth["release"])
        assertEquals("Bearer tok123", auth["asset"])
        assertNull("the pre-signed storage URL must not see the token", auth["storage"])
    }

    @Test fun verifiesTheDigestWhenGitHubProvidesOne() {
        digest = "sha256:" + sha256(presets)
        assertTrue(run() is PresetUpdater.Result.Updated)
        saved.clear()
        digest = "sha256:" + "0".repeat(64)
        val r = run()
        assertTrue(r is PresetUpdater.Result.Failed)
        assertTrue((r as PresetUpdater.Result.Failed).message.contains("integrity"))
        assertTrue("nothing is saved after a failed check", saved.isEmpty())
    }

    @Test fun reportsAuthenticationAndMissingReleaseErrors() {
        releaseStatus = 401
        assertTrue((run() as PresetUpdater.Result.Failed).message.contains("rejected the token"))
        releaseStatus = 404
        assertTrue((run() as PresetUpdater.Result.Failed).message.contains("not found"))
    }

    @Test fun refusesWithoutAToken() {
        assertTrue(run(token = null) is PresetUpdater.Result.Failed)
        assertTrue(run(token = "  ") is PresetUpdater.Result.Failed)
        assertTrue(auth.isEmpty())
    }

    @Test fun rejectsInvalidAndOversizedDownloads() {
        redirectToStorage = false
        presets = "this is not json"
        assertTrue(run() is PresetUpdater.Result.Failed)
        presets = "x".repeat(300 * 1024)
        val r = run()
        assertTrue(r is PresetUpdater.Result.Failed)
        assertFalse((r as PresetUpdater.Result.Failed).message.isEmpty())
    }
}
