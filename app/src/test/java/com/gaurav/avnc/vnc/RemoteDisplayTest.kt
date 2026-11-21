/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.vnc

import com.gaurav.avnc.model.ServerProfile
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RemoteDisplayTest {

    @Test
    fun canSwitchDisplaysRequiresMultipleDisplaysAndServerSupport() {
        val d1 = RemoteDisplay(id = 101L, index = 1, width = 1920, height = 1080, isMain = true, isMirrored = false, isVirtual = true)
        val d2 = RemoteDisplay(id = 202L, index = 2, width = 1920, height = 1080, isMain = false, isMirrored = false, isVirtual = true)

        assertFalse(RemoteDisplayState().canSwitchDisplays)
        assertFalse(RemoteDisplayState(displays = listOf(d1), supportsSelection = true).canSwitchDisplays)
        assertFalse(RemoteDisplayState(displays = listOf(d1, d2), supportsSelection = false).canSwitchDisplays)
        assertTrue(RemoteDisplayState(displays = listOf(d1, d2), supportsSelection = true).canSwitchDisplays)
    }

    @Test
    fun serverProfileDefaultsToStandardAllDisplays() {
        val profile = ServerProfile()
        assertEquals(ServerProfile.APPLE_DISPLAY_STANDARD, profile.appleDisplayMode)
        assertEquals(ServerProfile.APPLE_INITIAL_DISPLAY_ALL, profile.appleInitialDisplay)
    }

    @Test
    fun serverProfileJsonBackwardsCompatibilityAndRoundTrip() {
        val json = Json { ignoreUnknownKeys = true }
        val legacy = json.decodeFromString<ServerProfile>("""{"name":"Legacy Mac","host":"192.168.1.5"}""")
        assertEquals(ServerProfile.APPLE_DISPLAY_STANDARD, legacy.appleDisplayMode)
        assertEquals(ServerProfile.APPLE_INITIAL_DISPLAY_ALL, legacy.appleInitialDisplay)

        val macProfile = ServerProfile(
                name = "MacBook Pro",
                host = "192.168.1.10",
                appleDisplayMode = ServerProfile.APPLE_DISPLAY_VIRTUAL_2,
                appleInitialDisplay = 1
        )
        val encoded = json.encodeToString(ServerProfile.serializer(), macProfile)
        val decoded = json.decodeFromString<ServerProfile>(encoded)
        assertEquals(ServerProfile.APPLE_DISPLAY_VIRTUAL_2, decoded.appleDisplayMode)
        assertEquals(1, decoded.appleInitialDisplay)
    }
}
