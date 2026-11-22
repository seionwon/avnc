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
    fun canSwitchDisplaysSupportsUpToSixSingleDisplays() {
        val displays = (1..8).map { idx ->
            RemoteDisplay(
                    id = 100L + idx,
                    index = idx,
                    width = 1920,
                    height = 1080,
                    isMain = (idx == 1),
                    isMirrored = false,
                    isVirtual = false
            )
        }

        assertFalse(RemoteDisplayState().canSwitchDisplays)
        assertFalse(RemoteDisplayState(displays = displays.take(1), supportsSelection = true).canSwitchDisplays)
        assertFalse(RemoteDisplayState(displays = displays.take(2), supportsSelection = false).canSwitchDisplays)

        val sixState = RemoteDisplayState(displays = displays.take(6), supportsSelection = true)
        assertTrue(sixState.canSwitchDisplays)
        assertEquals(6, sixState.selectableDisplays.size)
        assertEquals(listOf(1, 2, 3, 4, 5, 6), sixState.selectableDisplays.map { it.index })

        // More than 6 host displays are capped to 6 selectable single displays
        val eightState = RemoteDisplayState(displays = displays, supportsSelection = true)
        assertEquals(RemoteDisplayState.MAX_SELECTABLE_DISPLAYS, eightState.selectableDisplays.size)
        assertEquals(listOf(1, 2, 3, 4, 5, 6), eightState.selectableDisplays.map { it.index })
    }

    @Test
    fun serverProfileDefaultsAndNormalizationEnforceSingleDisplayUpToSix() {
        val profile = ServerProfile()
        assertEquals(ServerProfile.APPLE_DISPLAY_STANDARD, profile.appleDisplayMode)
        assertEquals(ServerProfile.APPLE_INITIAL_DISPLAY_DEFAULT, profile.appleInitialDisplay)
        assertEquals(1, profile.appleInitialDisplay)

        // Legacy 2-virtual-display mode normalizes to 1 virtual display
        assertEquals(ServerProfile.APPLE_DISPLAY_STANDARD, ServerProfile.normalizeAppleDisplayMode(0))
        assertEquals(ServerProfile.APPLE_DISPLAY_VIRTUAL_1, ServerProfile.normalizeAppleDisplayMode(1))
        assertEquals(ServerProfile.APPLE_DISPLAY_VIRTUAL_1, ServerProfile.normalizeAppleDisplayMode(2))

        // Legacy 0 (All Displays) normalizes to Display 1; 1..6 preserved; >6 clamped to 6
        assertEquals(1, ServerProfile.normalizeAppleInitialDisplay(0))
        assertEquals(1, ServerProfile.normalizeAppleInitialDisplay(-1))
        for (i in 1..6) {
            assertEquals(i, ServerProfile.normalizeAppleInitialDisplay(i))
        }
        assertEquals(6, ServerProfile.normalizeAppleInitialDisplay(7))
    }

    @Test
    fun serverProfileJsonBackwardsCompatibilityAndRoundTrip() {
        val json = Json { ignoreUnknownKeys = true }
        val legacy = json.decodeFromString<ServerProfile>("""{"name":"Legacy Mac","host":"192.168.1.5"}""")
        assertEquals(ServerProfile.APPLE_DISPLAY_STANDARD, legacy.appleDisplayMode)
        assertEquals(1, legacy.appleInitialDisplay)

        val legacyDual = json.decodeFromString<ServerProfile>(
                """{"name":"Old Dual","host":"192.168.1.9","appleDisplayMode":2,"appleInitialDisplay":0}"""
        )
        assertEquals(ServerProfile.APPLE_DISPLAY_VIRTUAL_1, ServerProfile.normalizeAppleDisplayMode(legacyDual.appleDisplayMode))
        assertEquals(1, ServerProfile.normalizeAppleInitialDisplay(legacyDual.appleInitialDisplay))

        val macProfile = ServerProfile(
                name = "Mac Studio 6-Monitor",
                host = "192.168.1.10",
                appleDisplayMode = ServerProfile.APPLE_DISPLAY_STANDARD,
                appleInitialDisplay = 6
        )
        val encoded = json.encodeToString(ServerProfile.serializer(), macProfile)
        val decoded = json.decodeFromString<ServerProfile>(encoded)
        assertEquals(ServerProfile.APPLE_DISPLAY_STANDARD, decoded.appleDisplayMode)
        assertEquals(6, decoded.appleInitialDisplay)
    }
}
