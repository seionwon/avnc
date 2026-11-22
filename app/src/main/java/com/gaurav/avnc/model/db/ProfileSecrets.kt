package com.gaurav.avnc.model.db

import android.content.Context
import android.os.Build
import android.security.KeyPairGeneratorSpec
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import androidx.annotation.RequiresApi
import androidx.room.ProvidedTypeConverter
import androidx.room.TypeConverter
import java.io.File
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.util.Calendar
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import javax.security.auth.x500.X500Principal

@ProvidedTypeConverter
class ProfileSecrets internal constructor(private val getKey: (create: Boolean) -> SecretKey) {
    constructor(context: Context) : this(ProfileKey(context.applicationContext)::get)

    @TypeConverter
    fun encrypt(value: String): ByteArray {
        if (value.isEmpty()) return byteArrayOf()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, getKey(true))
        check(cipher.iv.size == 12)
        return byteArrayOf(1) + cipher.iv + cipher.doFinal(value.toByteArray(Charsets.UTF_8))
    }

    @TypeConverter
    fun decrypt(stored: ByteArray): String {
        if (stored.isEmpty()) return ""
        require(stored.size >= 29 && stored[0] == 1.toByte()) { "Invalid encrypted profile credential" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, getKey(false), GCMParameterSpec(128, stored, 1, 12))
        return String(cipher.doFinal(stored, 13, stored.size - 13), Charsets.UTF_8)
    }
}

private class ProfileKey(private val context: Context) {
    private val store by lazy { KeyStore.getInstance("AndroidKeyStore").apply { load(null) } }
    private val wrapped = AtomicFile(File(context.noBackupFilesDir, "profile-key.v1"))
    private val created = File(context.noBackupFilesDir, "profile-key-created.v1")
    private var cached: SecretKey? = null

    @Synchronized
    fun get(create: Boolean): SecretKey {
        cached?.let { return it }
        // Keep using an existing wrapped key when a device upgrades from Android 5.
        val mayGenerate = create && !created.exists()
        val key = if (Build.VERSION.SDK_INT >= 23 && !store.containsAlias(RSA_ALIAS))
            store.getKey(AES_ALIAS, null) as? SecretKey ?: run {
                check(mayGenerate) { "Profile encryption key is unavailable" }
                generateAesKey()
            }
        else legacyKey(mayGenerate)
        created.createNewFile()
        cached = key
        return key
    }

    @RequiresApi(23)
    private fun generateAesKey(): SecretKey {
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(KeyGenParameterSpec.Builder(AES_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256).build())
        return generator.generateKey()
    }

    @Suppress("DEPRECATION")
    private fun legacyKey(create: Boolean): SecretKey {
        val cipher = Cipher.getInstance("RSA/ECB/PKCS1Padding")
        val existing = try { wrapped.readFully() } catch (_: java.io.FileNotFoundException) { null }
        if (existing != null) {
            cipher.init(Cipher.DECRYPT_MODE, store.getKey(RSA_ALIAS, null))
            return SecretKeySpec(cipher.doFinal(existing), "AES")
        }
        check(create) { "Wrapped profile encryption key is unavailable" }
        if (!store.containsAlias(RSA_ALIAS)) {
            val start = Calendar.getInstance()
            val end = (start.clone() as Calendar).apply { add(Calendar.YEAR, 50) }
            val generator = KeyPairGenerator.getInstance("RSA", "AndroidKeyStore")
            generator.initialize(KeyPairGeneratorSpec.Builder(context).setAlias(RSA_ALIAS).setKeySize(2048)
                    .setSubject(X500Principal("CN=AVNC Profile Key")).setSerialNumber(BigInteger.ONE)
                    .setStartDate(start.time).setEndDate(end.time).build())
            generator.generateKeyPair()
        }
        val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        cipher.init(Cipher.ENCRYPT_MODE, store.getCertificate(RSA_ALIAS).publicKey)
        val bytes = cipher.doFinal(key.encoded)
        val output = wrapped.startWrite()
        try {
            output.write(bytes)
            wrapped.finishWrite(output)
        } catch (e: Exception) {
            wrapped.failWrite(output)
            throw e
        }
        return key
    }

    private companion object {
        const val AES_ALIAS = "avnc.profile.aes.v1"
        const val RSA_ALIAS = "avnc.profile.rsa.v1"
    }
}
