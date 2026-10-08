package com.linuxkernel44.audiovisualizer

import android.media.MediaMetadataRetriever
import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** End-to-end checks of the native engine on a real device: decode, analysis, cache, export. */
@RunWith(AndroidJUnit4::class)
class PipelineTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private lateinit var wav: File
    private lateinit var cache: File

    @Before fun setUp() {
        wav = File(ctx.cacheDir, "click120.wav")
        inst.context.assets.open("click120.wav").use { i -> wav.outputStream().use { i.copyTo(it) } }
        cache = File(ctx.cacheDir, "test-analysis").apply { deleteRecursively(); mkdirs() }
    }

    @After fun tearDown() {
        wav.delete()
        cache.deleteRecursively()
    }

    private fun openWav(): ParcelFileDescriptor = ParcelFileDescriptor.open(wav, ParcelFileDescriptor.MODE_READ_ONLY)

    private fun waitFor(timeoutMs: Long, what: String, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeoutMs
        while (!cond()) {
            if (System.currentTimeMillis() > end) throw AssertionError("Timed out waiting for $what")
            Thread.sleep(50)
        }
    }

    @Test fun engineReportsAVersion() {
        assertTrue(NativeEngine.version().matches(Regex("\\d+\\.\\d+\\.\\d+")))
    }

    @Test fun analysisFindsTheTempoAndCachesTheResult() {
        val engine = NativeEngine()
        try {
            engine.setCacheDir(cache.absolutePath)
            engine.setBeatModel("")  // DSP only, so the result does not depend on the model
            openWav().use { assertEquals(null, engine.load(it.fd)) }
            assertTrue("duration", engine.durationMs() in 5_800..6_200)
            waitFor(60_000, "analysis") { engine.analysisState() >= 2 }
            assertEquals("analysis state", 2, engine.analysisState())
            val s = AnalysisSummary.parse(engine.summaryJson())
            assertNotNull(s)
            assertEquals(120f, s!!.bpm, 3f)
            assertEquals("dsp", s.beatSource)
            assertTrue("beats found", s.downbeats.isNotEmpty())
            assertTrue("sections", s.sections.isNotEmpty())
            assertTrue("cache file written", cache.listFiles()?.any { it.name.endsWith(".avt") } == true)

            // Loading the same file again must be a cache hit.
            openWav().use { assertEquals(null, engine.load(it.fd)) }
            waitFor(10_000, "cached analysis") { engine.analysisState() >= 2 }
            assertTrue(engine.analysisFromCache())
        } finally {
            engine.release()
        }
    }

    @Test fun unreadableAudioIsReportedNotCrashed() {
        val junk = File(ctx.cacheDir, "junk.wav").apply { writeBytes(ByteArray(2048) { it.toByte() }) }
        val engine = NativeEngine()
        try {
            ParcelFileDescriptor.open(junk, ParcelFileDescriptor.MODE_READ_ONLY).use {
                assertNotNull(engine.load(it.fd))  // returns an error message
            }
        } finally {
            engine.release()
            junk.delete()
        }
    }

    @Test fun exportProducesAPlayableMp4() {
        val out = File(ctx.cacheDir, "export-test.mp4").apply { delete() }
        ParcelFileDescriptor.open(out, ParcelFileDescriptor.MODE_READ_WRITE or ParcelFileDescriptor.MODE_CREATE).use { raf ->
            openWav().use { audio ->
                val h = NativeExport.start(
                    audio.fd, raf.fd, 640, 360, 30, 2_000_000, 128_000, -1, 1f,
                    1f, 0f, 0f, 0f, 0.3f, -1, 1f, 1f, cache.absolutePath, "",
                    null, 0, 0, null, 0, 0, 1f, 0f,
                )
                try {
                    waitFor(120_000, "export") { NativeExport.state(h) != 0 }
                    assertEquals("export state (error: ${NativeExport.error(h)})", 1, NativeExport.state(h))
                } finally {
                    NativeExport.release(h)
                }
            }
        }
        val r = MediaMetadataRetriever()
        try {
            r.setDataSource(out.absolutePath)
            val dur = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)!!.toLong()
            assertTrue("duration $dur ms", dur in 5_800..6_300)
            assertEquals("640", r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH))
            assertEquals("360", r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT))
            assertEquals("yes", r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO))
            assertEquals("yes", r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_VIDEO))
            assertNotNull("a decodable frame", r.getFrameAtTime(3_000_000))
        } finally {
            r.release()
            out.delete()
        }
    }

    @Test fun exportCanBeCancelled() {
        val out = File(ctx.cacheDir, "cancel-test.mp4").apply { delete() }
        ParcelFileDescriptor.open(out, ParcelFileDescriptor.MODE_READ_WRITE or ParcelFileDescriptor.MODE_CREATE).use { raf ->
            openWav().use { audio ->
                val h = NativeExport.start(
                    audio.fd, raf.fd, 1280, 720, 60, 8_000_000, 128_000, -1, 1f,
                    1f, 0f, 0f, 0f, 0.3f, -1, 1f, 1f, cache.absolutePath, "",
                    null, 0, 0, null, 0, 0, 1f, 0f,
                )
                try {
                    Thread.sleep(300)
                    NativeExport.cancel(h)
                    assertEquals(3, NativeExport.state(h))
                } finally {
                    NativeExport.release(h)
                }
            }
        }
        out.delete()
    }
}

/** Decoding and analysis must cope with unusual sample rates, bit depths and channel layouts. */
@RunWith(AndroidJUnit4::class)
class FormatTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    @Test fun everyBundledFormatDecodesAndAnalyses() {
        val names = inst.context.assets.list("formats")!!.sorted()
        assertTrue("assets present", names.size >= 6)
        val failures = ArrayList<String>()
        for (name in names) {
            val f = File(ctx.cacheDir, "fmt-$name")
            inst.context.assets.open("formats/$name").use { i -> f.outputStream().use { i.copyTo(it) } }
            val cache = File(ctx.cacheDir, "fmt-cache").apply { deleteRecursively(); mkdirs() }
            val engine = NativeEngine()
            try {
                engine.setCacheDir(cache.absolutePath)
                engine.setBeatModel("")
                val err = ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY).use { engine.load(it.fd) }
                if (err != null) { failures += "$name: load failed: $err"; continue }
                val dur = engine.durationMs()
                if (dur !in 3_500..4_500) failures += "$name: duration $dur ms"
                val end = System.currentTimeMillis() + 60_000
                while (engine.analysisState() < 2 && System.currentTimeMillis() < end) Thread.sleep(50)
                if (engine.analysisState() != 2) { failures += "$name: analysis state ${engine.analysisState()} (${engine.analysisError()})"; continue }
                val s = AnalysisSummary.parse(engine.summaryJson())
                if (s == null || s.durationSec !in 3.5f..4.5f) failures += "$name: bad summary"
                // The analysis counts the samples it really decoded: dropped codec frames would make it
                // shorter than the container duration (priming/padding explain a few tens of ms).
                if (s != null && Math.abs(s.durationSec * 1000.0 - dur) > 150.0) failures += "$name: decoded ${s.durationSec} s but the file says $dur ms"
                // Playback path: start, let the spectrum worker run briefly, seek, stop.
                engine.play()
                Thread.sleep(400)
                engine.seekTo(1_000)
                Thread.sleep(200)
                engine.pause()
            } finally {
                engine.release()
                f.delete()
                cache.deleteRecursively()
            }
        }
        assertTrue(failures.joinToString("\n"), failures.isEmpty())
    }
}
