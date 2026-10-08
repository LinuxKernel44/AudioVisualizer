package com.linuxkernel44.audiovisualizer

import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Fetches `presets.json` from the latest GitHub Release of the project's (private) repository,
 * authenticated with the user's token. Runs only when the user asks. The token is sent to
 * api.github.com only: the asset download is redirected to a pre-signed storage URL that must be
 * fetched without credentials, so redirects are followed manually.
 */
object PresetUpdater {
    const val REPO = "LinuxKernel44/AudioVisualizer"
    private const val ASSET = "presets.json"
    private const val MAX_BYTES = 256 * 1024

    sealed class Result {
        data class Updated(val count: Int) : Result()
        data class Failed(val message: String) : Result()
    }

    /** Blocking; call from a background thread. */
    fun update(store: PresetStore, token: String?, baseUrl: String = "https://api.github.com"): Result {
        if (token.isNullOrBlank()) return Result.Failed("Enter a GitHub token in Settings first.")
        return try {
            val release = JSONObject(fetch("$baseUrl/repos/$REPO/releases/latest", token, "application/vnd.github+json").decodeToString())
            val assets = release.optJSONArray("assets") ?: return Result.Failed("The latest release has no assets.")
            var asset: JSONObject? = null
            for (i in 0 until assets.length()) if (assets.getJSONObject(i).optString("name") == ASSET) asset = assets.getJSONObject(i)
            asset ?: return Result.Failed("The latest release has no $ASSET.")
            val bytes = fetch(asset.getString("url"), token, "application/octet-stream")
            val digest = asset.optString("digest", "")
            if (digest.startsWith("sha256:")) {
                val actual = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
                if (!actual.equals(digest.removePrefix("sha256:"), ignoreCase = true)) return Result.Failed("The download failed its integrity check.")
            }
            Result.Updated(store.saveDownloaded(bytes.decodeToString()))
        } catch (e: Exception) {
            Result.Failed(e.message ?: "Update failed")
        }
    }

    private fun fetch(url: String, token: String, accept: String, hops: Int = 0): ByteArray {
        if (hops > 3) throw IllegalStateException("Too many redirects")
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.connectTimeout = 10_000
            c.readTimeout = 20_000
            c.instanceFollowRedirects = false
            c.setRequestProperty("Accept", accept)
            c.setRequestProperty("User-Agent", "AudioVisualizer")
            c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            if (hops == 0) c.setRequestProperty("Authorization", "Bearer $token")  // never on redirected hops
            when (val code = c.responseCode) {
                200 -> {
                    val out = java.io.ByteArrayOutputStream()
                    c.inputStream.use { input ->
                        val buf = ByteArray(8192)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            if (out.size() > MAX_BYTES) throw IllegalStateException("The file is unexpectedly large.")
                        }
                    }
                    return out.toByteArray()
                }
                301, 302, 303, 307, 308 -> {
                    val loc = c.getHeaderField("Location") ?: throw IllegalStateException("Redirect without a location")
                    if (!loc.startsWith("https://")) throw IllegalStateException("Refusing a non-HTTPS redirect")
                    return fetch(loc, token, accept, hops + 1)
                }
                401, 403 -> throw IllegalStateException("GitHub rejected the token (HTTP $code).")
                404 -> throw IllegalStateException("Release or file not found (check the token's access to the repository).")
                else -> throw IllegalStateException("GitHub returned HTTP $code.")
            }
        } finally {
            c.disconnect()
        }
    }
}
