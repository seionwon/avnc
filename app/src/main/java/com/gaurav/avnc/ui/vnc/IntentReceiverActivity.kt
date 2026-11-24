/*
 * Copyright (c) 2021  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */
package com.gaurav.avnc.ui.vnc

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.gaurav.avnc.R
import com.gaurav.avnc.model.ServerProfile
import com.gaurav.avnc.model.db.MainDb
import com.gaurav.avnc.vnc.VncUri
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.launch
import kotlin.coroutines.resume

/**
 * Handles "external" intents and launches [VncActivity] with appropriate profiles.
 *
 * Current intent types:
 *  - vnc:// URIs
 *  - App shortcuts
 */
class IntentReceiverActivity : AppCompatActivity() {

    companion object {
        private const val SHORTCUT_PROFILE_ID_KEY = "com.gaurav.avnc.shortcut_profile_id"

        fun createShortcutIntent(context: Context, profileId: Long): Intent {
            check(profileId != 0L) { "Cannot create shortcut with profileId = 0." }
            return Intent(context, IntentReceiverActivity::class.java).apply {
                action = Intent.ACTION_VIEW
                putExtra(SHORTCUT_PROFILE_ID_KEY, profileId)
            }
        }
    }

    private val profileDao by lazy { MainDb.getInstance(this).serverProfileDao }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        handleIntent()
    }

    private fun handleIntent() = lifecycleScope.launch {
        if (intent.data?.scheme == "vnc")
            handleVncUri(VncUri(intent.data!!.toString()))
        else if (intent.hasExtra(SHORTCUT_PROFILE_ID_KEY))
            launchFromProfileId(intent.getLongExtra(SHORTCUT_PROFILE_ID_KEY, 0))
        else
            toast("Invalid intent: Server info is missing!")

        finish()
    }

    private suspend fun handleVncUri(uri: VncUri) {
        if (!validateUri(uri))
            return

        if (uri.host.isNullOrBlank()) {
            val id = profileDao.findIdByName(uri.connectionName.orEmpty())
            if (id == null) toast("No server found with name '${uri.connectionName}'")
            else startVncActivity(this, ServerProfile(ID = id))
            return
        }

        val profile = uri.toServerProfile()
        if (uri.saveConnection) {
            if (!confirmSave(profile)) return
            val id = profileDao.addFromLink(profile)
            if (id == null) {
                toast(getString(R.string.msg_link_name_in_use))
                return
            }
            profile.ID = id
            toast(getString(R.string.msg_link_added))
        } else if (!confirmConnect(profile)) {
            return
        }
        startVncActivity(this, profile)
    }

    private suspend fun launchFromProfileId(profileId: Long) {
        if (!profileDao.exists(profileId)) toast(getString(R.string.msg_shortcut_server_deleted))
        else startVncActivity(this, ServerProfile(ID = profileId))
    }


    /**
     * A link can come from any web page or app, so the user sees where it leads
     * before a connection (and a possible password prompt) starts.
     */
    private suspend fun confirmConnect(profile: ServerProfile): Boolean = suspendCancellableCoroutine { result ->
        val dialog = MaterialAlertDialogBuilder(this)
                .setTitle(R.string.title_open_link)
                .setMessage(getString(R.string.msg_open_link, profile.endpointDescription()))
                .setPositiveButton(android.R.string.ok) { _, _ -> result.resume(true) }
                .setNegativeButton(android.R.string.cancel) { _, _ -> result.resume(false) }
                .setOnCancelListener { result.resume(false) }
                .show()
        result.invokeOnCancellation { dialog.dismiss() }
    }

    private suspend fun confirmSave(profile: ServerProfile): Boolean = suspendCancellableCoroutine { result ->
        val dialog = MaterialAlertDialogBuilder(this)
                .setTitle(R.string.title_save_link)
                .setMessage(getString(R.string.msg_save_link, profile.name, profile.endpointDescription()))
                .setPositiveButton(android.R.string.ok) { _, _ -> result.resume(true) }
                .setNegativeButton(android.R.string.cancel) { _, _ -> result.resume(false) }
                .setOnCancelListener { result.resume(false) }
                .show()
        result.invokeOnCancellation { dialog.dismiss() }
    }

    private fun validateUri(uri: VncUri): Boolean {
        if (uri.host.isNullOrBlank() && uri.connectionName.isNullOrBlank() ||
            uri.port?.let { it !in 0..65535 } == true || uri.sshPort?.let { it !in 1..65535 } == true) {
            toast(getString(R.string.msg_invalid_vnc_uri))
            return false
        }

        if (uri.channelType !in listOf(null, ServerProfile.CHANNEL_TCP, ServerProfile.CHANNEL_SSH_TUNNEL)) {
            toast("Unknown channel type: ${uri.channelType}")
            return false
        }

        val supportedSecurityTypes = resources.getStringArray(R.array.profile_editor_security_values)
        if (uri.securityType != null && uri.securityType.toString() !in supportedSecurityTypes) {
            toast("Unknown security type: ${uri.securityType}")
            return false
        }

        if (uri.channelType == ServerProfile.CHANNEL_SSH_TUNNEL && uri.sshHost.isNullOrBlank()) {
            toast("Missing SSH host in URI")
            return false
        }

        if (uri.channelType == ServerProfile.CHANNEL_SSH_TUNNEL && uri.sshUsername.isNullOrBlank()) {
            toast("Missing SSH username in URI")
            return false
        }

        return true
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
