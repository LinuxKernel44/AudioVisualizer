package com.linuxkernel44.audiovisualizer

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Keeps the user's GitHub token encrypted with a key that never leaves the Android Keystore
 * (AES-256-GCM). Only the ciphertext and IV are written to SharedPreferences.
 */
class TokenStore(ctx: Context) {
    private val prefs = ctx.getSharedPreferences("secure", Context.MODE_PRIVATE)

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return gen.generateKey()
    }

    fun save(token: String) {
        if (token.isBlank()) return clear()
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()) }
        val ct = c.doFinal(token.trim().toByteArray(Charsets.UTF_8))
        prefs.edit().putString("iv", Base64.encodeToString(c.iv, Base64.NO_WRAP))
            .putString("token", Base64.encodeToString(ct, Base64.NO_WRAP)).apply()
    }

    fun load(): String? {
        val iv = prefs.getString("iv", null) ?: return null
        val ct = prefs.getString("token", null) ?: return null
        return try {
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, Base64.decode(iv, Base64.NO_WRAP)))
            String(c.doFinal(Base64.decode(ct, Base64.NO_WRAP)), Charsets.UTF_8)
        } catch (e: Exception) {
            null  // key invalidated or data corrupted: behave as "no token"
        }
    }

    fun has(): Boolean = prefs.contains("token")
    fun clear() = prefs.edit().remove("iv").remove("token").apply()

    private companion object {
        const val ALIAS = "av_github_token"
    }
}
