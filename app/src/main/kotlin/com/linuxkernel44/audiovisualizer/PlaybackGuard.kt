package com.linuxkernel44.audiovisualizer

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager

/**
 * Makes playback a good citizen: takes audio focus while playing and pauses when another app
 * (a call, a navigation prompt, another player) takes it, and when headphones are unplugged.
 */
class PlaybackGuard(private val ctx: Context, private val onPause: () -> Unit) {
    private val audio = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var request: AudioFocusRequest? = null
    private var noisyRegistered = false

    private val noisy = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            if (i.action == AudioManager.ACTION_AUDIO_BECOMING_NOISY) onPause()
        }
    }

    /** Returns true if focus was granted. Callable from any thread. */
    @Synchronized
    fun acquire(): Boolean {
        val req = request ?: AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setOnAudioFocusChangeListener { change ->
                if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT ||
                    change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK) onPause()
            }.build().also { request = it }
        if (!noisyRegistered) {
            ctx.registerReceiver(noisy, IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY), Context.RECEIVER_NOT_EXPORTED)
            noisyRegistered = true
        }
        return audio.requestAudioFocus(req) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    @Synchronized
    fun release() {
        if (!noisyRegistered) return  // not held
        request?.let { audio.abandonAudioFocusRequest(it) }
        ctx.unregisterReceiver(noisy)
        noisyRegistered = false
    }
}
