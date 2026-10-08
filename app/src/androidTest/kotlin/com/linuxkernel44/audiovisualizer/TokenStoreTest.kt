package com.linuxkernel44.audiovisualizer

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TokenStoreTest {
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var store: TokenStore

    @Before fun setUp() {
        store = TokenStore(ctx)
        store.clear()
    }

    @After fun tearDown() = store.clear()

    @Test fun roundTrip() {
        assertNull(store.load())
        store.save("  ghp_exampleToken123  ")
        assertEquals("ghp_exampleToken123", store.load())
        assertTrue(store.has())
    }

    @Test fun ciphertextIsNotThePlaintext() {
        store.save("ghp_secretsecretsecret")
        val raw = ctx.getSharedPreferences("secure", android.content.Context.MODE_PRIVATE).all.values.joinToString()
        assertFalse(raw.contains("secretsecret"))
        assertNotEquals("", raw)
    }

    @Test fun replaceAndClear() {
        store.save("first")
        store.save("second")
        assertEquals("second", store.load())
        store.clear()
        assertNull(store.load())
        assertFalse(store.has())
    }

    @Test fun blankClears() {
        store.save("abc")
        store.save("   ")
        assertNull(store.load())
    }

    @Test fun corruptedDataReadsAsNoToken() {
        store.save("abc")
        ctx.getSharedPreferences("secure", android.content.Context.MODE_PRIVATE).edit().putString("token", "AAAA").apply()
        assertNull(store.load())
    }
}
