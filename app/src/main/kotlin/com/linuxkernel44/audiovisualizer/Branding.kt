package com.linuxkernel44.audiovisualizer

import android.content.Context
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Typeface
import android.net.Uri
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import java.nio.ByteBuffer

enum class TitlePosition(val label: String) { BottomLeft("Bottom left"), BottomCenter("Bottom"), TopLeft("Top left"), Center("Center") }

/** Optional title / artist text, logo and background image shown over the visuals and in exports. */
data class Branding(
    val enabled: Boolean = false,
    val title: String = "",
    val artist: String = "",
    val position: TitlePosition = TitlePosition.BottomLeft,
    val logoUri: String? = null,
    val backgroundUri: String? = null,
    val overlayOpacity: Float = 1f,
    val backgroundOpacity: Float = 0.45f,
) {
    val hasOverlay: Boolean get() = enabled && (title.isNotBlank() || artist.isNotBlank() || logoUri != null)
    val hasBackground: Boolean get() = enabled && backgroundUri != null

    fun save(p: SharedPreferences) = p.edit()
        .putBoolean("enabled", enabled).putString("title", title).putString("artist", artist)
        .putInt("position", position.ordinal).putString("logo", logoUri).putString("bg", backgroundUri)
        .putFloat("overlayOpacity", overlayOpacity).putFloat("bgOpacity", backgroundOpacity).apply()

    companion object {
        fun load(p: SharedPreferences) = Branding(
            enabled = p.getBoolean("enabled", false), title = p.getString("title", "") ?: "",
            artist = p.getString("artist", "") ?: "",
            position = TitlePosition.values().getOrElse(p.getInt("position", 0)) { TitlePosition.BottomLeft },
            logoUri = p.getString("logo", null), backgroundUri = p.getString("bg", null),
            overlayOpacity = p.getFloat("overlayOpacity", 1f), backgroundOpacity = p.getFloat("bgOpacity", 0.45f),
        )
    }
}

/** A branding layer as raw RGBA8 pixels (the native renderer stretches it over the frame). */
class Layer(val rgba: ByteArray, val width: Int, val height: Int)

/** Draws the branding layers at a given pixel size, so they match the target aspect ratio. */
object BrandingRenderer {
    fun overlay(ctx: Context, b: Branding, w: Int, h: Int): Layer? {
        if (!b.hasOverlay) return null
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        val unit = minOf(w, h).toFloat()
        val margin = unit * 0.06f

        loadBitmap(ctx, b.logoUri, (unit * 0.3f).toInt())?.let { logo ->
            val target = unit * 0.14f
            val scale = target / maxOf(logo.width, logo.height)
            val lw = logo.width * scale
            val lh = logo.height * scale
            val m = Matrix().apply { postScale(scale, scale); postTranslate(w - margin - lw, margin) }
            canvas.drawBitmap(logo, m, Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG))
            if (lh <= 0f) return null
            logo.recycle()
        }

        val title = b.title.trim()
        val artist = b.artist.trim()
        if (title.isNotEmpty() || artist.isNotEmpty()) {
            val titlePaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE; textSize = unit * 0.062f; typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
                setShadowLayer(unit * 0.012f, 0f, unit * 0.004f, Color.argb(200, 0, 0, 0))
            }
            val artistPaint = TextPaint(titlePaint).apply {
                color = Color.argb(230, 255, 255, 255); textSize = unit * 0.04f; typeface = Typeface.DEFAULT
            }
            val maxWidth = (w - 2 * margin).toInt().coerceAtLeast(16)
            val align = if (b.position == TitlePosition.BottomCenter || b.position == TitlePosition.Center) Layout.Alignment.ALIGN_CENTER else Layout.Alignment.ALIGN_NORMAL
            val layouts = buildList {
                if (title.isNotEmpty()) add(StaticLayout.Builder.obtain(title, 0, title.length, titlePaint, maxWidth).setAlignment(align).setMaxLines(2).setEllipsize(android.text.TextUtils.TruncateAt.END).build())
                if (artist.isNotEmpty()) add(StaticLayout.Builder.obtain(artist, 0, artist.length, artistPaint, maxWidth).setAlignment(align).setMaxLines(1).setEllipsize(android.text.TextUtils.TruncateAt.END).build())
            }
            val total = layouts.sumOf { it.height } + (layouts.size - 1) * (unit * 0.01f).toInt()
            val top = when (b.position) {
                TitlePosition.TopLeft -> margin
                TitlePosition.Center -> (h - total) / 2f
                else -> h - margin - total
            }
            var y = top
            for (l in layouts) {
                canvas.save()
                canvas.translate(margin, y)
                l.draw(canvas)
                canvas.restore()
                y += l.height + unit * 0.01f
            }
        }
        return layerOf(bmp)
    }

    fun background(ctx: Context, b: Branding, w: Int, h: Int): Layer? {
        if (!b.hasBackground) return null
        val src = loadBitmap(ctx, b.backgroundUri, maxOf(w, h)) ?: return null
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val scale = maxOf(w.toFloat() / src.width, h.toFloat() / src.height)  // center crop
        val m = Matrix().apply { postScale(scale, scale); postTranslate((w - src.width * scale) / 2f, (h - src.height * scale) / 2f) }
        Canvas(bmp).apply { drawColor(Color.BLACK); drawBitmap(src, m, Paint(Paint.FILTER_BITMAP_FLAG)) }
        src.recycle()
        return layerOf(bmp)
    }

    private fun layerOf(bmp: Bitmap): Layer {
        val buf = ByteBuffer.allocate(bmp.byteCount)
        bmp.copyPixelsToBuffer(buf)  // ARGB_8888 is stored as R,G,B,A bytes, premultiplied
        val l = Layer(buf.array(), bmp.width, bmp.height)
        bmp.recycle()
        return l
    }

    /** Decodes an image from a content/file Uri, sampled down so its longest side is about `maxSide`. */
    fun loadBitmap(ctx: Context, uri: String?, maxSide: Int): Bitmap? {
        if (uri == null) return null
        return try {
            val u = Uri.parse(uri)
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            ctx.contentResolver.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, bounds) }
            var sample = 1
            while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= maxSide) sample *= 2
            ctx.contentResolver.openInputStream(u)?.use {
                BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample })
            }
        } catch (e: Exception) {
            null
        }
    }
}
