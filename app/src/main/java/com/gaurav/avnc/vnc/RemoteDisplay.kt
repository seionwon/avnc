/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.vnc

/**
 * Represents a single remote display reported by macOS Screen Sharing
 * via Apple's DisplayLayout (encoding 1105 / 0x451).
 */
data class RemoteDisplay(
        val id: Long,
        val index: Int,
        val width: Int,
        val height: Int,
        val isMain: Boolean,
        val isMirrored: Boolean,
        val isVirtual: Boolean
) {
    companion object {
        const val FLAG_MAIN = 0x01
        const val FLAG_MIRRORED = 0x02
        const val FLAG_VIRTUAL = 0x04
    }
}

/**
 * Snapshot of available remote displays and the active selection.
 * Supports selecting a single display out of up to [MAX_SELECTABLE_DISPLAYS] monitors.
 */
data class RemoteDisplayState(
        val currentDisplayId: Long = ALL_DISPLAYS,
        val displays: List<RemoteDisplay> = emptyList(),
        val supportsSelection: Boolean = false
) {
    val selectableDisplays: List<RemoteDisplay>
        get() = displays.take(MAX_SELECTABLE_DISPLAYS)

    val canSwitchDisplays: Boolean
        get() = supportsSelection && selectableDisplays.size > 1

    companion object {
        const val ALL_DISPLAYS = 0L
        const val MAX_SELECTABLE_DISPLAYS = 6
    }
}
