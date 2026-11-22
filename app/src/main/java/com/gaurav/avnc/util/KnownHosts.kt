/*
 * Copyright (c) 2024  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.util

import android.content.Context
import android.util.AtomicFile
import android.util.Log
import com.gaurav.avnc.model.ServerProfile
import java.io.File
import java.security.MessageDigest
import java.security.cert.Certificate
import java.security.cert.X509Certificate

private const val TAG = "KnownHosts"

//-------------------- Known SSH Hosts ----------------------------
fun getKnownHostsFile(context: Context) = File(context.filesDir, "known-hosts")

fun forgetKnownHosts(context: Context): Boolean {
    return runCatching { getKnownHostsFile(context).let { !it.exists() || it.delete() } }
            .onFailure { Log.e(TAG, "Cannot forget known hosts", it) }
            .getOrDefault(false)
}

//------------------ Trusted certificates -------------------------
enum class CertificateTrust { Unknown, Trusted, Changed }

fun certificateFingerprint(certificate: Certificate): ByteArray =
        MessageDigest.getInstance("SHA-256").digest(certificate.encoded)

private fun pinFile(context: Context, profile: ServerProfile) =
        AtomicFile(File(context.filesDir, "trusted_certs/v2/${profile.endpointKey()}"))

@Synchronized
fun certificateTrust(context: Context, profile: ServerProfile, certificate: Certificate): CertificateTrust {
    val pin = pinFile(context, profile)
    val stored = try { pin.readFully() } catch (_: java.io.FileNotFoundException) { return CertificateTrust.Unknown }
    return if (MessageDigest.isEqual(stored, certificateFingerprint(certificate))) CertificateTrust.Trusted
           else CertificateTrust.Changed
}

@Synchronized
fun trustCertificate(context: Context, profile: ServerProfile, certificate: Certificate): Boolean {
    when (certificateTrust(context, profile, certificate)) {
        CertificateTrust.Trusted -> return true
        CertificateTrust.Changed -> return false
        CertificateTrust.Unknown -> Unit
    }
    val file = pinFile(context, profile)
    file.baseFile.parentFile?.mkdirs()
    val stream = file.startWrite()
    try {
        stream.write(certificateFingerprint(certificate))
        file.finishWrite(stream)
    } catch (e: Exception) {
        file.failWrite(stream)
        throw e
    }
    return certificateTrust(context, profile, certificate) == CertificateTrust.Trusted
}

@Synchronized
fun deleteTrustedCertificates(context: Context): Boolean =
        File(context.filesDir, "trusted_certs").let { !it.exists() || it.deleteRecursively() }

fun getUnknownCertificateMessage(context: Context, profile: ServerProfile, certificate: X509Certificate): String {
    val fingerprint = certificateFingerprint(certificate)
            .toHexString(HexFormat { upperCase = true; bytes { byteSeparator = ":" } })
    return context.getString(com.gaurav.avnc.R.string.msg_verify_certificate,
                             profile.endpointDescription(), fingerprint)
}
